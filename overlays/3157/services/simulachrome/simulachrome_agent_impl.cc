// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// TODO(simulachrome): convert RGBA/RGB pixel loops to base::span.
#pragma allow_unsafe_buffers
#endif

#include "services/simulachrome/simulachrome_agent_impl.h"

#include <algorithm>
#include <optional>
#include <cmath>
#include <cstring>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/system/sys_info.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include <GLES2/gl2.h>

#include "gpu/command_buffer/client/raster_interface.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/client/shared_memory_limits.h"
#include "gpu/command_buffer/common/context_result.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/scheduling_priority.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "services/viz/public/cpp/gpu/command_buffer_metrics.h"
#include "services/viz/public/cpp/gpu/context_provider_command_buffer.h"
#include "services/viz/public/cpp/gpu/gpu.h"
#include "url/gurl.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"

#include "llama.h"
#include "mtmd.h"
#include "mtmd-helper.h"
// clip_image_u8 + clip_build_img_from_pixels (tools/mtmd/clip.h)
#include "clip.h"

namespace simulachrome {
namespace {

constexpr int kContextSize = 4096;
constexpr int kMaxNewTokens = 128;

// UI-TARS 2B SFT ships as two GGUFs: the Qwen2-VL language model and a
// separate f16 vision projector. The LM file carries no clip.* metadata and
// no vision tensors at all, so without the projector mtmd_init_from_file
// fails and the agent is blind.
constexpr char kLlmFileName[] = "UI-TARS-2B-SFT-Q6_K_L.gguf";
constexpr char kMmprojFileName[] = "mmproj-UI-TARS-2B-SFT-f16.gguf";

// UI-TARS's scroll action carries no magnitude, unlike the previous contract's
// amount=1-9. One scroll is most of a viewport.
constexpr float kScrollViewportFraction = 0.8f;
// Action contract handed to the model. Deliberately narrower than the
// packaged system.txt, which describes a *desktop* agent (press/drag/
// long_press/navigate_back, Linux desktop menus) and advertises actions this
// browser executor cannot run. Must stay in sync with kActionGrammar.
constexpr char kBrowserActionContract[] =
    "You locate one control in a browser screenshot. Mark its center. "
    "Do not type, scroll, or decide what happens next.\n"
    "\n"
    "## Output Format\n"
    "Thought: <where the control is, one short sentence>\n"
    "Action: click(start_box='<|box_start|>(x,y)<|box_end|>')\n"
    "\n"
    "## Note\n"
    "- The click is only a location mark for the center of the control.\n"
    "- Coordinates are integers in [0,1000] relative to the screenshot.\n"
    "- Do not type text. Do not press keys. Do not say the task is finished.\n"
    "- Emit exactly one location.";

// GBNF locked to UI-TARS's own action space. Constraining this model to the
// previous <code>click(x=..)</code> syntax would have steered it off the
// distribution it was trained on, which is the whole reason for the swap --
// the grammar now enforces the format it already wants to emit.
// <|box_start|>/<|box_end|> are real vocab tokens (151648/151649) that the
// model may or may not wrap coordinates in, so both forms are accepted.
// Rule names avoid '_' because the GBNF name scanner treats it as a break.
constexpr char kActionGrammar[] = R"gbnf(
root ::= "Thought: " thought "\nAction: " click
thought ::= [^\n]+
click ::= "click(start_box='" box "')"
box ::= ("<|box_start|>")? "(" coord "," coord ")" ("<|box_end|>")?
coord ::= [0-9] | [1-9] [0-9] | [1-9] [0-9] [0-9] | "1000"
)gbnf";

// llama.cpp throughput peaks near the physical core count; SMT siblings add
// little and oversubscribing starves the browser/GPU processes sharing this
// machine. NumberOfProcessors() is logical, so halve it and clamp.
int InferenceThreadCount() {
  const int logical = base::SysInfo::NumberOfProcessors();
  return std::clamp(logical / 2, 4, 16);
}

base::FilePath ResolveLlmPath(const base::FilePath& model_path) {
  if (base::DirectoryExists(model_path)) {
    base::FilePath candidate =
        model_path.AppendASCII(kLlmFileName);
    if (base::PathExists(candidate)) {
      return candidate;
    }
    return base::FilePath();
  }
  return model_path;
}

base::FilePath ResolveMmprojPath(const base::FilePath& llm_path,
                                 const base::FilePath& model_path) {
  if (base::DirectoryExists(model_path)) {
    base::FilePath candidate = model_path.AppendASCII(kMmprojFileName);
    if (base::PathExists(candidate)) {
      return candidate;
    }
  }
  base::FilePath dir = llm_path.DirName();
  for (const char* name :
       {kMmprojFileName, "mmproj.gguf", "mmproj-model.gguf"}) {
    base::FilePath c = dir.AppendASCII(name);
    if (base::PathExists(c)) {
      return c;
    }
  }
  return base::FilePath();
}

std::string LoadOptionalText(const base::FilePath& dir, const char* name) {
  if (!base::DirectoryExists(dir)) {
    return std::string();
  }
  std::string out;
  base::ReadFileToString(dir.AppendASCII(name), &out);
  return out;
}

// Qwen2-VL ChatML, matching tokenizer.chat_template in the GGUF: lowercase
// role, a newline after it, and an <|im_end|>\n terminator.
//
// The media marker deliberately stays mtmd's <__media__>. mtmd_tokenize splits
// the prompt on that marker and emits <|vision_start|>, one <|image_pad|> per
// vision token the tower actually produced, then <|vision_end|> itself (see
// PROJECTOR_TYPE_QWEN2VL in mtmd.cpp). Writing those tags here instead would
// leave the marker unmatched, so the image would never be attached at all.
std::string BuildChatPrompt(const std::string& system_prompt,
                            const std::string& task,
                            const char* image_marker) {
  std::string prompt;
  prompt += "<|im_start|>system\n";
  prompt += system_prompt.empty() ? "You are a helpful assistant."
                                  : system_prompt;
  prompt += "<|im_end|>\n";
  prompt += "<|im_start|>user\n";
  prompt += image_marker ? image_marker : mtmd_default_marker();
  prompt += task;
  prompt += "<|im_end|>\n";
  prompt += "<|im_start|>assistant\n";
  return prompt;
}

bool SkBitmapFromRgbaBytes(const std::vector<uint8_t>& rgba,
                           const gfx::Size& size,
                           SkBitmap* out) {
  if (!out || size.IsEmpty()) {
    return false;
  }
  const size_t expected =
      static_cast<size_t>(size.width()) * size.height() * 4;
  if (rgba.size() < expected) {
    return false;
  }
  SkImageInfo info =
      SkImageInfo::Make(size.width(), size.height(), kRGBA_8888_SkColorType,
                        kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
  if (!out->tryAllocPixels(info)) {
    return false;
  }
  std::memcpy(out->getPixels(), rgba.data(), expected);
  return true;
}

}  // namespace

SimulachromeAgentImpl::SimulachromeAgentImpl(
    mojo::PendingReceiver<mojom::SimulachromeAgent> receiver)
    : receiver_(this, std::move(receiver)) {}

SimulachromeAgentImpl::~SimulachromeAgentImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Drop the raw raster pointer before the context that owns it.
  raster_interface_ = nullptr;
  shared_image_interface_.reset();
  raster_context_.reset();
  viz_gpu_.reset();
  ShutdownEngine();
}

void SimulachromeAgentImpl::SetIoTaskRunner(
    scoped_refptr<base::SingleThreadTaskRunner> io_task_runner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  io_task_runner_ = std::move(io_task_runner);
}

void SimulachromeAgentImpl::SetGpuReadbackInterfaces(
    scoped_refptr<gpu::SharedImageInterface> sii,
    gpu::raster::RasterInterface* raster) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  shared_image_interface_ = std::move(sii);
  raster_interface_ = raster;
}

void SimulachromeAgentImpl::AttachGpu(mojo::PendingRemote<viz::mojom::Gpu> gpu,
                                      AttachGpuCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  raster_interface_ = nullptr;
  shared_image_interface_.reset();
  raster_context_.reset();
  viz_gpu_.reset();

  if (!io_task_runner_ || !gpu) {
    LOG(ERROR) << "[SimulachromeAgent] AttachGpu missing "
               << (!io_task_runner_ ? "io task runner" : "gpu pipe");
    std::move(callback).Run(false);
    return;
  }

  // viz::Gpu binds its mojo remote on the utility IO thread and must be
  // created on this main thread. EstablishGpuChannelSync blocks here until the
  // browser UI thread answers; the caller does not wait on this method.
  viz_gpu_ = viz::Gpu::Create(std::move(gpu), io_task_runner_);
  scoped_refptr<gpu::GpuChannelHost> channel =
      viz_gpu_->EstablishGpuChannelSync();
  if (!channel) {
    LOG(ERROR) << "[SimulachromeAgent] EstablishGpuChannelSync failed";
    viz_gpu_.reset();
    std::move(callback).Run(false);
    return;
  }

  // Stream 0 is content::kGpuStreamIdDefault. SchedulingPriority::kNormal is
  // the same priority. A services target cannot include the content header.
  scoped_refptr<viz::ContextProviderCommandBuffer> context =
      viz::ContextProviderCommandBuffer::CreateForRaster(
          channel, /*stream_id=*/0, gpu::SchedulingPriority::kNormal,
          GURL("chrome://simulachrome-agent/"),
          /*automatic_flushes=*/false, /*support_locking=*/false,
          gpu::SharedMemoryLimits::ForMailboxContext(),
          viz::command_buffer_metrics::ContextType::UNKNOWN,
          /*enable_gpu_rasterization=*/false,
          /*lose_context_when_out_of_memory=*/false);
  if (!context) {
    LOG(ERROR) << "[SimulachromeAgent] CreateForRaster returned null";
    viz_gpu_.reset();
    std::move(callback).Run(false);
    return;
  }

  const gpu::ContextResult result = context->BindToCurrentSequence();
  if (result != gpu::ContextResult::kSuccess) {
    LOG(ERROR) << "[SimulachromeAgent] raster context bind failed result="
               << static_cast<int>(result);
    viz_gpu_.reset();
    std::move(callback).Run(false);
    return;
  }

  gpu::raster::RasterInterface* raster = context->RasterInterface();
  gpu::SharedImageInterface* sii = context->SharedImageInterface();
  if (!raster || !sii) {
    LOG(ERROR) << "[SimulachromeAgent] raster context has no readback "
                  "interface raster="
               << (raster ? "yes" : "no")
               << " shared_image=" << (sii ? "yes" : "no");
    viz_gpu_.reset();
    std::move(callback).Run(false);
    return;
  }

  raster_context_ = std::move(context);
  SetGpuReadbackInterfaces(base::WrapRefCounted(sii), raster);
  LOG(INFO) << "[SimulachromeAgent] mailbox readback ready channel_id="
            << channel->channel_id();
  std::move(callback).Run(true);
}

void SimulachromeAgentImpl::ShutdownEngine() {
  if (sampler_) {
    llama_sampler_free(sampler_);
    sampler_ = nullptr;
  }
  if (mtmd_ctx_) {
    mtmd_free(mtmd_ctx_);
    mtmd_ctx_ = nullptr;
  }
  if (llama_ctx_) {
    llama_free(llama_ctx_);
    llama_ctx_ = nullptr;
  }
  if (llama_model_) {
    llama_model_free(llama_model_);
    llama_model_ = nullptr;
  }
  if (backend_inited_) {
    llama_backend_free();
    backend_inited_ = false;
  }
  model_loaded_ = false;
}

bool SimulachromeAgentImpl::Initialize(const base::FilePath& model_path,
                                       std::string* error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  const base::FilePath llm_path = ResolveLlmPath(model_path);
  if (llm_path.empty() || !base::PathExists(llm_path)) {
    if (error) {
      *error = "llm_gguf_not_found";
    }
    return false;
  }
  const base::FilePath mmproj_path = ResolveMmprojPath(llm_path, model_path);
  if (mmproj_path.empty() || !base::PathExists(mmproj_path)) {
    if (error) {
      *error = "mmproj_gguf_not_found";
    }
    return false;
  }

  ShutdownEngine();

  llama_backend_init();
  backend_inited_ = true;

  // GPU offload hook (Metal/CUDA/Vulkan): set mparams.n_gpu_layers > 0 only
  // when GGML_USE_* is built AND this utility can open the device. Intel Mac
  // without usable Metal-for-llama keeps this at 0 (CPU inference).
  llama_model_params mparams = llama_model_default_params();
  mparams.n_gpu_layers = 0;

  llama_model_ = llama_model_load_from_file(llm_path.AsUTF8Unsafe().c_str(),
                                           mparams);
  if (!llama_model_) {
    if (error) {
      *error = "llama_model_load_failed";
    }
    ShutdownEngine();
    return false;
  }

  // The ChatML scaffolding and the UI-TARS grammar are both architecture
  // specific. A non-qwen2vl GGUF loads fine here and then produces confident
  // garbage, so the mismatch is called out rather than inferred later.
  char arch[64] = {};
  if (llama_model_meta_val_str(llama_model_, "general.architecture", arch,
                               sizeof(arch)) <= 0) {
    arch[0] = '\0';
  }
  char desc[160] = {};
  if (llama_model_desc(llama_model_, desc, sizeof(desc)) <= 0) {
    desc[0] = '\0';
  }
  const bool arch_ok = std::string(arch) == "qwen2vl";
  LOG(INFO) << "[SimulachromeAgent] arch=\"" << arch << "\" desc=\"" << desc
            << "\" params=" << llama_model_n_params(llama_model_);
  if (!arch_ok) {
    LOG(ERROR) << "[SimulachromeAgent] architecture mismatch: expected "
                  "qwen2vl, got \""
               << arch << "\"; ChatML prompt and UI-TARS grammar will not fit";
  }

  llama_context_params cparams = llama_context_default_params();
  cparams.n_ctx = kContextSize;
  const int n_threads = InferenceThreadCount();
  cparams.n_threads = n_threads;
  cparams.n_threads_batch = n_threads;
  llama_ctx_ = llama_init_from_model(llama_model_, cparams);
  if (!llama_ctx_) {
    if (error) {
      *error = "llama_context_init_failed";
    }
    ShutdownEngine();
    return false;
  }

  mtmd_context_params mt_params = mtmd_context_params_default();
  mt_params.use_gpu = false;
  mt_params.n_threads = n_threads;
  mt_params.print_timings = false;
  // 720p (1280x720) is 1196 Qwen2-VL tokens once aligned to the 28px grid
  // (1288x728). Min and max match so every frame is that size: a retina
  // capture is scaled down and a small window is scaled up. 1196 stays above
  // the ~1024 grounding floor (llama.cpp#16842) and is well under the 2048
  // budget that made CPU inference take forever.
  mt_params.image_min_tokens = 1196;
  mt_params.image_max_tokens = 1196;
  mtmd_ctx_ = mtmd_init_from_file(mmproj_path.AsUTF8Unsafe().c_str(),
                                 llama_model_, mt_params);
  if (!mtmd_ctx_) {
    if (error) {
      *error = "mtmd_init_failed";
    }
    ShutdownEngine();
    return false;
  }

  // vision=false means the projector loaded but carries no vision tower;
  // mrope=true confirms the Qwen2-VL M-RoPE position path is active, which is
  // what makes dynamic-resolution images line up with their position ids.
  const llama_vocab* vocab = llama_model_get_vocab(llama_model_);
  LOG(INFO) << "[SimulachromeAgent] mtmd vision="
            << mtmd_support_vision(mtmd_ctx_)
            << " mrope=" << mtmd_decode_use_mrope(mtmd_ctx_)
            << " eos_token=" << llama_vocab_eos(vocab)
            << " (qwen2vl expects 151645 = <|im_end|>)";

  model_dir_ =
      base::DirectoryExists(model_path) ? model_path : llm_path.DirName();

  // system.txt ships the upstream desktop contract; system-override.txt is the
  // escape hatch for experimenting without a rebuild.
  system_prompt_ = LoadOptionalText(model_dir_, "system-override.txt");
  if (system_prompt_.empty()) {
    system_prompt_ = kBrowserActionContract;
  }

  auto sparams = llama_sampler_chain_default_params();
  sampler_ = llama_sampler_chain_init(sparams);

  std::string grammar = LoadOptionalText(model_dir_, "grammar.gbnf");
  if (grammar.empty()) {
    grammar = kActionGrammar;
  }
  // Returns NULL on a GBNF parse error; chaining that would crash on the
  // first sample, so degrade to unconstrained decoding instead.
  llama_sampler* grammar_sampler = llama_sampler_init_grammar(
      llama_model_get_vocab(llama_model_), grammar.c_str(), "root");
  grammar_active_ = grammar_sampler != nullptr;
  if (grammar_active_) {
    llama_sampler_chain_add(sampler_, grammar_sampler);
  } else {
    LOG(ERROR) << "[SimulachromeAgent] GBNF parse failed; output is "
                  "unconstrained and may not parse";
  }
  llama_sampler_chain_add(sampler_, llama_sampler_init_greedy());

  model_loaded_ = true;
  LOG(INFO) << "[SimulachromeAgent] Initialize ok llm=" << llm_path
            << " mmproj=" << mmproj_path << " n_threads=" << n_threads
            << " (logical=" << base::SysInfo::NumberOfProcessors() << ")"
            << " grammar=" << (grammar_active_ ? "on" : "OFF");
  return true;
}

void SimulachromeAgentImpl::LoadModel(const base::FilePath& model_path,
                                      LoadModelCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::string error;
  std::move(callback).Run(Initialize(model_path, &error), error);
}

bool SimulachromeAgentImpl::PrepareFrameForInference(
    const mojom::FrameTexture& frame,
    std::string* diagnostics) {
  const bool has_mailbox = !frame.mailbox.IsZero();
  const bool has_cpu_pixels = !frame.rgba_pixels.empty();
  if (!has_mailbox && !has_cpu_pixels) {
    if (diagnostics) {
      *diagnostics = "no_mailbox_or_pixels";
    }
    return false;
  }
  if (diagnostics) {
    *diagnostics = base::StringPrintf(
        "mailbox=%s pixels=%zu size_px=%dx%d dsf=%.2f raster=%s",
        has_mailbox ? "yes" : "no", frame.rgba_pixels.size(),
        frame.size_px.width(), frame.size_px.height(), frame.device_scale_factor,
        raster_interface_ ? "yes" : "no");
  }
  return true;
}

bool SimulachromeAgentImpl::ReadbackMailboxToBitmap(
    const gpu::Mailbox& mailbox,
    const gpu::SyncToken& sync_token,
    const gfx::Size& size,
    SkBitmap* out_bitmap,
    std::string* error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!out_bitmap) {
    if (error) {
      *error = "null_bitmap";
    }
    return false;
  }
  if (mailbox.IsZero() || size.IsEmpty()) {
    if (error) {
      *error = "invalid_mailbox_or_size";
    }
    return false;
  }
  if (!shared_image_interface_ || !raster_interface_) {
    if (error) {
      *error = "no_gpu_readback_interfaces";
    }
    return false;
  }

  // 1) Ensure the producer (Viz CopyOutput) has finished writing the SI.
  shared_image_interface_->WaitSyncToken(sync_token);
  // Raster cmds must also see the wait (same GPU channel typically).
  raster_interface_->WaitSyncTokenCHROMIUM(
      reinterpret_cast<const GLbyte*>(sync_token.GetConstData()));

  // 2) Allocate destination SkBitmap (platform N32 = BGRA or RGBA).
  const SkImageInfo info =
      SkImageInfo::MakeN32Premul(size.width(), size.height(),
                                 SkColorSpace::MakeSRGB());
  if (!out_bitmap->tryAllocPixels(info)) {
    if (error) {
      *error = "skbitmap_alloc_failed";
    }
    return false;
  }

  // 3) Synchronous GPU→CPU readback into |out_bitmap|.
  const bool ok = raster_interface_->ReadbackImagePixels(
      mailbox, out_bitmap->info(),
      static_cast<GLuint>(out_bitmap->rowBytes()),
      /*src_x=*/0, /*src_y=*/0, /*plane_index=*/0, out_bitmap->getPixels());
  if (!ok) {
    if (error) {
      *error = "ReadbackImagePixels_failed";
    }
    out_bitmap->reset();
    return false;
  }

  // Flush so any dependent GPU work is submitted (safe even if unused).
  raster_interface_->OrderingBarrierCHROMIUM();
  return true;
}

void SimulachromeAgentImpl::ReadbackFrame(
    mojom::FrameTexturePtr frame_texture,
    ReadbackFrameCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto image = mojom::FrameImage::New();
  if (!frame_texture) {
    image->ok = false;
    image->error = "null_frame";
    std::move(callback).Run(std::move(image));
    return;
  }
  SkBitmap bitmap;
  std::string error;
  if (!ResolveFrameToBitmap(*frame_texture, &bitmap, &error)) {
    image->ok = false;
    image->error = error.empty() ? "readback_failed" : error;
    std::move(callback).Run(std::move(image));
    return;
  }
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  if (!png || png->empty()) {
    image->ok = false;
    image->error = "png_encode_failed";
    std::move(callback).Run(std::move(image));
    return;
  }
  image->ok = true;
  image->size_px = gfx::Size(bitmap.width(), bitmap.height());
  image->device_scale_factor = frame_texture->device_scale_factor;
  image->viewport_size_dip = frame_texture->viewport_size_dip;
  image->png = std::move(*png);
  LOG(INFO) << "[SimulachromeAgent] mailbox frame png bytes="
            << image->png.size() << " size_px=" << image->size_px.ToString();
  std::move(callback).Run(std::move(image));
}

bool SimulachromeAgentImpl::PrepareClipImage(const SkBitmap& bitmap,
                                             std::vector<uint8_t>* out_rgb,
                                             int* out_width,
                                             int* out_height,
                                             clip_image_u8* out_clip_img,
                                             std::string* error) {
  if (!out_rgb || !out_width || !out_height) {
    if (error) {
      *error = "null_out_args";
    }
    return false;
  }
  if (bitmap.drawsNothing() || bitmap.width() <= 0 || bitmap.height() <= 0) {
    if (error) {
      *error = "empty_bitmap";
    }
    return false;
  }

  // ImageOperations::Resize hard-requires kN32 and silently returns an empty
  // bitmap for anything else. kN32 is BGRA on macOS but RGBA elsewhere, and
  // the CPU fallback frame always arrives as kRGBA_8888 -- so accepting
  // RGBA/BGRA here used to guarantee resize_failed on mac. Normalize instead.
  SkBitmap src = bitmap;
  if (src.colorType() != kN32_SkColorType) {
    SkBitmap converted;
    const SkImageInfo n32 = SkImageInfo::MakeN32Premul(src.width(), src.height(),
                                                       src.refColorSpace());
    if (!converted.tryAllocPixels(n32) || !src.readPixels(converted.pixmap())) {
      if (error) {
        *error = base::StringPrintf("color_convert_failed:ct=%d",
                                    static_cast<int>(src.colorType()));
      }
      return false;
    }
    src = std::move(converted);
  }

  // Shrink frames larger than 720p on the long edge. The token budget is the
  // same size, so extra pixels here are discarded on the next step. A 384px
  // long edge made every button the same blob; mtmd cannot recover detail we
  // throw away.
  const int long_edge = std::max(src.width(), src.height());
  if (long_edge > kClipTargetEdge) {
    const float scale =
        static_cast<float>(kClipTargetEdge) / static_cast<float>(long_edge);
    const int tw = std::max(1, static_cast<int>(std::lround(src.width() * scale)));
    const int th =
        std::max(1, static_cast<int>(std::lround(src.height() * scale)));
    SkBitmap resized = skia::ImageOperations::Resize(
        src, skia::ImageOperations::RESIZE_BETTER, tw, th);
    if (resized.drawsNothing()) {
      if (error) {
        *error = base::StringPrintf("resize_failed:%dx%d->%dx%d ct=%d",
                                    src.width(), src.height(), tw, th,
                                    static_cast<int>(src.colorType()));
      }
      return false;
    }
    src = std::move(resized);
  }

  const int w = src.width();
  const int h = src.height();
  out_rgb->assign(static_cast<size_t>(w) * h * 3, 0);

  const SkPixmap pixmap = src.pixmap();
  const bool bgra = (pixmap.colorType() == kBGRA_8888_SkColorType);
  for (int y = 0; y < h; ++y) {
    const uint8_t* row =
        static_cast<const uint8_t*>(pixmap.addr(0, y));
    uint8_t* dst = out_rgb->data() + static_cast<size_t>(y) * w * 3;
    for (int x = 0; x < w; ++x) {
      const uint8_t* px = row + x * 4;
      if (bgra) {
        // B,G,R,A → R,G,B
        dst[x * 3 + 0] = px[2];
        dst[x * 3 + 1] = px[1];
        dst[x * 3 + 2] = px[0];
      } else {
        // R,G,B,A
        dst[x * 3 + 0] = px[0];
        dst[x * 3 + 1] = px[1];
        dst[x * 3 + 2] = px[2];
      }
    }
  }

  *out_width = w;
  *out_height = h;

  // Classic clip_build_img_from_pixels was removed upstream; callers that still
  // accept an optional clip_image_u8* should pass nullptr and use |out_rgb| with
  // mtmd_bitmap_init instead.
  (void)out_clip_img;
  return true;
}

bool SimulachromeAgentImpl::ResolveFrameToBitmap(
    const mojom::FrameTexture& frame,
    SkBitmap* out_bitmap,
    std::string* error) {
  // Preferred: zero-copy mailbox → GPU readback in this process.
  if (!frame.mailbox.IsZero() && shared_image_interface_ &&
      raster_interface_) {
    if (ReadbackMailboxToBitmap(frame.mailbox, frame.sync_token, frame.size_px,
                                out_bitmap, error)) {
      LOG(INFO) << "[SimulachromeAgent] mailbox readback ok size_px="
                << frame.size_px.ToString();
      return true;
    }
    // Fall through to CPU pixels if mailbox readback failed.
    LOG(WARNING) << "[SimulachromeAgent] mailbox readback failed: "
                 << (error ? *error : "");
  }

  if (!frame.rgba_pixels.empty() && !frame.size_px.IsEmpty()) {
    if (SkBitmapFromRgbaBytes(frame.rgba_pixels, frame.size_px, out_bitmap)) {
      if (error) {
        error->clear();
      }
      return true;
    }
    if (error) {
      *error = "rgba_pixels_invalid";
    }
    return false;
  }

  if (error) {
    *error = "no_readable_frame";
  }
  return false;
}

mojom::AgentActionPtr SimulachromeAgentImpl::ParseAgentAction(
    const std::string& text,
    int image_width,
    int image_height) {
  auto action = mojom::AgentAction::New();
  action->confidence = 0.5f;
  action->payload = text;

  // UI-TARS answers "Thought: ...\nAction: <call>". Only the tail after the
  // last Action: label is executable; the reasoning above it is not. rfind so
  // a Thought that mentions the word cannot shadow the real label.
  std::string body = text;
  const auto action_at = body.rfind("Action:");
  if (action_at != std::string::npos) {
    body = body.substr(action_at + 7);
  }

  // <|box_start|>/<|box_end|> are real vocab tokens that wrap each coordinate
  // pair, and llama_token_to_piece renders them literally because we detokenize
  // with special=true. Strip them once here rather than in every regex.
  for (const char* tok : {"<|box_start|>", "<|box_end|>", "<|im_end|>"}) {
    base::ReplaceSubstringsAfterOffset(&body, 0, tok, "");
  }
  std::string trimmed(base::TrimWhitespaceASCII(body, base::TRIM_ALL));
  body = std::move(trimmed);
  // The panel and the manager only see payload. Keep the executable call,
  // not the Thought sentence that precedes it.
  action->payload = body;

  // UI-TARS grounds on a 0-1000 integer grid. A decimal point means the model
  // fell back to [0,1] floats, which only happens with grammar=OFF -- without
  // that distinction a bare "1" is ambiguous between 0.001 and the right edge.
  auto to_unit = [](const std::string& raw, float* out) {
    double v = 0;
    if (!base::StringToDouble(raw, &v)) {
      return false;
    }
    const bool fractional = raw.find('.') != std::string::npos;
    *out =
        static_cast<float>(std::clamp(fractional ? v : v / 1000.0, 0.0, 1.0));
    return true;
  };

  std::smatch m;

  const std::regex click_re(
      R"((click|left_double)\s*\(\s*start_box\s*=\s*['"]?\(?\s*)"
      R"(([0-9]*\.?[0-9]+)\s*,\s*([0-9]*\.?[0-9]+)\s*\)?)",
      std::regex::icase);
  if (std::regex_search(body, m, click_re) && m.size() >= 4) {
    float x = 0.f;
    float y = 0.f;
    // Numeric conversion must not throw: this TU is built -fno-exceptions, so
    // a wild coordinate would abort the utility instead of failing one step.
    if (!to_unit(m[2].str(), &x) || !to_unit(m[3].str(), &y)) {
      action->type = mojom::AgentActionType::kFail;
      action->payload = body;
      return action;
    }
    action->type = mojom::AgentActionType::kClick;
    action->x = x;
    action->y = y;
    // The injector has no double-click primitive, so left_double runs as a
    // single click. Logged rather than silently downgraded.
    if (base::EqualsCaseInsensitiveASCII(m[1].str(), "left_double")) {
      LOG(INFO) << "[SimulachromeAgent] left_double executed as single click";
    }
    LOG(INFO) << "[SimulachromeAgent] " << m[1].str() << " grid=("
              << m[2].str() << "," << m[3].str() << ") -> norm=(" << x << ","
              << y << ") img=" << image_width << "x" << image_height;
    return action;
  }

  // type(content='...'). A trailing newline is UI-TARS's submit convention, so
  // it is preserved in the payload for the injector to turn into Enter.
  const std::regex type_re(
      R"(type\s*\(\s*content\s*=\s*['"]([\s\S]*?)['"]\s*\))",
      std::regex::icase);
  if (std::regex_search(body, m, type_re) && m.size() >= 2) {
    std::string content = m[1].str();
    base::ReplaceSubstringsAfterOffset(&content, 0, "\\n", "\n");
    action->type = mojom::AgentActionType::kType;
    action->payload = content;
    action->x = 0.5f;
    action->y = 0.5f;
    return action;
  }

  // hotkey(key='enter'). Single named keys only -- the injector has no modifier
  // chording, so a "ctrl c" style combo is rejected instead of half-applied.
  const std::regex hotkey_re(
      R"(hotkey\s*\(\s*key\s*=\s*['"]([A-Za-z 	+-]+)['"]\s*\))",
      std::regex::icase);
  if (std::regex_search(body, m, hotkey_re) && m.size() >= 2) {
    std::string key = base::ToLowerASCII(m[1].str());
    std::string key_trimmed(base::TrimWhitespaceASCII(key, base::TRIM_ALL));
    if (key_trimmed.find_first_of(" \t+-") != std::string::npos) {
      action->type = mojom::AgentActionType::kFail;
      action->payload = "unsupported_hotkey_combo:" + key_trimmed;
      return action;
    }
    action->type = mojom::AgentActionType::kKeyPress;
    action->payload = key_trimmed;
    return action;
  }

  // scroll(start_box='(x,y)', direction='down'). The start_box positions the
  // wheel event, so a scroll inside an overflow pane hits that pane.
  const std::regex scroll_re(
      R"(scroll\s*\([\s\S]*?direction\s*=\s*['"]?(up|down))",
      std::regex::icase);
  if (std::regex_search(body, m, scroll_re) && m.size() >= 2) {
    action->type = mojom::AgentActionType::kScroll;
    action->x = 0.5f;
    action->y = 0.5f;
    const std::regex box_re(
        R"(start_box\s*=\s*['"]?\(?\s*([0-9]*\.?[0-9]+)\s*,\s*([0-9]*\.?[0-9]+))",
        std::regex::icase);
    std::smatch bm;
    if (std::regex_search(body, bm, box_re) && bm.size() >= 3) {
      float x = 0.f;
      float y = 0.f;
      if (to_unit(bm[1].str(), &x) && to_unit(bm[2].str(), &y)) {
        action->x = x;
        action->y = y;
      }
    }
    action->scroll_dy =
        (base::EqualsCaseInsensitiveASCII(m[1].str(), "up") ? -1.f : 1.f) *
        kScrollViewportFraction;
    return action;
  }

  if (body.find("finished(") != std::string::npos) {
    action->type = mojom::AgentActionType::kDone;
    action->payload = body;
    return action;
  }

  if (body.find("wait(") != std::string::npos) {
    action->type = mojom::AgentActionType::kWait;
    return action;
  }

  // call_user() is UI-TARS reporting the task is unsolvable from this view.
  // Routing it to kElementNotFound makes the Planner re-target instead of
  // treating the step as a hard failure.
  if (body.find("call_user(") != std::string::npos) {
    action->type = mojom::AgentActionType::kElementNotFound;
    action->payload = "call_user";
    return action;
  }

  // Calls in UI-TARS's wider action space that this executor cannot perform.
  // The grammar should make these unreachable; naming them beats dumping a raw
  // generation when it does not (grammar=OFF, or a system-override.txt that
  // re-advertises them).
  for (const char* unsupported : {"right_single", "drag(", "long_press"}) {
    if (body.find(unsupported) != std::string::npos) {
      action->type = mojom::AgentActionType::kFail;
      action->payload = std::string("unsupported_action:") + unsupported;
      return action;
    }
  }

  action->type = mojom::AgentActionType::kFail;
  action->payload = body;
  return action;
}

mojom::AgentResultPtr SimulachromeAgentImpl::RunInference(
    const std::string& task_instruction,
    const mojom::FrameTexture& frame,
    const std::vector<uint8_t>& rgb,
    int width,
    int height) {
  auto result = mojom::AgentResult::New();
  if (!model_loaded_ || !llama_ctx_ || !mtmd_ctx_ || !sampler_) {
    result->ok = false;
    result->error = "model_not_loaded";
    return result;
  }
  if (rgb.size() < static_cast<size_t>(width) * height * 3 || width <= 0 ||
      height <= 0) {
    result->ok = false;
    result->error = "invalid_rgb_buffer";
    return result;
  }

  // Modern mtmd path (== llava_image_embed_make_with_bytes +
  // llava_eval_image_embed).
  mtmd_bitmap* bitmap = mtmd_bitmap_init(static_cast<uint32_t>(width),
                                         static_cast<uint32_t>(height),
                                         rgb.data());
  if (!bitmap) {
    result->ok = false;
    result->error = "mtmd_bitmap_init_failed";
    return result;
  }

  const std::string prompt =
      BuildChatPrompt(system_prompt_, task_instruction, mtmd_default_marker());

  mtmd_input_text text_chunk;
  text_chunk.text = prompt.c_str();
  text_chunk.add_special = true;
  text_chunk.parse_special = true;

  mtmd_input_chunks* chunks = mtmd_input_chunks_init();
  const mtmd_bitmap* bitmaps[1] = {bitmap};
  const int32_t tok_rc =
      mtmd_tokenize(mtmd_ctx_, chunks, &text_chunk, bitmaps, 1);
  if (tok_rc != 0) {
    mtmd_bitmap_free(bitmap);
    mtmd_input_chunks_free(chunks);
    result->ok = false;
    result->error = base::StringPrintf("mtmd_tokenize_failed:%d", tok_rc);
    return result;
  }

  llama_memory_clear(llama_get_memory(llama_ctx_), /*data=*/true);
  // Grammar samplers carry parse state: without a reset, the previous step's
  // completed parse rejects the first token of this one.
  llama_sampler_reset(sampler_);

  llama_pos n_past = 0;
  const int32_t eval_rc = mtmd_helper_eval_chunks(
      mtmd_ctx_, llama_ctx_, chunks, /*n_past=*/0, /*seq_id=*/0,
      /*n_batch=*/512, /*logits_last=*/true, &n_past);
  mtmd_bitmap_free(bitmap);
  mtmd_input_chunks_free(chunks);
  if (eval_rc != 0) {
    result->ok = false;
    result->error = base::StringPrintf("mtmd_eval_failed:%d", eval_rc);
    return result;
  }

  std::string generated;
  generated.reserve(1024);
  for (int i = 0; i < kMaxNewTokens; ++i) {
    // llama_sampler_sample already applies the chain and accepts the token.
    // A second llama_sampler_accept feeds the same piece into the grammar
    // again; the stack is already past it, accept throws, and ggml aborts
    // the utility (signal 6) before any text is produced.
    llama_token token_id =
        llama_sampler_sample(sampler_, llama_ctx_, /*idx=*/-1);

    if (llama_vocab_is_eog(llama_model_get_vocab(llama_model_), token_id)) {
      break;
    }

    char buf[256];
    const int n = llama_token_to_piece(llama_model_get_vocab(llama_model_),
                                       token_id, buf, sizeof(buf),
                                       /*lstrip=*/0, /*special=*/true);
    if (n > 0) {
      generated.append(buf, buf + n);
    }
    // llama_vocab_is_eog above catches <|im_end|> as a token. This is the
    // belt-and-braces case where it arrives as plain text instead, which
    // happens when grammar constraints steer sampling off the special token.
    if (generated.find("<|im_end|>") != std::string::npos) {
      break;
    }

    llama_batch batch = llama_batch_get_one(&token_id, 1);
    if (llama_decode(llama_ctx_, batch) != 0) {
      result->ok = false;
      result->error = "llama_decode_failed";
      result->action = ParseAgentAction(generated, width, height);
      return result;
    }
  }

  result->ok = true;
  result->error = base::StringPrintf("gen_chars=%zu rgb=%dx%d", generated.size(),
                                     width, height);
  result->action = ParseAgentAction(generated, width, height);
  LOG(INFO) << "[SimulachromeAgent] generated=" << generated;
  return result;
}

void SimulachromeAgentImpl::ExecuteAction(
    const std::string& task_instruction,
    mojom::FrameTexturePtr frame_texture,
    ExecuteActionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto result = mojom::AgentResult::New();
  if (!frame_texture) {
    result->ok = false;
    result->error = "missing_frame_texture";
    std::move(callback).Run(std::move(result));
    return;
  }

  std::string diagnostics;
  if (!PrepareFrameForInference(*frame_texture, &diagnostics)) {
    result->ok = false;
    result->error = diagnostics;
    std::move(callback).Run(std::move(result));
    return;
  }

  // --- Last mile: mailbox/pixels → SkBitmap → RGB → CLIP/mtmd ---
  SkBitmap sk_bitmap;
  std::string resolve_error;
  if (!ResolveFrameToBitmap(*frame_texture, &sk_bitmap, &resolve_error)) {
    result->ok = false;
    result->error = diagnostics + ";" + resolve_error;
    std::move(callback).Run(std::move(result));
    return;
  }

  std::vector<uint8_t> rgb;
  int width = 0;
  int height = 0;
  std::string clip_error;
  const bool clip_ok =
      PrepareClipImage(sk_bitmap, &rgb, &width, &height, nullptr, &clip_error);
  if (!clip_ok) {
    result->ok = false;
    result->error = diagnostics + ";" + clip_error;
    std::move(callback).Run(std::move(result));
    return;
  }
  LOG(INFO) << "[SimulachromeAgent] frame for model " << width << "x" << height;

  if (!model_loaded_) {
    result->ok = true;
    result->error = diagnostics + ";mock_no_model";
    auto action = mojom::AgentAction::New();
    action->type = mojom::AgentActionType::kClick;
    action->x = 0.5f;
    action->y = 0.5f;
    action->payload = task_instruction;
    action->confidence = 0.1f;
    result->action = std::move(action);
    std::move(callback).Run(std::move(result));
    return;
  }

  result = RunInference(task_instruction, *frame_texture, rgb, width, height);
  if (!result->error.empty() && !diagnostics.empty()) {
    result->error = diagnostics + ";" + result->error;
  }
  // Mojo callback returns to browser → SimulachromeAgentHost::OnAgentResult
  // → FrameCaptureClient::ReleasePending() (SharedImage lifetime).
  std::move(callback).Run(std::move(result));
}

void SimulachromeAgentImpl::ExecuteActionWithMailbox(
    const std::string& task_instruction,
    const gpu::Mailbox& frame_texture,
    ExecuteActionWithMailboxCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto frame = mojom::FrameTexture::New();
  frame->mailbox = frame_texture;
  frame->device_scale_factor = 1.f;
  // size_px unknown in this convenience path — readback will fail without
  // dimensions; prefer ExecuteAction(FrameTexture) from the browser host.
  ExecuteAction(task_instruction, std::move(frame), std::move(callback));
}

}  // namespace simulachrome
