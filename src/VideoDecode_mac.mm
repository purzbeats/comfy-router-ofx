// AVFoundation frame extraction so macOS users don't need ffmpeg installed.
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include "Media.h"
#include "Settings.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // tracksWithMediaType: sync API is fine off the main thread

namespace comfy {

bool extractFramesAVFoundation(const std::string& videoPath, const std::string& framesDir, VideoInfo& info,
                               std::string& err, const std::atomic<bool>* cancel,
                               const std::function<void(int, int)>& progress) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:videoPath.c_str()]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
        NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
        if (tracks.count == 0) {
            err = "AVFoundation: no video track";
            return false;
        }
        AVAssetTrack* track = tracks.firstObject;
        double fps = track.nominalFrameRate > 0 ? track.nominalFrameRate : 24.0;
        int total = (int)std::lround(CMTimeGetSeconds(asset.duration) * fps);

        NSError* e = nil;
        AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&e];
        if (!reader) {
            err = std::string("AVFoundation: ") + (e ? e.localizedDescription.UTF8String : "cannot open");
            return false;
        }
        NSDictionary* settings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)};
        AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:settings];
        output.alwaysCopiesSampleData = NO;
        [reader addOutput:output];
        if (![reader startReading]) {
            err = "AVFoundation: startReading failed";
            return false;
        }
        int n = 0, w = 0, h = 0;
        Rgba8 frame;
        while (true) {
            if (cancel && cancel->load()) {
                [reader cancelReading];
                err = "Cancelled";
                return false;
            }
            CMSampleBufferRef sb = [output copyNextSampleBuffer];
            if (!sb) break;
            CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sb);
            if (pb) {
                CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
                w = (int)CVPixelBufferGetWidth(pb);
                h = (int)CVPixelBufferGetHeight(pb);
                size_t bpr = CVPixelBufferGetBytesPerRow(pb);
                const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pb));
                frame.w = w;
                frame.h = h;
                frame.px.resize((size_t)w * h * 4);
                for (int y = 0; y < h; ++y) {
                    const uint8_t* s = base + y * bpr;
                    uint8_t* d = frame.px.data() + (size_t)y * w * 4;
                    for (int x = 0; x < w; ++x, s += 4, d += 4) {
                        d[0] = s[2];
                        d[1] = s[1];
                        d[2] = s[0];
                        d[3] = 255;
                    }
                }
                CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
                std::string jpg = encodeJpeg(frame, 95);
                std::string werr;
                if (!writeFileAtomic(joinPath(framesDir, frameFileName(n)), jpg, &werr)) {
                    CFRelease(sb);
                    [reader cancelReading];
                    err = werr;
                    return false;
                }
                ++n;
                if (progress) progress(n, total);
            }
            CFRelease(sb);
        }
        if (reader.status == AVAssetReaderStatusFailed || n == 0) {
            err = std::string("AVFoundation: decode failed") +
                  (reader.error ? std::string(" — ") + reader.error.localizedDescription.UTF8String : "");
            return false;
        }
        info.frames = n;
        info.fps = fps;
        info.w = w;
        info.h = h;
        return true;
    }
}

}  // namespace comfy
