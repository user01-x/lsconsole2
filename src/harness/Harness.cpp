// Harness -- runs the extension's overlay and host link outside InnerSpace, over a fake "game"
// window, so the render host, the pipe, the shared surface and the layered window can be checked
// without a game. Input is injected through the pipe (as the overlay would forward it); the real
// mouse and keyboard are never touched.
//
//   LSConsole2Harness.exe <path to LSConsole2Host.exe> <screenshot.bmp>
#include <windows.h>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "../ext/Console.h"

namespace {

std::mutex g_mu;
std::vector<json> g_q;
bool g_ready = false;
FILE *g_log = nullptr;

void Log(const std::string &s) { if (g_log) { fprintf(g_log, "%s\n", s.c_str()); fflush(g_log); } }

void Lines(std::vector<std::string> lines)
{
	hostlink::Send({{"t", "toPage"}, {"m", {{"t", "lines"}, {"lines", lines}}}});
}

void Pulse()
{
	std::vector<json> q;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		q.swap(g_q);
	}
	for (auto &m : q)
	{
		Log("main <- " + m.dump());
		const std::string t = m.value("t", "");
		if (t == "ready")
		{
			g_ready = true;
			hostlink::Send({{"t", "toPage"}, {"m", {{"t", "reset"}}}});
			std::vector<std::string> ls;
			for (int i = 1; i <= 60; i++) ls.push_back("line " + std::to_string(i) + " of the backlog");
			ls.push_back("\x07ycolour: yellow\x07x, \x07rred\x07x, \x07ggreen\x07x, \x07ublue\x07x, back to plain");
			Lines(ls);
			hostlink::Send({{"t", "toPage"}, {"m", {{"t", "ac"}, {"data", {
				{"commands", {"echo", "End", "EndScript", "ext", "relay", "Run", "RunScript", "lsconsole2"}},
				{"aliases", {{{"n", "edit"}, {"w", "runscript custom/Editor"}}, {{"n", "actors"}, {"w", "runscript custom/ActorLoop"}}}},
				{"tlos", {"Actor", "Me", "EQ2"}}, {"scripts", {"OgreBot", "statuspoller"}},
				{"variables", {{{"n", "MyCounter"}, {"v", "42"}}, {{"n", "MyTarget"}, {"v", "a goblin warrior"}}, {{"n", "MyList"}}}}}}}}});
		}
		else if (t == "exec")
		{
			std::string text = m.value("text", "");
			Lines({"\x01" + text, "\x07gexecuted:\x07x " + text});
		}
		else if (t == "members" && m.value("name", "") == "Me")
		{
			// With documentation: Effect has two forms; Ghost is documented but not live.
			json docs = {{"members", {
				{"Effect", {{"description", "One of its effects."}, {"forms", {
					{{"params", {{{"name", "index"}, {"type", "int"}}}}, {"returns", "actoreffect"}},
					{{"params", {{{"name", "name"}, {"type", "string"}}}}, {"returns", "actoreffect"}}}}}},
				{"Ghost", {{"description", "A member that was removed."}}}}},
				{"methods", json::object()}};
			hostlink::Send({{"t", "toPage"}, {"m", {{"t", "members"}, {"name", "Me"}, {"type", "character"}, {"live", true},
				{"members", {"Name", "NameID", "Level", "Inventory", "Effect"}}, {"methods", {"Face"}}, {"docs", docs}}}});
		}
		else if (t == "values" && m.value("chain", "") == "Me")
		{
			json v = json::object();
			for (auto &n : m["names"]) v[n.get<std::string>()] = n == "Name" ? json("Stormfall") : n == "NameID" ? json("123456") : json(nullptr);
			hostlink::Send({{"t", "toPage"}, {"m", {{"t", "values"}, {"chain", "Me"}, {"values", v}}}});
		}
	}
}

void SendChar(wchar_t ch) { hostlink::Send({{"t", "key"}, {"k", "char"}, {"w", (int)ch}, {"l", 1}, {"sys", false}}); }

void SendVk(int vk, wchar_t ch)
{
	UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	hostlink::Send({{"t", "key"}, {"k", "down"}, {"w", vk}, {"l", (int64_t)(1 | (sc << 16))}, {"sys", false}});
	if (ch) SendChar(ch);
	hostlink::Send({{"t", "key"}, {"k", "up"}, {"w", vk}, {"l", (int64_t)(1 | (sc << 16) | 0xC0000000u)}, {"sys", false}});
}

bool SaveRegion(const RECT &r, const std::wstring &path)
{
	int w = r.right - r.left, h = r.bottom - r.top;
	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	BITMAPINFO bi = {};
	bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 24, BI_RGB};
	void *bits = nullptr;
	HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	HGDIOBJ old = SelectObject(mem, bmp);
	BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
	SelectObject(mem, old);
	int stride = ((w * 3 + 3) & ~3);
	BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof(fh) + sizeof(BITMAPINFOHEADER) + stride * h), 0, 0, sizeof(fh) + sizeof(BITMAPINFOHEADER)};
	FILE *f = nullptr;
	_wfopen_s(&f, path.c_str(), L"wb");
	if (f)
	{
		fwrite(&fh, sizeof(fh), 1, f);
		fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
		fwrite(bits, stride * h, 1, f);
		fclose(f);
	}
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
	return f != nullptr;
}

LRESULT CALLBACK GameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_PAINT)
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(h, &ps);
		RECT r;
		GetClientRect(h, &r);
		for (int y = 0; y < r.bottom; y += 8)
		{
			HBRUSH b = CreateSolidBrush(RGB(20, 60 + y * 120 / r.bottom, 40));
			RECT band = {0, y, r.right, y + 8};
			FillRect(dc, &band, b);
			DeleteObject(b);
		}
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(255, 255, 255));
		RECT tr = {0, r.bottom / 2, r.right, r.bottom};
		DrawTextW(dc, L"FAKE GAME WINDOW (the console should cover the top 40%)", -1, &tr, DT_CENTER | DT_SINGLELINE);
		EndPaint(h, &ps);
		return 0;
	}
	return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

// Runtime.cpp's module: the harness passes the staged host's path, so nothing is ever unpacked.
HMODULE g_hModule = nullptr;

void PostToMain(json m)
{
	std::lock_guard<std::mutex> lk(g_mu);
	g_q.push_back(std::move(m));
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argc < 3) return 2;
	std::wstring hostExe = argv[1], shot = argv[2];
	_wfopen_s(&g_log, (shot + L".log").c_str(), L"w");

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = GameProc;
	wc.hInstance = inst;
	wc.lpszClassName = L"LSC2FakeGame";
	wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
	RegisterClassW(&wc);
	HWND game = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Fake Game", WS_POPUP | WS_BORDER,
	                            40, 40, 1100, 650, nullptr, nullptr, inst, nullptr);
	ShowWindow(game, SW_SHOWNOACTIVATE);
	UpdateWindow(game);

	// The host this starts keeps its data (history) beside the screenshot, not in the user's real one.
	std::wstring data = shot + L".data";
	CreateDirectoryW(data.c_str(), nullptr);
	{
		FILE *h = nullptr;
		_wfopen_s(&h, (data + L"\\history.jsonl").c_str(), L"wb");
		if (h)
		{
			const char *seed[] = {"echo asdf", "run asdf xyz", "echo xyz then asdf", "lsconsole2 status", "echo ASDF XYZ together"};
			for (const char *s : seed) fprintf(h, "{\"c\":\"%s\"}\n", s);
			fclose(h);
		}
	}
	SetEnvironmentVariableW(L"LSCONSOLE2_DATA", data.c_str());
	SetEnvironmentVariableW(L"LSCONSOLE2_NS", L"harness"); // its own host, even while the user's runs

	overlay::Start();
	overlay::SetToggleKey("`");
	hostlink::Start(hostExe);
	overlay::Show(false);

	ULONGLONG t0 = GetTickCount64();
	int step = 0;
	MSG msg;
	for (;;)
	{
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
		Pulse();
		ULONGLONG t = GetTickCount64() - t0;
		if (step == 0 && t > 2500 && g_ready)
		{
			// Click the input line (bottom of the 260-px console), type, Enter.
			Log("inject: click + type");
			hostlink::Send({{"t", "mouse"}, {"e", "down"}, {"x", 300}, {"y", 245}, {"b", 1}});
			hostlink::Send({{"t", "mouse"}, {"e", "up"}, {"x", 300}, {"y", 245}, {"b", 1}});
			hostlink::Send({{"t", "focus"}, {"on", true}});
			for (wchar_t ch : std::wstring(L"echo hello from the harness")) SendChar(ch);
			step = 1;
		}
		else if (step == 1 && t > 3200)
		{
			SendVk(VK_RETURN, L'\r');
			// History autocomplete: the list should show the two entries holding both words.
			for (wchar_t ch : std::wstring(L"hist asdf xyz")) SendChar(ch);
			step = 2;
		}
		else if (step == 2 && t > 4500)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot) ? "screenshot saved" : "screenshot FAILED");
			Log("status: " + overlay::Status());
			// Where the overlay is, and whether it sits above the game in z-order.
			if (HWND ov = FindWindowW(L"LSConsole2Overlay", nullptr))
			{
				RECT orc;
				GetWindowRect(ov, &orc);
				bool above = false;
				for (HWND w = GetWindow(game, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
					if (w == ov) { above = true; break; }
				char b[256];
				snprintf(b, sizeof(b), "overlay rect %ld,%ld-%ld,%ld visible=%d owner=%p game=%p above-game=%d topmost=%d",
				         orc.left, orc.top, orc.right, orc.bottom, IsWindowVisible(ov), (void *)GetWindow(ov, GW_OWNER),
				         (void *)game, above, (GetWindowLongW(ov, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
				Log(b);
			}
			else Log("overlay window not found");
			// Completion: clear the input (Esc hides the list, Esc again clears), then commands and aliases
			// at the beginning after a relay target.
			SendVk(VK_ESCAPE, 0x1B);
			SendVk(VK_ESCAPE, 0x1B);
			for (wchar_t ch : std::wstring(L"relay all e")) SendChar(ch);
			step = 10;
		}
		else if (step == 10 && t > 5300)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-2.bmp") ? "screenshot 2 saved" : "screenshot 2 FAILED");
			// Variables with their values, inside ${.
			SendVk(VK_ESCAPE, 0x1B);
			SendVk(VK_ESCAPE, 0x1B);
			for (wchar_t ch : std::wstring(L"echo ${Me.Ef")) SendChar(ch);
			step = 11;
		}
		else if (step == 11 && t > 6000)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-3.bmp") ? "screenshot 3 saved" : "screenshot 3 FAILED");
			SendVk(VK_TAB, L'\t'); // accept: the call with placeholders
			step = 111;
		}
		else if (step == 111 && t > 6600)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-3b.bmp") ? "screenshot 3b saved" : "screenshot 3b FAILED");
			for (wchar_t ch : std::wstring(L"2")) SendChar(ch);
			SendVk(VK_ESCAPE, 0x1B); // drops the rest: echo ${Me.Effect[2]
			step = 112;
		}
		else if (step == 112 && t > 7100)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-3c.bmp") ? "screenshot 3c saved" : "screenshot 3c FAILED");
			// Find: ctrl+F, a query, Enter twice (two matches older than the newest).
			SendVk(VK_ESCAPE, 0x1B);
			SendVk(VK_ESCAPE, 0x1B);
			UINT sc = MapVirtualKeyW('F', MAPVK_VK_TO_VSC);
			hostlink::Send({{"t", "key"}, {"k", "down"}, {"w", 'F'}, {"l", (int64_t)(1 | (sc << 16))}, {"sys", false}, {"m", 2}});
			hostlink::Send({{"t", "key"}, {"k", "up"}, {"w", 'F'}, {"l", (int64_t)(1 | (sc << 16) | 0xC0000000u)}, {"sys", false}, {"m", 2}});
			for (wchar_t ch : std::wstring(L"line 5")) SendChar(ch);
			SendVk(VK_RETURN, L'\r');
			SendVk(VK_RETURN, L'\r');
			step = 12;
		}
		else if (step == 12 && t > 7900)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-4.bmp") ? "screenshot 4 saved" : "screenshot 4 FAILED");
			// Brackets: typing ${ and [ adds the closers (echo ${Math.Calc[|]}), [] highlighted.
			SendVk(VK_ESCAPE, 0x1B); // closes find
			SendVk(VK_ESCAPE, 0x1B);
			SendVk(VK_ESCAPE, 0x1B);
			for (wchar_t ch : std::wstring(L"echo ${Math.Calc[")) SendChar(ch);
			step = 13;
		}
		else if (step == 13 && t > 8700)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-5.bmp") ? "screenshot 5 saved" : "screenshot 5 FAILED");
			SendChar(L']');          // steps over the ]: echo ${Math.Calc[]|}, [] and {} highlighted
			step = 14;
		}
		else if (step == 14 && t > 9400)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-6.bmp") ? "screenshot 6 saved" : "screenshot 6 FAILED");
			// Repeated lines collapse into one with a count; a suggestion from this session's history
			// (the first step ran "echo hello from the harness") shows greyed after the caret.
			for (int i = 0; i < 3; i++) SendVk(VK_ESCAPE, 0x1B);
			Lines({"repeat me", "repeat me", "repeat me", "repeat me"});
			for (wchar_t ch : std::wstring(L"echo hel")) SendChar(ch);
			step = 15;
		}
		else if (step == 15 && t > 10200)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-7.bmp") ? "screenshot 7 saved" : "screenshot 7 FAILED");
			// Scrolled up, new lines arrive: the "Back to current" button counts them.
			SendVk(VK_ESCAPE, 0x1B);
			SendVk(VK_ESCAPE, 0x1B);
			hostlink::Send({{"t", "mouse"}, {"e", "move"}, {"x", 300}, {"y", 80}, {"b", 0}});
			hostlink::Send({{"t", "wheel"}, {"dx", 0}, {"dy", 600}}); // up, as a real wheel notch away is +96
			step = 151;
		}
		else if (step == 151 && t > 10700)
		{
			Lines({"new line A", "new line B", "new line C"}); // after the scroll has landed
			step = 16;
		}
		else if (step == 16 && t > 11200)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-8.bmp") ? "screenshot 8 saved" : "screenshot 8 FAILED");
			// Right-click on the output: the menu (Copy all; no Copy without a selection).
			hostlink::Send({{"t", "mouse"}, {"e", "down"}, {"x", 300}, {"y", 80}, {"b", 3}});
			hostlink::Send({{"t", "mouse"}, {"e", "up"}, {"x", 300}, {"y", 80}, {"b", 3}});
			step = 17;
		}
		else if (step == 17 && t > 11800)
		{
			RECT r;
			GetWindowRect(game, &r);
			Log(SaveRegion(r, shot + L"-9.bmp") ? "screenshot 9 saved" : "screenshot 9 FAILED");
			// The console key, delivered to our window, must close the console rather than type.
			if (HWND ov = FindWindowW(L"LSConsole2Overlay", nullptr))
				PostMessageW(ov, WM_KEYDOWN, VK_OEM_3, 1 | (MapVirtualKeyW(VK_OEM_3, MAPVK_VK_TO_VSC) << 16));
			step = 3;
		}
		else if (step == 3 && t > 12500)
		{
			Log("after console key: " + overlay::Status());
			step = 4;
		}
		else if (step == 4 || t > 20000) break;
		Sleep(10);
	}

	hostlink::Stop();
	overlay::Stop();
	DestroyWindow(game);
	if (g_log) fclose(g_log);
	return 0;
}
