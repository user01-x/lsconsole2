// Metadata -- what members, methods, objects, commands and events take and return
// (README.md, "Console metadata"). Three sources, merged in this order, later ones winning field by field:
//   1. built-in: RCDATA resources inside this DLL (src/metadata/*.json)
//   2. published: ${<Object>.ConsoleMetadata~} of a loaded extension, a jsonvalue (LoadPublished)
//   3. directory: <extensions path>\ConsoleMetadata\*.json
// Main thread only (it evaluates data sequences).
#include <isxdk.h>
#include "Metadata.h"
#include "../common/Protocol.h"
#include <algorithm>
#include <map>
#include <unordered_set>
#include <vector>

extern ISInterface *pISInterface;
extern HMODULE g_hModule;

namespace meta {
namespace {

json g_doc;                       // the merged model: {types, objects, commands, events}
std::vector<json> g_builtins;     // parsed once
std::vector<std::string> g_sources, g_problems;
bool g_loaded = false;
bool g_reloadPending = false;
ULONGLONG g_reloadAt = 0;
std::string g_dir;

std::string Lower(std::string s)
{
	for (auto &c : s) c = (char)tolower((unsigned char)c);
	return s;
}

// Objects merge key by key, recursively; anything else (strings, form lists) is replaced.
void Merge(json &dst, const json &src)
{
	if (!dst.is_object() || !src.is_object()) { dst = src; return; }
	for (auto &kv : src.items())
	{
		if (dst.contains(kv.key()) && dst[kv.key()].is_object() && kv.value().is_object()) Merge(dst[kv.key()], kv.value());
		else dst[kv.key()] = kv.value();
	}
}

// One document into the model. Type names are stored lower case (LavishScript type names are).
void Add(const json &d, const std::string &from)
{
	if (!d.is_object() || d.value("schema", 0) != 1)
	{
		g_problems.push_back(from + ": not a schema 1 document");
		return;
	}
	if (d.contains("types") && d["types"].is_object())
		for (auto &kv : d["types"].items()) Merge(g_doc["types"][Lower(kv.key())], kv.value());
	for (const char *k : {"objects", "commands", "events"})
		if (d.contains(k) && d[k].is_object()) Merge(g_doc[k], d[k]);
	std::string name = d.value("name", ""), version = d.value("version", "");
	g_sources.push_back(from + (name.empty() ? "" : " (" + name + (version.empty() ? "" : " " + version) + ")"));
}

BOOL CALLBACK CollectResource(HMODULE h, LPCSTR type, LPSTR name, LONG_PTR p)
{
	HRSRC r = FindResourceA(h, name, type);
	HGLOBAL g = r ? LoadResource(h, r) : nullptr;
	const char *data = g ? (const char *)LockResource(g) : nullptr;
	if (data)
	{
		json d = json::parse(data, data + SizeofResource(h, r), nullptr, false);
		if (d.is_discarded()) g_problems.push_back("built-in resource: not JSON");
		else ((std::vector<json> *)p)->push_back(std::move(d));
	}
	return TRUE;
}

void LoadBuiltins()
{
	if (!g_builtins.empty()) return;
	EnumResourceNamesA(g_hModule, RT_RCDATA, CollectResource, (LONG_PTR)&g_builtins);
}

// Whether a type has a ConsoleMetadata member, its own or inherited: asked of the SDK, nothing
// evaluated.
bool HasMetadataMember(LSTypeDefinition *def)
{
	char member[] = "ConsoleMetadata";
	for (int depth = 0; def && depth < 16; depth++, def = def->GetInheritance())
		if (def->FindMember(member)) return true;
	return false;
}

// Published metadata (the user, 2026-10-26). Asking for a member that doesn't exist prints an error,
// so it's found in three steps:
//   1. every datatype is checked through the SDK for a ConsoleMetadata member (no evaluation);
//   2. only if one has it, each top-level object's type is read with ${<Object>(type)};
//   3. the objects of those types are read with ${<Object>.ConsoleMetadata~}.
void LoadPublished()
{
	static std::vector<char> buf(4 << 20);
	auto read = [](const std::string &seq) {
		buf[0] = 0;
		pISInterface->DataParse(seq.c_str(), buf.data(), buf.size());
		return json::parse(buf.data(), nullptr, false);
	};
	std::unordered_set<std::string> publishing; // lower case
	json types = read("${LavishScript.Types~}");
	if (types.is_array())
		for (auto &t : types)
			if (t.is_string() && HasMetadataMember(pISInterface->FindLSTypeDefinition(t.get<std::string>().c_str())))
				publishing.insert(Lower(t.get<std::string>()));
	if (publishing.empty()) return; // the usual case: nothing else is read
	json tlos = read("${LavishScript.TopLevelObjects~}");
	if (!tlos.is_array()) return;
	for (auto &t : tlos)
	{
		if (!t.is_string()) continue;
		std::string name = t.get<std::string>();
		char type[256] = "";
		pISInterface->DataParse(("${" + name + "(type)}").c_str(), type, sizeof(type));
		if (!publishing.count(Lower(type))) continue;
		json d = read("${" + name + ".ConsoleMetadata~}");
		if (d.is_discarded()) g_problems.push_back(name + ".ConsoleMetadata: not JSON");
		else Add(d, "published by " + name);
	}
}

void LoadDirectory()
{
	char ext[MAX_PATH] = "";
	pISInterface->GetExtensionsPath(ext, sizeof(ext));
	g_dir = std::string(ext) + "\\ConsoleMetadata";
	std::vector<std::string> files;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((g_dir + "\\*.json").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
	FindClose(h);
	std::sort(files.begin(), files.end());
	for (auto &f : files)
	{
		FILE *fp = nullptr;
		if (fopen_s(&fp, (g_dir + "\\" + f).c_str(), "rb") || !fp) { g_problems.push_back(f + ": cannot read"); continue; }
		std::string s;
		char chunk[65536];
		size_t n;
		while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) s.append(chunk, n);
		fclose(fp);
		json d = json::parse(s, nullptr, false);
		if (d.is_discarded()) g_problems.push_back(f + ": not JSON");
		else Add(d, f);
	}
}

// A type's entries of one kind (members or methods), own first, then each inherited type's,
// following the documented "inherits" when the caller's chain runs out.
void Collect(const std::string &type, const char *kind, json &out, std::unordered_set<std::string> &seen)
{
	if (!g_doc["types"].contains(type)) return;
	const json &t = g_doc["types"][type];
	if (!t.contains(kind) || !t[kind].is_object()) return;
	for (auto &kv : t[kind].items())
		if (seen.insert(Lower(kv.key())).second) out[kv.key()] = kv.value();
}

} // namespace

void Load()
{
	g_doc = {{"types", json::object()}, {"objects", json::object()}, {"commands", json::object()}, {"events", json::object()}};
	g_sources.clear();
	g_problems.clear();
	LoadBuiltins();
	for (size_t i = 0; i < g_builtins.size(); i++) Add(g_builtins[i], "built-in");
	LoadPublished();
	LoadDirectory();
	g_loaded = true;
	g_reloadPending = false;
}

bool Loaded() { return g_loaded; }

void NoteExtensionsChanged()
{
	g_reloadPending = true;
	g_reloadAt = GetTickCount64() + 2000; // let the extension finish registering its objects
}

bool PulseReload()
{
	if (!g_loaded || !g_reloadPending || GetTickCount64() < g_reloadAt) return false;
	Load();
	return true;
}

json Globals()
{
	return {{"commands", g_doc["commands"]}, {"objects", g_doc["objects"]}, {"events", g_doc["events"]}};
}

json TypeDocs(const std::vector<std::string> &chain)
{
	json members = json::object(), methods = json::object();
	std::unordered_set<std::string> seenMembers, seenMethods, visited;
	std::vector<std::string> types;
	for (auto &t : chain) types.push_back(Lower(t));
	// Past the live chain, follow the documented inheritance.
	for (size_t i = 0; i < types.size() + 16 && i < 32; i++)
	{
		if (i >= types.size())
		{
			const std::string &last = types.back();
			std::string next = g_doc["types"].contains(last) ? Lower(g_doc["types"][last].value("inherits", "")) : "";
			if (next.empty()) break;
			types.push_back(next);
		}
		if (!visited.insert(types[i]).second) continue;
		Collect(types[i], "members", members, seenMembers);
		Collect(types[i], "methods", methods, seenMethods);
	}
	json d = {{"members", members}, {"methods", methods}};
	if (!types.empty() && g_doc["types"].contains(types[0]) && g_doc["types"][types[0]].contains("description"))
		d["description"] = g_doc["types"][types[0]]["description"];
	return d;
}

const json &Doc() { return g_doc; }

std::string Summary()
{
	std::string s = std::to_string(g_doc["types"].size()) + " types, " + std::to_string(g_doc["objects"].size()) + " objects, " +
	                std::to_string(g_doc["commands"].size()) + " commands, " + std::to_string(g_doc["events"].size()) +
	                " events; from: ";
	for (size_t i = 0; i < g_sources.size(); i++) s += (i ? ", " : "") + g_sources[i];
	if (g_sources.empty()) s += "nothing";
	s += "; directory " + g_dir;
	for (auto &p : g_problems) s += "\n  problem: " + p;
	return s;
}

} // namespace meta
