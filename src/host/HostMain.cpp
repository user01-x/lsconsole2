// LSConsole2Host -- renders every session's console page with one Ultralight renderer.
//
// One process per Windows user (a named mutex), started by the first session that needs it and
// gone a few minutes after the last one disconnects. Ultralight never loads into a game process:
// ISXOgreX ships its own Ultralight there, and two owners of one process-wide Ultralight would
// clash.
//
// Threads: the pipe server accepts connections; each connection's PipeConn reads on its own
// thread; everything else -- Ultralight, the views, the surfaces -- is on the main thread, which
// takes work from one queue (Post / g_q).
#include <windows.h>
#include <Ultralight/Ultralight.h>
#include <AppCore/Platform.h>
#include <JavaScriptCore/JavaScript.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <share.h>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "../common/Protocol.h"
#include "../common/PipeConn.h"
#include "../common/third_party/json.hpp"

using namespace ultralight;
using json = nlohmann::json;

namespace {

constexpr DWORD kIdleExitMs = 5 * 60 * 1000; // no sessions connected for this long: exit

std::wstring g_dir;     // the exe's folder, with a trailing backslash (read-only under Program Files)
std::wstring g_dataDir; // %LOCALAPPDATA%\LSConsole2\: the log and Ultralight's cache
FILE *g_log = nullptr;

void Log(const char *fmt, ...)
{
	if (!g_log) return;
	SYSTEMTIME t; GetLocalTime(&t);
	fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
	fputc('\n', g_log);
	fflush(g_log);
}

// ---------------------------------------------------------------------------------------------
// The main thread's work queue
// ---------------------------------------------------------------------------------------------
struct Event {
	enum Kind { Connected, Message, Closed } kind;
	int client = 0;
	HANDLE pipe = INVALID_HANDLE_VALUE;
	std::string text;
};
HANDLE g_wake = nullptr; // auto-reset: the queue has something
std::mutex g_qmu;
std::vector<Event> g_q;

void Post(Event e)
{
	{
		std::lock_guard<std::mutex> lk(g_qmu);
		g_q.push_back(std::move(e));
	}
	SetEvent(g_wake);
}

// ---------------------------------------------------------------------------------------------
// Platform pieces
// ---------------------------------------------------------------------------------------------
class HostLogger : public Logger {
public:
	void LogMessage(LogLevel, const String &m) override { Log("[ul] %s", m.utf8().data()); }
};

// The clipboard needs an owner window (OpenClipboard(NULL) + EmptyClipboard makes
// SetClipboardData fail), so the host keeps one hidden message-only window.
class WinClipboard : public Clipboard {
public:
	HWND hwnd = nullptr;
	void Clear() override
	{
		if (OpenClipboard(hwnd)) { EmptyClipboard(); CloseClipboard(); }
	}
	String ReadPlainText() override
	{
		String r;
		if (!OpenClipboard(hwnd)) return r;
		if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
		{
			if (auto *p = (const wchar_t *)GlobalLock(h))
			{
				r = String((const Char16 *)p, wcslen(p));
				GlobalUnlock(h);
			}
		}
		CloseClipboard();
		return r;
	}
	void WritePlainText(const String &text) override
	{
		String16 u = text.utf16();
		WriteW(std::wstring((const wchar_t *)u.data(), u.length()));
	}
	void WriteW(const std::wstring &w)
	{
		if (!OpenClipboard(hwnd)) return;
		EmptyClipboard();
		size_t bytes = (w.size() + 1) * sizeof(wchar_t);
		if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes))
		{
			memcpy(GlobalLock(g), w.c_str(), bytes);
			GlobalUnlock(g);
			if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
		}
		CloseClipboard();
	}
};

HostLogger g_logger;

// ---------------------------------------------------------------------------------------------
// Command history: one per PC, shared by every session's console and kept across restarts
// %LOCALAPPDATA%\LSConsole2\history.jsonl, one {"c": command} per line, appended as
// commands run; duplicates are collapsed (and the file compacted) on load.
// ---------------------------------------------------------------------------------------------
constexpr size_t kHistoryMax = 5000;
std::vector<std::string> g_history; // oldest first, no duplicates

std::string Dump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

void SaveHistory()
{
	std::ofstream f(g_dataDir + L"history.jsonl", std::ios::trunc | std::ios::binary);
	for (auto &c : g_history) f << Dump({{"c", c}}) << '\n';
}

void LoadHistory()
{
	std::ifstream f(g_dataDir + L"history.jsonl", std::ios::binary);
	std::vector<std::string> all;
	std::string line;
	while (std::getline(f, line))
	{
		json j = json::parse(line, nullptr, false);
		if (j.is_object() && j.contains("c") && j["c"].is_string()) all.push_back(j["c"].get<std::string>());
	}
	// Keep each command's last occurrence, in order.
	for (auto it = all.rbegin(); it != all.rend() && g_history.size() < kHistoryMax; ++it)
		if (std::find(g_history.begin(), g_history.end(), *it) == g_history.end()) g_history.push_back(*it);
	std::reverse(g_history.begin(), g_history.end());
	if (all.size() > g_history.size() + 500) SaveHistory(); // compact
	Log("history: %zu commands", g_history.size());
}

std::string HistoryJs()
{
	return "window.lsc2&&lsc2.rx(" + Dump({{"t", "history"}, {"items", g_history}}) + ")";
}
WinClipboard g_clipboard;
RefPtr<Renderer> g_renderer;
JSClassRef g_sendClass = nullptr; // a callable JS object carrying its Client in private data

// ---------------------------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------------------------
struct Client;
void HandleFromPage(Client *c, const json &m);

class ViewHandler : public LoadListener, public ViewListener {
public:
	explicit ViewHandler(Client *c) : c_(c) {}
	void OnWindowObjectReady(View *caller, uint64_t, bool isMainFrame, const String &) override;
	void OnDOMReady(View *caller, uint64_t, bool isMainFrame, const String &) override;
	void OnFailLoading(View *, uint64_t, bool isMainFrame, const String &url, const String &desc,
	                   const String &, int code) override
	{
		if (isMainFrame) Log("page failed to load: %s (%d %s)", url.utf8().data(), code, desc.utf8().data());
	}
	void OnChangeCursor(View *, Cursor cursor) override;
	void OnAddConsoleMessage(View *, const ConsoleMessage &m) override
	{
		Log("[page] %s:%u %s", m.source_id().utf8().data(), m.line_number(), m.message().utf8().data());
	}

private:
	Client *c_;
};

struct Client {
	int id = 0;
	std::unique_ptr<lsc2::PipeConn> conn;
	DWORD pid = 0;
	RefPtr<View> view;
	std::unique_ptr<ViewHandler> handler;
	HANDLE frameEv = nullptr;
	HANDLE map = nullptr;
	lsc2::SurfaceHeader *hdr = nullptr;
	uint8_t *px = nullptr;
	uint32_t w = 0, h = 0, gen = 0;
	bool ready = false;    // DOM ready: page scripts may be called
	bool visible = false;  // the session shows the overlay
	bool fullCopy = true;  // next frame copies everything (new surface)
	std::vector<std::string> pendingJs;

	void Send(const json &j) { if (conn) conn->Send(j.dump()); }

	void FreeSurface()
	{
		if (hdr) UnmapViewOfFile(hdr);
		if (map) CloseHandle(map);
		hdr = nullptr; px = nullptr; map = nullptr;
	}

	~Client()
	{
		view = nullptr; // first: the page's lsc2Send points at this Client
		handler.reset();
		FreeSurface();
		if (frameEv) CloseHandle(frameEv);
		if (conn) conn->Close();
	}
};

std::map<int, std::unique_ptr<Client>> g_clients;
int g_nextId = 1;

JSValueRef SendCallback(JSContextRef ctx, JSObjectRef function, JSObjectRef, size_t argc,
                        const JSValueRef argv[], JSValueRef *exception)
{
	auto *c = (Client *)JSObjectGetPrivate(function);
	if (c && argc >= 1)
	{
		if (JSStringRef s = JSValueToStringCopy(ctx, argv[0], exception))
		{
			size_t max = JSStringGetMaximumUTF8CStringSize(s);
			std::string buf(max, '\0');
			size_t n = JSStringGetUTF8CString(s, &buf[0], max);
			buf.resize(n ? n - 1 : 0);
			JSStringRelease(s);
			json m = json::parse(buf, nullptr, false);
			if (!m.is_discarded()) HandleFromPage(c, m);
		}
	}
	return JSValueMakeUndefined(ctx);
}

void ViewHandler::OnWindowObjectReady(View *caller, uint64_t, bool isMainFrame, const String &)
{
	if (!isMainFrame) return;
	RefPtr<JSContext> lock = caller->LockJSContext();
	JSContextRef ctx = lock->ctx();
	JSObjectRef fn = JSObjectMake(ctx, g_sendClass, c_);
	JSStringRef name = JSStringCreateWithUTF8CString("lsc2Send");
	JSObjectSetProperty(ctx, JSContextGetGlobalObject(ctx), name, fn, kJSPropertyAttributeReadOnly, nullptr);
	JSStringRelease(name);
}

void ViewHandler::OnDOMReady(View *caller, uint64_t, bool isMainFrame, const String &)
{
	if (!isMainFrame) return;
	c_->ready = true;
	caller->EvaluateScript(HistoryJs().c_str());
	for (auto &js : c_->pendingJs) caller->EvaluateScript(js.c_str());
	c_->pendingJs.clear();
}

void ViewHandler::OnChangeCursor(View *, Cursor cursor)
{
	c_->Send({{"t", "cursor"}, {"c", (int)cursor}});
}

// What the page asks for. Copying is the host's own business (it owns the clipboard); the rest
// goes to the session.
void AddHistory(const std::string &cmd)
{
	auto it = std::find(g_history.begin(), g_history.end(), cmd);
	if (it != g_history.end()) g_history.erase(it);
	g_history.push_back(cmd);
	if (g_history.size() > kHistoryMax) g_history.erase(g_history.begin());
	{
		std::ofstream f(g_dataDir + L"history.jsonl", std::ios::app | std::ios::binary);
		f << Dump({{"c", cmd}}) << '\n';
	}
	// Every open console gets it, not only the one it was typed in.
	std::string js = "window.lsc2&&lsc2.rx(" + Dump({{"t", "histAdd"}, {"c", cmd}}) + ")";
	for (auto &kv : g_clients)
		if (kv.second->view && kv.second->ready) kv.second->view->EvaluateScript(js.c_str());
}

void HandleFromPage(Client *c, const json &m)
{
	if (m.value("t", "") == "exec" && !m.value("text", "").empty()) AddHistory(m.value("text", ""));
	if (m.value("t", "") == "copy")
	{
		g_clipboard.WriteW(lsc2::Widen(m.value("text", "")));
		return;
	}
	if (m.value("t", "") == "paste")
	{
		// The page decides what to do with it (one line goes in; more is refused).
		std::string text = g_clipboard.ReadPlainText().utf8().data();
		std::string js = "window.lsc2&&lsc2.rx(" + Dump({{"t", "paste"}, {"text", text}}) + ")";
		if (c->view) c->view->EvaluateScript(js.c_str());
		return;
	}
	c->Send({{"t", "fromPage"}, {"m", m}});
}

bool MakeSurface(Client *c, uint32_t w, uint32_t h)
{
	size_t bytes = lsc2::kSurfaceHeaderSize + (size_t)w * h * 4;
	std::wstring name = lsc2::SurfaceName(c->pid, c->gen + 1);
	static lsc2::UserSecurity sec;
	HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, sec.get(), PAGE_READWRITE, (DWORD)((uint64_t)bytes >> 32),
	                              (DWORD)bytes, name.c_str());
	if (!m) { Log("CreateFileMapping %ux%u failed: %lu", w, h, GetLastError()); return false; }
	void *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
	if (!p) { Log("MapViewOfFile failed: %lu", GetLastError()); CloseHandle(m); return false; }

	// The old mapping lives on in the session until it opens this one.
	c->FreeSurface();
	c->map = m;
	c->hdr = (lsc2::SurfaceHeader *)p;
	c->px = (uint8_t *)p + lsc2::kSurfaceHeaderSize;
	c->w = w; c->h = h; c->gen++;
	c->hdr->magic = lsc2::kSurfaceMagic;
	c->hdr->width = w; c->hdr->height = h; c->hdr->stride = w * 4;
	c->hdr->seq = 0; c->hdr->ack = 0;
	c->fullCopy = true;
	c->Send({{"t", "surface"}, {"name", lsc2::Narrow(name)}, {"w", w}, {"h", h}, {"gen", c->gen}});
	return true;
}

void CreateView(Client *c, uint32_t w, uint32_t h, double scale)
{
	ViewConfig vc;
	vc.is_accelerated = false;
	vc.is_transparent = true;
	vc.initial_device_scale = scale;
	vc.initial_focus = false;
	c->view = g_renderer->CreateView(w, h, vc, nullptr);
	c->handler = std::make_unique<ViewHandler>(c);
	c->view->set_load_listener(c->handler.get());
	c->view->set_view_listener(c->handler.get());
	c->frameEv = OpenEventW(EVENT_MODIFY_STATE, FALSE, lsc2::FrameEventName(c->pid).c_str());
	if (!c->frameEv) Log("client %d: no frame event for pid %lu (%lu)", c->id, c->pid, GetLastError());
	MakeSurface(c, w, h);
	c->view->LoadURL("file:///assets/console.html");
}

uint32_t ClampDim(const json &j, const char *k) { return (uint32_t)std::max(16, std::min(8192, j.value(k, 400))); }

void OnClientMessage(Client *c, const std::string &text)
{
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object()) return;
	const std::string t = j.value("t", "");

	if (t == "hello")
	{
		if (c->view) return;
		c->pid = j.value("pid", 0u);
		Log("client %d: hello pid=%lu %ux%u", c->id, c->pid, ClampDim(j, "w"), ClampDim(j, "h"));
		CreateView(c, ClampDim(j, "w"), ClampDim(j, "h"), j.value("scale", 1.0));
		return;
	}
	if (!c->view) return;

	if (t == "resize")
	{
		uint32_t w = ClampDim(j, "w"), h = ClampDim(j, "h");
		double scale = j.value("scale", 1.0);
		if (scale != c->view->device_scale()) c->view->set_device_scale(scale);
		c->view->Resize(w, h);
		MakeSurface(c, w, h);
	}
	else if (t == "visible")
	{
		c->visible = j.value("on", false);
		if (c->visible) c->fullCopy = true;
	}
	else if (t == "mouse")
	{
		MouseEvent e;
		const std::string ev = j.value("e", "move");
		e.type = ev == "down" ? MouseEvent::kType_MouseDown : ev == "up" ? MouseEvent::kType_MouseUp : MouseEvent::kType_MouseMoved;
		e.x = j.value("x", 0);
		e.y = j.value("y", 0);
		e.button = (MouseEvent::Button)std::max(0, std::min(3, j.value("b", 0)));
		c->view->FireMouseEvent(e);
	}
	else if (t == "wheel")
	{
		ScrollEvent e;
		e.type = ScrollEvent::kType_ScrollByPixel;
		e.delta_x = j.value("dx", 0);
		e.delta_y = j.value("dy", 0);
		c->view->FireScrollEvent(e);
	}
	else if (t == "key")
	{
		const std::string k = j.value("k", "");
		KeyEvent::Type type = k == "down" ? KeyEvent::kType_RawKeyDown : k == "up" ? KeyEvent::kType_KeyUp : KeyEvent::kType_Char;
		KeyEvent e(type, (uintptr_t)j.value("w", (uint64_t)0), (intptr_t)j.value("l", (int64_t)0), j.value("sys", false));
		// The constructor reads modifiers from this process's keyboard state, which never sees the keys:
		// the session sends the real ones (KeyEvent::kMod_* bits).
		e.modifiers = j.value("m", 0u);
		c->view->FireKeyEvent(e);
	}
	else if (t == "focus")
	{
		if (j.value("on", false)) c->view->Focus(); else c->view->Unfocus();
	}
	else if (t == "toPage")
	{
		std::string js = "window.lsc2&&lsc2.rx(" + j["m"].dump() + ")";
		if (c->ready) c->view->EvaluateScript(js.c_str());
		else c->pendingJs.push_back(std::move(js));
	}
}

// Copy what changed into the session's surface and wake it -- only once it has presented the
// previous frame (ack == seq), so the buffer it's reading is never written. Dirty bounds keep
// accumulating in Ultralight until then.
void Present(Client *c)
{
	if (!c->view || !c->hdr || !c->visible) return;
	if (c->hdr->ack != c->hdr->seq) return;
	auto *s = (BitmapSurface *)c->view->surface();
	if (!s || s->width() != c->w || s->height() != c->h) return;

	IntRect d = c->fullCopy ? IntRect{0, 0, (int)c->w, (int)c->h} : s->dirty_bounds();
	d.left = std::max(0, d.left); d.top = std::max(0, d.top);
	d.right = std::min((int)c->w, d.right); d.bottom = std::min((int)c->h, d.bottom);
	if (d.right <= d.left || d.bottom <= d.top) return;

	auto *src = (const uint8_t *)s->LockPixels();
	if (!src) return;
	uint32_t rb = s->row_bytes();
	size_t rowBytes = (size_t)(d.right - d.left) * 4;
	for (int y = d.top; y < d.bottom; y++)
		memcpy(c->px + (size_t)y * c->w * 4 + (size_t)d.left * 4, src + (size_t)y * rb + (size_t)d.left * 4, rowBytes);
	s->UnlockPixels();
	s->ClearDirtyBounds();
	c->fullCopy = false;

	c->hdr->dirtyL = d.left; c->hdr->dirtyT = d.top; c->hdr->dirtyR = d.right; c->hdr->dirtyB = d.bottom;
	InterlockedIncrement(&c->hdr->seq); // full barrier: pixels and dirty rect are visible first
	if (c->frameEv) SetEvent(c->frameEv);
}

// ---------------------------------------------------------------------------------------------
// Pipe server
// ---------------------------------------------------------------------------------------------
void ServerThread(HANDLE stopEv)
{
	const std::wstring name = lsc2::HostPipeName();
	lsc2::UserSecurity sec;
	for (;;)
	{
		HANDLE p = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		                            PIPE_UNLIMITED_INSTANCES, 1 << 20, 1 << 20, 0, sec.get());
		if (p == INVALID_HANDLE_VALUE)
		{
			Log("CreateNamedPipe failed: %lu", GetLastError());
			if (WaitForSingleObject(stopEv, 1000) == WAIT_OBJECT_0) return;
			continue;
		}
		OVERLAPPED ov = {};
		ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		bool connected = false, stop = false;
		if (ConnectNamedPipe(p, &ov)) connected = true;
		else
		{
			DWORD e = GetLastError();
			if (e == ERROR_PIPE_CONNECTED) connected = true;
			else if (e == ERROR_IO_PENDING)
			{
				HANDLE hs[2] = {stopEv, ov.hEvent};
				DWORD n = 0;
				if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
					connected = GetOverlappedResult(p, &ov, &n, FALSE) != FALSE;
				else
				{
					CancelIoEx(p, &ov);
					GetOverlappedResult(p, &ov, &n, TRUE);
					stop = true;
				}
			}
		}
		CloseHandle(ov.hEvent);
		if (stop) { CloseHandle(p); return; }
		if (connected) Post({Event::Connected, 0, p, {}});
		else CloseHandle(p);
	}
}

// ---------------------------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------------------------
void Pump()
{
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

void RunLoop()
{
	ULONGLONG lastActive = GetTickCount64();
	for (;;)
	{
		std::vector<Event> evs;
		{
			std::lock_guard<std::mutex> lk(g_qmu);
			evs.swap(g_q);
		}
		for (auto &e : evs)
		{
			if (e.kind == Event::Connected)
			{
				auto c = std::make_unique<Client>();
				c->id = g_nextId++;
				c->conn = std::make_unique<lsc2::PipeConn>();
				int id = c->id;
				c->conn->Start(e.pipe,
				               [id](std::string &&s) { Post({Event::Message, id, INVALID_HANDLE_VALUE, std::move(s)}); },
				               [id] { Post({Event::Closed, id, INVALID_HANDLE_VALUE, {}}); });
				Log("client %d connected (%zu total)", id, g_clients.size() + 1);
				g_clients[id] = std::move(c);
			}
			else if (e.kind == Event::Message)
			{
				auto it = g_clients.find(e.client);
				if (it != g_clients.end()) OnClientMessage(it->second.get(), e.text);
			}
			else
			{
				Log("client %d disconnected", e.client);
				g_clients.erase(e.client);
			}
		}

		bool anyVisible = false;
		if (!g_clients.empty())
		{
			g_renderer->Update();
			g_renderer->RefreshDisplay(0);
			g_renderer->Render();
			for (auto &kv : g_clients)
			{
				Present(kv.second.get());
				anyVisible |= kv.second->visible;
			}
			lastActive = GetTickCount64();
		}
		else if (GetTickCount64() - lastActive > kIdleExitMs)
		{
			Log("no sessions for %lu s, exiting", kIdleExitMs / 1000);
			return;
		}

		// ~60 Hz only while something is on screen; otherwise just keep timers ticking.
		DWORD timeout = anyVisible ? 16 : g_clients.empty() ? 1000 : 100;
		MsgWaitForMultipleObjects(1, &g_wake, FALSE, timeout, QS_ALLINPUT);
		Pump();
	}
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
	HANDLE mutex = CreateMutexW(nullptr, TRUE, lsc2::HostMutexName().c_str());
	if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0; // another host is running

	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	g_dir = path;
	g_dir.resize(g_dir.find_last_of(L'\\') + 1);
	wchar_t lad[MAX_PATH] = L"";
	GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
	g_dataDir = std::wstring(lad[0] ? lad : g_dir.c_str()) + L"\\LSConsole2\\";
	wchar_t over[MAX_PATH] = L"";
	if (GetEnvironmentVariableW(L"LSCONSOLE2_DATA", over, MAX_PATH)) g_dataDir = std::wstring(over) + L"\\"; // tests: keep out of the real history
	CreateDirectoryW(g_dataDir.c_str(), nullptr);
	g_log = _wfsopen((g_dataDir + L"host.log").c_str(), L"w", _SH_DENYNO); // readable while we run
	Log("LSConsole2Host starting, protocol %u, dir %s, data %s", lsc2::kProtocolVersion, lsc2::Narrow(g_dir).c_str(),
	    lsc2::Narrow(g_dataDir).c_str());

	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

	WNDCLASSW wc = {};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = hInst;
	wc.lpszClassName = L"LSConsole2HostMsg";
	RegisterClassW(&wc);
	g_clipboard.hwnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);

	Config cfg;
	cfg.resource_path_prefix = "resources/";
	cfg.cache_path = lsc2::Narrow(g_dataDir + L"cache").c_str();
	cfg.num_renderer_threads = 2;
	Platform::instance().set_config(cfg);
	Platform::instance().set_font_loader(GetPlatformFontLoader());
	Platform::instance().set_file_system(GetPlatformFileSystem(lsc2::Narrow(g_dir).c_str()));
	Platform::instance().set_logger(&g_logger);
	Platform::instance().set_clipboard(&g_clipboard);
	LoadHistory();
	g_renderer = Renderer::Create();
	if (!g_renderer) { Log("Renderer::Create failed"); return 1; }

	JSClassDefinition def = kJSClassDefinitionEmpty;
	def.className = "LSC2Send";
	def.callAsFunction = SendCallback;
	g_sendClass = JSClassCreate(&def);

	g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	HANDLE stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	std::thread server(ServerThread, stopEv);

	RunLoop();

	SetEvent(stopEv);
	server.join();
	g_clients.clear();
	g_renderer = nullptr;
	JSClassRelease(g_sendClass);
	Log("LSConsole2Host exiting");
	if (g_log) fclose(g_log);
	ReleaseMutex(mutex);
	return 0;
}
