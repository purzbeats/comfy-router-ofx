#include "ResolveBridge.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "Settings.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace comfy {

namespace {

const char* kImportScript =
#include "ImportScript.inc"
    ;

const char* kScriptFileName = "Comfy Router - Import Generated Media.lua";

std::atomic<bool> gImportRunning{false};
std::mutex gDirsMutex;

std::string lastImportPath() { return joinPath(configDir(), "last_import.txt"); }
std::string outputDirsPath() { return joinPath(configDir(), "output_dirs.txt"); }

std::string exePath() {
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return buf;
#elif defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::string(buf, n);
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.string();
#endif
    return "";
}

std::string findFuscript() {
    std::vector<fs::path> candidates;
    fs::path exe = exePath();
    fs::path dir = exe.parent_path();  // the plugin runs inside Resolve's own process
#if defined(__APPLE__)
    candidates.push_back(dir / ".." / "Libraries" / "Fusion" / "fuscript");
    candidates.push_back("/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fuscript");
#elif defined(_WIN32)
    candidates.push_back(dir / "fuscript.exe");
    candidates.push_back("C:\\Program Files\\Blackmagic Design\\DaVinci Resolve\\fuscript.exe");
#else
    candidates.push_back(dir / ".." / "libs" / "Fusion" / "fuscript");
    candidates.push_back(dir / "fuscript");
    candidates.push_back("/opt/resolve/libs/Fusion/fuscript");
    candidates.push_back("/opt/resolve/bin/fuscript");
#endif
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return fs::weakly_canonical(c, ec).string();
    }
    return "";
}

// Runs a program with a hard timeout. Returns the exit code, or -1 on failure / timeout.
int runWithTimeout(const std::string& prog, const std::vector<std::string>& args, int timeoutSec) {
#ifdef _WIN32
    std::string cmd = "\"" + prog + "\"";
    for (const auto& a : args) cmd += " \"" + a + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    DWORD w = WaitForSingleObject(pi.hProcess, DWORD(timeoutSec) * 1000);
    DWORD code = DWORD(-1);
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return w == WAIT_OBJECT_0 ? int(code) : -1;
#else
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(prog.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, prog.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return -1;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    int status = 0;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0) return -1;
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

}  // namespace

std::string resolveScriptsDir() {
    std::string home;
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA"))
        return joinPath(a, "Blackmagic Design\\DaVinci Resolve\\Support\\Fusion\\Scripts\\Utility");
    home = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "C:\\";
    return joinPath(home, "AppData\\Roaming\\Blackmagic Design\\DaVinci Resolve\\Support\\Fusion\\Scripts\\Utility");
#else
    home = std::getenv("HOME") ? std::getenv("HOME") : "/tmp";
#if defined(__APPLE__)
    return joinPath(home, "Library/Application Support/Blackmagic Design/DaVinci Resolve/Fusion/Scripts/Utility");
#else
    return joinPath(home, ".local/share/DaVinciResolve/Fusion/Scripts/Utility");
#endif
#endif
}

std::string importScriptPath() { return joinPath(resolveScriptsDir(), kScriptFileName); }

bool installImportScript(std::string* err) {
    std::string path = importScriptPath(), current;
    if (readFile(path, current) && current == kImportScript) return true;
    if (!ensureDir(resolveScriptsDir(), err)) return false;
    return writeFileAtomic(path, kImportScript, err);
}

void recordOutputDir(const std::string& dir) {
    if (dir.empty()) return;
    std::lock_guard<std::mutex> g(gDirsMutex);
    std::string data;
    readFile(outputDirsPath(), data);
    std::istringstream in(data);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == dir) return;
    }
    ensureDir(configDir());
    std::ofstream out(outputDirsPath(), std::ios::app);
    out << dir << "\n";
}

bool startImportViaFuscript(std::string* why) {
    if (gImportRunning.load()) {
        if (why) *why = "An import is already running.";
        return false;
    }
    std::string err;
    if (!installImportScript(&err)) {
        if (why) *why = "Couldn't install the import script: " + err;
        return false;
    }
    std::string fuscript = findFuscript();
    if (fuscript.empty()) {
        if (why) *why = "Resolve's fuscript wasn't found.";
        return false;
    }
    gImportRunning = true;
    std::string script = importScriptPath();
    // Detached: Resolve's scripting server may need the main thread we'd otherwise block.
    std::thread([fuscript, script] {
        auto started = fs::file_time_type::clock::now() - std::chrono::seconds(1);
        int rc = runWithTimeout(fuscript, {"-l", "lua", script}, 45);
        // If the script never ran (free edition / external scripting off), say so.
        bool fresh = false;
        std::error_code ec;
        auto t = fs::last_write_time(lastImportPath(), ec);
        if (!ec) fresh = t >= started;
        if (!fresh) {
            std::string msg = "error unreachable";
            if (rc != 0) msg += " (fuscript exit " + std::to_string(rc) + ")";
            writeFileAtomic(lastImportPath(), msg, nullptr);
        }
        gImportRunning = false;
    }).detach();
    return true;
}

bool importRunning() { return gImportRunning.load(); }

ImportResult readImportResult(long long since) {
    ImportResult r;
    std::error_code ec;
    auto t = fs::last_write_time(lastImportPath(), ec);
    if (ec) return r;
    auto sys = std::chrono::time_point_cast<std::chrono::seconds>(
        t - decltype(t)::clock::now() + std::chrono::system_clock::now());
    if (sys.time_since_epoch().count() + 1 < since) return r;
    std::string data;
    if (!readFile(lastImportPath(), data)) return r;
    r.present = true;
    if (data.rfind("ok", 0) == 0) {
        r.ok = true;
        r.count = std::atoi(data.c_str() + 2);
        r.message = r.count == 0 ? "Media Pool is up to date — nothing new to import."
                                 : "Imported " + std::to_string(r.count) + " generation" + (r.count == 1 ? "" : "s") +
                                       " into the \"Comfy Router\" bin.";
    } else if (data.find("unreachable") != std::string::npos) {
        r.message = "Can't reach the Media Pool from the effect (needs Resolve Studio with External scripting = "
                    "Local). Use Workspace → Scripts → Comfy Router - Import Generated Media instead.";
    } else {
        r.message = "Import failed: " + (data.size() > 6 ? data.substr(6) : data);
    }
    return r;
}

}  // namespace comfy
