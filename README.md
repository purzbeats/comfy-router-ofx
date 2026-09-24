# Comfy Router for DaVinci Resolve

An OpenFX plugin that generates media inside Resolve through the [Comfy API Router](https://comfy.org/platform/router/), billed to your own Comfy API key.

| Model | Router ID | What it's for |
|---|---|---|
| **Nano Banana 2** | `vertexai/gemini-3.1-flash-image` | Stills: text-to-image, or edits of the current frame and reference images |
| **GPT Image 2.5** (Flare / Sunburst) | `openai/gpt-image-2.5-flare`, `openai/gpt-image-2.5-sunburst` | Stills with a **transparent background**: titles, lower thirds, stickers, overlay elements |
| **Seedance 2.5** | `byteplus/dreamina-seedance-2-5-260628` | Video: text-to-video, first-frame or first-and-last-frame, reference-image video, with optional generated audio |

The effect shows the result right in the viewer. Stills are fitted to the frame. Video plays from the frame where you pressed Generate. Every result is also saved to an output folder as a normal PNG or MP4, so you can drag it into the Media Pool. That's also how you get Seedance's audio, because OFX effects can't output audio.

## Install

### Download a build (easiest)

Grab the zip for your platform from [**Releases**](https://github.com/purzbeats/comfy-router-ofx/releases/latest). GitHub Actions builds every commit, so the newest builds are also under [Actions → Build](https://github.com/purzbeats/comfy-router-ofx/actions/workflows/ci.yml) → a run → **Artifacts**.

| Platform | Zip | Install |
|---|---|---|
| macOS 11+ (Apple silicon & Intel) | `ComfyRouter-macOS-universal.zip` | Double-click `install-macos.command` |
| Windows 10/11 x64 | `ComfyRouter-Windows-x64.zip` | Right-click `install-windows.bat` → **Run as administrator** |
| Linux x86_64 (Rocky 8+ and newer) | `ComfyRouter-Linux-x86_64.zip` | `./install-linux.sh` |

The macOS build is ad-hoc signed, not notarized. The installer clears the download quarantine flag so Resolve will load it.

To cut a release: `git tag v1.0.1 && git push origin v1.0.1`. The Build workflow then attaches all three zips to a new GitHub Release.

### Build from source: macOS (Apple silicon or Intel)

```sh
./build_macos.sh install      # builds a universal bundle, copies it to /Library/OFX/Plugins (sudo)
```

Restart Resolve. You need the Xcode command-line tools (`xcode-select --install`), but not CMake or Homebrew. Video frames are decoded with AVFoundation, so ffmpeg isn't needed either.

### Build from source: Windows

```bat
vcpkg install curl:x64-windows-static
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
xcopy /E /I build\ComfyRouter.ofx.bundle "C:\Program Files\Common Files\OFX\Plugins\ComfyRouter.ofx.bundle"
```

In-effect video playback on Windows and Linux uses [ffmpeg](https://ffmpeg.org/download.html). The plugin looks in common install locations; if yours is elsewhere, set **Settings → FFmpeg Path**.

### Build from source: Linux

```sh
sudo apt install libcurl4-openssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
sudo cp -R build/ComfyRouter.ofx.bundle /usr/OFX/Plugins/
```

## Use it

1. Add it to your timeline. You don't need a clip; pick whichever route fits:
   - **Comfy Router Generator** (Effects → Generators, or OpenFX → Comfy): drag it onto any track, like a Solid Color. Transparent GPT Image results key over the tracks below.
   - **On an Adjustment Clip** (Effects → Toolbox → Effects → Adjustment Clip): drop the Adjustment Clip on a track above your footage, then drag **Comfy Router** onto it. "Current frame" inputs then see everything on the tracks below.
   - **On a clip** (OpenFX → Comfy → Comfy Router): to generate from, edit or animate that clip's frames.
2. **Settings → API Key**: paste your Comfy API key (`comfyui-…`) and press Enter. You only do this once per computer (see [Your API key](#your-api-key)).
3. Pick a model in **Generate**, write a **Prompt**, and press **Generate**.
4. A progress bar shows in the viewer while the job runs. Stills take about 10 seconds; Seedance takes about 2–3 minutes. When it's done, move the playhead or press **Refresh Viewer**.

   Generations are queued on the Router, so they survive a dropped connection. If you quit Resolve mid-generation, the result is collected when you reopen the project, and the credits you spent aren't wasted.

### Controls

| Section | Controls |
|---|---|
| **Image · Nano Banana 2** | Aspect Ratio (Match Timeline picks the closest supported ratio), Image Size 1K/2K/4K, Image Input (none, current frame, reference images) |
| **Image · GPT Image 2.5** | Variant (Flare/Sunburst), Size (Match Timeline = timeline aspect at a 1536 px long edge), Quality, **Background: Transparent/Opaque/Auto**, Image Input |
| **Video · Seedance 2.5** | Resolution 480p/720p/1080p, Duration 4–15 s, Aspect Ratio, Generate Audio, Seed, Image Input: first frame from the current frame or a file, first + last frame, or reference images |
| **Reference Images** | Two image files used by the Image Input options |
| **Placement** | Fit / Fill / Stretch, **Behind Result** (Source / Black / Transparent), Opacity, **Solid Alpha**, Video Start Frame, After Video Ends (hold / loop / show source) |
| **Buttons** | Generate, Cancel, Refresh Viewer, **Import Generated Media** |
| **Settings** | API Key, Forget API Key, Provider (Router leg: Default / fal / WaveSpeed / Runware), Output Folder, Reveal Output Folder, FFmpeg Path |

**Transparent elements.** Choose **Image · GPT Image 2.5** with **Background: Transparent**. Prompt for an isolated element, for example: *"a glossy gold 3D star badge with the word COMFY, isolated element, no background"*.

- The saved PNG keeps its alpha, so you can import it and use it anywhere.
- In the effect, **Behind Result** decides what shows through the transparent pixels. **Source** is your clip, or on a generator, the tracks below.
- GPT Image returns about 99% alpha inside "solid" areas. **Solid Alpha** (on by default) snaps those pixels to fully opaque so the clip doesn't faintly show through.

**Image-to-video from a still.** Generate a still with Nano Banana 2, set **Reference Image 1** to the saved PNG, then generate Seedance with **First Frame: Reference Image 1**. To continue the shot under the playhead, use **First Frame: Current Frame** instead. The video starts at the frame where you pressed Generate.

### Import generations into the Media Pool

Resolve doesn't let effects touch the Media Pool, so imports go through a small Lua script that the plugin installs for you: **Workspace → Scripts → Comfy Router - Import Generated Media**. It works in Resolve and Resolve Studio.

- It creates a **Comfy Router** bin and imports every generation that isn't in the Media Pool yet, oldest first. Stills keep their alpha; Seedance MP4s keep their audio.
- It skips files that are already in the pool, so it's safe to run any time.
- It scans every output folder the plugin has used.

The plugin writes the script on load, so it appears in the menu after one Resolve restart. It lives in `~/Library/Application Support/Blackmagic Design/DaVinci Resolve/Fusion/Scripts/Utility/` on macOS, `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\Fusion\Scripts\Utility\` on Windows, and `~/.local/share/DaVinciResolve/Fusion/Scripts/Utility/` on Linux.

The effect's **Import Generated Media** button runs the same script through Resolve's bundled `fuscript`. That only reaches a running Resolve in **Studio** with **Preferences → System → General → External scripting using: Local**. Otherwise the Status line points you to the Scripts menu entry.

### Where results go

```
~/Movies/ComfyRouter/                     (~/Videos/ComfyRouter on Windows/Linux; change in Settings)
  ComfyRouter_NanoBanana2_<id>.png
  ComfyRouter_GPTImage25_<id>.png         alpha preserved
  ComfyRouter_Seedance25_<id>.mp4         with audio
  .comfyrouter/meta/<id>.json             what the effect reads to redraw a result
  .comfyrouter/frames/<id>/000001.jpg …   decoded frames for in-effect playback
```

The project stores only the job ID and folder, never the media or your key. When you reopen a project, each effect finds its result again without spending credits. If you move the folder, point **Output Folder** at the new location.

## Your API key

- You type the key into the plugin once. It's saved to a per-user file, readable only by you:
  - macOS: `~/Library/Application Support/ComfyRouterOFX/config.json`
  - Windows: `%APPDATA%\ComfyRouterOFX\config.json`
  - Linux: `~/.config/comfy-router-ofx/config.json`
- The **API Key field clears itself** right after saving, so the key is never stored in your Resolve project. Sharing a `.drp` or project archive doesn't leak it.
- If no key is saved, the plugin uses the `COMFY_API_KEY` environment variable.
- **Forget API Key** deletes the saved key.

Requests go only to `https://api.comfy.org`. Result downloads also go to the storage host the Router returns, for example Google Cloud Storage for Seedance.

## How it works

- **The Router call.** Each generation uses the Router's queued API with an `X-API-Key` header and the model's native body: Gemini `contents` for Nano Banana, OpenAI Images for GPT Image, BytePlus `content[]` for Seedance.
  1. Submit with `POST https://api.comfy.org/v2/models/{model}/requests`.
  2. Poll `…/requests/{id}/status` until it reads `COMPLETED`.
  3. Collect the result with `GET …/requests/{id}`.
- **Why queued, not synchronous.** No connection is held open for minutes. A long synchronous Seedance call can be dropped mid-generation, and the Router won't replay a finished result for the same `Idempotency-Key`.
- **Saving and resuming.** The Router's request ID is written to `.comfyrouter/meta/<id>.pending.json` the moment the job is accepted. Any interruption resumes from there. **Cancel** sends `PUT …/cancel`.
- **Fallback and background work.** If a model or leg has no queue, the plugin falls back to one synchronous `POST /v2/models/{model}`. Everything runs on a background thread, so Resolve's UI never blocks. The job ID doubles as the `Idempotency-Key`, so an automatically retried submit isn't charged twice.
- **Input images.** The current frame and any reference images are sent inline as base64 or `data:` URIs. They're downscaled to at most 2048 px, and upscaled to at least 300 px for Seedance, which rejects smaller inputs.
- **Response parsing.** Responses are parsed leg-agnostically: Gemini `inlineData`, OpenAI `b64_json`, `data:` URIs, or provider URLs, which are downloaded immediately.
- **Status reporting.** `X-Comfy-Router-Dropped-Params` shows up in the Status line, and so does `X-Comfy-Credits-Used` when the Router sends it (currently only on synchronous calls).
- **Rendering.** Output is premultiplied RGBA at 8-bit, 16-bit or float, supports tiles and multi-resolution, and works in filter, general and generator contexts.

## Develop

```sh
./build_macos.sh                            # → build/ComfyRouter.ofx.bundle
clang -Ithird_party/openfx/include tools/ofx_probe.c -o build/ofx_probe
build/ofx_probe build/ComfyRouter.ofx.bundle/Contents/MacOS/ComfyRouter.ofx   # loads it like a host

# CLI harness: same pipeline as the effect, no Resolve needed. Spends credits.
clang++ -std=c++17 -Ithird_party -Isrc tools/router_cli.cpp src/{RouterClient,Media,Jobs,Settings}.cpp \
  -x objective-c++ -fobjc-arc src/VideoDecode_mac.mm -lcurl \
  -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation -o build/router_cli
export COMFY_API_KEY=comfyui-…        # or save it from the plugin first
build/router_cli gpt  "gold star badge, isolated" --bg transparent --size 1536x864 --quality low
build/router_cli nb2  "orange sunset sky behind the subject" --in photo.png --aspect 16:9
build/router_cli sd25 "slow push-in" --in still.png --res 480p --dur 4
build/router_cli nb2  "anything" --dry  # print the request body without sending it
build/router_cli resume <job-id>        # collect a job whose process was killed mid-generation
```

**Keeping keys out of git.**

- `scripts/check-secrets.sh` runs as a pre-commit hook. Enable it with `git config core.hooksPath .githooks`.
- CI runs the same scan on every tracked file and on the full history.
- `.gitignore` excludes `.env`, `config.json` and generated output.

Third-party code in `third_party/` is used under its own license:

- [OpenFX](https://github.com/AcademySoftwareFoundation/openfx) (BSD-3-Clause)
- [nlohmann/json](https://github.com/nlohmann/json) (MIT)
- [stb](https://github.com/nothings/stb) (public domain / MIT)

## Known limits

- **Audio.** Seedance audio is in the saved MP4 only; OFX can't output audio.
- **Refresh.** Resolve redraws an effect only when something changes. When a job finishes, move the playhead or press **Refresh Viewer**.
- **Color.** Frames are sent and results drawn as display-referred 0–1 values. That's correct for Rec.709 and sRGB timelines. On wide-gamut or scene-linear timelines, convert around the effect.
- **Credits.** The Router's queued API doesn't report credits used yet, so the Status line usually won't show a cost. Check usage on your Comfy account.
- **Provider legs.** Legs other than Default can behave differently. For example, WaveSpeed and Runware can't turn Seedance audio off, and WaveSpeed Seedance is 480p and at most 5 s. See the Status line for anything the Router dropped.
