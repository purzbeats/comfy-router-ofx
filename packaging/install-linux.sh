#!/bin/sh
# Installs Comfy Router into DaVinci Resolve. Needs libcurl (installed on most distros).
set -e
cd "$(dirname "$0")"
DEST=/usr/OFX/Plugins
echo "Installing Comfy Router to $DEST …"
sudo mkdir -p "$DEST"
sudo rm -rf "$DEST/ComfyRouter.ofx.bundle"
sudo cp -R ComfyRouter.ofx.bundle "$DEST/"
echo "Done. Restart DaVinci Resolve, then find it under OpenFX → Comfy → Comfy Router."
