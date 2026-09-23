#include "Media.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <list>
#include <mutex>
#include <unordered_map>

#include "Settings.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

namespace fs = std::filesystem;

namespace comfy {

bool decodeImage(const std::string& bytes, Rgba8& out) {
    int w, h, n;
    unsigned char* data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), (int)bytes.size(), &w, &h, &n, 4);
    if (!data) return false;
    out.w = w;
    out.h = h;
    out.px.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

bool loadImageFile(const std::string& path, Rgba8& out) {
    std::string bytes;
    if (!readFile(path, bytes)) return false;
    return decodeImage(bytes, out);
}

Rgba8 fitForUpload(const Rgba8& in, int maxEdge, int minEdge) {
    double s = 1.0;
    int longE = std::max(in.w, in.h), shortE = std::min(in.w, in.h);
    if (longE > maxEdge) s = double(maxEdge) / longE;
    else if (shortE < minEdge) s = std::min(double(minEdge) / shortE, double(maxEdge) / longE);
    if (s == 1.0) return in;
    Rgba8 out;
    out.w = std::max(1, int(in.w * s + 0.5));
    out.h = std::max(1, int(in.h * s + 0.5));
    out.px.resize((size_t)out.w * out.h * 4);
    stbir_resize_uint8_srgb(in.px.data(), in.w, in.h, 0, out.px.data(), out.w, out.h, 0, STBIR_RGBA);
    return out;
}

static void appendBytes(void* ctx, void* data, int size) {
    static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), size);
}

std::string encodeJpeg(const Rgba8& img, int quality) {
    std::string out;
    stbi_write_jpg_to_func(appendBytes, &out, img.w, img.h, 4, img.px.data(), quality);
    return out;
}

std::string encodePng(const Rgba8& img) {
    std::string out;
    stbi_write_png_to_func(appendBytes, &out, img.w, img.h, 4, img.px.data(), img.w * 4);
    return out;
}

// ---------------------------------------------------------------- LRU cache

namespace {
struct Lru {
    std::mutex m;
    std::list<std::string> order;  // front = most recent
    struct Entry {
        std::shared_ptr<const Rgba8> img;
        std::list<std::string>::iterator it;
    };
    std::unordered_map<std::string, Entry> map;
    size_t bytes = 0;
    const size_t cap = size_t(1024) * 1024 * 1024;  // 1 GiB of decoded frames
};
Lru& lru() {
    static Lru l;
    return l;
}
}  // namespace

std::shared_ptr<const Rgba8> cachedImage(const std::string& path) {
    Lru& c = lru();
    {
        std::lock_guard<std::mutex> g(c.m);
        auto it = c.map.find(path);
        if (it != c.map.end()) {
            c.order.splice(c.order.begin(), c.order, it->second.it);
            return it->second.img;
        }
    }
    auto img = std::make_shared<Rgba8>();
    if (!loadImageFile(path, *img)) return nullptr;  // decode outside the lock
    std::lock_guard<std::mutex> g(c.m);
    auto it = c.map.find(path);
    if (it != c.map.end()) return it->second.img;  // another thread won the race
    c.order.push_front(path);
    c.map[path] = {img, c.order.begin()};
    c.bytes += img->px.size();
    while (c.bytes > c.cap && c.order.size() > 1) {
        auto& victim = c.order.back();
        c.bytes -= c.map[victim].img->px.size();
        c.map.erase(victim);
        c.order.pop_back();
    }
    return img;
}

void purgeImageCache() {
    Lru& c = lru();
    std::lock_guard<std::mutex> g(c.m);
    c.map.clear();
    c.order.clear();
    c.bytes = 0;
}

// ---------------------------------------------------------------- video

std::string frameFileName(int index) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%06d.jpg", index + 1);
    return buf;
}

static std::string findFfmpeg(const std::string& hint) {
    std::vector<std::string> candidates;
    if (!hint.empty()) candidates.push_back(expandUser(hint));
#ifdef _WIN32
    candidates.push_back("C:\\ffmpeg\\bin\\ffmpeg.exe");
    candidates.push_back("C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe");
#else
    // Resolve launched from the Dock doesn't inherit the shell PATH, so probe the usual spots.
    candidates.push_back("/opt/homebrew/bin/ffmpeg");
    candidates.push_back("/usr/local/bin/ffmpeg");
    candidates.push_back("/usr/bin/ffmpeg");
    candidates.push_back("/opt/local/bin/ffmpeg");
#endif
    for (const auto& c : candidates)
        if (fileExists(c)) return c;
    // Fall back to PATH lookup by bare name.
#ifdef _WIN32
    return "ffmpeg.exe";
#else
    return "ffmpeg";
#endif
}

static int countFrames(const std::string& dir) {
    int n = 0;
    while (fileExists(joinPath(dir, frameFileName(n)))) ++n;
    return n;
}

static bool extractFramesFfmpeg(const std::string& videoPath, const std::string& framesDir, const std::string& hint,
                                VideoInfo& info, std::string& err) {
    std::string ffmpeg = findFfmpeg(hint);
    std::string cmd = "\"" + ffmpeg + "\" -hide_banner -loglevel error -y -i \"" + videoPath + "\" -q:v 2 \"" +
                      joinPath(framesDir, "%06d.jpg") + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe strips the outer quotes
#endif
    int rc = std::system(cmd.c_str());
    int n = countFrames(framesDir);
    if (rc != 0 || n == 0) {
        err = "ffmpeg failed (" + ffmpeg + "). Install ffmpeg or set FFmpeg Path in Settings.";
        return false;
    }
    int w = 0, h = 0, comp = 0;
    stbi_info(joinPath(framesDir, frameFileName(0)).c_str(), &w, &h, &comp);
    info.frames = n;
    info.w = w;
    info.h = h;
    return true;
}

bool extractFrames(const std::string& videoPath, const std::string& framesDir, const std::string& ffmpegHint,
                   VideoInfo& info, std::string& err, const std::atomic<bool>* cancel,
                   const std::function<void(int, int)>& progress) {
    if (!ensureDir(framesDir, &err)) return false;
#ifdef __APPLE__
    std::string avErr;
    if (extractFramesAVFoundation(videoPath, framesDir, info, avErr, cancel, progress)) return true;
    if (cancel && cancel->load()) {
        err = "Cancelled";
        return false;
    }
    if (extractFramesFfmpeg(videoPath, framesDir, ffmpegHint, info, err)) return true;
    err = avErr + "; " + err;
    return false;
#else
    (void)cancel;
    if (progress) progress(0, 0);
    return extractFramesFfmpeg(videoPath, framesDir, ffmpegHint, info, err);
#endif
}

}  // namespace comfy
