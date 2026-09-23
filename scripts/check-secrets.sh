#!/usr/bin/env bash
# Fails if anything that looks like a Comfy API key (or a filled-in COMFY_API_KEY=) is present.
#   scripts/check-secrets.sh            → scan staged changes (pre-commit)
#   scripts/check-secrets.sh --all      → scan every tracked file (CI)
set -euo pipefail
PATTERN='comfyui-[A-Za-z0-9_-]{16,}|COMFY_API_KEY[[:space:]]*=[[:space:]]*["'"'"']?[A-Za-z0-9_-]{16,}'
if [[ "${1:-}" == "--all" ]]; then
  hits=$(git grep -nIE "$PATTERN" -- . ':!scripts/check-secrets.sh' || true)
else
  hits=$(git diff --cached -U0 --no-color -- . ':!scripts/check-secrets.sh' | grep -E '^\+' | grep -nE "$PATTERN" || true)
fi
if [[ -n "$hits" ]]; then
  echo "✖ Possible Comfy API key detected — refusing to continue:" >&2
  echo "$hits" | sed -E 's/(comfyui-[A-Za-z0-9_-]{4})[A-Za-z0-9_-]+/\1…REDACTED/g' >&2
  exit 1
fi
echo "✓ no API keys found"
