// HostLink -- keeps this session connected to LSConsole2Host.exe.
//
// A connect thread loops: open the pipe (starting the host if nobody is listening), say hello,
// wait for the connection to drop, back off, repeat. A host restart is just another round of the
// loop: the new host makes a fresh view and the page asks for the backlog again ("ready").
#include "Console.h"
#include "../common/PipeConn.h"
#include "../common/Protocol.h"
#include "Runtime.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace hostlink {
namespace {

std::wstring g_hostExe;
std::thread g_thread;
HANDLE g_stopEv = nullptr;
HANDLE g_closedEv = nullptr;
lsc2::PipeConn g_conn;
std::mutex g_connMu; // Send() from other threads vs Start/Close here
std::atomic<bool> g_connected{false};
std::mutex g_sizeMu;
uint32_t g_w = 800, g_h = 300;
double g_scale = 1.0;

bool Stopping(DWORD waitMs)
{
	return WaitForSingleObject(g_stopEv, waitMs) == WAIT_OBJECT_0;
}

// Started detached from InnerSpace's job when allowed, so closing the session that happened to
// start the host doesn't take every other session's console with it.
void LaunchHost()
{
	std::wstring dir = g_hostExe.substr(0, g_hostExe.find_last_of(L'\\'));
	std::wstring cmd = L"\"" + g_hostExe + L"\"";
	if (!lsc2::RuntimeId().empty()) cmd += L" --runtime " + lsc2::RuntimeId();
	STARTUPINFOW si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	DWORD flags = CREATE_NO_WINDOW | DETACHED_PROCESS | BELOW_NORMAL_PRIORITY_CLASS;
	std::vector<wchar_t> line(cmd.begin(), cmd.end());
	line.push_back(0);
	BOOL ok = CreateProcessW(g_hostExe.c_str(), line.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB,
	                         nullptr, dir.c_str(), &si, &pi);
	if (!ok)
	{
		line.assign(cmd.begin(), cmd.end());
		line.push_back(0);
		ok = CreateProcessW(g_hostExe.c_str(), line.data(), nullptr, nullptr, FALSE, flags, nullptr, dir.c_str(), &si, &pi);
	}
	if (!ok)
	{
		PostToMain({{"t", "_log"}, {"text", "could not start LSConsole2Host.exe (error " + std::to_string(GetLastError()) + ")"}});
		return;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
}

HANDLE OpenPipe()
{
	const std::wstring name = lsc2::HostPipeName();
	ULONGLONG lastLaunch = 0;
	DWORD backoff = 250;
	for (;;)
	{
		HANDLE p = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (p != INVALID_HANDLE_VALUE)
		{
			DWORD mode = PIPE_READMODE_MESSAGE;
			SetNamedPipeHandleState(p, &mode, nullptr, nullptr);
			return p;
		}
		DWORD e = GetLastError();
		if (e == ERROR_PIPE_BUSY) { WaitNamedPipeW(name.c_str(), 1000); if (Stopping(0)) return INVALID_HANDLE_VALUE; continue; }
		if (e == ERROR_FILE_NOT_FOUND && GetTickCount64() - lastLaunch > 10000)
		{
			LaunchHost();
			lastLaunch = GetTickCount64();
		}
		if (Stopping(backoff)) return INVALID_HANDLE_VALUE;
		backoff = backoff < 4000 ? backoff * 2 : 4000;
	}
}

void Run()
{
	// The runtime travels inside this DLL: unpacked here, off the game's thread (about a second the
	// first time a version loads). Without it there is no console: the session falls back to stock.
	if (g_hostExe.empty())
	{
		std::string error;
		g_hostExe = runtime::Unpack(error);
		if (g_hostExe.empty())
		{
			PostToMain({{"t", "_runtimeFailed"}, {"text", error}});
			return;
		}
		lsc2::RuntimeId() = runtime::Id();
	}
	while (!Stopping(0))
	{
		HANDLE p = OpenPipe();
		if (p == INVALID_HANDLE_VALUE) break;

		ResetEvent(g_closedEv);
		std::unique_lock<std::mutex> clk(g_connMu);
		g_conn.Start(p, [](std::string &&m) { overlay::OnHostMessage(std::move(m)); }, [] { SetEvent(g_closedEv); });
		{
			std::lock_guard<std::mutex> lk(g_sizeMu);
			g_conn.Send(json{{"t", "hello"}, {"v", lsc2::kProtocolVersion}, {"pid", GetCurrentProcessId()},
			                 {"w", g_w}, {"h", g_h}, {"scale", g_scale}}.dump(-1, ' ', false, json::error_handler_t::replace));
		}
		g_connected = true;
		clk.unlock();
		overlay::OnLinkUp();

		HANDLE hs[2] = {g_stopEv, g_closedEv};
		DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
		{
			std::lock_guard<std::mutex> lk(g_connMu);
			g_connected = false;
			g_conn.Close();
		}
		overlay::OnLinkDown();
		PostToMain({{"t", "_down"}}); // the page is gone until the next "ready"
		if (w == WAIT_OBJECT_0) break;
		if (Stopping(1000)) break; // the host went away: give a restart a moment
	}
}

} // namespace

void Start(const std::wstring &hostExe)
{
	if (g_thread.joinable()) return;
	g_hostExe = hostExe;
	g_stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_closedEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_thread = std::thread(Run);
}

void Stop()
{
	if (!g_thread.joinable()) return;
	SetEvent(g_stopEv);
	g_thread.join();
	CloseHandle(g_stopEv); g_stopEv = nullptr;
	CloseHandle(g_closedEv); g_closedEv = nullptr;
}

void Send(const json &j)
{
	std::lock_guard<std::mutex> lk(g_connMu);
	// Console text needn't be valid UTF-8; replace bad bytes rather than throw in the game.
	if (g_connected) g_conn.Send(j.dump(-1, ' ', false, json::error_handler_t::replace));
}

bool Connected() { return g_connected; }

void SetSize(uint32_t w, uint32_t h, double scale)
{
	std::lock_guard<std::mutex> lk(g_sizeMu);
	g_w = w; g_h = h; g_scale = scale;
}

} // namespace hostlink
