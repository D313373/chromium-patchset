#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -d "$ROOT/src/.git" ]]; then
  echo "already cloned: $ROOT/src"
  exit 0
fi
git clone --depth 1 https://github.com/ggerganov/llama.cpp.git "$ROOT/src"
echo "cloned into $ROOT/src"
