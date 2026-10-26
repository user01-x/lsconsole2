// LSConsole2 -- the session side of the console.
//
// This file is the only one that talks to InnerSpace, and only on InnerSpace's thread:
//   - every console line, from the "Console" service, into a ring buffer;
//   - the `lsconsole2` command, and standing in for `console`;
//   - on the pulse: what the page asked for (PostToMain), and new lines out to the page.
// The overlay window (Overlay.cpp) and the pipe to the render host (HostLink.cpp) start when the
// extension loads and run on threads of their own.
#include <isxdk.h>
#include "Console.h"
#include "Metadata.h"
#include "../common/Protocol.h"
#include <algorithm>
#include <cstdarg>
#include <deque>
#include <mutex>
#include <atomic>
#include <map>
#include <regex>
#include <thread>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "isxdk.lib")

#define EXTENSION_VERSION "0.1.0-poc"

class LSConsole2 : public ISXInterface {
public:
	bool Initialize(ISInterface *p_ISInterface) override;
	void Shutdown() override;
};

ISInterface *pISInterface = nullptr; // ISXPreSetup's exports refer to it
HISXSERVICE hPulseService = 0;
HISXSERVICE hConsoleService = 0;
HISXSERVICE hExtensionsService = 0;

ISXPreSetup("LSConsole2", LSConsole2);

namespace {

constexpr size_t kMaxLines = 50000;  // the ring
constexpr size_t kBatch = 1000;      // lines per message to the page
constexpr ULONGLONG kFlushMs = 50;   // at most 20 sends a second

// Capture. g_first is the absolute index of g_lines.front(); g_sent the next one to send.
std::mutex g_capMu;
std::deque<std::string> g_lines;
uint64_t g_first = 0;
uint64_t g_sent = 0;

std::mutex g_mainMu;
std::vector<json> g_mainQ;

bool g_started = false;
bool g_pageReady = false;
ULONGLONG g_lastFlush = 0;

// The console bind, while we hold it.
fLSCommand g_stockConsole = nullptr; // InnerSpace's `console`, while ours stands in for it

void AddLine(std::string s)
{
	std::lock_guard<std::mutex> lk(g_capMu);
	g_lines.push_back(std::move(s));
	while (g_lines.size() > kMaxLines) { g_lines.pop_front(); g_first++; }
}

void Say(const char *fmt, ...)
{
	char buf[1024];
	va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
	pISInterface->Printf("\aylsconsole2\ax: %s", buf);
}

std::string Parse(const char *seq)
{
	char b[1024] = "";
	if (!pISInterface->DataParse(seq, b, sizeof(b))) return "";
	if (strcmp(b, "NULL") == 0) return "";
	return b;
}

void Exec(const std::string &c) { pISInterface->ExecuteCommand(c.c_str()); }

std::string Quote(const std::string &s)
{
	std::string q = "\"";
	for (char ch : s) { if (ch == '"') q += '\\'; q += ch; }
	return q + "\"";
}

std::wstring HostExe()
{
	wchar_t w[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, ModulePath, -1, w, MAX_PATH);
	return std::wstring(w) + L"\\LSConsole2\\LSConsole2Host.exe";
}

bool EnsureStarted()
{
	if (g_started) return true;
	std::wstring exe = HostExe();
	if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		Say("\arthe render host is missing: %s", lsc2::Narrow(exe).c_str());
		return false;
	}
	meta::Load(); // built-ins, published, directory (Metadata.cpp)
	overlay::Start(); // first: the frame event and the window size must exist before "hello"
	overlay::SetToggleKey(Parse("${Input.Bind[console].Combo}"));
	std::string session = Parse("${Session}");
	overlay::SetLayoutKey(session.empty() ? "default" : session); // where this session's console was put
	hostlink::Start(exe);
	g_started = true;
	return true;
}

// ---------------------------------------------------------------------------------------------
// Standing in for the `console` command (the user, 2026-10-26)
// ---------------------------------------------------------------------------------------------
// `console on|off|toggle` show and hide this console, so the console key (its stock bind runs
// `console toggle`), scripts and UI buttons all open it. Every other form goes to InnerSpace's own
// command, and `console -stock <...>` reaches it for on/off/toggle too. Unloading puts the stock
// command back.
int __cdecl CMD_Console(int argc, char *argv[])
{
	if (argc > 1 && !_stricmp(argv[1], "-stock"))
	{
		std::vector<char *> rest{argv[0]};
		rest.insert(rest.end(), argv + 2, argv + argc);
		return g_stockConsole((int)rest.size(), rest.data());
	}
	if (argc == 2 && EnsureStarted())
	{
		if (!_stricmp(argv[1], "on")) { overlay::Show(true); return 0; }
		if (!_stricmp(argv[1], "off")) { overlay::Hide(); return 0; }
		if (!_stricmp(argv[1], "toggle")) { overlay::Toggle(); return 0; }
	}
	return g_stockConsole(argc, argv);
}

void WrapConsole()
{
	g_stockConsole = pISInterface->IsCommand("console");
	if (!g_stockConsole) { Say("\arthere is no `console` command to stand in for"); return; }
	pISInterface->RemoveCommand("console");
	pISInterface->AddCommand("console", CMD_Console, true, false);
}

void UnwrapConsole()
{
	if (!g_stockConsole) return;
	if (pISInterface->IsCommand("console") == CMD_Console)
	{
		pISInterface->RemoveCommand("console");
		pISInterface->AddCommand("console", g_stockConsole, true, false);
	}
	else // someone wrapped ours: putting the stock one back would drop theirs
		Say("\ar`console` was replaced after LSConsole2 loaded; it is left as it is");
	g_stockConsole = nullptr;
}

// ---------------------------------------------------------------------------------------------
// Pulse
// ---------------------------------------------------------------------------------------------
void Flush()
{
	json lines = json::array();
	{
		std::lock_guard<std::mutex> lk(g_capMu);
		if (g_sent < g_first) g_sent = g_first; // the ring dropped them before we got there
		size_t from = (size_t)(g_sent - g_first);
		size_t n = std::min(kBatch, g_lines.size() - from);
		for (size_t i = 0; i < n; i++) lines.push_back(g_lines[from + i]);
		g_sent += n;
	}
	if (!lines.empty()) hostlink::Send({{"t", "toPage"}, {"m", {{"t", "lines"}, {"lines", std::move(lines)}}}});
}

// ---------------------------------------------------------------------------------------------
// Autocomplete data (the user's spec, 2026-10-26; sizes and costs from `lsconsole2 probe`)
//
// The LavishScript lists, read whole as JSON: Types, TopLevelObjects (minus exact, case-sensitive
// matches in Types -- every type is also a top-level object of the same name), Events, Commands
// (LavishScript's list leaves out script-only commands like Wait and Call, unlike the SDK's),
// Aliases (name = "replace"), Scripts (running) and VariableScope.Keys (the variables the console
// can see). The first six cost about 3 ms of this thread (lsconsole2 probe), so they
// are read when the page comes up and every 5 s while the console is on screen, and a list is sent
// only when it changed. Script file names come from a background walk of HomeDirectory\Scripts.
// ---------------------------------------------------------------------------------------------
constexpr ULONGLONG kAcRefreshMs = 5000;
constexpr ULONGLONG kFilesRefreshMs = 60000;
std::map<std::string, std::string> g_acSent; // list -> what the page has (JSON)
ULONGLONG g_lastAc = 0;

// A JSON array read from a data sequence; anything else reads as empty.
json ReadArray(const std::string &seq)
{
	static std::vector<char> buf(1 << 20);
	buf[0] = 0;
	pISInterface->DataParse(seq.c_str(), buf.data(), buf.size());
	json v = json::parse(buf.data(), nullptr, false);
	return v.is_array() ? v : json::array();
}

json ReadList(const char *name) { return ReadArray(std::string("${LavishScript.") + name + "~}"); }

// The value of `expr` (a variable, or a chain ending in a member) for the completion list, only when
// its type is a basic one (the user, 2026-10-26: an object's text is noise); cut to 80 characters.
// False when it isn't basic.
bool BasicValue(const std::string &expr, std::string &value)
{
	static const std::unordered_set<std::string> kBasic = {
		"bool", "byte", "int", "uint", "int64", "float", "float64", "string", "unistring", "mutablestring",
		"boolptr", "byteptr", "intptr", "uintptr", "int64ptr", "floatptr", "float64ptr", "stringptr"};
	char b[256] = "";
	pISInterface->DataParse(("${" + expr + "(type)}").c_str(), b, sizeof(b));
	if (!kBasic.count(b)) return false;
	b[0] = 0;
	pISInterface->DataParse(("${" + expr + "}").c_str(), b, sizeof(b));
	value = b;
	if (value.size() > 80) value = value.substr(0, 77) + "...";
	return true;
}

// ---------------------------------------------------------------------------------------------
// lsconsole2 exclude <text> [<replacement>] (the user, 2026-10-26; replaced `anon`): a map that
// keeps text out of completions, for recordings or anything else. It lasts until the extension
// unloads. The page applies it, ignoring case, to every completion it lists:
//   - no replacement: a completion containing the text isn't listed, and a value preview
//     containing it isn't shown;
//   - a replacement: shown in place of the text, in names and value previews. Accepting a
//     completion still inserts the real name.
// Completions only: the console output is never changed.
// ---------------------------------------------------------------------------------------------
std::vector<std::pair<std::string, json>> g_excludes; // text -> replacement, or null: leave it out

void SendExcludes()
{
	json list = json::array();
	for (auto &e : g_excludes) list.push_back({{"s", e.first}, {"r", e.second}});
	hostlink::Send({{"t", "toPage"}, {"m", {{"t", "exclude"}, {"list", list}}}});
}

void Exclude(int argc, char *argv[])
{
	if (argc < 3) // the map so far
	{
		if (g_excludes.empty()) Say("nothing excluded: lsconsole2 exclude <text> [<replacement>]");
		for (auto &e : g_excludes)
		{
			std::string what = e.second.is_null() ? "left out" : "shown as \"" + e.second.get<std::string>() + "\"";
			Say("\"%s\": %s", e.first.c_str(), what.c_str());
		}
		return;
	}
	std::string text = argv[2];
	if (text.empty()) { Say("\arexclude: the text to exclude is empty"); return; }
	json with = argc > 3 ? json(std::string(argv[3])) : json(nullptr);
	auto it = std::find_if(g_excludes.begin(), g_excludes.end(), [&](const auto &e) { return !_stricmp(e.first.c_str(), text.c_str()); });
	if (it != g_excludes.end()) *it = {text, with}; // updates the replacement
	else g_excludes.push_back({text, with});
	SendExcludes();
	if (with.is_null()) Say("completions containing \"%s\" are left out, and so are value previews containing it", text.c_str());
	else Say("\"%s\" is shown as \"%s\" in completions", text.c_str(), with.get<std::string>().c_str());
}

// Extensions for `ext[ension] <name>` and `ext[ension] -unload <name>` (the user, 2026-10-26):
// every .dll in the extensions folder (the parent of this DLL's folder) and its subfolders (ISXDK35,
// ...), named as `ext` takes them, each with whether it's loaded now (${Extension[name]}).
json ReadExtensions()
{
	wchar_t mine[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, ModulePath, -1, mine, MAX_PATH);
	std::wstring root(mine);
	size_t cut = root.find_last_of(L"\\/");
	if (cut != std::wstring::npos) root.resize(cut);
	std::vector<std::wstring> dirs{root};
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') dirs.push_back(root + L"\\" + fd.cFileName);
		while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::map<std::string, std::string> names; // lower case -> as on disk
	for (auto &dir : dirs)
	{
		h = FindFirstFileW((dir + L"\\*.dll").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) continue;
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			std::string n = lsc2::Narrow(fd.cFileName);
			n.resize(n.size() - 4); // ".dll"
			bool plain = !n.empty();
			for (char ch : n) plain = plain && (isalnum((unsigned char)ch) || ch == '_' || ch == '-' || ch == '.');
			if (!plain) continue; // it goes inside ${Extension[...]} below
			std::string key = n;
			for (auto &ch : key) ch = (char)tolower((unsigned char)ch);
			names.emplace(key, n);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	json list = json::array();
	for (auto &kv : names)
		list.push_back({{"n", kv.second}, {"l", Parse(("${Extension[" + kv.second + "]}").c_str()) == "TRUE"}});
	return list;
}

// Sends the lists that changed since the page last got them (all of them after "ready").
void SendAc(json extra = json::object())
{
	json types = ReadList("Types");
	std::unordered_set<std::string> typeSet;
	for (auto &t : types) if (t.is_string()) typeSet.insert(t.get<std::string>());
	json tlos = json::array();
	for (auto &t : ReadList("TopLevelObjects"))
		if (t.is_string() && !typeSet.count(t.get<std::string>())) tlos.push_back(t);
	json aliases = json::array();
	for (auto &a : ReadList("Aliases"))
		if (a.is_object() && a.contains("replace") && a["replace"].is_string())
			aliases.push_back({{"n", a["replace"]}, {"w", a.value("with", "")}});

	// Variables with their current value (shown beside the name in the list, never inserted), for
	// basic types only. Only plain names are read; anything odder is listed without a value.
	json variables = json::array();
	for (auto &k : ReadArray("${LavishScript.VariableScope.Keys~}"))
	{
		if (!k.is_string()) continue;
		std::string name = k.get<std::string>(), v;
		bool plain = !name.empty();
		for (char ch : name) plain = plain && (isalnum((unsigned char)ch) || ch == '_');
		json e = {{"n", name}};
		if (plain && BasicValue(name, v)) e["v"] = v;
		variables.push_back(e);
	}

	json lists = {{"types", types}, {"tlos", tlos}, {"events", ReadList("Events")}, {"commands", ReadList("Commands")},
	              {"aliases", aliases}, {"scripts", ReadList("Scripts")}, {"variables", variables},
	              {"extensions", ReadExtensions()},
	              {"meta", meta::Globals()}}; // commands, objects, events documented
	for (auto &kv : extra.items()) lists[kv.key()] = kv.value();
	json changed = json::object();
	for (auto &kv : lists.items())
	{
		std::string s = kv.value().dump(-1, ' ', false, json::error_handler_t::replace);
		if (g_acSent[kv.key()] != s) { g_acSent[kv.key()] = s; changed[kv.key()] = kv.value(); }
	}
	if (!changed.empty()) hostlink::Send({{"t", "toPage"}, {"m", {{"t", "ac"}, {"data", changed}}}});
}

// Script files for `run`: every .iss under HomeDirectory\Scripts, as run takes them (relative,
// forward slashes, no extension). Walked on a low-priority thread; junctions below the root are
// not followed (they can loop), nor are folders whose name starts with "." (.git, .vscode, ...).
std::thread g_filesThread;
std::atomic<bool> g_filesStop{false}, g_filesBusy{false};
std::mutex g_filesMu;
std::vector<std::string> g_files;
bool g_filesFresh = false;
ULONGLONG g_lastFilesWalk = 0;

void WalkScripts(const std::wstring &dir, const std::wstring &rel, std::vector<std::string> &out)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		if (g_filesStop) break;
		std::wstring name = fd.cFileName;
		if (name == L"." || name == L"..") continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && name[0] != L'.')
				WalkScripts(dir + L"\\" + name, rel + name + L"/", out);
		}
		else if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".iss") == 0)
			out.push_back(lsc2::Narrow(rel + name.substr(0, name.size() - 4)));
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

void StartFilesWalk()
{
	if (g_filesBusy) return;
	if (g_filesThread.joinable()) g_filesThread.join();
	std::string home = Parse("${LavishScript.HomeDirectory}");
	if (home.empty()) return;
	g_lastFilesWalk = GetTickCount64();
	g_filesBusy = true;
	g_filesThread = std::thread([root = lsc2::Widen(home) + L"\\Scripts"] {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
		std::vector<std::string> files;
		WalkScripts(root, L"", files);
		std::sort(files.begin(), files.end(), [](const std::string &a, const std::string &b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
		{
			std::lock_guard<std::mutex> lk(g_filesMu);
			g_files.swap(files);
			g_filesFresh = true;
		}
		g_filesBusy = false;
	});
}

void StopFilesWalk()
{
	g_filesStop = true;
	if (g_filesThread.joinable()) g_filesThread.join();
}

// The members and methods of what a name (a top-level object or a variable) holds, for completing
// `name.` / `name:`, and of a chain of members before them (`OgreBotAPI.Bot:`). The type comes from
// evaluating ${chain(type)}. Only a chain of member reads is ever evaluated: names joined by ".",
// each optionally with [arguments] that hold no ${ and no "]"; nothing with ":" (a method) gets
// through. A single name that needs an index (Actor) reads as no type that way; then the type of
// the same name stands in (actor). Inherited members and methods are included, the type's own first.
void __cdecl CollectUnique(const char *name, void *p)
{
	auto *d = (std::pair<json *, std::unordered_set<std::string> *> *)p;
	if (name && d->second->insert(name).second) d->first->push_back(name);
}

// name ( [args] )? ( . name ( [args] )? )*
bool IsMemberChain(const std::string &s, bool &bareName)
{
	if (s.empty() || s.size() > 256 || s.find("${") != std::string::npos || s.find(':') != std::string::npos) return false;
	size_t i = 0, segments = 0;
	bool args = false;
	for (;;)
	{
		if (i >= s.size() || !(isalpha((unsigned char)s[i]) || s[i] == '_')) return false;
		while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
		segments++;
		if (i < s.size() && s[i] == '[')
		{
			size_t close = s.find(']', i);
			if (close == std::string::npos) return false;
			i = close + 1;
			args = true;
		}
		if (i == s.size()) break;
		if (s[i] != '.') return false;
		i++;
	}
	bareName = segments == 1 && !args;
	return true;
}

// A type's live members and methods (inherited ones included) and its documentation, into `reply`.
void DescribeType(LSTypeDefinition *def, json &reply)
{
	if (!def) { reply["docs"] = meta::TypeDocs({}); return; }
	std::unordered_set<std::string> seenMembers, seenMethods;
	std::pair<json *, std::unordered_set<std::string> *> members(&reply["members"], &seenMembers), methods(&reply["methods"], &seenMethods);
	std::vector<std::string> chain;
	for (int depth = 0; def && depth < 16; depth++, def = def->GetInheritance())
	{
		reply["chain"].push_back(def->GetName());
		chain.push_back(def->GetName());
		def->EnumMembers(CollectUnique, &members);
		def->EnumMethods(CollectUnique, &methods);
	}
	reply["type"] = reply["chain"][0];
	reply["docs"] = meta::TypeDocs(chain);
	reply["live"] = true; // the members and methods lists are the real ones
}

json EmptyTypeReply(const std::string &name)
{
	return {{"t", "members"}, {"name", name}, {"type", ""}, {"chain", json::array()},
	        {"members", json::array()}, {"methods", json::array()}, {"docs", json::object()}, {"live", false}};
}

void SendTypeInfo(const std::string &name)
{
	bool bareName = false;
	json reply = EmptyTypeReply(name);
	if (IsMemberChain(name, bareName))
	{
		std::string type = Parse(("${" + name + "(type)}").c_str());
		LSTypeDefinition *def = type.empty() ? nullptr : pISInterface->FindLSTypeDefinition(type.c_str());
		if (!def && bareName) def = pISInterface->FindLSTypeDefinition(name.c_str());
		if (!def && bareName)
		{
			std::string lower = name;
			for (auto &ch : lower) ch = (char)tolower((unsigned char)ch);
			def = pISInterface->FindLSTypeDefinition(lower.c_str());
		}
		if (def) DescribeType(def, reply);
	}
	hostlink::Send({{"t", "toPage"}, {"m", reply}});
}

// The same for a type by name, no evaluation at all: for chains resolved through documented return
// types (`Me.Effect[1].` -> actoreffect). Answers as name "@<type>".
void SendTypeByName(const std::string &type)
{
	json reply = EmptyTypeReply("@" + type);
	bool ok = !type.empty() && type.size() < 64;
	for (char ch : type) ok = ok && (isalnum((unsigned char)ch) || ch == '_');
	if (ok)
	{
		LSTypeDefinition *def = pISInterface->FindLSTypeDefinition(type.c_str());
		if (def) DescribeType(def, reply);
		else
		{
			// Not a live type (its extension isn't loaded, or the docs are wrong): docs only.
			reply["type"] = type;
			reply["docs"] = meta::TypeDocs({type});
		}
	}
	hostlink::Send({{"t", "toPage"}, {"m", reply}});
}

// The values of chain.name for the names the page shows (`${Me.Na` -> Name (Stormfall)): basic types
// only; null for the rest. The page asks only for its best matches once something is typed, never
// for methods, and each chain.name must pass the same member-chain check as SendTypeInfo.
// Script[x].VariableScope: that script's variables, for the completion list (the user, 2026-10-26:
// "only after Script[x].VariableScope. and fetch it on the fly there"). Nothing is read unless the
// script exists.
void SendScopeVars(const std::string &chain)
{
	static const std::regex kScope(R"(^Script\[([^\]]*)\]\.VariableScope$)", std::regex::icase);
	json names = json::array();
	std::smatch m;
	if (std::regex_match(chain, m, kScope) && Parse(("${Script[" + m[1].str() + "](exists)}").c_str()) == "TRUE")
		for (auto &k : ReadArray("${Script[" + m[1].str() + "].VariableScope.Keys~}"))
			if (k.is_string()) names.push_back(k);
	hostlink::Send({{"t", "toPage"}, {"m", {{"t", "scopeVars"}, {"chain", chain}, {"names", names}}}});
}

void SendValues(const std::string &chain, const json &names)
{
	json values = json::object();
	int n = 0;
	for (auto &nm : names)
	{
		if (!nm.is_string() || ++n > 20) continue;
		std::string expr = chain + "." + nm.get<std::string>(), v;
		bool bare;
		if (IsMemberChain(expr, bare) && BasicValue(expr, v)) values[nm.get<std::string>()] = v;
		else values[nm.get<std::string>()] = nullptr;
	}
	hostlink::Send({{"t", "toPage"}, {"m", {{"t", "values"}, {"chain", chain}, {"values", values}}}});
}

// From the pulse: refresh while the console is up, and pass on a finished file walk.
void PulseAc()
{
	if (meta::PulseReload()) { g_lastAc = 0; } // an extension came or went: its metadata did too
	if (!g_pageReady || !hostlink::Connected()) return;
	ULONGLONG now = GetTickCount64();
	if (overlay::Shown() && now - g_lastFilesWalk > kFilesRefreshMs) StartFilesWalk();
	json extra = json::object();
	{
		std::lock_guard<std::mutex> lk(g_filesMu);
		if (g_filesFresh) { extra["files"] = g_files; g_filesFresh = false; }
	}
	if (!extra.empty() || (overlay::Shown() && now - g_lastAc > kAcRefreshMs))
	{
		g_lastAc = now;
		SendAc(extra);
	}
}

void OnPulse(){
	std::vector<json> q;
	{
		std::lock_guard<std::mutex> lk(g_mainMu);
		q.swap(g_mainQ);
	}
	for (auto &m : q)
	{
		const std::string t = m.value("t", "");
		if (t == "ready")
		{
			// A fresh page (first open, or a restarted host): send it everything again.
			g_pageReady = true;
			{
				std::lock_guard<std::mutex> lk(g_capMu);
				g_sent = g_first;
			}
			hostlink::Send({{"t", "toPage"}, {"m", {{"t", "reset"}}}});
			SendExcludes();
			// ...and every autocomplete list.
			g_acSent.clear();
			g_lastAc = 0;
			std::lock_guard<std::mutex> fl(g_filesMu);
			if (!g_files.empty()) g_filesFresh = true;
		}
		else if (t == "exec")
		{
			std::string text = m.value("text", "");
			if (text.empty()) continue;
			// `consoleclear` typed here, the whole line in any case, clears this console instead of
			// going to InnerSpace's command of that name (the user, 2026-10-26: "just hijack it").
			{
				size_t a = text.find_first_not_of(" \t"), b = text.find_last_not_of(" \t");
				if (a != std::string::npos && _stricmp(text.substr(a, b - a + 1).c_str(), "consoleclear") == 0)
				{
					{
						std::lock_guard<std::mutex> lk(g_capMu);
						g_first += g_lines.size();
						g_lines.clear();
						g_sent = g_first;
					}
					hostlink::Send({{"t", "toPage"}, {"m", {{"t", "reset"}}}});
					continue;
				}
			}
			SYSTEMTIME now;
			GetLocalTime(&now);
			char at[16];
			snprintf(at, sizeof(at), "%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
			AddLine("\x01" + std::string(at) + "\x01" + text); // our own echo, with the time it was entered
			// Queued for InnerSpace's next frame, never run from here: a typed command may unload
			// this extension (`ext -unload LSConsole2`), and returning into our pulse after that
			// crashes the game (2026-10-26, dump EverQuest2.exe.63656.dmp).
			pISInterface->ExecuteTimedCommand(1, text.c_str());
		}
		else if (t == "members" && m.contains("type")) SendTypeByName(m.value("type", ""));
		else if (t == "members") SendTypeInfo(m.value("name", ""));
		else if (t == "values" && m.contains("names") && m["names"].is_array()) SendValues(m.value("chain", ""), m["names"]);
		else if (t == "scopeVars") SendScopeVars(m.value("chain", ""));
		else if (t == "_log") Say("%s", m.value("text", "").c_str());
		else if (t == "_down") g_pageReady = false;
	}

	PulseAc();

	if (g_pageReady && hostlink::Connected() && GetTickCount64() - g_lastFlush >= kFlushMs)
	{
		g_lastFlush = GetTickCount64();
		Flush();
	}
}

void __cdecl PulseService(bool, unsigned int MSG, void *)
{
	if (MSG != PULSE_PULSE) return;
	try { OnPulse(); }
	catch (const std::exception &e) { Say("\arpulse: %s", e.what()); }
}

void __cdecl ExtensionsService(bool, unsigned int MSG, void *)
{
	if (MSG == EXTENSIONS_ADDED || MSG == EXTENSIONS_REMOVED) meta::NoteExtensionsChanged();
}

void __cdecl ConsoleService(bool, unsigned int MSG, void *lpData)
{
	// CONSOLE_OUTPUT is the same line without its \a colour codes; taking both would store it twice.
	if (MSG == CONSOLE_OUTPUT_WITHCODES && lpData) AddLine((const char *)lpData);
}

// ---------------------------------------------------------------------------------------------
// `lsconsole2 probe` -- what the autocomplete sources look like in this session, written to
// %LOCALAPPDATA%\LSConsole2\probe.json: each LavishScript.<list> read whole as JSON (${...~}), with its
// size, the DataParse return, how long it took and whether it parsed; and the SDK's own enumerations
// of the same lists, to compare. Read-only.
// ---------------------------------------------------------------------------------------------
void __cdecl CollectName(const char *name, void *p) { ((json *)p)->push_back(name ? name : ""); }

double Ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}

void Probe()
{
	json d;
	static std::vector<char> buf(4 << 20); // 4 MB: room to see how big these get
	for (const char *list : {"Types", "TopLevelObjects", "Events", "Commands", "Aliases", "Scripts"})
	{
		std::string seq = std::string("${LavishScript.") + list + "~}";
		buf[0] = 0;
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		int ret = pISInterface->DataParse(seq.c_str(), buf.data(), buf.size());
		QueryPerformanceCounter(&t1);
		std::string raw(buf.data());
		json e = {{"seq", seq}, {"ret", ret}, {"bytes", raw.size()}, {"ms", Ms(t0, t1)}};
		json v = json::parse(raw, nullptr, false);
		e["parsed"] = !v.is_discarded();
		if (v.is_discarded()) e["raw_head"] = raw.substr(0, 400);
		else { e["count"] = v.is_array() ? v.size() : 0; e["value"] = v; }
		d["ls"][list] = e;
	}
	json sdk;
	auto timed = [&](const char *name, unsigned int (ISInterface::*fn)(fLSGenericEnumCallback, void *)) {
		json names = json::array();
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		(pISInterface->*fn)(CollectName, &names);
		QueryPerformanceCounter(&t1);
		sdk[name] = {{"count", names.size()}, {"ms", Ms(t0, t1)}, {"names", names}};
	};
	timed("types", &ISInterface::EnumLSTypeDefinitions);
	timed("tlos", &ISInterface::EnumTopLevelObjects);
	timed("commands", &ISInterface::EnumCommands);
	timed("aliases", &ISInterface::EnumAliases);
	timed("scripts", &ISInterface::EnumScripts);
	d["sdk"] = sdk;
	d["homeDirectory"] = Parse("${LavishScript.HomeDirectory}");

	wchar_t lad[MAX_PATH] = L"";
	GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
	std::wstring dir = std::wstring(lad) + L"\\LSConsole2", path = dir + L"\\probe.json";
	CreateDirectoryW(dir.c_str(), nullptr);
	FILE *f = nullptr;
	_wfopen_s(&f, path.c_str(), L"wb");
	if (!f) { Say("\arcould not write %s", lsc2::Narrow(path).c_str()); return; }
	std::string s = d.dump(1, '\t', false, json::error_handler_t::replace);
	fwrite(s.data(), 1, s.size(), f);
	fclose(f);
	Say("probe written: %s", lsc2::Narrow(path).c_str());
}

// ---------------------------------------------------------------------------------------------
// lsconsole2 meta [reload|check]
// ---------------------------------------------------------------------------------------------
std::string LowerStr(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }

// The live names vs the documented ones: what's undocumented, and what's documented but gone.
// Summary to the console; every name to %LOCALAPPDATA%\LSConsole2\meta-check.json.
void MetaCheck()
{
	const json &doc = meta::Doc();
	json report = {{"types", json::object()}, {"commands", json::object()}, {"objects", json::object()}};
	size_t undocumented = 0, missing = 0, typesSeen = 0;
	auto diff = [&](const json &live, const json &documented, json &into) {
		std::unordered_set<std::string> l, d;
		json und = json::array(), mis = json::array();
		for (auto &n : live) if (n.is_string()) l.insert(LowerStr(n.get<std::string>()));
		if (documented.is_object()) for (auto &kv : documented.items()) d.insert(LowerStr(kv.key()));
		for (auto &n : live) if (n.is_string() && !d.count(LowerStr(n.get<std::string>()))) und.push_back(n);
		if (documented.is_object()) for (auto &kv : documented.items()) if (!l.count(LowerStr(kv.key()))) mis.push_back(kv.key());
		undocumented += und.size(); missing += mis.size();
		if (!und.empty()) into["undocumented"] = und;
		if (!mis.empty()) into["missing"] = mis;
	};
	for (auto &t : ReadList("Types"))
	{
		if (!t.is_string()) continue;
		std::string type = t.get<std::string>(), lower = LowerStr(type);
		if (!doc["types"].contains(lower)) continue;
		LSTypeDefinition *def = pISInterface->FindLSTypeDefinition(type.c_str());
		if (!def) continue;
		typesSeen++;
		json members = json::array(), methods = json::array(), r = json::object(), rm = json::object();
		def->EnumMembers(CollectName, &members); // the type's own, not inherited
		def->EnumMethods(CollectName, &methods);
		const json &td = doc["types"][lower];
		diff(members, td.contains("members") ? td["members"] : json::object(), r);
		diff(methods, td.contains("methods") ? td["methods"] : json::object(), rm);
		if (!r.empty()) report["types"][type]["members"] = r;
		if (!rm.empty()) report["types"][type]["methods"] = rm;
	}
	diff(ReadList("Commands"), doc["commands"], report["commands"]);
	diff(ReadList("TopLevelObjects"), doc["objects"], report["objects"]);

	wchar_t lad[MAX_PATH] = L"";
	GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
	std::wstring path = std::wstring(lad) + L"\\LSConsole2\\meta-check.json";
	FILE *f = nullptr;
	_wfopen_s(&f, path.c_str(), L"wb");
	if (f)
	{
		std::string out = report.dump(1, '\t', false, json::error_handler_t::replace);
		fwrite(out.data(), 1, out.size(), f);
		fclose(f);
	}
	Say("meta check: %zu documented types are live; %zu live names undocumented, %zu documented names missing; details: %s",
	    typesSeen, undocumented, missing, lsc2::Narrow(path).c_str());
}

void MetaCommand(const char *what)
{
	if (!meta::Loaded()) meta::Load();
	if (!_stricmp(what, "reload")) { meta::Load(); g_acSent.erase("meta"); g_lastAc = 0; }
	if (!_stricmp(what, "check")) { MetaCheck(); return; }
	std::string s = meta::Summary();
	size_t at = 0;
	for (;;)
	{
		size_t nlpos = s.find('\n', at);
		Say("%s%s", at ? "" : "metadata: ", s.substr(at, nlpos == std::string::npos ? std::string::npos : nlpos - at).c_str());
		if (nlpos == std::string::npos) break;
		at = nlpos + 1;
	}
}

// ---------------------------------------------------------------------------------------------
// lsconsole2
// ---------------------------------------------------------------------------------------------
int __cdecl CMD_LSConsole2(int argc, char *argv[])
{
	const char *sub = argc > 1 ? argv[1] : "toggle";
	if (!_stricmp(sub, "toggle")) { if (EnsureStarted()) overlay::Toggle(); }
	else if (!_stricmp(sub, "show")) { if (EnsureStarted()) overlay::Show(true); }
	else if (!_stricmp(sub, "hide")) { if (g_started) overlay::Hide(); }
	else if (!_stricmp(sub, "probe")) Probe();
	else if (!_stricmp(sub, "meta")) MetaCommand(argc > 2 ? argv[2] : "");
	else if (!_stricmp(sub, "exclude")) Exclude(argc, argv);
	else if (!_stricmp(sub, "status"))
	{
		size_t n;
		{
			std::lock_guard<std::mutex> lk(g_capMu);
			n = g_lines.size();
		}
		Say("v%s, %s; %zu lines captured; page %s; `console` %s", EXTENSION_VERSION,
		    g_started ? overlay::Status().c_str() : "not started", n, g_pageReady ? "ready" : "not ready",
		    g_stockConsole ? "opens this console" : "is the stock one");
	}
	else Say("lsconsole2 [toggle|show|hide|status|probe|meta [reload|check]|exclude [<text> [<replacement>]]]");
	return 0;
}

} // namespace

void PostToMain(json m)
{
	std::lock_guard<std::mutex> lk(g_mainMu);
	g_mainQ.push_back(std::move(m));
}

bool LSConsole2::Initialize(ISInterface *p_ISInterface)
{
	pISInterface = p_ISInterface;
	hPulseService = pISInterface->ConnectService(this, "Pulse", PulseService);
	hConsoleService = pISInterface->ConnectService(this, "Console", ConsoleService);
	hExtensionsService = pISInterface->ConnectService(this, "Extensions", ExtensionsService);
	pISInterface->AddCommand("lsconsole2", CMD_LSConsole2, true, false);
	Say("LSConsole2 %s loaded; `lsconsole2` shows or hides it.", EXTENSION_VERSION);
	// The user (2026-10-26): on load, close the stock console, open this one, then stand in for
	// `console`.
	Exec("console off");
	if (EnsureStarted()) overlay::Show(true);
	WrapConsole();
	return true;
}

void LSConsole2::Shutdown()
{
	UnwrapConsole();
	StopFilesWalk();
	hostlink::Stop();  // first: the host drops this session's view
	overlay::Stop();
	pISInterface->RemoveCommand("lsconsole2");
	if (hConsoleService) pISInterface->DisconnectService(this, hConsoleService);
	if (hExtensionsService) pISInterface->DisconnectService(this, hExtensionsService);
	if (hPulseService) pISInterface->DisconnectService(this, hPulseService);
}
