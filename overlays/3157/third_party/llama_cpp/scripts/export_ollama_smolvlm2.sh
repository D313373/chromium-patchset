#!/bin/bash
# Export Ollama ahmadwaqar/smolvlm2-agentic-gui blobs to named GGUFs.
# Does NOT copy into Chromium src/ — use a pack dir or app resources.
set -euo pipefail
OLLAMA_ROOT="${OLLAMA_MODELS:-$HOME/.ollama/models}"
# Resolve symlink (your machine points ~/.ollama/models → chromium_129/... but
# current blobs live under $OLLAMA_MODELS=/Users/.../0llamaModels)
OUT="${1:-$HOME/simulachrome_models}"
mkdir -p "$OUT"
MODEL="$OLLAMA_ROOT/blobs/sha256-ead40252751bf78f56c7a44592dd1c9fb315da10dd1203aa5feb200668aab6eb"
MMPROJ="$OLLAMA_ROOT/blobs/sha256-3296c1e6fe63fc052770c3e0b762438debb0f6c071435c0ac29b950d6fd013d3"
ln -f "$MODEL" "$OUT/smolvlm2-agentic-gui.gguf" 2>/dev/null || ln -sf "$MODEL" "$OUT/smolvlm2-agentic-gui.gguf"
ln -f "$MMPROJ" "$OUT/smolvlm2-mmproj.gguf" 2>/dev/null || ln -sf "$MMPROJ" "$OUT/smolvlm2-mmproj.gguf"
echo "Exported to $OUT"
ls -lh "$OUT"/smolvlm2-*.gguf
