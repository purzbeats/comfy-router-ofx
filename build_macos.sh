#!/usr/bin/env bash
# Builds ComfyRouter.ofx.bundle (universal arm64 + x86_64) with the Xcode command-line tools.
#   ./build_macos.sh            → build/ComfyRouter.ofx.bundle
#   ./build_macos.sh install    → also copies it to /Library/OFX/Plugins (asks for sudo)
set -euo pipefail
cd "$(dirname "$0")"

ARCHS=${ARCHS:-"arm64 x86_64"}
MIN_MACOS=${MIN_MACOS:-11.0}
OUT=build
BUNDLE="$OUT/ComfyRouter.ofx.bundle"
OBJ="$OUT/obj"
mkdir -p "$OBJ" "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"

ARCH_FLAGS=()
for a in $ARCHS; do ARCH_FLAGS+=(-arch "$a"); done
# The Media Pool import script is embedded as a C++ raw string.
mkdir -p "$OUT/gen"
{ printf 'R"LUA('; cat src/ImportScript.lua; printf ')LUA"\n'; } > "$OUT/gen/ImportScript.inc.tmp"
cmp -s "$OUT/gen/ImportScript.inc.tmp" "$OUT/gen/ImportScript.inc" 2>/dev/null && rm "$OUT/gen/ImportScript.inc.tmp" \
  || mv "$OUT/gen/ImportScript.inc.tmp" "$OUT/gen/ImportScript.inc"

COMMON=(-O2 -g0 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden "-mmacosx-version-min=$MIN_MACOS" "${ARCH_FLAGS[@]}"
        -Ithird_party/openfx/include -Ithird_party/openfx/Support/include -Ithird_party -Isrc -I"$OUT/gen"
        -Wall -Wno-unused-parameter -Wno-deprecated-declarations)
CXX=${CXX:-clang++}

SOURCES=(src/ComfyRouterPlugin.cpp src/RouterClient.cpp src/Media.cpp src/Jobs.cpp src/Settings.cpp src/ResolveBridge.cpp)
SUPPORT=(third_party/openfx/Support/Library/*.cpp)

objs=()
compile() {  # src out extra-flags...
  local src=$1 out=$2; shift 2
  if [[ ! -f "$out" || "$src" -nt "$out" || src/RouterClient.h -nt "$out" || src/Jobs.h -nt "$out" || src/Media.h -nt "$out" \
        || src/ResolveBridge.h -nt "$out" || "$OUT/gen/ImportScript.inc" -nt "$out" ]]; then
    echo "  CXX $src"
    "$CXX" "${COMMON[@]}" "$@" -c "$src" -o "$out" &
  fi
  objs+=("$out")
}
for s in "${SOURCES[@]}"; do compile "$s" "$OBJ/$(basename "${s%.*}").o" -std=c++17; done
for s in "${SUPPORT[@]}"; do compile "$s" "$OBJ/support_$(basename "${s%.*}").o" -std=c++17 -w; done
compile src/VideoDecode_mac.mm "$OBJ/VideoDecode_mac.o" -std=c++17 -fobjc-arc
wait

echo "  LINK $BUNDLE"
"$CXX" "${ARCH_FLAGS[@]}" "-mmacosx-version-min=$MIN_MACOS" -bundle -o "$BUNDLE/Contents/MacOS/ComfyRouter.ofx" "${objs[@]}" \
  -Wl,-exported_symbols_list,resources/exports.txt -Wl,-dead_strip \
  -lcurl -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation -framework CoreFoundation
cp resources/Info.plist "$BUNDLE/Contents/Info.plist"
# Ad-hoc sign so Gatekeeper / hardened hosts will load it on Apple silicon.
codesign --force --sign - "$BUNDLE" >/dev/null 2>&1 || true
echo "Built $BUNDLE"

if [[ "${1:-}" == "install" ]]; then
  DEST=/Library/OFX/Plugins
  echo "Installing to $DEST (sudo)…"
  sudo mkdir -p "$DEST"
  sudo rm -rf "$DEST/ComfyRouter.ofx.bundle"
  sudo cp -R "$BUNDLE" "$DEST/"
  echo "Installed. Restart DaVinci Resolve, then find it under OpenFX → Comfy → Comfy Router."
fi
