#pragma once
// The wire between a session (LSConsole2.dll) and the render host (LSConsole2Host.exe).
//
// CONTROL: one duplex named pipe per session, message mode, one JSON object per pipe message.
// Every message has "t" (its type). Session -> host:
//   hello    {v, pid, w, h, scale}       first message; the host makes a view and replies "surface"
//   resize   {w, h, scale}               the console's pixel size changed; replies "surface"
//   visible  {on}                        the overlay is shown/hidden; the host renders only while shown
//   mouse    {e:"move"|"down"|"up", x, y, b}   b: 0 none, 1 left, 2 middle, 3 right
//   wheel    {dx, dy}                    pixels
//   key      {k:"down"|"up"|"char", w, l, sys} the raw WM_ wParam/lParam; Ultralight decodes them
//   focus    {on}                        our window gained/lost keyboard focus
//   toPage   {m}                         delivered to the page as lsc2.rx(m)
// Host -> session:
//   surface  {name, w, h, gen}           open this shared-memory surface (SurfaceHeader + pixels)
//   cursor   {c}                         ultralight::Cursor value
//   fromPage {m}                         what the page passed to lsc2Send(json)
//
// PIXELS: shared memory, one per view and size (`gen` changes on resize). Single buffer with a
// hand-shake, so it can't tear: the host writes only when ack == seq, bumps seq and signals the
// session's frame event; the session presents and sets ack = seq.
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <string>
#pragma comment(lib, "advapi32.lib")

namespace lsc2 {

constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kSurfaceMagic = 0x3243534C; // "LSC2"
constexpr size_t kSurfaceHeaderSize = 64;      // pixels start here; DWORD-aligned for CreateDIBSection

struct SurfaceHeader {
	uint32_t magic;
	uint32_t width, height, stride; // pixels are premultiplied BGRA, top-down
	volatile LONG seq;              // last frame the host wrote
	volatile LONG ack;              // last frame the session presented
	int32_t dirtyL, dirtyT, dirtyR, dirtyB; // what changed in frame `seq`
};
static_assert(sizeof(SurfaceHeader) <= kSurfaceHeaderSize, "header outgrew its slot");

inline std::wstring UserName()
{
	wchar_t u[256]; DWORD n = 256;
	if (!GetUserNameW(u, &n)) return L"user";
	return u;
}

// LSCONSOLE2_NS (tests) gives a separate host: its own pipe and mutex, never the user's.
inline std::wstring Namespace()
{
	wchar_t ns[64] = L"";
	return GetEnvironmentVariableW(L"LSCONSOLE2_NS", ns, 64) ? std::wstring(L"-") + ns : std::wstring();
}

// The runtime's version (src/ext/Runtime.cpp), set by the session after unpacking and passed to the
// host as --runtime <id>. It is part of the host's names, so a new LSConsole2.dll never joins an
// older host that's still running. Empty for a host started by hand (the test harness).
inline std::wstring &RuntimeId() { static std::wstring id; return id; }
inline std::wstring HostSuffix() { return Namespace() + (RuntimeId().empty() ? std::wstring() : L"-" + RuntimeId()); }

// Pipe names are machine-wide, so the user is part of the name.
inline std::wstring HostPipeName() { return L"\\\\.\\pipe\\lsconsole2-host-" + UserName() + HostSuffix(); }
inline std::wstring HostMutexName() { return L"Local\\lsconsole2-host" + HostSuffix(); }
inline std::wstring FrameEventName(DWORD pid) { return L"Local\\lsconsole2-frame-" + std::to_wstring(pid); }
inline std::wstring SurfaceName(DWORD pid, uint32_t gen)
{
	return L"Local\\lsconsole2-surf-" + std::to_wstring(pid) + L"-" + std::to_wstring(gen);
}

// Security for the pipe, the surfaces and the frame event. Sessions may run elevated or not (and
// the host inherits whichever started it), so the default -- owner = the creator's token, high
// integrity when elevated -- would lock one side out. Grant this user full access and lower the
// integrity label so a medium-integrity process can use an object a high-integrity one created.
class UserSecurity {
public:
	UserSecurity()
	{
		HANDLE tok = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
		BYTE buf[256];
		DWORD n = 0;
		LPWSTR sid = nullptr;
		if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &n) && ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid))
		{
			std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")(A;;GA;;;SY)S:(ML;;NW;;;LW)";
			LocalFree(sid);
			if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr))
			{
				sa_.nLength = sizeof(sa_);
				sa_.lpSecurityDescriptor = sd_;
				sa_.bInheritHandle = FALSE;
			}
		}
		CloseHandle(tok);
	}
	~UserSecurity() { if (sd_) LocalFree(sd_); }
	UserSecurity(const UserSecurity &) = delete;
	UserSecurity &operator=(const UserSecurity &) = delete;
	// nullptr (default security) if building the descriptor failed.
	SECURITY_ATTRIBUTES *get() { return sd_ ? &sa_ : nullptr; }

private:
	PSECURITY_DESCRIPTOR sd_ = nullptr;
	SECURITY_ATTRIBUTES sa_ = {};
};

inline std::string Narrow(const std::wstring &w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

inline std::wstring Widen(const std::string &s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

} // namespace lsc2
