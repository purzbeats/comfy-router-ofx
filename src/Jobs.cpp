#include "Jobs.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "Media.h"
#include "Settings.h"
#include "nlohmann/json.hpp"

using nlohmann::json;

namespace comfy {

namespace {

struct Job {
    JobStatus status;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::atomic<bool> cancel{false};
};

struct Registry {
    std::mutex m;
    std::map<std::string, std::shared_ptr<Job>> jobs;
    std::vector<std::thread> threads;
    std::map<std::string, std::shared_ptr<const ResultMeta>> results;  // positive lookups only
};

Registry& reg() {
    static Registry r;
    return r;
}

std::string metaDir(const std::string& outDir) { return joinPath(joinPath(outDir, ".comfyrouter"), "meta"); }
std::string metaPath(const std::string& outDir, const std::string& id) { return joinPath(metaDir(outDir), id + ".json"); }

void setMessage(const std::shared_ptr<Job>& job, const std::string& msg) {
    std::lock_guard<std::mutex> g(reg().m);
    job->status.message = msg;
}

void finishJob(const std::shared_ptr<Job>& job, JobState state, const std::string& msg, double credits) {
    std::lock_guard<std::mutex> g(reg().m);
    job->status.state = state;
    job->status.message = msg;
    job->status.credits = credits;
    job->status.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - job->started).count();
}

void writeMeta(const JobSpec& spec, const json& j) {
    std::string err;
    ensureDir(metaDir(spec.outDir), &err);
    writeFileAtomic(metaPath(spec.outDir, spec.id), j.dump(2), &err);
}

std::string pendingPath(const std::string& outDir, const std::string& id) {
    return joinPath(metaDir(outDir), id + ".pending.json");
}

void removePending(const JobSpec& spec) {
    std::error_code ec;
    std::filesystem::remove(pendingPath(spec.outDir, spec.id), ec);
}

void fail(const JobSpec& spec, const std::shared_ptr<Job>& job, const Result* res, const std::string& msg) {
    if (job->cancel.load() || (res && res->cancelled)) {
        removePending(spec);
        finishJob(job, JobState::Cancelled, "Cancelled", -1);
        return;  // no meta: a cancelled job simply has no result
    }
    if (res && res->stillRunning) {
        finishJob(job, JobState::Failed, msg, -1);  // keep the pending record so a reopen resumes it
        return;
    }
    json j = {{"error", msg}, {"kind", spec.kind == JobKind::Video ? "video" : "image"}};
    if (res) {
        j["request_id"] = res->requestId;
        j["error_type"] = res->errorType;
        j["http_status"] = res->status;
    }
    writeMeta(spec, j);
    removePending(spec);
    finishJob(job, JobState::Failed, msg, -1);
}

std::string creditsText(double credits) {
    if (credits < 0) return "";
    char buf[64];
    std::snprintf(buf, sizeof buf, " · %.2f credits", credits);
    return buf;
}

// What a spec sends, and how its files are named.
struct Plan {
    std::string model, body, prefix, prompt, label;
};

Plan planFor(const JobSpec& spec) {
    Plan p;
    if (spec.kind == JobKind::Video) {
        p = {kModelSeedance25, buildVideoBody(spec.video), "Seedance25", spec.video.prompt, "Seedance 2.5"};
    } else if (spec.imageModel == ImageModel::GptImage25) {
        p = {spec.gpt.model, buildGptImageBody(spec.gpt), "GPTImage25", spec.gpt.prompt, "GPT Image 2.5"};
    } else {
        p = {kModelNanoBanana2, buildImageBody(spec.image), "NanoBanana2", spec.image.prompt, "Nano Banana 2"};
    }
    if (!spec.resumeModel.empty()) p.model = spec.resumeModel;
    if (!spec.resumePrefix.empty()) p.prefix = spec.resumePrefix;
    if (!spec.resumePrompt.empty()) p.prompt = spec.resumePrompt;
    return p;
}

void run(JobSpec spec, std::shared_ptr<Job> job) {
    std::string err;
    if (!ensureDir(spec.outDir, &err) || !ensureDir(metaDir(spec.outDir), &err)) {
        fail(spec, job, nullptr, err);
        return;
    }
    const bool video = spec.kind == JobKind::Video;
    Plan plan = planFor(spec);
    RunHooks hooks;
    hooks.progress = [&](const std::string& m) {
        setMessage(job, m.rfind("Generating", 0) == 0 && m.find("with") == std::string::npos
                            ? m.substr(0, m.size() - 3) + " with " + plan.label + "…" : m);
    };
    hooks.onSubmitted = [&](const std::string& requestId) {
        // Written before we start waiting, so a crash / restart / dropped connection can resume.
        json pending = {{"request_id", requestId}, {"model", plan.model}, {"kind", video ? "video" : "image"},
                        {"prefix", plan.prefix}, {"prompt", plan.prompt}, {"created", (long long)std::time(nullptr)}};
        std::string e;
        writeFileAtomic(pendingPath(spec.outDir, spec.id), pending.dump(2), &e);
    };
    Result res = spec.resumeRequestId.empty()
                     ? runModel(plan.model, spec.provider, plan.body, spec.apiKey, spec.id, video, &job->cancel, hooks)
                     : collectRequest(plan.model, spec.resumeRequestId, spec.apiKey, video, &job->cancel, hooks);
    if (!res.ok) {
        fail(spec, job, &res, res.error);
        return;
    }
    const Media& m = res.media.front();
    std::string file = joinPath(spec.outDir, "ComfyRouter_" + plan.prefix + "_" + spec.id + m.ext);
    if (!writeFileAtomic(file, m.bytes, &err)) {
        fail(spec, job, &res, err);
        return;
    }
    json meta = {
        {"kind", video ? "video" : "image"},
        {"file", file},
        {"credits", res.credits},
        {"request_id", res.requestId},
        {"dropped", res.dropped},
        {"prompt", plan.prompt},
        {"model", plan.model},
        {"provider", spec.provider.empty() ? "default" : spec.provider},
        {"created", (long long)std::time(nullptr)},
    };
    if (video) {
        setMessage(job, "Decoding frames…");
        VideoInfo info;
        if (res.fps > 0) info.fps = res.fps;  // ffmpeg path keeps this; AVFoundation reads the track rate
        std::string framesDir = frameDirFor(spec.outDir, spec.id);
        bool ok = extractFrames(file, framesDir, spec.ffmpegHint, info, err, &job->cancel, [&](int done, int total) {
            char buf[96];
            if (total > 0) std::snprintf(buf, sizeof buf, "Decoding frames %d/%d…", done, total);
            else std::snprintf(buf, sizeof buf, "Decoding frames…");
            setMessage(job, buf);
        });
        if (!ok) {
            if (job->cancel.load()) {
                removePending(spec);
                finishJob(job, JobState::Cancelled, "Cancelled (video kept in output folder)", res.credits);
                return;
            }
            fail(spec, job, &res, "Video saved to " + file + " but could not be decoded for preview: " + err);
            return;
        }
        meta["frames_dir"] = framesDir;
        meta["frames"] = info.frames;
        meta["fps"] = info.fps;
        meta["width"] = info.w;
        meta["height"] = info.h;
    } else {
        Rgba8 img;
        if (!decodeImage(m.bytes, img)) {
            fail(spec, job, &res, "Router returned an image that could not be decoded");
            return;
        }
        meta["width"] = img.w;
        meta["height"] = img.h;
    }
    writeMeta(spec, meta);
    removePending(spec);
    std::string msg = std::string(video ? "Video ready" : "Image ready") + creditsText(res.credits);
    if (!res.dropped.empty()) msg += " · Router dropped: " + res.dropped;
    finishJob(job, JobState::Done, msg, res.credits);
}

}  // namespace

std::string newJobId() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
    static std::mt19937_64 rng{std::random_device{}()};
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    char out[64];
    std::snprintf(out, sizeof out, "%s-%04x", ts, unsigned(rng() & 0xffff));
    return out;
}

std::string frameDirFor(const std::string& outDir, const std::string& id) {
    return joinPath(joinPath(joinPath(outDir, ".comfyrouter"), "frames"), id);
}

void startJob(JobSpec spec) {
    auto job = std::make_shared<Job>();
    job->status.kind = spec.kind;
    job->status.message = spec.resumeRequestId.empty() ? "Submitting…" : "Resuming…";
    Registry& r = reg();
    std::lock_guard<std::mutex> g(r.m);
    r.jobs[spec.id] = job;
    r.threads.emplace_back([spec = std::move(spec), job]() mutable { run(std::move(spec), job); });
}

bool resumeJob(const std::string& outDir, const std::string& id, const std::string& apiKey, const std::string& ffmpegHint) {
    if (id.empty() || apiKey.empty()) return false;
    {
        std::lock_guard<std::mutex> g(reg().m);
        if (reg().jobs.count(id)) return true;  // already running (or finished) in this session
    }
    if (fileExists(metaPath(outDir, id))) return false;
    std::string data;
    if (!readFile(pendingPath(outDir, id), data)) return false;
    json p = json::parse(data, nullptr, false);
    if (!p.is_object() || !p.contains("request_id") || !p.contains("model")) return false;
    JobSpec spec;
    spec.id = id;
    spec.outDir = outDir;
    spec.apiKey = apiKey;
    spec.ffmpegHint = ffmpegHint;
    spec.kind = p.value("kind", "image") == "video" ? JobKind::Video : JobKind::Image;
    spec.resumeRequestId = p["request_id"].get<std::string>();
    spec.resumeModel = p["model"].get<std::string>();
    spec.resumePrefix = p.value("prefix", "");
    spec.resumePrompt = p.value("prompt", "");
    startJob(std::move(spec));
    return true;
}

std::optional<JobStatus> jobStatus(const std::string& id) {
    Registry& r = reg();
    std::lock_guard<std::mutex> g(r.m);
    auto it = r.jobs.find(id);
    if (it == r.jobs.end()) return std::nullopt;
    JobStatus s = it->second->status;
    if (s.state == JobState::Running)
        s.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - it->second->started).count();
    return s;
}

void cancelJob(const std::string& id) {
    Registry& r = reg();
    std::lock_guard<std::mutex> g(r.m);
    auto it = r.jobs.find(id);
    if (it != r.jobs.end()) it->second->cancel = true;
}

void shutdownJobs() {
    std::vector<std::thread> threads;
    {
        Registry& r = reg();
        std::lock_guard<std::mutex> g(r.m);
        for (auto& [id, job] : r.jobs) job->cancel = true;
        threads.swap(r.threads);
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();  // curl and frame decode both poll the cancel flag
}

std::shared_ptr<const ResultMeta> findResult(const std::string& outDir, const std::string& id) {
    if (id.empty()) return nullptr;
    std::string path = metaPath(outDir, id);
    Registry& r = reg();
    {
        std::lock_guard<std::mutex> g(r.m);
        auto it = r.results.find(path);
        if (it != r.results.end()) return it->second;
    }
    std::string data;
    if (!readFile(path, data)) return nullptr;
    json j = json::parse(data, nullptr, false);
    if (!j.is_object()) return nullptr;
    auto meta = std::make_shared<ResultMeta>();
    meta->kind = j.value("kind", "image") == "video" ? JobKind::Video : JobKind::Image;
    meta->error = j.value("error", "");
    meta->file = j.value("file", "");
    meta->framesDir = j.value("frames_dir", "");
    meta->frames = j.value("frames", 0);
    meta->fps = j.value("fps", 24.0);
    meta->w = j.value("width", 0);
    meta->h = j.value("height", 0);
    meta->dropped = j.value("dropped", "");
    if (j.contains("credits") && j["credits"].is_number()) meta->credits = j["credits"].get<double>();
    std::lock_guard<std::mutex> g(r.m);
    r.results[path] = meta;
    return meta;
}

}  // namespace comfy
