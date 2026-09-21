#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 --chromium-dir PATH"
  exit 2
}

CHROMIUM_DIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --chromium-dir)
      [[ $# -ge 2 ]] || usage
      CHROMIUM_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      ;;
    *)
      usage
      ;;
  esac
done

[[ -n "$CHROMIUM_DIR" ]] || usage
[[ -d "$CHROMIUM_DIR/.git" || -f "$CHROMIUM_DIR/.git" ]] || {
  echo "error: not a Git checkout: $CHROMIUM_DIR" >&2
  exit 1
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_COMMIT="$(tr -d '[:space:]' < "$ROOT_DIR/BASE_COMMIT")"
cd "$CHROMIUM_DIR"

[[ -z "$(git status --porcelain)" ]] || {
  echo "error: Chromium checkout has uncommitted changes" >&2
  exit 1
}

CURRENT_COMMIT="$(git rev-parse HEAD)"
[[ "$CURRENT_COMMIT" == "$BASE_COMMIT" ]] || {
  echo "error: expected Chromium HEAD $BASE_COMMIT, found $CURRENT_COMMIT" >&2
  exit 1
}

for patch in \
  "$ROOT_DIR/patches/3147-Add-dashboard-WebUI-resources.patch" \
  "$ROOT_DIR/patches/3148-Simulachrome-actor-dashboard-playback.patch" \
  "$ROOT_DIR/patches/3149-Simulachrome-dashboard-glic-phase-A-B.patch"; do
  echo "Applying $(basename "$patch") with git am"
  git am --3way "$patch"
done

PATCH_3150="$ROOT_DIR/patches/3150-Simulachrome-m144-apply-delta.patch"
echo "Applying $(basename "$PATCH_3150") with git apply"
git apply --index "$PATCH_3150"

echo "Patchset applied successfully."
echo "Current Chromium revision: $(git rev-parse HEAD)"
echo "Note: patch 3150 is applied to the index/worktree and is not committed by this script."
