// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_SIMULACHROME_SIMULACHROME_AGENT_IMPL_H_
#define SERVICES_SIMULACHROME_SIMULACHROME_AGENT_IMPL_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/single_thread_task_runner.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/simulachrome/public/mojom/simulachrome_agent.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/size.h"

struct llama_model;
struct llama_context;
struct llama_sampler;
struct mtmd_context;
struct clip_image_u8;

namespace gpu {
class SharedImageInterface;
namespace raster {
class RasterInterface;
}  // namespace raster
}  // namespace gpu

namespace viz {
class ContextProviderCommandBuffer;
class Gpu;
}  // namespace viz

namespace simulachrome {

// Utility-process VLM agent. Owns llama.cpp weights + CLIP/mmproj projector.
//
// Frame path:
//   FrameTexture{mailbox, sync_token} --ReadbackMailboxToBitmap--> SkBitmap
//     --PrepareClipImage--> RGB bytes --> mtmd_bitmap / CLIP --> llama sample
//
// Intel Mac / no Metal-for-llama: inference stays on CPU. The frame itself
// is a GPU mailbox. AttachGpu opens a raster context so ReadbackImagePixels
// can pull that mailbox across; rgba_pixels is only the fallback when that
// context never comes up.
class SimulachromeAgentImpl : public mojom::SimulachromeAgent {
 public:
  // Long edge of a 720p frame. Anything larger is scaled down before the RGB
  // walk. 384 blurred Log in into Sign up; 2048 made CPU inference crawl on
  // Intel. mtmd then aligns this to its 28px grid inside the same budget.
  static constexpr int kClipTargetEdge = 1280;

  explicit SimulachromeAgentImpl(
      mojo::PendingReceiver<mojom::SimulachromeAgent> receiver);
  ~SimulachromeAgentImpl() override;

  SimulachromeAgentImpl(const SimulachromeAgentImpl&) = delete;
  SimulachromeAgentImpl& operator=(const SimulachromeAgentImpl&) = delete;

  // Utility IO thread, set before the receiver starts dispatching so
  // AttachGpu can bind viz::Gpu. Null disables mailbox readback.
  void SetIoTaskRunner(
      scoped_refptr<base::SingleThreadTaskRunner> io_task_runner);

  // Optional: inject GPU clients when the utility process has a GpuChannel.
  void SetGpuReadbackInterfaces(
      scoped_refptr<gpu::SharedImageInterface> sii,
      gpu::raster::RasterInterface* raster);

  // mojom::SimulachromeAgent:
  void LoadModel(const base::FilePath& model_path,
                 LoadModelCallback callback) override;
  void AttachGpu(mojo::PendingRemote<viz::mojom::Gpu> gpu,
                 AttachGpuCallback callback) override;
  void ReadbackFrame(mojom::FrameTexturePtr frame_texture,
                     ReadbackFrameCallback callback) override;
  void ExecuteAction(const std::string& task_instruction,
                     mojom::FrameTexturePtr frame_texture,
                     ExecuteActionCallback callback) override;
  void ExecuteActionWithMailbox(
      const std::string& task_instruction,
      const gpu::Mailbox& frame_texture,
      ExecuteActionWithMailboxCallback callback) override;

 private:
  bool Initialize(const base::FilePath& model_path, std::string* error);
  void ShutdownEngine();

  // Waits |sync_token|, then RasterInterface::ReadbackImagePixels → SkBitmap
  // (kN32 / RGBA_8888 or BGRA_8888 depending on platform).
  bool ReadbackMailboxToBitmap(const gpu::Mailbox& mailbox,
                               const gpu::SyncToken& sync_token,
                               const gfx::Size& size,
                               SkBitmap* out_bitmap,
                               std::string* error);

  // SkBitmap → interleaved RGB8 for mtmd. Downsamples only when the long edge
  // exceeds kClipTargetEdge. On success, |out_rgb| is width*height*3 and
  // optionally fills |out_clip_img| via clip_build_img_from_pixels.
  bool PrepareClipImage(const SkBitmap& bitmap,
                        std::vector<uint8_t>* out_rgb,
                        int* out_width,
                        int* out_height,
                        clip_image_u8* out_clip_img,
                        std::string* error);

  // Prefer GPU mailbox readback; fall back to FrameTexture.rgba_pixels.
  bool ResolveFrameToBitmap(const mojom::FrameTexture& frame,
                            SkBitmap* out_bitmap,
                            std::string* error);

  mojom::AgentResultPtr RunInference(const std::string& task_instruction,
                                     const mojom::FrameTexture& frame,
                                     const std::vector<uint8_t>& rgb,
                                     int width,
                                     int height);

  static mojom::AgentActionPtr ParseAgentAction(const std::string& text,
                                               int image_width,
                                               int image_height);

  bool PrepareFrameForInference(const mojom::FrameTexture& frame,
                                std::string* diagnostics);

  mojo::Receiver<mojom::SimulachromeAgent> receiver_;
  base::FilePath model_dir_;
  bool model_loaded_ = false;
  bool backend_inited_ = false;
  // False when the GBNF failed to parse and decoding fell back to
  // unconstrained output, which the regex parser then has to cope with.
  bool grammar_active_ = false;

  raw_ptr<llama_model> llama_model_ = nullptr;
  raw_ptr<llama_context> llama_ctx_ = nullptr;
  raw_ptr<mtmd_context> mtmd_ctx_ = nullptr;
  raw_ptr<llama_sampler> sampler_ = nullptr;

  scoped_refptr<base::SingleThreadTaskRunner> io_task_runner_;
  // Declared before the raw raster pointer so destruction drops the context
  // after the pointer is cleared in the destructor body. The body also resets
  // them explicitly; this order is the backstop.
  std::unique_ptr<viz::Gpu> viz_gpu_;
  scoped_refptr<viz::ContextProviderCommandBuffer> raster_context_;
  scoped_refptr<gpu::SharedImageInterface> shared_image_interface_;
  raw_ptr<gpu::raster::RasterInterface> raster_interface_ = nullptr;

  std::string system_prompt_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<SimulachromeAgentImpl> weak_factory_{this};
};

}  // namespace simulachrome

#endif  // SERVICES_SIMULACHROME_SIMULACHROME_AGENT_IMPL_H_
