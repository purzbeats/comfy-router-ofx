#!/bin/bash
# Double-click to install Comfy Router into DaVinci Resolve (asks for your password).
set -e
cd "$(dirname "$0")"
DEST=/Library/OFX/Plugins
echo "Installing Comfy Router to $DEST …"
sudo mkdir -p "$DEST"
sudo rm -rf "$DEST/ComfyRouter.ofx.bundle"
sudo cp -R ComfyRouter.ofx.bundle "$DEST/"
# Downloaded files are quarantined; the plugin is ad-hoc signed, not notarized.
sudo xattr -dr com.apple.quarantine "$DEST/ComfyRouter.ofx.bundle" 2>/dev/null || true
echo "Done. Restart DaVinci Resolve, then find it under OpenFX → Comfy → Comfy Router."
read -n 1 -s -r -p "Press any key to close."
