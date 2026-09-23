// Command-line harness for the plugin's generation pipeline (same code the OFX effect runs):
// Router call → file saved to the output folder → frames decoded → metadata written.
//
//   COMFY_API_KEY=comfyui-… router_cli <nb2|gpt|sd25> "prompt" [options]
//   COMFY_API_KEY=comfyui-… router_cli resume <job-id> [--out DIR]
//     --out DIR          output folder (default ./cli-out)
//     --in FILE          input image (edit for nb2/gpt, first frame for sd25)
//     --aspect 16:9      nb2 aspect ratio / sd25 ratio
//     --size 1K|WxH      nb2 imageSize or gpt size
//     --bg transparent   gpt background (transparent|opaque)
//     --quality low      gpt quality
//     --variant flare    gpt variant (flare|sunburst)
//     --res 480p --dur 4 --no-audio   sd25 options
//     --provider fal     Router leg
//     --dry              print the request body (inputs elided) and exit
//
// The key is read from COMFY_API_KEY or the plugin's saved config — never from argv,
// so it doesn't land in shell history.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "Jobs.h"
#include "Media.h"
#include "RouterClient.h"
#include "Settings.h"

using namespace comfy;

static InputImage loadInput(const std::string& path, int minEdge) {
    Rgba8 img;
    if (!loadImageFile(path, img)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(2);
    }
    img = fitForUpload(img, 2048, minEdge);
    return {"image/jpeg", encodeJpeg(img, 95)};
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: router_cli <nb2|gpt|sd25> \"prompt\" [options]\n");
        return 2;
    }
    std::string model = argv[1];
    if (model == "resume") {
        // router_cli resume <job-id> [--out DIR]: collect a job whose connection was lost.
        std::string dir = argc > 4 && std::string(argv[3]) == "--out" ? argv[4] : "cli-out";
        std::string id = argv[2];
        if (!resumeJob(dir, id, loadApiKey(), "")) {
            std::fprintf(stderr, "nothing to resume for %s in %s\n", id.c_str(), dir.c_str());
            return 1;
        }
        std::string last;
        while (true) {
            auto st = jobStatus(id);
            if (st && st->message != last) {
                std::printf("  [%5.1fs] %s\n", st->elapsedSec, st->message.c_str());
                std::fflush(stdout);
                last = st->message;
            }
            if (st && st->state != JobState::Running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        shutdownJobs();
        auto meta = findResult(dir, id);
        std::printf("%s\n", meta ? (meta->error.empty() ? ("OK " + meta->file).c_str() : ("FAILED: " + meta->error).c_str())
                                  : "no result");
        return meta && meta->error.empty() ? 0 : 1;
    }
    JobSpec spec;
    spec.id = newJobId();
    spec.outDir = "cli-out";
    std::string in, aspect, size, bg = "transparent", quality = "low", variant = "flare", res = "480p";
    int dur = 4;
    bool audio = true, dry = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--out") spec.outDir = next();
        else if (a == "--in") in = next();
        else if (a == "--aspect") aspect = next();
        else if (a == "--size") size = next();
        else if (a == "--bg") bg = next();
        else if (a == "--quality") quality = next();
        else if (a == "--variant") variant = next();
        else if (a == "--res") res = next();
        else if (a == "--dur") dur = std::stoi(next());
        else if (a == "--no-audio") audio = false;
        else if (a == "--provider") spec.provider = next();
        else if (a == "--dry") dry = true;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    std::string body;
    if (model == "nb2") {
        spec.kind = JobKind::Image;
        spec.image.prompt = argv[2];
        spec.image.aspectRatio = aspect;
        spec.image.imageSize = size;
        if (!in.empty()) spec.image.images.push_back(loadInput(in, 0));
        body = buildImageBody(spec.image);
    } else if (model == "gpt") {
        spec.kind = JobKind::Image;
        spec.imageModel = ImageModel::GptImage25;
        spec.gpt.model = variant == "sunburst" ? kModelGptImage25Sunburst : kModelGptImage25Flare;
        spec.gpt.prompt = argv[2];
        spec.gpt.size = size.empty() ? "1024x1024" : size;
        spec.gpt.background = bg;
        spec.gpt.quality = quality;
        if (!in.empty()) spec.gpt.images.push_back(loadInput(in, 0));
        body = buildGptImageBody(spec.gpt);
    } else if (model == "sd25") {
        spec.kind = JobKind::Video;
        spec.video.prompt = argv[2];
        spec.video.resolution = res;
        spec.video.duration = dur;
        spec.video.generateAudio = audio;
        spec.video.ratio = aspect.empty() ? (in.empty() ? "16:9" : "adaptive") : aspect;
        if (!in.empty()) spec.video.images.emplace_back("first_frame", loadInput(in, 300));
        body = buildVideoBody(spec.video);
    } else {
        std::fprintf(stderr, "model must be nb2, gpt or sd25\n");
        return 2;
    }
    if (dry) {
        // Elide inline base64 so the body is readable.
        size_t pos = 0;
        while ((pos = body.find("base64,", pos)) != std::string::npos) {
            size_t end = body.find('"', pos);
            body.replace(pos + 7, end - pos - 7, "…");
            pos += 8;
        }
        pos = 0;
        while ((pos = body.find("\"data\":\"", pos)) != std::string::npos) {
            size_t start = pos + 8, end = body.find('"', start);
            body.replace(start, end - start, "…");
            pos = start + 1;
        }
        std::printf("%s\n", body.c_str());
        return 0;
    }
    spec.apiKey = loadApiKey();
    if (spec.apiKey.empty()) {
        std::fprintf(stderr, "set COMFY_API_KEY or save a key from the plugin first\n");
        return 2;
    }
    std::string id = spec.id, dir = spec.outDir;
    std::printf("job %s → %s\n", id.c_str(), dir.c_str());
    startJob(std::move(spec));
    std::string last;
    while (true) {
        auto st = jobStatus(id);
        if (st && st->message != last) {
            std::printf("  [%5.1fs] %s\n", st->elapsedSec, st->message.c_str());
            std::fflush(stdout);
            last = st->message;
        }
        if (st && st->state != JobState::Running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    shutdownJobs();
    auto meta = findResult(dir, id);
    if (!meta) {
        std::printf("no result metadata\n");
        return 1;
    }
    if (!meta->error.empty()) {
        std::printf("FAILED: %s\n", meta->error.c_str());
        return 1;
    }
    std::printf("OK %s %dx%d", meta->file.c_str(), meta->w, meta->h);
    if (meta->kind == JobKind::Video) std::printf(" frames=%d fps=%.2f dir=%s", meta->frames, meta->fps, meta->framesDir.c_str());
    if (meta->credits >= 0) std::printf(" credits=%.2f", meta->credits);
    std::printf("\n");
    return 0;
}
