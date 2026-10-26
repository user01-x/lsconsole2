#pragma once
// Runtime.cpp -- the render host and Ultralight travel inside this DLL and are unpacked once per
// version into %LOCALAPPDATA%\LSConsole2\runtime\<id>\.
#include <string>

namespace runtime {
// Unpacks the runtime if this version isn't there yet and returns the host's path, or an empty
// string with the reason in `error`. Slow the first time (about a second): call it off the game's
// thread.
std::wstring Unpack(std::string &error);
// The version's id (16 hex digits), once Unpack has read the pack.
std::wstring Id();
}
