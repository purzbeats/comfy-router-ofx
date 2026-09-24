// Getting generations into Resolve's Media Pool.
//
// OFX effects have no Media Pool API; only Resolve's scripting can import media. So the
// plugin installs a Lua script ("Comfy Router - Import Generated Media") into the user
// Scripts folder — it appears under Workspace → Scripts in Resolve and Resolve Studio —
// and the effect's Import button runs that same script through Resolve's bundled
// `fuscript`, which can reach a running Resolve only in Studio with external scripting
// set to Local. The script reports back through <configDir>/last_import.txt.
#pragma once

#include <string>

namespace comfy {

// e.g. ~/Library/Application Support/Blackmagic Design/DaVinci Resolve/Fusion/Scripts/Utility
std::string resolveScriptsDir();
// Full path of the installed import script.
std::string importScriptPath();
// Writes the embedded script if it is missing or out of date. Cheap to call repeatedly.
bool installImportScript(std::string* err = nullptr);

// Remembers an output folder so the import script scans it too.
void recordOutputDir(const std::string& dir);

// Starts the import in the background via fuscript. Returns false (with a reason) when
// fuscript can't be found; otherwise the outcome lands in last_import.txt.
bool startImportViaFuscript(std::string* why);

struct ImportResult {
    bool present = false;  // a result newer than `since` exists
    bool ok = false;
    int count = 0;
    std::string message;   // human-readable
};
// Reads last_import.txt if it was written at or after `since` (unix seconds).
ImportResult readImportResult(long long since);
bool importRunning();

}  // namespace comfy
