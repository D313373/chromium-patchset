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
  "$ROOT_DIR/patches/3151-Script-agent-pagemap-split-native-nav.patch"
  "$ROOT_DIR/patches/3152-Script-export-PII-credential-guard.patch"
  "$ROOT_DIR/patches/3153-Export-secret-auto-migrate.patch"
  "$ROOT_DIR/patches/3154-Glic-agent-coordinate-click-screenshot-grid.patch"
  "$ROOT_DIR/patches/3155-Sync-running-tip-autoheal-anthropic-launch-dirs.patch"
)
OVERLAYS=(
  "$ROOT_DIR/overlays/3154/ollama_client.ts"
  "$ROOT_DIR/overlays/3154/serve.py"
  "$ROOT_DIR/overlays/3154/APPLY.txt"
  "$ROOT_DIR/overlays/3155/APPLY.txt"
  "$ROOT_DIR/overlays/3155/chrome/test/data/webui/glic/test_client/start_glic_dev.sh"
  "$ROOT_DIR/overlays/3155/chrome/browser/resources/dashboard/settings.js"
  "$ROOT_DIR/overlays/3155/chrome/browser/resources/dashboard/agent/dashboard_auto_heal_agent_runner.js"
)
for patch in "${PATCHES[@]}"; do
  [[ -s "$patch" ]] || {
    echo "error: missing or empty patch: $patch" >&2
    exit 1
  }
done
for overlay in "${OVERLAYS[@]}"; do
  [[ -s "$overlay" ]] || {
    echo "error: missing or empty overlay: $overlay" >&2
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
  for patch in "${PATCHES[@]:4:3}"; do
    git -C "$CHROMIUM_DIR" apply --check "$patch"
  done
  echo "Patches 3147-3153 pass git apply --check against the base checkout."
  echo "Note: 3154/3155 are overlay copies (see overlays/*/APPLY.txt), not context hunk patches."
fi

echo "Patchset metadata is valid."
echo "Base revision: $BASE_COMMIT"
echo "Target marker: $TARGET_COMMIT"
printf 'Patch: %s\n' "${PATCHES[@]}"
printf 'Overlay: %s\n' "${OVERLAYS[@]}"
