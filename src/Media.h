// Image helpers (decode, encode, resize), a small LRU of decoded frames, and
// video → frame-sequence extraction (AVFoundation on macOS, ffmpeg elsewhere).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace comfy {

// 8-bit RGBA, rows top-down.
struct Rgba8 {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    bool empty() const { return w <= 0 || h <= 0; }
};

bool decodeImage(const std::string& bytes, Rgba8& out);
bool loadImageFile(const std::string& path, Rgba8& out);
// Resizes so the long edge is <= maxEdge and the short edge >= minEdge (when possible).
Rgba8 fitForUpload(const Rgba8& in, int maxEdge, int minEdge);
std::string encodeJpeg(const Rgba8& img, int quality);
std::string encodePng(const Rgba8& img);

// Shared decoded-frame cache keyed by file path (thread-safe, bounded by bytes).
std::shared_ptr<const Rgba8> cachedImage(const std::string& path);
void purgeImageCache();

struct VideoInfo {
    int frames = 0;
    double fps = 24;
    int w = 0, h = 0;
};

// Decodes every frame of `videoPath` to `<framesDir>/000001.jpg`, … .
// ffmpegHint may be empty; common install locations are searched.
bool extractFrames(const std::string& videoPath, const std::string& framesDir, const std::string& ffmpegHint,
                   VideoInfo& info, std::string& err, const std::atomic<bool>* cancel,
                   const std::function<void(int done, int total)>& progress);

std::string frameFileName(int index);  // 0-based index → "000001.jpg"

#ifdef __APPLE__
bool extractFramesAVFoundation(const std::string& videoPath, const std::string& framesDir, VideoInfo& info,
                               std::string& err, const std::atomic<bool>* cancel,
                               const std::function<void(int, int)>& progress);
#endif

}  // namespace comfy
