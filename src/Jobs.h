// Background generation jobs and their on-disk results.
//
// Layout inside the output folder:
//   ComfyRouter_NanoBanana2_<id>.png        ← import-ready still
//   ComfyRouter_GPTImage25_<id>.png         ← import-ready still, alpha preserved
//   ComfyRouter_Seedance25_<id>.mp4         ← import-ready video (with audio)
//   .comfyrouter/meta/<id>.json             ← what the effect reads to render
//   .comfyrouter/meta/<id>.pending.json     ← Router request id while a job is in flight
//   .comfyrouter/frames/<id>/000001.jpg …   ← decoded frames for in-effect playback
//
// Results are keyed by job id, which the effect stores in a hidden parameter, so a
// reopened project finds its generations again without re-running them.
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "RouterClient.h"

namespace comfy {

enum class JobKind { Image, Video };
enum class ImageModel { NanoBanana2, GptImage25 };

struct JobSpec {
    std::string id;
    std::string outDir;
    JobKind kind = JobKind::Image;
    std::string apiKey;
    std::string provider;  // "" = Router default
    std::string ffmpegHint;
    ImageModel imageModel = ImageModel::NanoBanana2;
    ImageRequest image;     // Nano Banana 2
    GptImageRequest gpt;    // GPT Image 2.5
    VideoRequest video;
    // Set when picking up a request submitted in an earlier session.
    std::string resumeRequestId, resumeModel, resumePrefix, resumePrompt;
};

enum class JobState { Running, Done, Failed, Cancelled };

struct JobStatus {
    JobState state = JobState::Running;
    JobKind kind = JobKind::Image;
    std::string message;
    double elapsedSec = 0;
    double credits = -1;
};

struct ResultMeta {
    JobKind kind = JobKind::Image;
    std::string file;       // image or mp4
    std::string framesDir;  // video only
    int frames = 0;
    double fps = 24;
    int w = 0, h = 0;
    double credits = -1;
    std::string error;      // non-empty = the job failed; nothing to show
    std::string dropped;    // Router-dropped params, for the status line
};

std::string newJobId();
void startJob(JobSpec spec);
// If `id` was submitted but never collected (dropped connection, Resolve quit), start
// collecting it again. Returns true if a job for `id` is now running.
bool resumeJob(const std::string& outDir, const std::string& id, const std::string& apiKey, const std::string& ffmpegHint);
std::optional<JobStatus> jobStatus(const std::string& id);
void cancelJob(const std::string& id);
void shutdownJobs();  // cancels and joins every worker (plugin unload)

std::shared_ptr<const ResultMeta> findResult(const std::string& outDir, const std::string& id);
std::string frameDirFor(const std::string& outDir, const std::string& id);

}  // namespace comfy
