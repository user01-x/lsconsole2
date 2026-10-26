// Overlay -- the console as a real Windows window.
//
// A layered popup owned by the session's main window, on a thread of its own. Windows composites
// it over the game, so nothing drawn inside the game can cover it, and a click on it goes to it
// before the game or any hook in the game sees it. Owned (not topmost): it rides the owner's
// z-order and minimises with it.
//
// Pixels come from the host's shared-memory surface: it is selected into a memory DC as a DIB
// section over the mapping itself (no copy) and handed to UpdateLayeredWindowIndirect.
//
// Rule: this thread never blocks. Everything it sends goes through hostlink::Send, which queues.
#include "Console.h"
#include "../common/Protocol.h"
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace overlay {
namespace {

enum : UINT {
	WM_LSC2_SHOW = WM_APP + 1, // wParam: focus the input line
	WM_LSC2_HIDE,
	WM_LSC2_TOGGLE,
	WM_LSC2_HOSTMSG,           // lParam: std::string* (owned by the receiver)
	WM_LSC2_LINKUP,
	WM_LSC2_LINKDOWN,
	WM_LSC2_QUIT,
	WM_LSC2_SETKEY,            // lParam: std::string* -- the console bind's combo
	WM_LSC2_SETLAYOUT,         // lParam: std::string* -- the InnerSpace session name (layout key)
};
constexpr UINT_PTR kRepositionTimer = 1;
constexpr UINT_PTR kResizeTimer = 2;

std::thread g_thread;
std::atomic<HWND> g_hwnd{nullptr};
HANDLE g_readyEv = nullptr;

// Everything below is touched only on the overlay thread, except g_status (under g_statusMu).
HWND g_owner = nullptr;
HANDLE g_frameEv = nullptr;
HWINEVENTHOOK g_hook = nullptr;
HCURSOR g_cursor = nullptr;

HANDLE g_map = nullptr;
lsc2::SurfaceHeader *g_hdr = nullptr;
HDC g_memDC = nullptr;
HBITMAP g_dib = nullptr;
HGDIOBJ g_oldBmp = nullptr;
uint32_t g_sw = 0, g_sh = 0;
LONG g_lastSeq = 0;
bool g_needFull = true;

bool g_wanted = false;      // the user asked for it
bool g_shown = false;       // it is on screen
std::atomic<bool> g_shownAny{false}; // the same, for other threads (overlay::Shown)
bool g_ownerHidden = false;
bool g_tracking = false;    // TrackMouseEvent armed
int g_buttons = 0;          // the button held for the current drag (MouseEvent::Button)
RECT g_place = {};          // where the console sits, screen pixels

// Moving and sizing (the user's spec, 2026-10-26). A 5 px band along each edge: the top band moves
// the console while it's on screen, else the bottom band does; the other edges and the corners
// size it. Once moved or sized, the console keeps that offset and size relative to the owner's
// client area (it still follows the window), saved per InnerSpace session in layout.json.
enum Zone { Z_NONE, Z_MOVE, Z_L, Z_R, Z_T, Z_B, Z_TL, Z_TR, Z_BL, Z_BR };
struct Drag { bool on = false; Zone zone = Z_NONE; POINT start = {}; RECT from = {}; };
Drag g_drag;
bool g_custom = false;      // the user placed it: g_rel applies
RECT g_rel = {};            // relative to the owner's client origin
std::string g_layoutKey;    // the InnerSpace session name
bool g_resizePending = false;
uint32_t g_askedW = 0, g_askedH = 0; // the last size asked of the host
constexpr int kMinW = 320, kMinH = 120;

// The console key (InnerSpace's `console` bind). While our window has focus InnerSpace never sees
// the key, so the window recognises it itself: it closes the console instead of typing.
struct Combo { UINT vk = 0; bool ctrl = false, shift = false, alt = false; };
Combo g_toggle;
UINT g_swallowUp = 0;        // the key-up that belongs to a swallowed toggle key-down

std::mutex g_statusMu;
std::string g_status = "not started";

// ---------------------------------------------------------------------------------------------
// The owner: this process's main window
// ---------------------------------------------------------------------------------------------
struct FindData { DWORD pid; HWND best; LONGLONG bestArea; };

BOOL CALLBACK FindMainProc(HWND h, LPARAM lp)
{
	auto *d = (FindData *)lp;
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid != d->pid || h == g_hwnd || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
	if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
	RECT r;
	GetWindowRect(h, &r);
	LONGLONG area = (LONGLONG)(r.right - r.left) * (r.bottom - r.top);
	if (area > d->bestArea) { d->best = h; d->bestArea = area; }
	return TRUE;
}

HWND FindMainWindow()
{
	FindData d = {GetCurrentProcessId(), nullptr, 0};
	EnumWindows(FindMainProc, (LPARAM)&d);
	return d.best;
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND h, LONG idObject, LONG idChild, DWORD, DWORD);

void SetOwner(HWND owner)
{
	if (g_hook) { UnhookWinEvent(g_hook); g_hook = nullptr; }
	g_owner = owner;
	g_ownerHidden = false;
	if (!owner) return;
	if (GetWindow(g_hwnd, GW_OWNER) != owner)
	{
		// Re-owning after creation doesn't move us in z-order: put us directly above the new owner.
		SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, (LONG_PTR)owner); // owner, despite the name
		HWND above = GetWindow(owner, GW_HWNDPREV);
		SetWindowPos(g_hwnd, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	DWORD pid = 0;
	DWORD tid = GetWindowThreadProcessId(owner, &pid);
	// DESTROY..LOCATIONCHANGE covers destroy, show, hide and move/size; filtered to the owner below.
	g_hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr, WinEventProc, pid, tid,
	                         WINEVENT_OUTOFCONTEXT);
}

// ---------------------------------------------------------------------------------------------
// Placement and visibility
// ---------------------------------------------------------------------------------------------
double Scale() { return GetDpiForWindow(g_hwnd) / 96.0; }

POINT OwnerOrigin()
{
	POINT tl = {0, 0};
	ClientToScreen(g_owner, &tl);
	return tl;
}

// The console's rectangle for the owner's current client area: where the user put it, or by
// default the top 40 %, full width.
bool ComputePlace(RECT &r)
{
	if (!g_owner || !IsWindow(g_owner)) return false;
	POINT tl = OwnerOrigin();
	if (g_custom)
	{
		r = {tl.x + g_rel.left, tl.y + g_rel.top, tl.x + g_rel.right, tl.y + g_rel.bottom};
		return true;
	}
	RECT cr;
	if (!GetClientRect(g_owner, &cr) || cr.right < 200 || cr.bottom < 150) return false;
	int h = std::max(200, (int)(cr.bottom * 2 / 5));
	r = {tl.x, tl.y, tl.x + cr.right, tl.y + std::min(h, (int)cr.bottom)};
	return true;
}

// Ask the host for a view of the console's current size (the window takes that size when the
// matching surface arrives: UpdateLayeredWindow sets it).
void AskSize()
{
	uint32_t w = g_place.right - g_place.left, h = g_place.bottom - g_place.top;
	hostlink::SetSize(w, h, Scale());
	if (w == g_askedW && h == g_askedH) return;
	g_askedW = w; g_askedH = h;
	hostlink::Send({{"t", "resize"}, {"w", w}, {"h", h}, {"scale", Scale()}});
}

void Reposition()
{
	RECT r;
	if (!ComputePlace(r)) return;
	g_place = r;
	AskSize();
	SetWindowPos(g_hwnd, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------------------------
// Moving and sizing
// ---------------------------------------------------------------------------------------------
int Edge() { return std::max(5, (int)(5 * Scale() + 0.5)); }

// The top band moves the console only while it's on screen; otherwise the bottom band does.
bool TopOnScreen()
{
	RECT wr;
	GetWindowRect(g_hwnd, &wr);
	MONITORINFO mi = {sizeof(mi)};
	if (!GetMonitorInfoW(MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST), &mi)) return true;
	return wr.top >= mi.rcMonitor.top && wr.top + Edge() <= mi.rcMonitor.bottom;
}

Zone ZoneAt(POINT p) // client coordinates
{
	RECT wr;
	GetWindowRect(g_hwnd, &wr);
	int w = wr.right - wr.left, h = wr.bottom - wr.top, e = Edge();
	bool l = p.x < e, r = p.x >= w - e, t = p.y < e, b = p.y >= h - e;
	if (t && l) return Z_TL;
	if (t && r) return Z_TR;
	if (b && l) return Z_BL;
	if (b && r) return Z_BR;
	bool topMoves = TopOnScreen();
	if (t) return topMoves ? Z_MOVE : Z_T;
	if (b) return topMoves ? Z_B : Z_MOVE;
	if (l) return Z_L;
	if (r) return Z_R;
	return Z_NONE;
}

HCURSOR ZoneCursor(Zone z)
{
	LPCWSTR id = z == Z_MOVE ? (LPCWSTR)IDC_SIZEALL : z == Z_L || z == Z_R ? (LPCWSTR)IDC_SIZEWE
	             : z == Z_T || z == Z_B ? (LPCWSTR)IDC_SIZENS : z == Z_TL || z == Z_BR ? (LPCWSTR)IDC_SIZENWSE
	             : (LPCWSTR)IDC_SIZENESW;
	return LoadCursorW(nullptr, id);
}

std::wstring LayoutPath()
{
	wchar_t lad[MAX_PATH] = L"";
	GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
	std::wstring dir = std::wstring(lad) + L"\\LSConsole2";
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir + L"\\layout.json";
}

json ReadLayouts()
{
	FILE *f = nullptr;
	_wfopen_s(&f, LayoutPath().c_str(), L"rb");
	if (!f) return json::object();
	std::string s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
	fclose(f);
	json j = json::parse(s, nullptr, false);
	return j.is_object() ? j : json::object();
}

void SaveLayout()
{
	if (g_layoutKey.empty() || !g_owner) return;
	POINT tl = OwnerOrigin();
	g_rel = {g_place.left - tl.x, g_place.top - tl.y, g_place.right - tl.x, g_place.bottom - tl.y};
	g_custom = true;
	json all = ReadLayouts();
	all[g_layoutKey] = {g_rel.left, g_rel.top, g_rel.right - g_rel.left, g_rel.bottom - g_rel.top};
	FILE *f = nullptr;
	_wfopen_s(&f, LayoutPath().c_str(), L"wb");
	if (!f) return;
	std::string s = all.dump(1, '\t');
	fwrite(s.data(), 1, s.size(), f);
	fclose(f);
}

void LoadLayout(const std::string &key)
{
	g_layoutKey = key;
	json all = ReadLayouts();
	if (!all.contains(key) || !all[key].is_array() || all[key].size() != 4) return;
	auto &a = all[key];
	int x = a[0].get<int>(), y = a[1].get<int>(), w = std::max(kMinW, a[2].get<int>()), h = std::max(kMinH, a[3].get<int>());
	g_rel = {x, y, x + w, y + h};
	g_custom = true;
	if (g_shown) Reposition();
}

void StartDrag(Zone z)
{
	g_drag.on = true;
	g_drag.zone = z;
	GetCursorPos(&g_drag.start);
	g_drag.from = g_place;
	SetCapture(g_hwnd);
	SetCursor(ZoneCursor(z));
}

void DragTo()
{
	POINT now;
	GetCursorPos(&now);
	int dx = now.x - g_drag.start.x, dy = now.y - g_drag.start.y;
	RECT r = g_drag.from;
	Zone z = g_drag.zone;
	if (z == Z_MOVE) OffsetRect(&r, dx, dy);
	if (z == Z_L || z == Z_TL || z == Z_BL) r.left = std::min(r.left + dx, r.right - kMinW);
	if (z == Z_R || z == Z_TR || z == Z_BR) r.right = std::max(r.right + dx, r.left + kMinW);
	if (z == Z_T || z == Z_TL || z == Z_TR) r.top = std::min(r.top + dy, r.bottom - kMinH);
	if (z == Z_B || z == Z_BL || z == Z_BR) r.bottom = std::max(r.bottom + dy, r.top + kMinH);
	bool sized = (r.right - r.left) != (g_place.right - g_place.left) || (r.bottom - r.top) != (g_place.bottom - g_place.top);
	g_place = r;
	SetWindowPos(g_hwnd, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	// Re-render at most every 40 ms while sizing; the window takes the new size with the new frame.
	if (sized && !g_resizePending) { g_resizePending = true; SetTimer(g_hwnd, kResizeTimer, 40, nullptr); }
}

void EndDrag()
{
	if (!g_drag.on) return;
	g_drag.on = false;
	if (GetCapture() == g_hwnd) ReleaseCapture();
	KillTimer(g_hwnd, kResizeTimer);
	g_resizePending = false;
	AskSize();
	SaveLayout();
}

void Apply()
{
	bool want = g_wanted && g_owner && IsWindow(g_owner) && !g_ownerHidden && !IsIconic(g_owner);
	if (want == g_shown) return;
	g_shown = want;
	g_shownAny = want;
	if (want)
	{
		Reposition();
		ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
		g_needFull = true;
	}
	else
	{
		if (GetForegroundWindow() == g_hwnd && g_owner) SetForegroundWindow(g_owner);
		ShowWindow(g_hwnd, SW_HIDE);
	}
	hostlink::Send({{"t", "visible"}, {"on", want}});
}

void DoShow(bool focusInput)
{
	if (!g_owner || !IsWindow(g_owner)) SetOwner(FindMainWindow());
	g_wanted = true;
	Apply();
	if (focusInput && g_shown)
	{
		SetForegroundWindow(g_hwnd);
		hostlink::Send({{"t", "toPage"}, {"m", {{"t", "focusInput"}}}});
	}
}

void DoHide()
{
	g_wanted = false;
	Apply();
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND h, LONG idObject, LONG idChild, DWORD, DWORD)
{
	if (h != g_owner || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
	switch (event)
	{
	case EVENT_OBJECT_LOCATIONCHANGE:
		SetTimer(g_hwnd, kRepositionTimer, 50, nullptr); // a drag fires dozens; settle first
		break;
	case EVENT_OBJECT_HIDE: g_ownerHidden = true; Apply(); break;
	case EVENT_OBJECT_SHOW: g_ownerHidden = false; Apply(); break;
	case EVENT_OBJECT_DESTROY:
		SetOwner(nullptr);
		Apply();
		break;
	}
}

// ---------------------------------------------------------------------------------------------
// Surface and presenting
// ---------------------------------------------------------------------------------------------
void FreeSurface()
{
	if (g_memDC && g_oldBmp) SelectObject(g_memDC, g_oldBmp);
	g_oldBmp = nullptr;
	if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
	if (g_hdr) { UnmapViewOfFile(g_hdr); g_hdr = nullptr; }
	if (g_map) { CloseHandle(g_map); g_map = nullptr; }
	g_sw = g_sh = 0;
}

void Present()
{
	if (!g_hdr || !g_dib) return;
	LONG seq = g_hdr->seq;
	if (seq == g_lastSeq) return;
	if (g_shown)
	{
		RECT dirty = {g_hdr->dirtyL, g_hdr->dirtyT, g_hdr->dirtyR, g_hdr->dirtyB};
		POINT dst = {g_place.left, g_place.top}, src = {0, 0};
		SIZE sz = {(LONG)g_sw, (LONG)g_sh};
		BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
		UPDATELAYEREDWINDOWINFO u = {sizeof(u)};
		u.pptDst = &dst;
		u.psize = &sz;
		u.hdcSrc = g_memDC;
		u.pptSrc = &src;
		u.pblend = &bf;
		u.dwFlags = ULW_ALPHA;
		u.prcDirty = g_needFull ? nullptr : &dirty;
		if (!UpdateLayeredWindowIndirect(g_hwnd, &u) && u.prcDirty)
		{
			u.prcDirty = nullptr;
			UpdateLayeredWindowIndirect(g_hwnd, &u);
		}
		g_needFull = false;
	}
	g_lastSeq = seq;
	InterlockedExchange(&g_hdr->ack, seq); // the host may write the next frame
}

void OpenSurface(const json &m)
{
	std::wstring name = lsc2::Widen(m.value("name", ""));
	uint32_t w = m.value("w", 0u), h = m.value("h", 0u);
	HANDLE map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
	if (!map) { PostToMain({{"t", "_log"}, {"text", "could not open surface " + m.value("name", "")}}); return; }
	auto *hdr = (lsc2::SurfaceHeader *)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, lsc2::kSurfaceHeaderSize);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = (LONG)w;
	bi.bmiHeader.biHeight = -(LONG)h; // top-down, as the host writes it
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void *bits = nullptr;
	HBITMAP dib = hdr ? CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &bits, map, (DWORD)lsc2::kSurfaceHeaderSize) : nullptr;
	if (!hdr || !dib || hdr->magic != lsc2::kSurfaceMagic)
	{
		if (dib) DeleteObject(dib);
		if (hdr) UnmapViewOfFile(hdr);
		CloseHandle(map);
		PostToMain({{"t", "_log"}, {"text", "surface " + m.value("name", "") + " is not usable"}});
		return;
	}
	FreeSurface();
	g_map = map;
	g_hdr = hdr;
	g_dib = dib;
	g_oldBmp = SelectObject(g_memDC, g_dib);
	g_sw = w; g_sh = h;
	g_lastSeq = 0;      // the host's first frame on this surface is seq 1
	g_needFull = true;
	Present();          // in case it was written before we got here
}

void OnHost(const std::string &text)
{
	json m = json::parse(text, nullptr, false);
	if (m.is_discarded()) return;
	const std::string t = m.value("t", "");
	if (t == "surface") OpenSurface(m);
	else if (t == "cursor")
	{
		// ultralight::Cursor: 0 pointer, 2 hand, 3 i-beam; anything else falls back to the arrow
		int c = m.value("c", 0);
		g_cursor = LoadCursorW(nullptr, (LPCWSTR)(c == 2 ? IDC_HAND : c == 3 ? IDC_IBEAM : IDC_ARROW));  // int resources: the A/W cast is safe
		POINT pt;
		GetCursorPos(&pt);
		if (WindowFromPoint(pt) == g_hwnd) SetCursor(g_cursor);
	}
	else if (t == "fromPage")
	{
		const json &pm = m["m"];
		const std::string pt = pm.value("t", "");
		if (pt == "focus") SetForegroundWindow(g_hwnd);        // the page's input line got focus
		else if (pt == "blur") { if (GetForegroundWindow() == g_hwnd && g_owner) SetForegroundWindow(g_owner); }
		else if (pt == "hide") DoHide();
		else PostToMain(pm);                                    // exec, ready, ...
	}
}

// ---------------------------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------------------------
void SendMouse(const char *e, LPARAM lp, int b)
{
	hostlink::Send({{"t", "mouse"}, {"e", e}, {"x", GET_X_LPARAM(lp)}, {"y", GET_Y_LPARAM(lp)}, {"b", b}});
}

bool Down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

// Modifiers are read here, on the thread that receives the keys: the host builds Ultralight's
// KeyEvent from wParam/lParam, and anything it read itself would be its own keyboard state.
void SendKey(const char *k, WPARAM wp, LPARAM lp, bool sys)
{
	int mods = (Down(VK_MENU) ? 1 : 0) | (Down(VK_CONTROL) ? 2 : 0) | (Down(VK_LWIN) || Down(VK_RWIN) ? 4 : 0) | (Down(VK_SHIFT) ? 8 : 0);
	hostlink::Send({{"t", "key"}, {"k", k}, {"w", (uint64_t)wp}, {"l", (int64_t)lp}, {"sys", sys}, {"m", mods}});
}

// An InnerSpace combo -- "`", "Ctrl+`", "Shift+F1", "Alt+Num 5" -- as a virtual key and modifiers.
// Unknown key names leave vk 0 (no toggle key).
Combo ParseCombo(const std::string &s)
{
	Combo c;
	std::vector<std::string> parts;
	size_t start = 0;
	for (size_t i = 0; i <= s.size(); i++)
	{
		if (i == s.size() || (s[i] == '+' && i > start)) { parts.push_back(s.substr(start, i - start)); start = i + 1; }
	}
	if (parts.empty()) return c;
	auto lower = [](std::string x) { for (auto &ch : x) ch = (char)tolower((unsigned char)ch); return x; };
	for (size_t i = 0; i + 1 < parts.size(); i++)
	{
		std::string m = lower(parts[i]);
		if (m == "ctrl" || m == "control") c.ctrl = true;
		else if (m == "shift") c.shift = true;
		else if (m == "alt") c.alt = true;
	}
	std::string key = parts.back(), k = lower(key);
	if (key.size() == 1)
	{
		SHORT r = VkKeyScanA(key[0]);
		if (r != -1) { c.vk = LOBYTE(r); if (HIBYTE(r) & 1) c.shift = true; }
		return c;
	}
	if (k.size() >= 2 && k[0] == 'f' && isdigit((unsigned char)k[1])) { int n = atoi(k.c_str() + 1); if (n >= 1 && n <= 24) c.vk = VK_F1 + n - 1; return c; }
	if ((k.rfind("num ", 0) == 0 || k.rfind("numpad", 0) == 0) && isdigit((unsigned char)k.back())) { c.vk = VK_NUMPAD0 + (k.back() - '0'); return c; }
	static const struct { const char *name; UINT vk; } names[] = {
		{"grave", VK_OEM_3}, {"tilde", VK_OEM_3}, {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE}, {"tab", VK_TAB},
		{"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN}, {"backspace", VK_BACK}, {"back", VK_BACK},
		{"insert", VK_INSERT}, {"ins", VK_INSERT}, {"delete", VK_DELETE}, {"del", VK_DELETE}, {"home", VK_HOME},
		{"end", VK_END}, {"pageup", VK_PRIOR}, {"page up", VK_PRIOR}, {"pgup", VK_PRIOR}, {"pagedown", VK_NEXT},
		{"page down", VK_NEXT}, {"pgdn", VK_NEXT}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
		{"pause", VK_PAUSE}, {"scroll lock", VK_SCROLL}, {"scrolllock", VK_SCROLL}, {"num lock", VK_NUMLOCK}, {"numlock", VK_NUMLOCK},
	};
	for (auto &n : names) if (k == n.name) { c.vk = n.vk; break; }
	return c;
}

bool IsToggle(const MSG &m)
{
	if (!g_toggle.vk || m.hwnd != g_hwnd || (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN)) return false;
	return m.wParam == g_toggle.vk && Down(VK_CONTROL) == g_toggle.ctrl && Down(VK_SHIFT) == g_toggle.shift && Down(VK_MENU) == g_toggle.alt;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
	// A click anywhere on the console gives it keyboard focus (the user, 2026-10-26: ctrl+C/X must work
	// after selecting in the output). WS_EX_NOACTIVATE keeps Windows from doing it on its own -- and
	// keeps showing the console from stealing focus -- so the button-down handler does it explicitly.
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT)
		{
			POINT p;
			GetCursorPos(&p);
			ScreenToClient(h, &p);
			Zone z = g_drag.on ? g_drag.zone : (g_buttons ? Z_NONE : ZoneAt(p));
			SetCursor(z != Z_NONE ? ZoneCursor(z) : g_cursor);
			return TRUE;
		}
		break;
	case WM_MOUSEMOVE:
		if (g_drag.on) { DragTo(); return 0; }
		if (!g_tracking)
		{
			TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
			g_tracking = TrackMouseEvent(&tme) != FALSE;
		}
		if (!g_buttons && ZoneAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) != Z_NONE) return 0; // the frame, not the page
		SendMouse("move", lp, g_buttons);
		return 0;
	case WM_MOUSELEAVE: g_tracking = false; return 0;
	case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
		if (GetForegroundWindow() != h) SetForegroundWindow(h); // allowed: this click is our input
		if (msg == WM_LBUTTONDOWN)
		{
			Zone z = ZoneAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
			if (z != Z_NONE) { StartDrag(z); return 0; }
		}
		g_buttons = msg == WM_LBUTTONDOWN ? 1 : msg == WM_MBUTTONDOWN ? 2 : 3;
		SetCapture(h);
		SendMouse("down", lp, g_buttons);
		return 0;
	case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP:
		if (g_drag.on) { EndDrag(); return 0; }
		SendMouse("up", lp, msg == WM_LBUTTONUP ? 1 : msg == WM_MBUTTONUP ? 2 : 3);
		g_buttons = 0;
		ReleaseCapture();
		return 0;
	case WM_CAPTURECHANGED:
		if (g_drag.on && (HWND)lp != h) EndDrag();
		return 0;
	case WM_MOUSEWHEEL:
		hostlink::Send({{"t", "wheel"}, {"dx", 0}, {"dy", GET_WHEEL_DELTA_WPARAM(wp) * 4 / 5}});
		return 0;
	case WM_KEYDOWN: case WM_SYSKEYDOWN: SendKey("down", wp, lp, msg == WM_SYSKEYDOWN); return 0;
	case WM_KEYUP: case WM_SYSKEYUP: SendKey("up", wp, lp, msg == WM_SYSKEYUP); return 0;
	case WM_CHAR: case WM_SYSCHAR: SendKey("char", wp, lp, msg == WM_SYSCHAR); return 0;
	case WM_ACTIVATE:
		hostlink::Send({{"t", "focus"}, {"on", LOWORD(wp) != WA_INACTIVE}});
		break; // DefWindowProc gives us keyboard focus on activation
	case WM_CLOSE: DoHide(); return 0; // alt+F4 on the console hides it, never destroys it
	case WM_TIMER:
		if (wp == kRepositionTimer) { KillTimer(h, kRepositionTimer); if (g_shown && !g_drag.on) Reposition(); }
		if (wp == kResizeTimer) { KillTimer(h, kResizeTimer); g_resizePending = false; AskSize(); }
		return 0;

	case WM_LSC2_SHOW: DoShow(wp != 0); return 0;
	case WM_LSC2_HIDE: DoHide(); return 0;
	case WM_LSC2_TOGGLE: if (g_wanted) DoHide(); else DoShow(true); return 0;
	case WM_LSC2_HOSTMSG:
	{
		std::unique_ptr<std::string> s((std::string *)lp);
		OnHost(*s);
		return 0;
	}
	case WM_LSC2_LINKUP:
		// The new view starts from scratch: tell it what we show. (Its "surface" arrives separately.)
		hostlink::Send({{"t", "visible"}, {"on", g_shown}});
		return 0;
	case WM_LSC2_LINKDOWN:
		FreeSurface(); // the host's mapping goes with the host; keep the last image on screen
		return 0;
	case WM_LSC2_QUIT: DestroyWindow(h); return 0;
	case WM_LSC2_SETKEY:
	{
		std::unique_ptr<std::string> s((std::string *)lp);
		g_toggle = ParseCombo(*s);
		return 0;
	}
	case WM_LSC2_SETLAYOUT:
	{
		std::unique_ptr<std::string> s((std::string *)lp);
		LoadLayout(*s);
		return 0;
	}
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

void SetStatus()
{
	char buf[512];
	wchar_t cls[128] = L"", title[128] = L"";
	if (g_owner) { GetClassNameW(g_owner, cls, 128); GetWindowTextW(g_owner, title, 128); }
	snprintf(buf, sizeof(buf), "%s, owner %p [%s] \"%s\", surface %ux%u, frame %ld, link %s",
	         g_shown ? "shown" : g_wanted ? "wanted, not shown" : "hidden", (void *)g_owner,
	         lsc2::Narrow(cls).c_str(), lsc2::Narrow(title).c_str(), g_sw, g_sh, g_lastSeq,
	         hostlink::Connected() ? "up" : "down");
	std::lock_guard<std::mutex> lk(g_statusMu);
	g_status = buf;
}

void ThreadMain()
{
	// Physical pixels for both our window and the owner's rectangles, whatever the game declares.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	HINSTANCE inst = GetModuleHandleW(nullptr);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.lpszClassName = L"LSConsole2Overlay";
	wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
	RegisterClassW(&wc);
	g_cursor = wc.hCursor;

	lsc2::UserSecurity sec; // the host may run at a different integrity level than this session
	g_frameEv = CreateEventW(sec.get(), FALSE, FALSE, lsc2::FrameEventName(GetCurrentProcessId()).c_str());
	// The owner is given at creation: that is what puts the window in the owner's z-order band
	// (a topmost owner makes it topmost too). SetOwner re-owns it later if the main window changes.
	HWND owner = FindMainWindow();
	HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"LSConsole2",
	                         WS_POPUP, 0, 0, 1, 1, owner, nullptr, inst, nullptr);
	g_hwnd = h;
	HDC screen = GetDC(nullptr);
	g_memDC = CreateCompatibleDC(screen);
	ReleaseDC(nullptr, screen);
	SetOwner(owner);
	RECT r;
	if (ComputePlace(r))
	{
		g_place = r;
		g_askedW = r.right - r.left; // what "hello" will ask for
		g_askedH = r.bottom - r.top;
		hostlink::SetSize(g_askedW, g_askedH, Scale());
	}
	SetEvent(g_readyEv);

	ULONGLONG lastStatus = 0;
	for (;;)
	{
		DWORD w = MsgWaitForMultipleObjectsEx(1, &g_frameEv, 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		if (w == WAIT_OBJECT_0) Present();
		MSG msg;
		bool quit = false;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT) { quit = true; break; }
			// The console key closes the console. Caught before TranslateMessage, so it never becomes
			// a character in the input line; its key-up is dropped too.
			if (IsToggle(msg)) { g_swallowUp = (UINT)msg.wParam; if (!(msg.lParam & (1 << 30))) DoHide(); continue; }
			if ((msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) && msg.wParam == g_swallowUp) { g_swallowUp = 0; continue; }
			TranslateMessage(&msg); // WM_KEYDOWN -> WM_CHAR for the page
			DispatchMessageW(&msg);
		}
		if (quit) break;
		if (GetTickCount64() - lastStatus > 250) { SetStatus(); lastStatus = GetTickCount64(); }
	}

	if (g_hook) { UnhookWinEvent(g_hook); g_hook = nullptr; }
	FreeSurface();
	DeleteDC(g_memDC); g_memDC = nullptr;
	CloseHandle(g_frameEv); g_frameEv = nullptr;
	g_hwnd = nullptr;
	UnregisterClassW(wc.lpszClassName, inst);
}

void Post(UINT msg, WPARAM wp = 0, LPARAM lp = 0)
{
	if (HWND h = g_hwnd) PostMessageW(h, msg, wp, lp);
}

} // namespace

// Waits on `h` from InnerSpace's thread -- which owns the game window, our window's owner. Windows
// sends some owner/owned messages synchronously (creating, activating, destroying), so a plain
// wait here could deadlock against the overlay thread. Sent messages are handled while waiting;
// posted ones stay queued for the game.
void WaitHandlingSent(HANDLE h, DWORD timeoutMs)
{
	ULONGLONG end = GetTickCount64() + timeoutMs;
	for (;;)
	{
		ULONGLONG now = GetTickCount64();
		if (now >= end) return;
		DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, (DWORD)(end - now), QS_SENDMESSAGE);
		if (r != WAIT_OBJECT_0 + 1) return; // signalled, timed out or failed
		MSG msg;
		PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE); // dispatches the sent ones
	}
}

void Start()
{
	if (g_thread.joinable()) return;
	g_readyEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_thread = std::thread(ThreadMain);
	WaitHandlingSent(g_readyEv, 5000); // the frame event and the size must exist before "hello"
	CloseHandle(g_readyEv);
	g_readyEv = nullptr;
}

void Stop()
{
	if (!g_thread.joinable()) return;
	Post(WM_LSC2_QUIT);
	WaitHandlingSent(g_thread.native_handle(), 5000);
	if (WaitForSingleObject(g_thread.native_handle(), 0) == WAIT_OBJECT_0) g_thread.join();
	else g_thread.detach(); // never hang the game on shutdown
}

void Show(bool focusInput) { Post(WM_LSC2_SHOW, focusInput ? 1 : 0); }
void Hide() { Post(WM_LSC2_HIDE); }
void Toggle() { Post(WM_LSC2_TOGGLE); }

void SetLayoutKey(const std::string &session)
{
	HWND h = g_hwnd;
	auto *s = new std::string(session);
	if (!h || !PostMessageW(h, WM_LSC2_SETLAYOUT, 0, (LPARAM)s)) delete s;
}

void SetToggleKey(const std::string &combo)
{
	HWND h = g_hwnd;
	auto *s = new std::string(combo);
	if (!h || !PostMessageW(h, WM_LSC2_SETKEY, 0, (LPARAM)s)) delete s;
}

void OnHostMessage(std::string &&msg)
{
	HWND h = g_hwnd;
	auto *s = new std::string(std::move(msg));
	if (!h || !PostMessageW(h, WM_LSC2_HOSTMSG, 0, (LPARAM)s)) delete s;
}

void OnLinkUp() { Post(WM_LSC2_LINKUP); }
void OnLinkDown() { Post(WM_LSC2_LINKDOWN); }

bool Shown() { return g_shownAny; }

std::string Status()
{
	std::lock_guard<std::mutex> lk(g_statusMu);
	return g_status;
}

} // namespace overlay
