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

for patch in \
  "$ROOT_DIR/patches/3151-Script-agent-pagemap-split-native-nav.patch" \
  "$ROOT_DIR/patches/3152-Script-export-PII-credential-guard.patch" \
  "$ROOT_DIR/patches/3153-Export-secret-auto-migrate.patch"; do
  echo "Applying $(basename "$patch") with git am"
  git am --3way "$patch"
done

echo "Applying overlay 3154 (coordinate click + screenshot grid)"
cp "$ROOT_DIR/overlays/3154/ollama_client.ts" \
  "$CHROMIUM_DIR/chrome/test/data/webui/glic/test_client/ollama_client.ts"
cp "$ROOT_DIR/overlays/3154/serve.py" \
  "$CHROMIUM_DIR/chrome/test/data/webui/glic/test_client/serve.py"
git -C "$CHROMIUM_DIR" add \
  chrome/test/data/webui/glic/test_client/ollama_client.ts \
  chrome/test/data/webui/glic/test_client/serve.py

echo "Applying overlay 3155 (running-tip sync)"
if command -v rsync >/dev/null 2>&1; then
  rsync -a "$ROOT_DIR/overlays/3155/chrome/" "$CHROMIUM_DIR/chrome/"
else
  # Fallback without rsync: copy each file under overlays/3155/chrome
  while IFS= read -r -d '' src; do
    rel="${src#"$ROOT_DIR/overlays/3155/"}"
    mkdir -p "$CHROMIUM_DIR/$(dirname "$rel")"
    cp -f "$src" "$CHROMIUM_DIR/$rel"
  done < <(find "$ROOT_DIR/overlays/3155/chrome" -type f -print0)
fi
git -C "$CHROMIUM_DIR" add -A chrome/browser/resources/dashboard   chrome/browser/ui/webui/dashboard   chrome/test/data/webui/glic/test_client/start_glic_dev.sh   chrome/test/data/webui/glic/test_client/ollama_client.ts   chrome/test/data/webui/glic/test_client/serve.py || true

echo "Applying overlay 3156 (bake Glic/Simulachrome product defaults + packaged agent UI)"
if command -v rsync >/dev/null 2>&1; then
  rsync -a "$ROOT_DIR/overlays/3156/chrome/" "$CHROMIUM_DIR/chrome/"
else
  while IFS= read -r -d '' src; do
    rel="${src#"$ROOT_DIR/overlays/3156/"}"
    mkdir -p "$CHROMIUM_DIR/$(dirname "$rel")"
    cp -f "$src" "$CHROMIUM_DIR/$rel"
  done < <(find "$ROOT_DIR/overlays/3156/chrome" -type f -print0)
fi
git -C "$CHROMIUM_DIR" add -A \
  chrome/browser/ui/webui/dashboard \
  chrome/browser/ui/webui/simulachrome_agent \
  chrome/browser/ui/webui/chrome_web_ui_configs.cc \
  chrome/browser/chrome_browser_main.cc \
  chrome/browser/glic \
  chrome/browser/resources/dashboard \
  chrome/test/data/webui/glic/test_client || true

echo "Patchset applied successfully."
echo "Current Chromium revision: $(git rev-parse HEAD)"
echo "Note: patches 3150/3154/3155/3156 update the index/worktree and are not committed by this script."
echo "Next: build Chromium, package agent UI, then double-click Chromium.app or run:"
echo "  $CHROMIUM_DIR/chrome/test/data/webui/glic/test_client/start_glic_dev.sh"
echo "Binary defaults: chrome://simulachrome-agent guest, Glic features, Scripts/Headless under SIMULACHROME_HOME."
echo "Remote debugging is off unless Settings → remoteDebuggingEnabled (or SIMULACHROME_REMOTE_DEBUGGING=1)."
