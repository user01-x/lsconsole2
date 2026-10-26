#pragma once
// The extension's three parts and what they call on each other. Only LSConsole2.cpp touches
// the InnerSpace API (pISInterface), and only from InnerSpace's thread; the link and the overlay
// run on threads of their own and hand anything for InnerSpace to PostToMain().
#include <windows.h>
#include <cstdint>
#include <string>
#include "../common/third_party/json.hpp"

using json = nlohmann::json;

// HostLink.cpp -- the pipe to LSConsole2Host.exe, (re)connecting and starting the host as needed.
namespace hostlink {
void Start(const std::wstring &hostExe = L""); // idempotent; empty: unpack the runtime this DLL carries
void Stop();
void Send(const json &j);                // any thread; dropped while disconnected
bool Connected();
void SetSize(uint32_t w, uint32_t h, double scale); // what "hello" asks for on (re)connect
}

// Overlay.cpp -- the layered console window, owned by the session's main window, on its own thread.
namespace overlay {
void Start(); // idempotent; creates the window (hidden) and the frame event
void Stop();
void Show(bool focusInput);
void Hide();
void Toggle();
void SetToggleKey(const std::string &combo); // the console bind's combo, e.g. "`"
void SetLayoutKey(const std::string &session); // where this session's console was put (layout.json)
void OnHostMessage(std::string &&msg); // from the link's I/O thread
void OnLinkUp();
void OnLinkDown();
std::string Status();
bool Shown(); // on screen right now (any thread)
}

// LSConsole2.cpp -- a message from the page for InnerSpace's thread (exec, ready, ...).
void PostToMain(json m);
