#!/usr/bin/env bash
set -euo pipefail

usage(){ echo "Usage: $0 --chromium-dir PATH"; exit 2; }
CHROMIUM_DIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --chromium-dir) [[ $# -ge 2 ]] || usage; CHROMIUM_DIR="$2"; shift 2;;
    -h|--help) usage;;
    *) usage;;
  esac
done
[[ -n "$CHROMIUM_DIR" ]] || usage
[[ -d "$CHROMIUM_DIR/.git" || -f "$CHROMIUM_DIR/.git" ]] || { echo "error: not a Git checkout: $CHROMIUM_DIR" >&2; exit 1; }
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_COMMIT="$(tr -d '[:space:]' < "$ROOT_DIR/BASE_COMMIT")"
cd "$CHROMIUM_DIR"
[[ -z "$(git status --porcelain)" ]] || { echo "error: Chromium checkout has uncommitted changes" >&2; exit 1; }
[[ "$(git rev-parse HEAD)" == "$BASE_COMMIT" ]] || { echo "error: expected Chromium HEAD $BASE_COMMIT" >&2; exit 1; }
for patch in "$ROOT_DIR"/patches/3147-*.patch "$ROOT_DIR"/patches/3148-*.patch "$ROOT_DIR"/patches/3149-*.patch; do git am --3way "$patch"; done
git apply --index "$ROOT_DIR/patches/3150-Simulachrome-m144-apply-delta.patch"
echo "Patchset applied. Run from this Chromium checkout: chrome/test/data/webui/glic/test_client/start_glic_dev.sh"
