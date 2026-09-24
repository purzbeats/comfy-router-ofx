// Per-user settings for the Comfy Router plugin.
//
// The API key is deliberately kept out of the Resolve project: it is written to a
// per-user config file (0600 on POSIX) so that sharing a .drp never leaks a key.
#pragma once

#include <string>

namespace comfy {

// ~/Library/Application Support/ComfyRouterOFX (macOS), %APPDATA%\ComfyRouterOFX (Windows),
// $XDG_CONFIG_HOME/comfy-router-ofx or ~/.config/comfy-router-ofx (Linux).
std::string configDir();

// ~/Movies/ComfyRouter on macOS, ~/Videos/ComfyRouter elsewhere.
std::string defaultOutputDir();

// Saved key, else the COMFY_API_KEY environment variable, else "".
std::string loadApiKey();
bool saveApiKey(const std::string& key, std::string* err = nullptr);
void clearApiKey();

// Small per-user flags kept next to the key in config.json.
std::string getConfigString(const std::string& name);
void setConfigString(const std::string& name, const std::string& value);

// Removes the saved key and anything key-shaped (comfyui-…) from text that will be shown
// or saved (status line, overlays, error files). Nothing user-visible should skip this.
std::string redactSecrets(const std::string& text);

bool ensureDir(const std::string& path, std::string* err = nullptr);
bool writeFileAtomic(const std::string& path, const std::string& data, std::string* err = nullptr);
bool readFile(const std::string& path, std::string& out);
bool fileExists(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);
std::string expandUser(const std::string& path);

// Opens a folder in Finder / Explorer / the desktop file manager.
void revealInFileManager(const std::string& path);

}  // namespace comfy
