// Minimal Comfy Router client: submit to /v2/models/{provider}/{model}, wait, pull the media out.
//
// Generation takes ~10 s for images and ~2–3 min for Seedance 2.5, so callers run these on a
// worker thread. The queued API is used so no connection is held open for minutes.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace comfy {

constexpr const char* kRouterBase = "https://api.comfy.org";
constexpr const char* kModelNanoBanana2 = "vertexai/gemini-3.1-flash-image";
constexpr const char* kModelSeedance25 = "byteplus/dreamina-seedance-2-5-260628";
constexpr const char* kModelGptImage25Flare = "openai/gpt-image-2.5-flare";
constexpr const char* kModelGptImage25Sunburst = "openai/gpt-image-2.5-sunburst";

using CancelFlag = std::atomic<bool>;

struct HttpResponse {
    long status = 0;                             // 0 = transport failure
    std::map<std::string, std::string> headers;  // lower-cased names
    std::string body;
    std::string error;                           // transport error text
    bool cancelled = false;
};

HttpResponse httpPostJson(const std::string& url, const std::string& apiKey, const std::string& json,
                          const std::string& idempotencyKey, long timeoutSec, const CancelFlag* cancel);
HttpResponse httpGet(const std::string& url, long timeoutSec, const CancelFlag* cancel);

// An encoded image (PNG/JPEG/WebP bytes) sent to the model.
struct InputImage {
    std::string mime;
    std::string bytes;
};

struct ImageRequest {
    std::string prompt;
    std::string aspectRatio;  // "" = let the model decide
    std::string imageSize;    // "1K" | "2K" | "4K"; "" = model default
    std::vector<InputImage> images;
};

// OpenAI Images body (gpt-image-2.5-*). Adding images turns it into an edit.
// Native OpenAI edits only accept data: URIs or Comfy-signed URLs, so inputs go inline.
struct GptImageRequest {
    std::string model = kModelGptImage25Flare;
    std::string prompt;
    std::string size = "auto";         // "WxH" or "auto"
    std::string quality = "medium";    // low | medium | high | auto
    std::string background = "transparent";  // transparent | opaque | "" (model default)
    std::vector<InputImage> images;
};

struct VideoRequest {
    std::string prompt;
    std::string resolution = "720p";  // 480p | 720p | 1080p
    int duration = 5;                 // seconds
    std::string ratio;                // "" = omit (adaptive to the input image)
    bool generateAudio = true;
    long long seed = -1;              // -1 = random
    // role is "first_frame" | "last_frame" | "reference_image"
    std::vector<std::pair<std::string, InputImage>> images;
};

struct Media {
    std::string bytes;
    std::string mime;
    std::string ext;  // ".png", ".mp4", ...
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    long status = 0;
    std::string error;      // human-readable, suitable for the status line
    std::string errorType;  // X-Comfy-Error-Type
    std::string requestId;  // X-Comfy-Request-Id
    double credits = -1;    // X-Comfy-Credits-Used, -1 when absent
    std::string dropped;    // X-Comfy-Router-Dropped-Params (raw)
    std::vector<Media> media;
    double fps = 0;         // seedance reports framespersecond
    bool stillRunning = false;  // gave up waiting, but the request is still alive server-side
};

using ProgressFn = std::function<void(const std::string&)>;

struct RunHooks {
    ProgressFn progress;
    // Called once the Router has accepted the work, with its request id. Persist it:
    // collectRequest() can pick the result up later (after a dropped connection or a restart).
    std::function<void(const std::string& requestId)> onSubmitted;
};

std::string buildImageBody(const ImageRequest& req);
std::string buildVideoBody(const VideoRequest& req);
std::string buildGptImageBody(const GptImageRequest& req);

// Queued run: POST /v2/models/{model}/requests → poll …/requests/{id}/status → GET …/requests/{id}.
// Falls back to a synchronous POST /v2/models/{model} where the queue isn't offered.
// provider: "" = Router default leg, or "fal" / "wavespeed" / "runware".
Result runModel(const std::string& model, const std::string& provider, const std::string& body,
                const std::string& apiKey, const std::string& idempotencyKey, bool wantVideo,
                const CancelFlag* cancel, const RunHooks& hooks);
// Polls an already-submitted request to completion and downloads its media.
Result collectRequest(const std::string& model, const std::string& requestId, const std::string& apiKey,
                      bool wantVideo, const CancelFlag* cancel, const RunHooks& hooks);

HttpResponse httpAuthed(const std::string& method, const std::string& url, const std::string& apiKey, long timeoutSec,
                        const CancelFlag* cancel);

std::string base64Encode(const std::string& in);
std::string base64Decode(const std::string& in);
// Returns {mime, ext} from magic bytes; {"", ""} if unknown.
std::pair<std::string, std::string> sniffMedia(const std::string& bytes);

}  // namespace comfy
