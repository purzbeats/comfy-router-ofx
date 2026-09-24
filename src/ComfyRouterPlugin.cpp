// Comfy Router — an OpenFX effect/generator for DaVinci Resolve that generates stills
// with Nano Banana 2 or GPT Image 2.5 (transparent PNGs for overlays) and video with
// Seedance 2.5 through the Comfy API Router.
//
// Flow: Generate (push button) → a background job POSTs to the Router, saves the
// PNG/MP4 to the output folder and (for video) decodes frames → render() draws the
// result fitted to the frame. The job id lives in a hidden param so projects reopen
// with their generations intact. While a job runs, render() overlays its progress.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

#include "Jobs.h"
#include "Media.h"
#include "ResolveBridge.h"
#include "RouterClient.h"
#include "Settings.h"
#include "stb/stb_easy_font.h"

#define kPluginName "Comfy Router"
#define kPluginGrouping "Comfy"
#define kPluginDescription \
    "Generate images (Nano Banana 2, GPT Image 2.5 with transparency) and video (Seedance 2.5) " \
    "through the Comfy API Router. " \
    "Enter your Comfy API key under Settings, write a prompt, and press Generate."
#define kPluginIdentifier "org.comfy.ComfyRouter"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

using namespace OFX;

namespace {

// ---------------------------------------------------------------- parameter names & options

const char* kMode = "mode";
const char* kPrompt = "prompt";
const char* kGenerate = "generate";
const char* kCancel = "cancel";
const char* kRefresh = "refreshViewer";
const char* kImportMedia = "importMedia";
const char* kStatus = "status";

const char* kNbAspect = "nbAspect";
const char* kNbSize = "nbSize";
const char* kNbInput = "nbInput";

const char* kGptVariant = "gptVariant";
const char* kGptSize = "gptSize";
const char* kGptQuality = "gptQuality";
const char* kGptBackground = "gptBackground";
const char* kGptInput = "gptInput";

const char* kSdResolution = "sdResolution";
const char* kSdDuration = "sdDuration";
const char* kSdRatio = "sdRatio";
const char* kSdAudio = "sdAudio";
const char* kSdSeed = "sdSeed";
const char* kSdInput = "sdInput";

const char* kRef1 = "refImage1";
const char* kRef2 = "refImage2";

const char* kFit = "fit";
const char* kLetterbox = "letterbox";
const char* kOpacity = "opacity";
const char* kSolidAlpha = "solidAlpha";
const char* kStartFrame = "startFrame";
const char* kAfterEnd = "afterEnd";

const char* kApiKey = "apiKey";
const char* kClearKey = "clearKey";
const char* kProvider = "provider";
const char* kOutDir = "outputFolder";
const char* kReveal = "revealOutput";
const char* kFfmpeg = "ffmpegPath";

const char* kJobId = "jobId";
const char* kJobDir = "jobDir";
const char* kRefreshTick = "refreshTick";

enum Mode { kModeImage = 0, kModeGpt, kModeVideo };

const std::vector<std::string> kNbAspectOpts = {"Match Timeline", "Auto (model decides)", "1:1", "16:9", "9:16", "4:3",
                                                "3:4", "3:2", "2:3", "5:4", "4:5", "21:9"};
const std::vector<std::string> kNbAspectRatios = {"1:1", "16:9", "9:16", "4:3", "3:4", "3:2", "2:3", "5:4", "4:5", "21:9"};
const std::vector<std::string> kNbSizeOpts = {"1K", "2K", "4K"};
enum NbInput { kNbInNone = 0, kNbInFrame, kNbInFrameRefs, kNbInRefs };
const std::vector<std::string> kNbInputOpts = {"None (text to image)", "Current Frame (edit it)",
                                               "Current Frame + Reference Images", "Reference Images Only"};

const std::vector<std::string> kGptVariantOpts = {"Flare", "Sunburst"};
const std::vector<std::string> kGptVariantModels = {comfy::kModelGptImage25Flare, comfy::kModelGptImage25Sunburst};
const std::vector<std::string> kGptSizeOpts = {"Match Timeline", "Auto", "1024×1024", "1536×1024", "1024×1536",
                                               "2048×2048", "2048×1152", "1152×2048"};
const std::vector<std::string> kGptSizes = {"", "auto", "1024x1024", "1536x1024", "1024x1536",
                                            "2048x2048", "2048x1152", "1152x2048"};
const std::vector<std::string> kGptQualityOpts = {"Low", "Medium", "High", "Auto"};
const std::vector<std::string> kGptQualities = {"low", "medium", "high", "auto"};
const std::vector<std::string> kGptBackgroundOpts = {"Transparent", "Opaque", "Auto"};
const std::vector<std::string> kGptBackgrounds = {"transparent", "opaque", ""};

const std::vector<std::string> kSdResOpts = {"480p", "720p", "1080p"};
const std::vector<std::string> kSdRatioOpts = {"Match Timeline", "Adaptive (follow input image)", "16:9", "9:16",
                                               "1:1", "4:3", "3:4", "21:9"};
const std::vector<std::string> kSdRatios = {"16:9", "9:16", "1:1", "4:3", "3:4", "21:9"};
enum SdInput { kSdInNone = 0, kSdInFirstFrame, kSdInFirstRef1, kSdInFirstFrameLastRef1, kSdInFirstRef1LastRef2,
               kSdInRefFrame, kSdInRefFiles };
const std::vector<std::string> kSdInputOpts = {
    "None (text to video)",
    "First Frame: Current Frame",
    "First Frame: Reference Image 1",
    "First: Current Frame · Last: Reference Image 1",
    "First: Reference Image 1 · Last: Reference Image 2",
    "Reference: Current Frame",
    "Reference: Reference Images 1–2",
};

enum Fit { kFitFit = 0, kFitFill, kFitStretch };
enum Letterbox { kLbSource = 0, kLbBlack, kLbTransparent };
enum AfterEnd { kAfterHold = 0, kAfterLoop, kAfterSource };

const std::vector<std::string> kProviderOpts = {"Default (Comfy routing)", "fal", "WaveSpeed", "Runware"};
const std::vector<std::string> kProviderValues = {"", "fal", "wavespeed", "runware"};

// Sentinel for "fill with the current source frame once we can fetch it".
const char* kPendingFrameMime = "pending/current-frame";

// ---------------------------------------------------------------- pixel helpers

template <typename T> struct PixMax { static constexpr float v = 1.f; };
template <> struct PixMax<unsigned char> { static constexpr float v = 255.f; };
template <> struct PixMax<unsigned short> { static constexpr float v = 65535.f; };

template <typename T> inline T toPix(float f) {
    if constexpr (std::is_floating_point<T>::value) {
        return f;
    } else {
        f = std::min(1.f, std::max(0.f, f));
        return T(f * PixMax<T>::v + 0.5f);
    }
}

template <typename T> inline float fromPix(T v) { return float(v) / PixMax<T>::v; }

template <typename F> void parallelRows(int y1, int y2, F&& fn) {
    int rows = y2 - y1;
    unsigned n = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    if (rows < 64) n = 1;
    if (n == 1) {
        for (int y = y1; y < y2; ++y) fn(y);
        return;
    }
    std::vector<std::thread> ts;
    int chunk = (rows + int(n) - 1) / int(n);
    for (unsigned i = 0; i < n; ++i) {
        int a = y1 + int(i) * chunk, b = std::min(y2, a + chunk);
        if (a >= b) break;
        ts.emplace_back([a, b, &fn] {
            for (int y = a; y < b; ++y) fn(y);
        });
    }
    for (auto& t : ts) t.join();
}

// Bilinear sample of an 8-bit top-down RGBA image at continuous pixel coords.
inline void sampleBilinear(const comfy::Rgba8& img, float u, float v, float out[4]) {
    u -= 0.5f;
    v -= 0.5f;
    int x0 = int(std::floor(u)), y0 = int(std::floor(v));
    float fx = u - x0, fy = v - y0;
    auto clampi = [](int a, int lo, int hi) { return a < lo ? lo : (a > hi ? hi : a); };
    int xa = clampi(x0, 0, img.w - 1), xb = clampi(x0 + 1, 0, img.w - 1);
    int ya = clampi(y0, 0, img.h - 1), yb = clampi(y0 + 1, 0, img.h - 1);
    const uint8_t* p00 = &img.px[(size_t(ya) * img.w + xa) * 4];
    const uint8_t* p10 = &img.px[(size_t(ya) * img.w + xb) * 4];
    const uint8_t* p01 = &img.px[(size_t(yb) * img.w + xa) * 4];
    const uint8_t* p11 = &img.px[(size_t(yb) * img.w + xb) * 4];
    for (int c = 0; c < 4; ++c) {
        float top = p00[c] + (p10[c] - p00[c]) * fx;
        float bot = p01[c] + (p11[c] - p01[c]) * fx;
        out[c] = (top + (bot - top) * fy) / 255.f;
    }
}

// ---------------------------------------------------------------- overlay text

struct TextMask {
    int w = 0, h = 0;
    std::vector<uint8_t> a;
};

// Rasterises stb_easy_font quads at an integer scale. Handles '\n'.
TextMask makeText(const std::string& text, int scale) {
    TextMask m;
    std::vector<char> buf(text.begin(), text.end());
    buf.push_back(0);
    int tw = stb_easy_font_width(buf.data());
    int th = stb_easy_font_height(buf.data());
    if (tw <= 0 || th <= 0) return m;
    m.w = tw * scale;
    m.h = th * scale;
    m.a.assign(size_t(m.w) * m.h, 0);
    std::vector<char> vbuf(text.size() * 270 + 1024);
    int quads = stb_easy_font_print(0, 0, buf.data(), nullptr, vbuf.data(), int(vbuf.size()));
    const float* v = reinterpret_cast<const float*>(vbuf.data());
    for (int q = 0; q < quads; ++q) {
        // 4 vertices × (x, y, z, rgba) = 16 bytes each
        const float* q0 = v + q * 16;
        float x0 = q0[0], y0 = q0[1], x1 = q0[8], y1 = q0[9];
        int ax = int(std::min(x0, x1) * scale), bx = int(std::max(x0, x1) * scale);
        int ay = int(std::min(y0, y1) * scale), by = int(std::max(y0, y1) * scale);
        for (int y = std::max(0, ay); y < std::min(m.h, by); ++y)
            for (int x = std::max(0, ax); x < std::min(m.w, bx); ++x) m.a[size_t(y) * m.w + x] = 255;
    }
    return m;
}

std::string wrapText(const std::string& s, size_t width) {
    std::string out, line, word;
    auto flushWord = [&] {
        if (word.empty()) return;
        if (!line.empty() && line.size() + 1 + word.size() > width) {
            out += line + "\n";
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flushWord();
        else if (c == '\n') { flushWord(); out += line + "\n"; line.clear(); }
        else word += c;
    }
    flushWord();
    return out + line;
}

// Everything the renderer draws on top of the frame, in top-down coordinates
// relative to the RoD's top-left corner.
struct Overlay {
    bool active = false;
    float dim = 0;  // 0..1 darkening of the whole frame
    int boxX = 0, boxY = 0, boxW = 0, boxH = 0;
    TextMask text;
    int textX = 0, textY = 0;
    float textColor[3] = {1, 1, 1};
    int barX = 0, barY = 0, barW = 0, barH = 0;
    float barFrac = -1;  // < 0 = no bar

    void apply(int tx, int ty, float px[4]) const {
        if (dim > 0) for (int c = 0; c < 3; ++c) px[c] *= (1 - dim);
        if (tx >= boxX && tx < boxX + boxW && ty >= boxY && ty < boxY + boxH) {
            for (int c = 0; c < 3; ++c) px[c] = px[c] * 0.25f;
            px[3] = std::max(px[3], 0.85f);
        }
        if (barFrac >= 0 && tx >= barX && tx < barX + barW && ty >= barY && ty < barY + barH) {
            bool filled = tx < barX + int(barW * barFrac);
            const float on[3] = {0.95f, 0.87f, 0.2f}, off[3] = {0.25f, 0.25f, 0.25f};
            for (int c = 0; c < 3; ++c) px[c] = filled ? on[c] : off[c];
            px[3] = 1;
        }
        int lx = tx - textX, ly = ty - textY;
        if (lx >= 0 && ly >= 0 && lx < text.w && ly < text.h && text.a[size_t(ly) * text.w + lx]) {
            for (int c = 0; c < 3; ++c) px[c] = textColor[c];
            px[3] = 1;
        }
    }
};

Overlay buildOverlay(const std::string& message, int frameW, int frameH, float barFrac, bool isError, float dim) {
    Overlay o;
    o.active = true;
    o.dim = dim;
    int scale = std::max(1, frameH / 300);
    int pad = 6 * scale;
    size_t wrap = size_t(std::max(20, (frameW - 4 * pad) / (6 * scale)));
    o.text = makeText(wrapText(message, wrap), scale);
    o.barFrac = barFrac;
    int barH = barFrac >= 0 ? 4 * scale : 0;
    o.boxW = std::min(frameW, o.text.w + 2 * pad);
    if (barFrac >= 0) o.boxW = std::max(o.boxW, std::min(frameW, frameW / 3));
    o.boxH = o.text.h + 2 * pad + (barH ? barH + pad : 0);
    o.boxX = pad;
    o.boxY = std::max(0, frameH - o.boxH - pad);
    o.textX = o.boxX + pad;
    o.textY = o.boxY + pad;
    if (barFrac >= 0) {
        o.barX = o.boxX + pad;
        o.barW = o.boxW - 2 * pad;
        o.barH = barH;
        o.barY = o.textY + o.text.h + pad;
    }
    if (isError) { o.textColor[0] = 1.f; o.textColor[1] = 0.45f; o.textColor[2] = 0.4f; }
    return o;
}

std::string nearestRatio(double r, const std::vector<std::string>& ratios) {
    std::string best;
    double bestD = 1e9;
    for (const auto& s : ratios) {
        auto c = s.find(':');
        double v = std::stod(s.substr(0, c)) / std::stod(s.substr(c + 1));
        double d = std::abs(std::log(v / r));
        if (d < bestD) { bestD = d; best = s; }
    }
    return best;
}

std::string fmtSeconds(double s) {
    int t = int(s + 0.5);
    char buf[32];
    if (t >= 60) std::snprintf(buf, sizeof buf, "%dm %02ds", t / 60, t % 60);
    else std::snprintf(buf, sizeof buf, "%ds", t);
    return buf;
}

}  // namespace

// ================================================================= the effect

class ComfyRouterPlugin : public ImageEffect {
public:
    explicit ComfyRouterPlugin(OfxImageEffectHandle handle) : ImageEffect(handle) {
        dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
        if (getContext() != eContextGenerator) srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
        mode_ = fetchChoiceParam(kMode);
        prompt_ = fetchStringParam(kPrompt);
        status_ = fetchStringParam(kStatus);
        nbAspect_ = fetchChoiceParam(kNbAspect);
        nbSize_ = fetchChoiceParam(kNbSize);
        nbInput_ = fetchChoiceParam(kNbInput);
        gptVariant_ = fetchChoiceParam(kGptVariant);
        gptSize_ = fetchChoiceParam(kGptSize);
        gptQuality_ = fetchChoiceParam(kGptQuality);
        gptBackground_ = fetchChoiceParam(kGptBackground);
        gptInput_ = fetchChoiceParam(kGptInput);
        sdRes_ = fetchChoiceParam(kSdResolution);
        sdDuration_ = fetchIntParam(kSdDuration);
        sdRatio_ = fetchChoiceParam(kSdRatio);
        sdAudio_ = fetchBooleanParam(kSdAudio);
        sdSeed_ = fetchIntParam(kSdSeed);
        sdInput_ = fetchChoiceParam(kSdInput);
        ref1_ = fetchStringParam(kRef1);
        ref2_ = fetchStringParam(kRef2);
        fit_ = fetchChoiceParam(kFit);
        letterbox_ = fetchChoiceParam(kLetterbox);
        opacity_ = fetchDoubleParam(kOpacity);
        solidAlpha_ = fetchBooleanParam(kSolidAlpha);
        startFrame_ = fetchIntParam(kStartFrame);
        afterEnd_ = fetchChoiceParam(kAfterEnd);
        apiKey_ = fetchStringParam(kApiKey);
        provider_ = fetchChoiceParam(kProvider);
        outDir_ = fetchStringParam(kOutDir);
        ffmpeg_ = fetchStringParam(kFfmpeg);
        jobId_ = fetchStringParam(kJobId);
        jobDir_ = fetchStringParam(kJobDir);
        refreshTick_ = fetchIntParam(kRefreshTick);
        updateEnabled();
        // Reopened project with a generation still in flight: keep collecting it.
        std::string id, dir, ff;
        jobId_->getValue(id);
        jobDir_->getValue(dir);
        ffmpeg_->getValue(ff);
        if (!id.empty()) comfy::resumeJob(dir, id, comfy::loadApiKey(), ff);
    }

    void render(const RenderArguments& args) override;
    bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime) override;
    void changedParam(const InstanceChangedArgs& args, const std::string& name) override;
    void changedClip(const InstanceChangedArgs& args, const std::string& name) override { (void)args; (void)name; updateEnabled(); }
    bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) override;
    void getClipPreferences(ClipPreferencesSetter& prefs) override {
        prefs.setOutputFrameVarying(true);
        prefs.setOutputPremultiplication(eImagePreMultiplied);  // GPT Image results carry alpha
    }

private:
    bool hasSource() const { return srcClip_ && srcClip_->isConnected(); }
    std::string outputDir() {
        std::string d;
        outDir_->getValue(d);
        return d.empty() ? comfy::defaultOutputDir() : comfy::expandUser(d);
    }
    double timelineAspect() const {
        OfxPointD ext = getProjectExtent();
        double par = getProjectPixelAspectRatio();
        return ext.y > 0 ? ext.x * (par > 0 ? par : 1) / ext.y : 16.0 / 9.0;
    }
    void setStatus(const std::string& s) {
        std::string cur;
        status_->getValue(cur);
        if (cur != s) status_->setValue(s);
    }
    void updateEnabled();
    void syncStatus();
    void onGenerate(const InstanceChangedArgs& args);
    void onApiKey();
    bool readFileInput(const std::string& path, int minEdge, comfy::InputImage& out, std::string& err);
    bool frameFromImage(Image* img, int minEdge, comfy::InputImage& out);
    void launch(comfy::JobSpec spec);

    template <typename T> void renderT(const RenderArguments& args, Image* dst, Image* src);

    Clip* dstClip_ = nullptr;
    Clip* srcClip_ = nullptr;
    ChoiceParam *mode_, *nbAspect_, *nbSize_, *nbInput_, *gptVariant_, *gptSize_, *gptQuality_, *gptBackground_,
        *gptInput_, *sdRes_, *sdRatio_, *sdInput_, *fit_, *letterbox_, *afterEnd_,
        *provider_;
    StringParam *prompt_, *status_, *ref1_, *ref2_, *apiKey_, *outDir_, *ffmpeg_, *jobId_, *jobDir_;
    IntParam *sdDuration_, *sdSeed_, *startFrame_, *refreshTick_;
    BooleanParam *sdAudio_, *solidAlpha_;
    DoubleParam* opacity_;

    long long importClickedAt_ = 0;  // unix seconds of the last Import press, 0 = none pending

    // A job waiting for render() to supply the current frame (hosts that refuse
    // clipGetImage during instanceChanged).
    std::mutex pendingMutex_;
    std::optional<comfy::JobSpec> pending_;
    int pendingMinEdge_ = 0;
};

void ComfyRouterPlugin::updateEnabled() {
    int mode = 0;
    mode_->getValue(mode);
    for (Param* p : std::initializer_list<Param*>{nbAspect_, nbSize_, nbInput_}) p->setEnabled(mode == kModeImage);
    for (Param* p : std::initializer_list<Param*>{gptVariant_, gptSize_, gptQuality_, gptBackground_, gptInput_})
        p->setEnabled(mode == kModeGpt);
    for (Param* p : std::initializer_list<Param*>{sdRes_, sdDuration_, sdRatio_, sdAudio_, sdSeed_, sdInput_, startFrame_, afterEnd_})
        p->setEnabled(mode == kModeVideo);
}

void ComfyRouterPlugin::syncStatus() {
    if (importClickedAt_ > 0) {
        if (comfy::importRunning()) {
            setStatus("Importing into the Media Pool…");
            return;
        }
        auto r = comfy::readImportResult(importClickedAt_);
        if (r.present) {
            importClickedAt_ = 0;
            setStatus(r.message);
            return;
        }
    }
    std::string id, dir;
    jobId_->getValue(id);
    jobDir_->getValue(dir);
    std::string key = comfy::loadApiKey();
    std::string keyNote = key.empty() ? "No API key — add it under Settings." : "";
    if (id.empty()) {
        setStatus(keyNote.empty() ? "Ready · API key " + comfy::maskKey(key) : keyNote);
        return;
    }
    if (auto st = comfy::jobStatus(id)) {
        switch (st->state) {
            case comfy::JobState::Running: setStatus(st->message + " (" + fmtSeconds(st->elapsedSec) + ")"); return;
            case comfy::JobState::Cancelled: setStatus(st->message); return;
            case comfy::JobState::Failed: setStatus("Error: " + st->message); return;
            case comfy::JobState::Done: {
                // The first generation on this computer also says how to get results into the Media Pool.
                std::string hinted = comfy::getConfigString("import_hint_job");
                if (hinted.empty()) comfy::setConfigString("import_hint_job", hinted = id);
                setStatus(hinted == id ? st->message + " · Tip: add it to the Media Pool with Workspace → Scripts → "
                                                       "Comfy Router - Import Generated Media"
                                       : st->message);
                return;
            }
        }
    }
    if (auto meta = comfy::findResult(dir, id)) {
        if (!meta->error.empty()) setStatus("Error: " + meta->error);
        else setStatus(std::string(meta->kind == comfy::JobKind::Video ? "Video" : "Image") + " · " + meta->file);
        return;
    }
    setStatus("Result " + id + " not found in " + dir);
}

void ComfyRouterPlugin::onApiKey() {
    std::string key;
    apiKey_->getValue(key);
    // Trim whitespace from pastes.
    key.erase(0, key.find_first_not_of(" \t\r\n"));
    key.erase(key.find_last_not_of(" \t\r\n") + 1);
    if (key.empty()) return;  // our own clear below re-enters here
    std::string err;
    if (comfy::saveApiKey(key, &err)) {
        apiKey_->setValue("");  // never keep the key in the project file
        setStatus("API key saved on this computer (" + comfy::maskKey(key) + ")");
    } else {
        setStatus("Could not save API key: " + err);
    }
}

bool ComfyRouterPlugin::readFileInput(const std::string& rawPath, int minEdge, comfy::InputImage& out, std::string& err) {
    std::string path = comfy::expandUser(rawPath);
    if (path.empty()) {
        err = "Reference image path is empty";
        return false;
    }
    comfy::Rgba8 img;
    if (!comfy::loadImageFile(path, img)) {
        err = "Cannot read image: " + path;
        return false;
    }
    img = comfy::fitForUpload(img, 2048, minEdge);
    out.mime = "image/jpeg";
    out.bytes = comfy::encodeJpeg(img, 95);
    return true;
}

// Converts an OFX image (bottom-up rows, any depth) into an upload-ready JPEG.
bool ComfyRouterPlugin::frameFromImage(Image* img, int minEdge, comfy::InputImage& out) {
    if (!img) return false;
    OfxRectI b = img->getBounds();
    int w = b.x2 - b.x1, h = b.y2 - b.y1;
    if (w <= 0 || h <= 0) return false;
    PixelComponentEnum comps = img->getPixelComponents();
    int nc = comps == ePixelComponentRGBA ? 4 : comps == ePixelComponentRGB ? 3 : comps == ePixelComponentAlpha ? 1 : 0;
    if (!nc) return false;
    comfy::Rgba8 rgba;
    rgba.w = w;
    rgba.h = h;
    rgba.px.resize(size_t(w) * h * 4);
    BitDepthEnum depth = img->getPixelDepth();
    for (int y = 0; y < h; ++y) {
        const char* row = static_cast<const char*>(img->getPixelData()) + size_t(y) * img->getRowBytes();
        uint8_t* d = &rgba.px[size_t(h - 1 - y) * w * 4];
        for (int x = 0; x < w; ++x, d += 4) {
            float c[4] = {0, 0, 0, 1};
            for (int k = 0; k < nc; ++k) {
                float v;
                if (depth == eBitDepthUByte) v = fromPix(reinterpret_cast<const unsigned char*>(row)[x * nc + k]);
                else if (depth == eBitDepthUShort) v = fromPix(reinterpret_cast<const unsigned short*>(row)[x * nc + k]);
                else v = reinterpret_cast<const float*>(row)[x * nc + k];
                c[nc == 1 ? 3 : k] = v;
            }
            if (nc == 1) c[0] = c[1] = c[2] = c[3];
            for (int k = 0; k < 3; ++k) d[k] = uint8_t(std::min(1.f, std::max(0.f, c[k])) * 255.f + 0.5f);
            d[3] = 255;
        }
    }
    rgba = comfy::fitForUpload(rgba, 2048, minEdge);
    out.mime = "image/jpeg";
    out.bytes = comfy::encodeJpeg(rgba, 95);
    return true;
}

void ComfyRouterPlugin::launch(comfy::JobSpec spec) {
    comfy::startJob(std::move(spec));
}

void ComfyRouterPlugin::onGenerate(const InstanceChangedArgs& args) {
    comfy::JobSpec spec;
    spec.apiKey = comfy::loadApiKey();
    if (spec.apiKey.empty()) {
        setStatus("No API key — paste your Comfy API key into Settings → API Key.");
        sendMessage(Message::eMessageError, "", "Comfy Router: paste your Comfy API key into Settings → API Key first.");
        return;
    }
    std::string prompt;
    prompt_->getValue(prompt);
    if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) {
        setStatus("Write a prompt first.");
        sendMessage(Message::eMessageError, "", "Comfy Router: write a prompt first.");
        return;
    }
    int mode = 0, providerIdx = 0;
    mode_->getValue(mode);
    provider_->getValue(providerIdx);
    spec.provider = kProviderValues[std::clamp(providerIdx, 0, int(kProviderValues.size()) - 1)];
    ffmpeg_->getValue(spec.ffmpegHint);
    spec.outDir = outputDir();
    spec.id = comfy::newJobId();
    comfy::recordOutputDir(spec.outDir);  // so Import Generated Media scans this folder

    std::string r1, r2, err;
    ref1_->getValue(r1);
    ref2_->getValue(r2);
    bool needFrame = false;
    int minEdge = 0;
    auto frameSlot = [&]() {
        needFrame = true;
        return comfy::InputImage{kPendingFrameMime, ""};
    };
    auto fileSlot = [&](const std::string& path, comfy::InputImage& out) {
        if (!readFileInput(path, minEdge, out, err)) return false;
        return true;
    };

    if (mode == kModeImage) {
        spec.kind = comfy::JobKind::Image;
        spec.image.prompt = prompt;
        int a = 0, s = 0, in = 0;
        nbAspect_->getValue(a);
        nbSize_->getValue(s);
        nbInput_->getValue(in);
        spec.image.imageSize = kNbSizeOpts[std::clamp(s, 0, int(kNbSizeOpts.size()) - 1)];
        if (a == 0) spec.image.aspectRatio = nearestRatio(timelineAspect(), kNbAspectRatios);
        else if (a >= 2) spec.image.aspectRatio = kNbAspectOpts[a];
        if (in == kNbInFrame || in == kNbInFrameRefs) spec.image.images.push_back(frameSlot());
        if (in == kNbInFrameRefs || in == kNbInRefs) {
            for (const auto& p : {r1, r2}) {
                if (p.empty()) continue;
                comfy::InputImage im;
                if (!fileSlot(p, im)) { setStatus(err); return; }
                spec.image.images.push_back(std::move(im));
            }
            if (in == kNbInRefs && spec.image.images.empty()) {
                setStatus("Set Reference Image 1 and/or 2 first.");
                return;
            }
        }
    } else if (mode == kModeGpt) {
        spec.kind = comfy::JobKind::Image;
        spec.imageModel = comfy::ImageModel::GptImage25;
        auto& g = spec.gpt;
        g.prompt = prompt;
        int v = 0, sz = 0, q = 1, bg = 0, in = 0;
        gptVariant_->getValue(v);
        gptSize_->getValue(sz);
        gptQuality_->getValue(q);
        gptBackground_->getValue(bg);
        gptInput_->getValue(in);
        g.model = kGptVariantModels[std::clamp(v, 0, int(kGptVariantModels.size()) - 1)];
        g.quality = kGptQualities[std::clamp(q, 0, int(kGptQualities.size()) - 1)];
        g.background = kGptBackgrounds[std::clamp(bg, 0, int(kGptBackgrounds.size()) - 1)];
        sz = std::clamp(sz, 0, int(kGptSizes.size()) - 1);
        if (sz == 0) {
            // Timeline aspect at a 1536 px long edge, edges on a 16 px grid.
            double r = timelineAspect();
            int longE = 1536, shortE = int(std::lround(longE / std::max(r, 1.0 / r) / 16.0)) * 16;
            shortE = std::max(512, shortE);
            g.size = r >= 1 ? std::to_string(longE) + "x" + std::to_string(shortE)
                            : std::to_string(shortE) + "x" + std::to_string(longE);
        } else {
            g.size = kGptSizes[sz];
        }
        if (in == kNbInFrame || in == kNbInFrameRefs) g.images.push_back(frameSlot());
        if (in == kNbInFrameRefs || in == kNbInRefs) {
            for (const auto& p : {r1, r2}) {
                if (p.empty()) continue;
                comfy::InputImage im;
                if (!fileSlot(p, im)) { setStatus(err); return; }
                g.images.push_back(std::move(im));
            }
            if (in == kNbInRefs && g.images.empty()) {
                setStatus("Set Reference Image 1 and/or 2 first.");
                return;
            }
        }
    } else {
        spec.kind = comfy::JobKind::Video;
        minEdge = 300;  // Seedance (native BytePlus) rejects inputs under 300 px
        spec.video.prompt = prompt;
        int res = 1, ratio = 0, in = 0, dur = 5, seed = -1;
        bool audio = true;
        sdRes_->getValue(res);
        sdRatio_->getValue(ratio);
        sdInput_->getValue(in);
        sdDuration_->getValue(dur);
        sdSeed_->getValue(seed);
        sdAudio_->getValue(audio);
        spec.video.resolution = kSdResOpts[std::clamp(res, 0, int(kSdResOpts.size()) - 1)];
        spec.video.duration = dur;
        spec.video.generateAudio = audio;
        spec.video.seed = seed;
        if (ratio == 0) spec.video.ratio = nearestRatio(timelineAspect(), kSdRatios);
        else if (ratio == 1) spec.video.ratio = "adaptive";
        else spec.video.ratio = kSdRatioOpts[ratio];
        auto addFile = [&](const std::string& role, const std::string& path, const char* label) {
            if (path.empty()) { err = std::string("Set ") + label + " first."; return false; }
            comfy::InputImage im;
            if (!fileSlot(path, im)) return false;
            spec.video.images.emplace_back(role, std::move(im));
            return true;
        };
        bool ok = true;
        switch (in) {
            case kSdInFirstFrame: spec.video.images.emplace_back("first_frame", frameSlot()); break;
            case kSdInFirstRef1: ok = addFile("first_frame", r1, "Reference Image 1"); break;
            case kSdInFirstFrameLastRef1:
                spec.video.images.emplace_back("first_frame", frameSlot());
                ok = addFile("last_frame", r1, "Reference Image 1");
                break;
            case kSdInFirstRef1LastRef2:
                ok = addFile("first_frame", r1, "Reference Image 1") && addFile("last_frame", r2, "Reference Image 2");
                break;
            case kSdInRefFrame: spec.video.images.emplace_back("reference_image", frameSlot()); break;
            case kSdInRefFiles:
                if (!r1.empty()) ok = addFile("reference_image", r1, "Reference Image 1");
                if (ok && !r2.empty()) ok = addFile("reference_image", r2, "Reference Image 2");
                if (ok && r1.empty() && r2.empty()) { ok = false; err = "Set Reference Image 1 and/or 2 first."; }
                break;
            default: break;
        }
        if (!ok) { setStatus(err); return; }
        // Match the timeline only for text-to-video; with a first frame the input decides.
        if (ratio == 0 && (in == kSdInFirstFrame || in == kSdInFirstRef1 || in == kSdInFirstFrameLastRef1 || in == kSdInFirstRef1LastRef2))
            spec.video.ratio = "adaptive";
    }

    if (needFrame && !hasSource()) {
        setStatus("No source clip to read the current frame from.");
        return;
    }

    // Try to grab the frame now; otherwise render() fills it in.
    bool deferred = false;
    if (needFrame) {
        comfy::InputImage frame;
        std::unique_ptr<Image> img;
        try { img.reset(srcClip_->fetchImage(args.time)); } catch (...) {}
        if (img && frameFromImage(img.get(), minEdge, frame)) {
            auto fill = [&](comfy::InputImage& slot) { if (slot.mime == kPendingFrameMime) slot = frame; };
            for (auto& im : spec.image.images) fill(im);
            for (auto& im : spec.gpt.images) fill(im);
            for (auto& [role, im] : spec.video.images) fill(im);
        } else {
            deferred = true;
        }
    }

    std::string id = spec.id, dir = spec.outDir;
    if (mode == kModeVideo) startFrame_->setValue(int(std::floor(args.time + 0.5)));
    if (deferred) {
        std::lock_guard<std::mutex> g(pendingMutex_);
        pending_ = std::move(spec);
        pendingMinEdge_ = minEdge;
        setStatus("Reading current frame…");
    } else {
        launch(std::move(spec));
        setStatus(mode == kModeVideo ? "Generating video with Seedance 2.5…"
                  : mode == kModeGpt ? "Generating image with GPT Image 2.5…"
                                     : "Generating image with Nano Banana 2…");
    }
    // Setting these triggers a re-render, which shows progress (and fills a deferred frame).
    jobDir_->setValue(dir);
    jobId_->setValue(id);
}

void ComfyRouterPlugin::changedParam(const InstanceChangedArgs& args, const std::string& name) {
    if (name == kStatus || name == kJobId || name == kJobDir || name == kRefreshTick) return;
    if (name == kGenerate) {
        onGenerate(args);
        return;
    }
    if (name == kApiKey) {
        onApiKey();
        return;
    }
    if (name == kCancel) {
        std::string id;
        jobId_->getValue(id);
        {
            std::lock_guard<std::mutex> g(pendingMutex_);
            pending_.reset();
        }
        comfy::cancelJob(id);
        setStatus("Cancelling…");
        return;
    }
    if (name == kClearKey) {
        comfy::clearApiKey();
        setStatus(comfy::loadApiKey().empty() ? "Saved API key removed." : "Saved key removed; COMFY_API_KEY env var still set.");
        return;
    }
    if (name == kImportMedia) {
        comfy::recordOutputDir(outputDir());
        std::string why;
        importClickedAt_ = (long long)std::time(nullptr);
        if (comfy::startImportViaFuscript(&why)) {
            setStatus("Importing into the Media Pool… (press Refresh Viewer to update this line)");
        } else {
            importClickedAt_ = 0;
            comfy::installImportScript();
            setStatus(why + " Use Workspace → Scripts → Comfy Router - Import Generated Media "
                      "(restart Resolve once if it isn't listed yet).");
        }
        return;
    }
    if (name == kReveal) {
        comfy::revealInFileManager(outputDir());
        return;
    }
    if (name == kRefresh) {
        int t = 0;
        refreshTick_->getValue(t);
        refreshTick_->setValue(t + 1);  // forces the host to drop cached frames
        syncStatus();
        return;
    }
    if (name == kMode) updateEnabled();
    syncStatus();
}

bool ComfyRouterPlugin::isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime) {
    if (!hasSource()) return false;
    std::string id;
    jobId_->getValueAtTime(args.time, id);
    if (!id.empty()) return false;
    identityClip = srcClip_;
    identityTime = args.time;
    return true;
}

bool ComfyRouterPlugin::getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) {
    (void)args;
    if (hasSource()) return false;  // default: the source's RoD
    OfxPointD off = getProjectOffset(), ext = getProjectExtent();
    rod.x1 = off.x;
    rod.y1 = off.y;
    rod.x2 = off.x + ext.x;
    rod.y2 = off.y + ext.y;
    return true;
}

void ComfyRouterPlugin::render(const RenderArguments& args) {
    std::unique_ptr<Image> dst(dstClip_->fetchImage(args.time));
    if (!dst) throwSuiteStatusException(kOfxStatFailed);
    std::unique_ptr<Image> src;
    if (hasSource()) src.reset(srcClip_->fetchImage(args.time));
    if (src && (src->getPixelDepth() != dst->getPixelDepth() || src->getPixelComponents() != dst->getPixelComponents()))
        throwSuiteStatusException(kOfxStatErrImageFormat);

    // Deferred launch: fill in the current frame from this render.
    {
        std::lock_guard<std::mutex> g(pendingMutex_);
        if (pending_) {
            comfy::InputImage frame;
            if (src && frameFromImage(src.get(), pendingMinEdge_, frame)) {
                auto fill = [&](comfy::InputImage& slot) { if (slot.mime == kPendingFrameMime) slot = frame; };
                for (auto& im : pending_->image.images) fill(im);
                for (auto& im : pending_->gpt.images) fill(im);
                for (auto& [role, im] : pending_->video.images) fill(im);
                launch(std::move(*pending_));
            }
            pending_.reset();
        }
    }

    switch (dst->getPixelDepth()) {
        case eBitDepthUByte: renderT<unsigned char>(args, dst.get(), src.get()); break;
        case eBitDepthUShort: renderT<unsigned short>(args, dst.get(), src.get()); break;
        case eBitDepthFloat: renderT<float>(args, dst.get(), src.get()); break;
        default: throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

template <typename T>
void ComfyRouterPlugin::renderT(const RenderArguments& args, Image* dst, Image* src) {
    const double t = args.time;
    std::string id, dir;
    jobId_->getValueAtTime(t, id);
    jobDir_->getValueAtTime(t, dir);
    int fitMode = 0, lbMode = 0, afterEnd = 0, startFrame = 0;
    double opacity = 1;
    fit_->getValueAtTime(t, fitMode);
    letterbox_->getValueAtTime(t, lbMode);
    afterEnd_->getValueAtTime(t, afterEnd);
    startFrame_->getValueAtTime(t, startFrame);
    opacity_->getValueAtTime(t, opacity);
    bool solidAlpha = true;
    solidAlpha_->getValueAtTime(t, solidAlpha);
    // GPT Image tops out around alpha 253/255 inside "opaque" areas; stretch the top 2% to solid.
    const float alphaGain = solidAlpha ? 1.f / 0.98f : 1.f;

    OfxRectI rod = dst->getRegionOfDefinition();
    OfxRectI bounds = dst->getBounds();
    if (rod.x2 <= rod.x1 || rod.y2 <= rod.y1) rod = bounds;
    const int frameW = rod.x2 - rod.x1, frameH = rod.y2 - rod.y1;
    const double par = dst->getPixelAspectRatio() > 0 ? dst->getPixelAspectRatio() : 1.0;

    // Work out what to draw: a result frame, or an overlay explaining why not.
    std::shared_ptr<const comfy::Rgba8> gen;
    Overlay overlay;
    auto meta = comfy::findResult(dir, id);
    auto st = id.empty() ? std::nullopt : comfy::jobStatus(id);
    if (meta && meta->error.empty()) {
        if (meta->kind == comfy::JobKind::Image) {
            gen = comfy::cachedImage(meta->file);
        } else if (meta->frames > 0) {
            double hostFps = getFrameRate() > 0 ? getFrameRate() : 24.0;
            long idx = long(std::floor((t - startFrame) * meta->fps / hostFps + 1e-6));
            if (idx >= meta->frames) {
                if (afterEnd == kAfterHold) idx = meta->frames - 1;
                else if (afterEnd == kAfterLoop) idx %= meta->frames;
                else idx = -1;
            }
            if (idx >= 0) gen = comfy::cachedImage(comfy::joinPath(meta->framesDir, comfy::frameFileName(int(idx))));
        }
        if (!gen && meta->kind == comfy::JobKind::Image)
            overlay = buildOverlay("Comfy Router: cannot open " + meta->file, frameW, frameH, -1, true, 0);
    } else if (st && st->state == comfy::JobState::Running) {
        double expected = 20;
        if (st->kind == comfy::JobKind::Video) {
            int res = 1, dur = 5;
            sdRes_->getValueAtTime(t, res);
            sdDuration_->getValueAtTime(t, dur);
            expected = (res == 0 ? 100 : res == 1 ? 150 : 240) * std::max(1.0, dur / 5.0);
        }
        float frac = float(std::min(0.95, 1.0 - std::exp(-st->elapsedSec / expected)));
        overlay = buildOverlay("Comfy Router · " + st->message + "  " + fmtSeconds(st->elapsedSec) +
                                   "\nResult appears here when ready (move the playhead or press Refresh Viewer).",
                               frameW, frameH, frac, false, 0.35f);
    } else if ((meta && !meta->error.empty()) || (st && st->state == comfy::JobState::Failed)) {
        std::string msg = meta ? meta->error : st->message;
        overlay = buildOverlay("Comfy Router error: " + msg, frameW, frameH, -1, true, 0.35f);
    } else if (!id.empty() && !(st && st->state == comfy::JobState::Cancelled)) {
        bool waitingForFrame;
        {
            std::lock_guard<std::mutex> g(pendingMutex_);
            waitingForFrame = pending_.has_value();
        }
        if (!waitingForFrame)
            overlay = buildOverlay("Comfy Router: result " + id + " not found in " + dir +
                                       "\nPress Generate, or point Output Folder at the folder that holds it.",
                                   frameW, frameH, -1, true, 0.35f);
    } else if (!src) {
        overlay = buildOverlay("Comfy Router · write a prompt and press Generate", frameW, frameH, -1, false, 0);
    }

    // Placement of the generated frame inside the RoD (pixel space, y up).
    double sx = 1, sy = 1, ox = rod.x1, oy = rod.y1;
    if (gen && !gen->empty()) {
        double dispW = frameW * par;  // display-space width
        double fx = dispW / gen->w, fy = double(frameH) / gen->h;
        double s = fitMode == kFitFill ? std::max(fx, fy) : std::min(fx, fy);
        double scaleX = fitMode == kFitStretch ? fx : s, scaleY = fitMode == kFitStretch ? fy : s;
        sx = scaleX / par;  // back to pixel columns
        sy = scaleY;
        ox = rod.x1 + (frameW - gen->w * sx) * 0.5;
        oy = rod.y1 + (frameH - gen->h * sy) * 0.5;
    }

    const OfxRectI win = args.renderWindow;
    const bool genOk = gen && !gen->empty();
    const float op = float(std::min(1.0, std::max(0.0, opacity)));
    OfxRectI sb = src ? src->getBounds() : OfxRectI{0, 0, 0, 0};

    parallelRows(win.y1, win.y2, [&](int y) {
        T* out = static_cast<T*>(dst->getPixelAddress(win.x1, y));
        if (!out) return;
        const T* srow = (src && y >= sb.y1 && y < sb.y2) ? static_cast<const T*>(src->getPixelAddress(sb.x1, y)) : nullptr;
        int ty = rod.y2 - 1 - y;  // top-down row for the overlay
        for (int x = win.x1; x < win.x2; ++x, out += 4) {
            float base[4] = {0, 0, 0, 0};  // no source (generator): transparent, so results key over lower tracks
            if (srow && x >= sb.x1 && x < sb.x2) {
                const T* s = srow + size_t(x - sb.x1) * 4;
                for (int c = 0; c < 4; ++c) base[c] = fromPix(s[c]);
            }
            float px[4] = {base[0], base[1], base[2], base[3]};
            if (genOk) {
                double u = (x + 0.5 - ox) / sx;
                double v = gen->h - (y + 0.5 - oy) / sy;  // image rows are top-down
                // What sits behind the result: outside a fitted image and under transparent pixels.
                float behind[4] = {0, 0, 0, 0};
                if (lbMode == kLbSource) std::copy(base, base + 4, behind);
                else if (lbMode == kLbBlack) behind[3] = 1;
                float comp[4];
                if (u >= 0 && v >= 0 && u < gen->w && v < gen->h) {
                    float layer[4];
                    sampleBilinear(*gen, float(u), float(v), layer);  // straight alpha
                    float a = std::min(1.f, layer[3] * alphaGain);
                    for (int c = 0; c < 3; ++c) comp[c] = layer[c] * a + behind[c] * (1 - a);  // premultiplied over
                    comp[3] = a + behind[3] * (1 - a);
                } else {
                    std::copy(behind, behind + 4, comp);
                }
                for (int c = 0; c < 4; ++c) px[c] = base[c] + (comp[c] - base[c]) * op;
            }
            if (overlay.active) overlay.apply(x - rod.x1, ty, px);
            for (int c = 0; c < 4; ++c) out[c] = toPix<T>(px[c]);
        }
    });
}

// ================================================================= factory

static void unloadPlugin() { comfy::shutdownJobs(); }

// Installing the import script at load puts it in Workspace → Scripts from the next launch.
mDeclarePluginFactory(ComfyRouterFactory, { comfy::installImportScript(); }, { unloadPlugin(); });

void ComfyRouterFactory::describe(ImageEffectDescriptor& desc) {
    desc.setLabels(kPluginName, kPluginName, kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextGenerator);
    desc.addSupportedBitDepth(eBitDepthUByte);
    desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(true);
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setSupportsMultipleClipDepths(false);
    desc.setRenderThreadSafety(eRenderFullySafe);
}

namespace {

ChoiceParamDescriptor* defineChoice(ImageEffectDescriptor& desc, const char* name, const char* label, const char* hint,
                                    const std::vector<std::string>& opts, int def, PageParamDescriptor* page,
                                    GroupParamDescriptor* group) {
    auto* p = desc.defineChoiceParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    for (const auto& o : opts) p->appendOption(o);
    p->setDefault(def);
    p->setAnimates(false);
    if (group) p->setParent(*group);
    if (page) page->addChild(*p);
    return p;
}

StringParamDescriptor* defineString(ImageEffectDescriptor& desc, const char* name, const char* label, const char* hint,
                                    StringTypeEnum type, PageParamDescriptor* page, GroupParamDescriptor* group) {
    auto* p = desc.defineStringParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setStringType(type);
    p->setAnimates(false);
    if (group) p->setParent(*group);
    if (page) page->addChild(*p);
    return p;
}

PushButtonParamDescriptor* defineButton(ImageEffectDescriptor& desc, const char* name, const char* label, const char* hint,
                                        PageParamDescriptor* page, GroupParamDescriptor* group) {
    auto* p = desc.definePushButtonParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    if (group) p->setParent(*group);
    if (page) page->addChild(*p);
    return p;
}

IntParamDescriptor* defineInt(ImageEffectDescriptor& desc, const char* name, const char* label, const char* hint, int def,
                              int lo, int hi, PageParamDescriptor* page, GroupParamDescriptor* group) {
    auto* p = desc.defineIntParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(lo, std::min(hi, std::max(lo + 1, lo + 100)));
    p->setAnimates(false);
    if (group) p->setParent(*group);
    if (page) page->addChild(*p);
    return p;
}

GroupParamDescriptor* defineGroup(ImageEffectDescriptor& desc, const char* name, const char* label, bool open,
                                  PageParamDescriptor* page) {
    auto* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    g->setOpen(open);
    if (page) page->addChild(*g);
    return g;
}

}  // namespace

void ComfyRouterFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context) {
    if (context != eContextGenerator) {
        ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
        src->addSupportedComponent(ePixelComponentRGBA);
        src->setTemporalClipAccess(false);
        src->setSupportsTiles(true);
        src->setIsMask(false);
        if (context == eContextGeneral) src->setOptional(true);
    }
    ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(ePixelComponentRGBA);
    dst->setSupportsTiles(true);

    PageParamDescriptor* page = desc.definePageParam("Controls");

    defineChoice(desc, kMode, "Generate", "What to generate: a still with Nano Banana 2 or a clip with Seedance 2.5.",
                 {"Image · Nano Banana 2", "Image · GPT Image 2.5 (transparent)", "Video · Seedance 2.5"}, kModeImage, page,
                 nullptr);
    defineString(desc, kPrompt, "Prompt", "Describe what to generate, or how to change the current frame.",
                 eStringTypeMultiLine, page, nullptr);
    defineButton(desc, kGenerate, "Generate", "Send the request to Comfy Router. Uses Comfy credits.", page, nullptr);
    defineButton(desc, kCancel, "Cancel", "Stop the running generation.", page, nullptr);
    auto* status = defineString(desc, kStatus, "Status", "What the plugin is doing.", eStringTypeLabel, page, nullptr);
    status->setEvaluateOnChange(false);
    status->setCanUndo(false);
    status->setDefault("Ready");
    defineButton(desc, kRefresh, "Refresh Viewer", "Redraw once a generation finishes.", page, nullptr);
    defineButton(desc, kImportMedia, "Import Generated Media",
                 "Import every Comfy Router generation that isn't in the Media Pool yet into a \"Comfy Router\" bin "
                 "(stills keep alpha, videos keep audio). Works from the effect in Resolve Studio with External "
                 "scripting set to Local; in any edition, use Workspace → Scripts → Comfy Router - Import Generated "
                 "Media.", page, nullptr);

    auto* nb = defineGroup(desc, "grpImage", "Image · Nano Banana 2", true, page);
    defineChoice(desc, kNbAspect, "Aspect Ratio", "Output aspect ratio. Match Timeline picks the closest supported ratio.",
                 kNbAspectOpts, 0, page, nb);
    defineChoice(desc, kNbSize, "Image Size", "Output resolution tier (long edge ≈ 1K/2K/4K px).", kNbSizeOpts, 1, page, nb);
    defineChoice(desc, kNbInput, "Image Input", "Send the current frame and/or reference images for editing.",
                 kNbInputOpts, kNbInNone, page, nb);

    auto* gp = defineGroup(desc, "grpGpt", "Image · GPT Image 2.5", true, page);
    defineChoice(desc, kGptVariant, "Variant", "GPT Image 2.5 variant served by the Router.", kGptVariantOpts, 0, page, gp);
    defineChoice(desc, kGptSize, "Size", "Output size. Match Timeline uses the timeline aspect at a 1536 px long edge.",
                 kGptSizeOpts, 0, page, gp);
    defineChoice(desc, kGptQuality, "Quality", "Higher quality costs more credits.", kGptQualityOpts, 1, page, gp);
    defineChoice(desc, kGptBackground, "Background", "Transparent returns a PNG with alpha — ideal for titles, "
                 "lower thirds, stickers and other elements.", kGptBackgroundOpts, 0, page, gp);
    defineChoice(desc, kGptInput, "Image Input", "Send the current frame and/or reference images to edit.",
                 kNbInputOpts, kNbInNone, page, gp);

    auto* sd = defineGroup(desc, "grpVideo", "Video · Seedance 2.5", true, page);
    defineChoice(desc, kSdResolution, "Resolution", "Output resolution.", kSdResOpts, 1, page, sd);
    defineInt(desc, kSdDuration, "Duration (s)", "Clip length in seconds.", 5, 4, 15, page, sd);
    defineChoice(desc, kSdRatio, "Aspect Ratio", "Output aspect ratio. Match Timeline picks the closest supported ratio "
                 "for text-to-video and follows the image when a first frame is given.", kSdRatioOpts, 0, page, sd);
    auto* audio = desc.defineBooleanParam(kSdAudio);
    audio->setLabels("Generate Audio", "Generate Audio", "Generate Audio");
    audio->setHint("Seedance can generate a soundtrack. Audio lives in the saved MP4 (OFX effects can't output audio).");
    audio->setDefault(true);
    audio->setAnimates(false);
    audio->setParent(*sd);
    page->addChild(*audio);
    defineInt(desc, kSdSeed, "Seed", "-1 for random.", -1, -1, 2147483647, page, sd);
    defineChoice(desc, kSdInput, "Image Input", "Condition the video on the current frame and/or reference images.",
                 kSdInputOpts, kSdInNone, page, sd);

    auto* refs = defineGroup(desc, "grpRefs", "Reference Images", false, page);
    defineString(desc, kRef1, "Reference Image 1", "PNG/JPEG file used by the Image Input options.", eStringTypeFilePath, page, refs)
        ->setFilePathExists(true);
    defineString(desc, kRef2, "Reference Image 2", "PNG/JPEG file used by the Image Input options.", eStringTypeFilePath, page, refs)
        ->setFilePathExists(true);

    auto* place = defineGroup(desc, "grpPlacement", "Placement", false, page);
    defineChoice(desc, kFit, "Fit", "How the result fits the frame.", {"Fit", "Fill (crop)", "Stretch"}, kFitFit, page, place);
    defineChoice(desc, kLetterbox, "Behind Result", "What shows outside a fitted result and through transparent pixels. "
                 "Source on a generator is transparent, so elements key over lower tracks.",
                 {"Source", "Black", "Transparent"}, kLbSource, page, place);
    auto* opacity = desc.defineDoubleParam(kOpacity);
    opacity->setLabels("Opacity", "Opacity", "Opacity");
    opacity->setHint("Blend the result over the source.");
    opacity->setDefault(1);
    opacity->setRange(0, 1);
    opacity->setDisplayRange(0, 1);
    opacity->setParent(*place);
    page->addChild(*opacity);
    auto* solid = desc.defineBooleanParam(kSolidAlpha);
    solid->setLabels("Solid Alpha", "Solid Alpha", "Solid Alpha");
    solid->setHint("Treat nearly-opaque pixels (GPT Image returns ~99% alpha inside elements) as fully opaque. "
                   "The saved PNG is untouched.");
    solid->setDefault(true);
    solid->setAnimates(false);
    solid->setParent(*place);
    page->addChild(*solid);
    defineInt(desc, kStartFrame, "Video Start Frame", "Effect frame where the generated video starts. Set to the playhead "
              "when you press Generate.", 0, -1000000, 1000000, page, place);
    defineChoice(desc, kAfterEnd, "After Video Ends", "What to show past the last generated frame.",
                 {"Hold Last Frame", "Loop", "Show Source"}, kAfterHold, page, place);

    auto* settings = defineGroup(desc, "grpSettings", "Settings", true, page);
    auto* key = defineString(desc, kApiKey, "API Key", "Paste your Comfy API key (comfyui-…) and press Enter. It is saved "
                             "on this computer only — never in the project — and the field clears itself.",
                             eStringTypeSingleLine, page, settings);
    key->setEvaluateOnChange(false);
    defineButton(desc, kClearKey, "Forget API Key", "Remove the key saved on this computer.", page, settings);
    defineChoice(desc, kProvider, "Provider", "Which Router leg serves the request. Default lets Comfy route it.",
                 kProviderOpts, 0, page, settings);
    defineString(desc, kOutDir, "Output Folder", "Where results are saved. Empty = ~/Movies/ComfyRouter (macOS) or "
                 "~/Videos/ComfyRouter.", eStringTypeDirectoryPath, page, settings)->setFilePathExists(true);
    defineButton(desc, kReveal, "Reveal Output Folder", "Open the output folder to drag results into the Media Pool.", page, settings);
    defineString(desc, kFfmpeg, "FFmpeg Path", "Optional. Used to decode video when AVFoundation isn't available "
                 "(Windows/Linux). Empty = search common locations.", eStringTypeFilePath, page, settings)->setFilePathExists(true);

    // Hidden state saved with the project.
    auto* jobId = defineString(desc, kJobId, "Job", "", eStringTypeSingleLine, nullptr, nullptr);
    jobId->setIsSecret(true);
    auto* jobDir = defineString(desc, kJobDir, "Job Folder", "", eStringTypeSingleLine, nullptr, nullptr);
    jobDir->setIsSecret(true);
    auto* tick = desc.defineIntParam(kRefreshTick);
    tick->setIsSecret(true);
    tick->setAnimates(false);
    tick->setCanUndo(false);
}

ImageEffect* ComfyRouterFactory::createInstance(OfxImageEffectHandle handle, ContextEnum) {
    return new ComfyRouterPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(PluginFactoryArray& ids) {
    static ComfyRouterFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
}  // namespace Plugin
}  // namespace OFX
