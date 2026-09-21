#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_COMMIT="$(tr -d '[:space:]' < "$ROOT_DIR/BASE_COMMIT")"
TARGET_COMMIT="$(tr -d '[:space:]' < "$ROOT_DIR/HEAD_COMMIT")"
[[ "$BASE_COMMIT" =~ ^[0-9a-fA-F]{40}$ ]] || { echo "error: invalid BASE_COMMIT" >&2; exit 1; }
[[ "$TARGET_COMMIT" =~ ^[0-9a-fA-F]{7,40}$ ]] || { echo "error: invalid HEAD_COMMIT" >&2; exit 1; }
for patch in "$ROOT_DIR"/patches/*.patch; do [[ -s "$patch" ]] || { echo "error: empty patch $patch" >&2; exit 1; }; done
if [[ "${1:-}" == "--chromium-dir" ]]; then
  [[ -n "${2:-}" ]] || exit 2
  CHROMIUM_DIR="$2"
  [[ "$(git -C "$CHROMIUM_DIR" rev-parse HEAD)" == "$BASE_COMMIT" ]] || { echo "error: Chromium checkout is not at BASE_COMMIT" >&2; exit 1; }
  [[ -z "$(git -C "$CHROMIUM_DIR" status --porcelain)" ]] || { echo "error: Chromium checkout is dirty" >&2; exit 1; }
  for patch in "$ROOT_DIR"/patches/*.patch; do git -C "$CHROMIUM_DIR" apply --check "$patch"; done
fi
echo "Patchset metadata is valid."
echo "Base: $BASE_COMMIT"
echo "Target marker: $TARGET_COMMIT"
echo "Development entry point: chrome/test/data/webui/glic/test_client/start_glic_dev.sh"
