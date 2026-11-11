# llama.cpp for Simulachrome

## Clone (required before first build)

```bash
cd third_party/llama_cpp
git clone https://github.com/ggerganov/llama.cpp.git src
# pin a known-good revision when ready
```

Upstream moved multimodal from `examples/llava/` → `tools/mtmd/` (clip.cpp + mtmd.cpp).
Our `BUILD.gn` targets the modern layout. Classic names map as:

| Classic (user docs) | Modern upstream |
|---------------------|-----------------|
| `examples/llava/clip.cpp` | `tools/mtmd/clip.cpp` |
| `examples/llava/llava.cpp` | `tools/mtmd/mtmd.cpp` + `mtmd-helper.cpp` |
| `clip_model_load` | `mtmd_init_from_file` / `clip_init` |
| `llava_image_embed_make_with_bytes` | `mtmd_bitmap_init` + `mtmd_tokenize`/`mtmd_encode` |
| `llava_eval_image_embed` | `mtmd_helper_eval_chunks` |

## Metal / CUDA / Vulkan

CPU-only is the default GN config (utility sandbox friendly).
See `BUILD.gn` comments on `llama_model_params.n_gpu_layers` for offload hooks.
