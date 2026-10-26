#pragma once
// Metadata.cpp -- the merged console metadata. Main thread only.
#include <string>
#include <vector>
#include "../common/third_party/json.hpp"

using json = nlohmann::json;

namespace meta {
void Load();                  // built-ins, then published, then the directory
bool Loaded();
void NoteExtensionsChanged(); // an extension loaded or unloaded: reload shortly
bool PulseReload();           // does a pending reload; true if it did
json Globals();               // {commands, objects, events} for the page
// Members and methods documented for a type and the types it inherits (`chain`, the live
// inheritance, own type first; the documented "inherits" continues it).
json TypeDocs(const std::vector<std::string> &chain);
const json &Doc();
std::string Summary();
}
