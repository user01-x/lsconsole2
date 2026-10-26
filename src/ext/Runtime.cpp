// Runtime -- installing LSConsole2 is copying one file: LSConsole2Host.exe, Ultralight's DLLs and
// resources, the console page and the licences travel inside this DLL as one resource (packed by
// tools/pack-runtime.ps1) and are unpacked into %LOCALAPPDATA%\LSConsole2\runtime\<id>\ the first
// time this version loads. Every later load finds them there.
//
// Sessions may load at the same moment, elevated or not: a mutex lets one unpack while the others
// wait. A ".complete" file, written last, says a version's folder is whole; one without it (a crash
// part-way) is unpacked again. Other versions' folders are removed once nothing runs from them.
#include <windows.h>
#include <compressapi.h>
#include "Runtime.h"
#include "../common/Protocol.h"
#include <cstdint>
#include <cstring>
#include <vector>
#pragma comment(lib, "cabinet.lib")

extern HMODULE g_hModule;

namespace runtime {
namespace {

std::wstring g_id;

struct Reader {
	const uint8_t *p, *end;
	bool Take(void *out, size_t n)
	{
		if ((size_t)(end - p) < n) return false;
		memcpy(out, p, n);
		p += n;
		return true;
	}
	const uint8_t *Skip(size_t n)
	{
		if ((size_t)(end - p) < n) return nullptr;
		const uint8_t *at = p;
		p += n;
		return at;
	}
};

bool WriteAll(const std::wstring &path, const void *data, size_t n)
{
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD done = 0;
	bool ok = (n == 0 || WriteFile(f, data, (DWORD)n, &done, nullptr)) && done == n;
	CloseHandle(f);
	return ok;
}

// Every folder on the way to `path` (a file path, or a folder path ending in \).
void MakeDirs(const std::wstring &path)
{
	for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1))
		CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
}

// Deletes a folder tree as far as it can (files in use stay). Links are removed, not followed.
void RemoveTree(const std::wstring &dir)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			std::wstring n = fd.cFileName, p = dir + L"\\" + n;
			if (n == L"." || n == L"..") continue;
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveTree(p);
			else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
			else DeleteFileW(p.c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

// Other versions' folders, unless a host still runs from one (its exe can't be deleted while it runs).
void RemoveOldVersions(const std::wstring &base)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do
	{
		std::wstring n = fd.cFileName, dir = base + L"\\" + n;
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n == L"." || n == L".." || n == g_id) continue;
		std::wstring exe = dir + L"\\LSConsole2Host.exe";
		if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(exe.c_str())) continue; // in use
		DeleteFileW((dir + L"\\.complete").c_str());
		RemoveTree(dir);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

// Unpacks every file of the pack into `dir`; "" or the reason it failed.
std::string UnpackFiles(Reader &rd, uint32_t count, const std::wstring &dir)
{
	DECOMPRESSOR_HANDLE dh = nullptr;
	if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &dh)) return "no decompressor (error " + std::to_string(GetLastError()) + ")";
	std::string error;
	std::vector<uint8_t> raw;
	for (uint32_t i = 0; i < count && error.empty(); i++)
	{
		uint16_t n = 0;
		uint32_t size = 0, packed = 0;
		const uint8_t *name = rd.Take(&n, 2) ? rd.Skip(n) : nullptr;
		const uint8_t *bytes = name && rd.Take(&size, 4) && rd.Take(&packed, 4) ? rd.Skip(packed) : nullptr;
		if (!bytes) { error = "the runtime pack is damaged"; break; }
		std::string rel((const char *)name, n);
		if (rel.empty() || rel.find("..") != std::string::npos || rel.find(':') != std::string::npos || rel[0] == '\\')
		{
			error = "the runtime pack names a bad path";
			break;
		}
		raw.resize(size);
		SIZE_T got = 0;
		if (size && (!Decompress(dh, bytes, packed, raw.data(), size, &got) || got != size))
		{
			error = "could not unpack " + rel;
			break;
		}
		std::wstring path = dir + L"\\" + lsc2::Widen(rel);
		MakeDirs(path);
		if (!WriteAll(path, raw.data(), size)) error = "could not write " + lsc2::Narrow(path) + " (error " + std::to_string(GetLastError()) + ")";
	}
	CloseDecompressor(dh);
	return error;
}

} // namespace

std::wstring Id() { return g_id; }

std::wstring Unpack(std::string &error)
{
	HRSRC r = FindResourceW(g_hModule, MAKEINTRESOURCEW(1), L"LSC2RUNTIME");
	HGLOBAL g = r ? LoadResource(g_hModule, r) : nullptr;
	const uint8_t *data = g ? (const uint8_t *)LockResource(g) : nullptr;
	if (!data) { error = "this build of LSConsole2.dll carries no runtime"; return L""; }
	Reader rd{data, data + SizeofResource(g_hModule, r)};
	char magic[8];
	uint32_t count = 0;
	uint64_t id = 0;
	if (!rd.Take(magic, 8) || memcmp(magic, "LSC2PAK1", 8) || !rd.Take(&count, 4) || !rd.Take(&id, 8))
	{
		error = "the runtime pack is damaged";
		return L"";
	}
	wchar_t hex[17];
	swprintf(hex, 17, L"%016llx", (unsigned long long)id);
	g_id = hex;

	wchar_t lad[MAX_PATH] = L"";
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH)) { error = "LOCALAPPDATA is not set"; return L""; }
	std::wstring base = std::wstring(lad) + L"\\LSConsole2\\runtime", dir = base + L"\\" + g_id;
	std::wstring done = dir + L"\\.complete";

	lsc2::UserSecurity sec; // elevated and non-elevated sessions share the mutex
	HANDLE m = CreateMutexW(sec.get(), FALSE, L"Local\\lsconsole2-unpack");
	DWORD w = m ? WaitForSingleObject(m, 120000) : WAIT_FAILED;
	bool owned = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
	if (GetFileAttributesW(done.c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		if (!owned) error = "another session is still unpacking the runtime";
		else
		{
			RemoveTree(dir); // whatever a crash part-way left
			MakeDirs(dir + L"\\");
			error = UnpackFiles(rd, count, dir);
			if (error.empty() && !WriteAll(done, "", 0)) error = "could not finish " + lsc2::Narrow(dir);
		}
	}
	if (error.empty() && owned) RemoveOldVersions(base);
	if (owned) ReleaseMutex(m);
	if (m) CloseHandle(m);
	return error.empty() ? dir + L"\\LSConsole2Host.exe" : L"";
}

} // namespace runtime
