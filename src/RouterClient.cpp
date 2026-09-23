#include "RouterClient.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <regex>
#include <chrono>
#include <set>
#include <thread>

#include "nlohmann/json.hpp"

using nlohmann::json;

namespace comfy {

// ---------------------------------------------------------------- base64

static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    const auto* s = reinterpret_cast<const unsigned char*>(in.data());
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (s[i] << 16) | (s[i + 1] << 8) | s[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i < in.size()) {
        unsigned v = s[i] << 16;
        if (i + 1 < in.size()) v |= s[i + 1] << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < in.size()) ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& in) {
    static int table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        std::fill(std::begin(table), std::end(table), -1);
        for (int i = 0; i < 64; ++i) table[(unsigned char)kB64[i]] = i;
        table[(unsigned char)'-'] = 62;  // url-safe variants
        table[(unsigned char)'_'] = 63;
    });
    std::string out;
    out.reserve(in.size() * 3 / 4);
    unsigned v = 0;
    int bits = 0;
    for (unsigned char c : in) {
        int d = table[c];
        if (d < 0) continue;  // skips '=', whitespace, newlines
        v = (v << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((v >> bits) & 0xFF);
        }
    }
    return out;
}

std::pair<std::string, std::string> sniffMedia(const std::string& b) {
    auto starts = [&](const char* sig, size_t n) { return b.size() >= n && b.compare(0, n, sig, n) == 0; };
    if (starts("\x89PNG", 4)) return {"image/png", ".png"};
    if (starts("\xff\xd8\xff", 3)) return {"image/jpeg", ".jpg"};
    if (starts("RIFF", 4) && b.size() > 12 && b.compare(8, 4, "WEBP") == 0) return {"image/webp", ".webp"};
    if (starts("GIF8", 4)) return {"image/gif", ".gif"};
    if (b.size() > 8 && b.compare(4, 4, "ftyp") == 0) {
        std::string brand = b.substr(8, 4);
        if (brand == "qt  ") return {"video/quicktime", ".mov"};
        return {"video/mp4", ".mp4"};
    }
    if (starts("\x1a\x45\xdf\xa3", 4)) return {"video/webm", ".webm"};
    return {"", ""};
}

// ---------------------------------------------------------------- HTTP

namespace {

std::once_flag gCurlInit;

struct Transfer {
    std::string body;
    std::map<std::string, std::string> headers;
    const CancelFlag* cancel = nullptr;
};

size_t onBody(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* t = static_cast<Transfer*>(ud);
    t->body.append(ptr, size * nmemb);
    return size * nmemb;
}

size_t onHeader(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* t = static_cast<Transfer*>(ud);
    std::string line(ptr, size * nmemb);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
            size_t i = 0;
            while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
            s.erase(0, i);
        };
        trim(value);
        t->headers[name] = value;
    } else if (line.rfind("HTTP/", 0) == 0) {
        t->headers.clear();  // new response (redirect / 100-continue): drop earlier headers
    }
    return size * nmemb;
}

int onProgress(void* ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* t = static_cast<Transfer*>(ud);
    return (t->cancel && t->cancel->load()) ? 1 : 0;  // non-zero aborts the transfer
}

HttpResponse perform(CURL* h, Transfer& t, const CancelFlag* cancel) {
    t.cancel = cancel;
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "ComfyRouterOFX/1.0");
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // gzip/deflate if offered
    // Keep long provider waits alive through proxies / NAT.
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);

    HttpResponse r;
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        r.cancelled = (rc == CURLE_ABORTED_BY_CALLBACK);
        r.error = r.cancelled ? "cancelled" : curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.status);
    }
    r.headers = std::move(t.headers);
    r.body = std::move(t.body);
    return r;
}

}  // namespace

HttpResponse httpPostJson(const std::string& url, const std::string& apiKey, const std::string& body,
                          const std::string& idempotencyKey, long timeoutSec, const CancelFlag* cancel) {
    std::call_once(gCurlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* h = curl_easy_init();
    if (!h) return HttpResponse{0, {}, "", "curl init failed"};
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, ("X-API-Key: " + apiKey).c_str());
    if (!idempotencyKey.empty()) hdrs = curl_slist_append(hdrs, ("Idempotency-Key: " + idempotencyKey).c_str());
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeoutSec);
    Transfer t;
    HttpResponse r = perform(h, t, cancel);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return r;
}

HttpResponse httpGet(const std::string& url, long timeoutSec, const CancelFlag* cancel) {
    std::call_once(gCurlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* h = curl_easy_init();
    if (!h) return HttpResponse{0, {}, "", "curl init failed"};
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeoutSec);
    Transfer t;
    HttpResponse r = perform(h, t, cancel);
    curl_easy_cleanup(h);
    return r;
}

// ---------------------------------------------------------------- request bodies

static std::string dataUri(const InputImage& img) {
    return "data:" + img.mime + ";base64," + base64Encode(img.bytes);
}

std::string buildImageBody(const ImageRequest& req) {
    json parts = json::array();
    parts.push_back({{"text", req.prompt}});
    for (const auto& img : req.images)
        parts.push_back({{"inlineData", {{"mimeType", img.mime}, {"data", base64Encode(img.bytes)}}}});
    json body = {{"contents", json::array({{{"role", "user"}, {"parts", parts}}})}};
    json imageConfig = json::object();
    if (!req.aspectRatio.empty()) imageConfig["aspectRatio"] = req.aspectRatio;
    if (!req.imageSize.empty()) imageConfig["imageSize"] = req.imageSize;
    if (!imageConfig.empty()) body["generationConfig"] = {{"imageConfig", imageConfig}};
    return body.dump();
}

std::string buildVideoBody(const VideoRequest& req) {
    json content = json::array();
    content.push_back({{"type", "text"}, {"text", req.prompt}});
    for (const auto& [role, img] : req.images)
        content.push_back({{"type", "image_url"}, {"role", role}, {"image_url", {{"url", dataUri(img)}}}});
    json body = {
        {"content", content},
        {"resolution", req.resolution},
        {"duration", req.duration},
        {"generate_audio", req.generateAudio},
    };
    if (!req.ratio.empty()) body["ratio"] = req.ratio;
    if (req.seed >= 0) body["seed"] = req.seed;
    return body.dump();
}

std::string buildGptImageBody(const GptImageRequest& req) {
    json body = {{"prompt", req.prompt}, {"n", 1}, {"output_format", "png"}};  // png keeps the alpha channel
    if (!req.size.empty()) body["size"] = req.size;
    if (!req.quality.empty()) body["quality"] = req.quality;
    if (!req.background.empty()) body["background"] = req.background;
    if (req.images.size() == 1) {
        body["image"] = dataUri(req.images[0]);
    } else if (!req.images.empty()) {
        json arr = json::array();
        for (const auto& img : req.images) arr.push_back(dataUri(img));
        body["image"] = arr;
    }
    return body.dump();
}

// ---------------------------------------------------------------- response handling

namespace {

// Walks any leg's response shape and returns media references in order:
// native Gemini inlineData, OpenAI-style b64_json, data: URIs, and http(s) URLs
// (native seedance re-hosts to GCS; fal/wavespeed/runware hand back provider URLs).
struct Found {
    enum Kind { B64, DataUri, Url } kind;
    std::string value;
};

const std::set<std::string> kUrlKeys = {"url", "fileUri", "uri", "video_url", "image_url", "video", "image"};

void walk(const json& node, const std::string& key, std::vector<Found>& out) {
    static const std::regex mediaExt(R"(\.(png|jpe?g|webp|mp4|webm|mov)(\?|$))", std::regex::icase);
    if (node.is_object()) {
        if (node.contains("b64_json") && node["b64_json"].is_string())
            out.push_back({Found::B64, node["b64_json"].get<std::string>()});
        for (const char* k : {"inlineData", "inline_data"}) {
            if (node.contains(k) && node[k].is_object() && node[k].contains("data") && node[k]["data"].is_string())
                out.push_back({Found::B64, node[k]["data"].get<std::string>()});
        }
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "b64_json" || it.key() == "inlineData" || it.key() == "inline_data") continue;
            walk(it.value(), it.key(), out);
        }
    } else if (node.is_array()) {
        for (const auto& v : node) walk(v, key, out);
    } else if (node.is_string()) {
        const std::string& s = node.get_ref<const std::string&>();
        if (s.rfind("data:", 0) == 0 && s.find(";base64,") != std::string::npos && s.find(";base64,") < 80) {
            out.push_back({Found::DataUri, s});
        } else if (s.rfind("http", 0) == 0 && (kUrlKeys.count(key) || std::regex_search(s, mediaExt))) {
            out.push_back({Found::Url, s});
        }
    }
}

std::string describeError(const HttpResponse& r, const json& body) {
    std::string detail;
    if (body.is_object() && body.contains("detail")) {
        const auto& d = body["detail"];
        if (d.is_string()) {
            detail = d.get<std::string>();
        } else if (d.is_array()) {
            for (const auto& e : d) {
                if (!detail.empty()) detail += "; ";
                std::string loc;
                if (e.contains("loc") && e["loc"].is_array())
                    for (const auto& p : e["loc"]) loc += (loc.empty() ? "" : ".") + (p.is_string() ? p.get<std::string>() : p.dump());
                detail += (loc.empty() ? "" : loc + ": ") + (e.contains("msg") ? e["msg"].get<std::string>() : e.dump());
            }
        } else if (d.is_object() && d.contains("detail") && d["detail"].is_string()) {
            detail = d["detail"].get<std::string>();
        } else {
            detail = d.dump();
        }
    } else if (body.is_object() && body.contains("error")) {
        const auto& e = body["error"];
        detail = e.is_string() ? e.get<std::string>() : (e.contains("message") ? e["message"].get<std::string>() : e.dump());
    } else if (!r.body.empty() && r.body.size() < 400) {
        detail = r.body;
    }
    std::string head;
    switch (r.status) {
        case 401: case 403: head = "API key rejected"; break;
        case 402: head = "Out of credits"; break;
        case 429: head = "Rate limited — try again shortly"; break;
        default: head = "HTTP " + std::to_string(r.status);
    }
    return detail.empty() ? head : head + ": " + detail;
}

Result finish(const HttpResponse& r, const CancelFlag* cancel, const ProgressFn& progress) {
    Result res;
    res.status = r.status;
    auto hdr = [&](const char* k) {
        auto it = r.headers.find(k);
        return it == r.headers.end() ? std::string() : it->second;
    };
    res.requestId = hdr("x-comfy-request-id");
    res.errorType = hdr("x-comfy-error-type");
    res.dropped = hdr("x-comfy-router-dropped-params");
    if (auto c = hdr("x-comfy-credits-used"); !c.empty()) {
        try { res.credits = std::stod(c); } catch (...) {}
    }
    if (r.cancelled) {
        res.cancelled = true;
        res.error = "Cancelled";
        return res;
    }
    if (r.status == 0) {
        res.error = "Network error: " + r.error;
        return res;
    }
    json body = json::parse(r.body, nullptr, false);
    if (r.status != 200) {
        res.error = describeError(r, body);
        return res;
    }
    if (body.is_discarded()) {
        // A non-JSON 200 is the media itself.
        auto [mime, ext] = sniffMedia(r.body);
        if (mime.empty()) {
            res.error = "Unexpected response from Router";
            return res;
        }
        res.media.push_back({r.body, mime, ext});
        res.ok = true;
        return res;
    }
    // Seedance-style task envelopes can report failure inside a 200.
    if (body.is_object() && body.contains("status") && body["status"].is_string()) {
        std::string st = body["status"];
        if (st == "failed" || st == "cancelled" || st == "expired") {
            res.error = "Generation " + st + (body.contains("error") ? ": " + body["error"].dump() : "");
            return res;
        }
    }
    if (body.is_object() && body.contains("framespersecond") && body["framespersecond"].is_number())
        res.fps = body["framespersecond"].get<double>();

    std::vector<Found> found;
    walk(body, "", found);
    std::set<std::string> seen;
    for (const auto& f : found) {
        if (!seen.insert(f.value).second) continue;
        std::string bytes;
        if (f.kind == Found::B64) {
            bytes = base64Decode(f.value);
        } else if (f.kind == Found::DataUri) {
            bytes = base64Decode(f.value.substr(f.value.find(',') + 1));
        } else {
            if (progress) progress("Downloading result…");
            HttpResponse g = httpGet(f.value, 600, cancel);
            if (g.cancelled) {
                res.cancelled = true;
                res.error = "Cancelled";
                return res;
            }
            if (g.status != 200) continue;
            bytes = std::move(g.body);
        }
        auto [mime, ext] = sniffMedia(bytes);
        if (mime.empty()) continue;  // not media (e.g. a thought signature or a page URL)
        res.media.push_back({std::move(bytes), mime, ext});
    }
    if (res.media.empty()) {
        // Gemini returns text instead of an image when it declines a prompt.
        std::string text;
        if (body.contains("candidates"))
            for (const auto& c : body["candidates"])
                if (c.contains("content") && c["content"].contains("parts"))
                    for (const auto& p : c["content"]["parts"])
                        if (p.contains("text") && p["text"].is_string()) text += p["text"].get<std::string>();
        std::string reason;
        if (body.contains("candidates") && !body["candidates"].empty() && body["candidates"][0].contains("finishReason"))
            reason = body["candidates"][0]["finishReason"].get<std::string>();
        res.error = "No media in response";
        if (!reason.empty() && reason != "STOP") res.error += " (" + reason + ")";
        if (!text.empty()) res.error += ": " + text.substr(0, 300);
        return res;
    }
    res.ok = true;
    return res;
}

std::string routerUrl(const std::string& model, const std::string& provider, const std::string& suffix = "") {
    std::string url = std::string(kRouterBase) + "/v2/models/" + model + suffix;
    if (!provider.empty()) url += "?model_provider=" + provider;
    return url;
}

std::string requestUrl(const std::string& model, const std::string& requestId, const std::string& suffix = "") {
    return std::string(kRouterBase) + "/v2/models/" + model + "/requests/" + requestId + suffix;
}

std::string header(const HttpResponse& r, const char* k) {
    auto it = r.headers.find(k);
    return it == r.headers.end() ? std::string() : it->second;
}

double headerCredits(const HttpResponse& r) {
    std::string c = header(r, "x-comfy-credits-used");
    if (c.empty()) return -1;
    try { return std::stod(c); } catch (...) { return -1; }
}

bool transient(const HttpResponse& r) { return r.status == 0 || r.status == 429 || r.status >= 500; }

// Sleeps in short slices so Cancel is responsive. Returns false if cancelled.
bool napFor(double seconds, const CancelFlag* cancel) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(long(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        if (cancel && cancel->load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return !(cancel && cancel->load());
}

// Keep only the video; some legs also echo a poster image.
void keepVideoOnly(Result& res) {
    if (!res.ok) return;
    std::vector<Media> vids;
    for (auto& m : res.media)
        if (m.mime.rfind("video/", 0) == 0) vids.push_back(std::move(m));
    if (vids.empty()) {
        res.ok = false;
        res.error = "Response contained no video";
    }
    res.media = std::move(vids);
}

}  // namespace

HttpResponse httpAuthed(const std::string& method, const std::string& url, const std::string& apiKey, long timeoutSec,
                        const CancelFlag* cancel) {
    std::call_once(gCurlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* h = curl_easy_init();
    if (!h) return HttpResponse{0, {}, "", "curl init failed"};
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    hdrs = curl_slist_append(hdrs, ("X-API-Key: " + apiKey).c_str());
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeoutSec);
    Transfer t;
    HttpResponse r = perform(h, t, cancel);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return r;
}

Result collectRequest(const std::string& model, const std::string& requestId, const std::string& apiKey,
                      bool wantVideo, const CancelFlag* cancel, const RunHooks& hooks) {
    auto progress = [&](const std::string& m) { if (hooks.progress) hooks.progress(m); };
    Result res;
    res.requestId = requestId;
    const auto started = std::chrono::steady_clock::now();
    const double maxWait = 60 * 60;  // an hour; the request id stays saved beyond that
    double delay = 2;
    int failures = 0;
    std::string lastMsg;
    while (true) {
        if (cancel && cancel->load()) {
            httpAuthed("PUT", requestUrl(model, requestId, "/cancel"), apiKey, 10, nullptr);  // best effort
            res.cancelled = true;
            res.error = "Cancelled";
            return res;
        }
        HttpResponse r = httpAuthed("GET", requestUrl(model, requestId, "/status"), apiKey, 30, cancel);
        if (r.cancelled) continue;  // loops back into the cancel branch
        if (r.status == 200) {
            failures = 0;
            json st = json::parse(r.body, nullptr, false);
            std::string status = st.is_object() ? st.value("status", "") : "";
            if (status == "COMPLETED") {
                if (st.contains("error_type") && st["error_type"].is_string() && !st["error_type"].get<std::string>().empty()) {
                    res.errorType = st["error_type"];
                    HttpResponse fake = r;
                    fake.status = 0;
                    std::string d = describeError(fake, st);
                    auto colon = d.find(": ");
                    res.error = (res.errorType == "cancelled" ? "Cancelled" : "Generation failed (" + res.errorType + ")") +
                                (colon != std::string::npos ? d.substr(colon) : "");
                    res.cancelled = res.errorType == "cancelled";
                    return res;
                }
                break;
            }
            std::string msg;
            if (st.is_object() && st.contains("queue_position") && st["queue_position"].is_number_integer() &&
                st["queue_position"].get<int>() > 0)
                msg = "Queued (position " + std::to_string(st["queue_position"].get<int>()) + ")…";
            else if (status == "IN_QUEUE" || status == "QUEUED" || status == "PENDING")
                msg = "Queued…";
            else
                msg = wantVideo ? "Generating video…" : "Generating image…";
            if (msg != lastMsg) progress(lastMsg = msg);
            std::string ra = header(r, "retry-after");
            double wait = delay;
            if (!ra.empty()) {
                try { wait = std::min(15.0, std::max(1.0, std::stod(ra))); } catch (...) {}
            }
            delay = std::min(5.0, delay * 1.3);
            if (!napFor(wait, cancel)) continue;
        } else if (r.status == 404) {
            res.status = 404;
            res.error = "The Router no longer knows request " + requestId + " (expired or wrong key).";
            return res;
        } else if (transient(r) && ++failures < 30) {
            progress("Connection hiccup — still waiting…");
            if (!napFor(std::min(30.0, 2.0 * failures), cancel)) continue;
        } else {
            json body = json::parse(r.body, nullptr, false);
            res.status = r.status;
            res.error = r.status ? describeError(r, body) : "Network error: " + r.error;
            return res;
        }
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() > maxWait) {
            res.error = "Still not finished after an hour. Reopen the project later to keep waiting.";
            res.stillRunning = true;
            return res;
        }
    }
    progress("Downloading result…");
    HttpResponse r;
    for (int attempt = 0; attempt < 6; ++attempt) {
        r = httpAuthed("GET", requestUrl(model, requestId), apiKey, 300, cancel);
        if (!transient(r) || r.cancelled) break;
        if (!napFor(2.0 * (attempt + 1), cancel)) break;
    }
    Result out = finish(r, cancel, hooks.progress);
    out.requestId = requestId;  // the queue id, not the per-HTTP-call id of the download
    if (wantVideo) keepVideoOnly(out);
    return out;
}

Result runModel(const std::string& model, const std::string& provider, const std::string& body,
                const std::string& apiKey, const std::string& idem, bool wantVideo, const CancelFlag* cancel,
                const RunHooks& hooks) {
    auto progress = [&](const std::string& m) { if (hooks.progress) hooks.progress(m); };
    progress("Submitting…");
    // Queued submit: the Router answers once it has accepted the work, so nothing
    // hangs on a multi-minute connection and a lost connection can be resumed.
    HttpResponse sub;
    for (int attempt = 0; attempt < 3; ++attempt) {
        sub = httpPostJson(routerUrl(model, provider, "/requests"), apiKey, body, idem, 120, cancel);
        if (!(sub.status == 0 && !sub.cancelled)) break;  // retry pure transport failures; the key dedupes
        if (!napFor(2.0 * (attempt + 1), cancel)) break;
    }
    if (sub.cancelled) {
        Result res;
        res.cancelled = true;
        res.error = "Cancelled";
        return res;
    }
    if (sub.status == 404 || sub.status == 405) {
        // No queue for this model/leg: fall back to one synchronous call.
        json b = json::parse(sub.body, nullptr, false);
        bool modelMissing = b.is_object() && b.value("error_type", "") == "model_not_found";
        if (!modelMissing) {
            progress(wantVideo ? "Generating video…" : "Generating image…");
            HttpResponse r = httpPostJson(routerUrl(model, provider), apiKey, body, idem, wantVideo ? 1800 : 600, cancel);
            Result res = finish(r, cancel, hooks.progress);
            if (wantVideo) keepVideoOnly(res);
            return res;
        }
    }
    if (sub.status < 200 || sub.status >= 300) return finish(sub, cancel, hooks.progress);  // error path
    json b = json::parse(sub.body, nullptr, false);
    std::string requestId = b.is_object() && b.contains("request_id") && b["request_id"].is_string()
                                ? b["request_id"].get<std::string>() : "";
    if (requestId.empty() || requestId.find('/') != std::string::npos || requestId.size() > 256) {
        Result res;
        res.error = "Router accepted the request but returned no request id";
        return res;
    }
    if (hooks.onSubmitted) hooks.onSubmitted(requestId);
    double submitCredits = headerCredits(sub);
    std::string dropped = header(sub, "x-comfy-router-dropped-params");
    Result res = collectRequest(model, requestId, apiKey, wantVideo, cancel, hooks);
    if (res.credits < 0) res.credits = submitCredits;
    if (res.dropped.empty()) res.dropped = dropped;
    return res;
}


}  // namespace comfy
