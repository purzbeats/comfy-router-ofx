#include "Settings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "nlohmann/json.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace comfy {

static std::string homeDir() {
#ifdef _WIN32
    if (const char* p = std::getenv("USERPROFILE")) return p;
    return "C:\\";
#else
    if (const char* p = std::getenv("HOME")) return p;
    return "/tmp";
#endif
}

std::string expandUser(const std::string& path) {
    if (!path.empty() && path[0] == '~') return homeDir() + path.substr(1);
    return path;
}

std::string joinPath(const std::string& a, const std::string& b) {
    return (fs::path(a) / fs::path(b)).string();
}

std::string configDir() {
#if defined(__APPLE__)
    return joinPath(homeDir(), "Library/Application Support/ComfyRouterOFX");
#elif defined(_WIN32)
    if (const char* p = std::getenv("APPDATA")) return joinPath(p, "ComfyRouterOFX");
    return joinPath(homeDir(), "AppData\\Roaming\\ComfyRouterOFX");
#else
    if (const char* p = std::getenv("XDG_CONFIG_HOME")) return joinPath(p, "comfy-router-ofx");
    return joinPath(homeDir(), ".config/comfy-router-ofx");
#endif
}

std::string defaultOutputDir() {
#if defined(__APPLE__)
    return joinPath(homeDir(), "Movies/ComfyRouter");
#else
    return joinPath(homeDir(), "Videos/ComfyRouter");
#endif
}

static std::string configPath() { return joinPath(configDir(), "config.json"); }

static nlohmann::json loadConfig() {
    std::string data;
    if (!readFile(configPath(), data)) return nlohmann::json::object();
    auto j = nlohmann::json::parse(data, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

static bool saveConfig(const nlohmann::json& j, std::string* err) {
    if (!ensureDir(configDir(), err)) return false;
    if (!writeFileAtomic(configPath(), j.dump(2), err)) return false;
#ifndef _WIN32
    chmod(configPath().c_str(), 0600);
#endif
    return true;
}

std::string loadApiKey() {
    auto j = loadConfig();
    if (j.contains("api_key") && j["api_key"].is_string()) {
        std::string k = j["api_key"];
        if (!k.empty()) return k;
    }
    if (const char* e = std::getenv("COMFY_API_KEY")) return e;
    return "";
}

bool saveApiKey(const std::string& key, std::string* err) {
    auto j = loadConfig();
    j["api_key"] = key;
    return saveConfig(j, err);
}

void clearApiKey() {
    auto j = loadConfig();
    j.erase("api_key");
    saveConfig(j, nullptr);
}

std::string getConfigString(const std::string& name) {
    auto j = loadConfig();
    return j.contains(name) && j[name].is_string() ? j[name].get<std::string>() : "";
}

void setConfigString(const std::string& name, const std::string& value) {
    auto j = loadConfig();
    j[name] = value;
    saveConfig(j, nullptr);
}

std::string redactSecrets(const std::string& text) {
    std::string out = text;
    std::string key = loadApiKey();
    if (key.size() >= 8) {
        for (size_t pos; (pos = out.find(key)) != std::string::npos;) out.replace(pos, key.size(), "[API key]");
    }
    static const std::regex keyShaped(R"(comfyui-[A-Za-z0-9_\-]{6,})");
    return std::regex_replace(out, keyShaped, "[API key]");
}

bool ensureDir(const std::string& path, std::string* err) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec && !fs::is_directory(path)) {
        if (err) *err = "cannot create " + path + ": " + ec.message();
        return false;
    }
    return true;
}

bool writeFileAtomic(const std::string& path, const std::string& data, std::string* err) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (err) *err = "cannot write " + tmp;
            return false;
        }
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) {
            if (err) *err = "write failed for " + tmp;
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        if (err) *err = "cannot rename " + tmp + ": " + ec.message();
        return false;
    }
    return true;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

void revealInFileManager(const std::string& path) {
    ensureDir(path);
#if defined(__APPLE__)
    std::string cmd = "open \"" + path + "\" &";
    std::system(cmd.c_str());
#elif defined(_WIN32)
    ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}

}  // namespace comfy
