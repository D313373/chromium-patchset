#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 [--chromium-dir PATH]"
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

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_COMMIT="$(tr -d '[:space:]' < "$ROOT_DIR/BASE_COMMIT")"
TARGET_COMMIT="$(tr -d '[:space:]' < "$ROOT_DIR/HEAD_COMMIT")"

[[ "$BASE_COMMIT" =~ ^[0-9a-fA-F]{40}$ ]] || {
  echo "error: BASE_COMMIT is not a full hexadecimal commit SHA" >&2
  exit 1
}
[[ "$TARGET_COMMIT" =~ ^[0-9a-fA-F]{7,40}$ ]] || {
  echo "error: HEAD_COMMIT is not a valid abbreviated/full commit SHA" >&2
  exit 1
}

PATCHES=(
  "$ROOT_DIR/patches/3147-Add-dashboard-WebUI-resources.patch"
  "$ROOT_DIR/patches/3148-Simulachrome-actor-dashboard-playback.patch"
  "$ROOT_DIR/patches/3149-Simulachrome-dashboard-glic-phase-A-B.patch"
  "$ROOT_DIR/patches/3150-Simulachrome-m144-apply-delta.patch"
)
for patch in "${PATCHES[@]}"; do
  [[ -s "$patch" ]] || {
    echo "error: missing or empty patch: $patch" >&2
    exit 1
  }
done

if [[ -n "$CHROMIUM_DIR" ]]; then
  [[ -d "$CHROMIUM_DIR/.git" || -f "$CHROMIUM_DIR/.git" ]] || {
    echo "error: not a Git checkout: $CHROMIUM_DIR" >&2
    exit 1
  }
  CURRENT_COMMIT="$(git -C "$CHROMIUM_DIR" rev-parse HEAD)"
  [[ "$CURRENT_COMMIT" == "$BASE_COMMIT" ]] || {
    echo "error: --chromium-dir is at $CURRENT_COMMIT, expected $BASE_COMMIT" >&2
    exit 1
  }
  [[ -z "$(git -C "$CHROMIUM_DIR" status --porcelain)" ]] || {
    echo "error: --chromium-dir has uncommitted changes" >&2
    exit 1
  }

  echo "Checking patch applicability in $CHROMIUM_DIR"
  for patch in "${PATCHES[@]:0:3}"; do
    git -C "$CHROMIUM_DIR" apply --check "$patch"
  done
  git -C "$CHROMIUM_DIR" apply --check "${PATCHES[3]}"
  echo "All patches pass git apply --check against the base checkout."
fi

echo "Patchset metadata is valid."
echo "Base revision: $BASE_COMMIT"
echo "Target marker: $TARGET_COMMIT"
printf 'Patch: %s\n' "${PATCHES[@]}"
