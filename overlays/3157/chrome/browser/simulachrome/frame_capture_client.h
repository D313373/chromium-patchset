// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SIMULACHROME_FRAME_CAPTURE_CLIENT_H_
#define CHROME_BROWSER_SIMULACHROME_FRAME_CAPTURE_CLIENT_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "components/viz/common/resources/release_callback.h"
#include "services/simulachrome/public/mojom/simulachrome_agent.mojom.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/size.h"

namespace content {
class WebContents;
}

namespace gpu {
class ClientSharedImage;
}

namespace simulachrome {

// Mailbox via CopySharedImageFromSurface. CopyFromSurface (a full RGBA copy
// through mojo) runs only when the utility has not confirmed mailbox readback,
// or when the shared image itself failed.
class FrameCaptureClient : public content::WebContentsObserver {
 public:
  using CaptureCallback =
      base::OnceCallback<void(mojom::FrameTexturePtr frame,
                              std::string error)>;

  explicit FrameCaptureClient(content::WebContents* web_contents);
  ~FrameCaptureClient() override;

  // True after the utility's AttachGpu reply says the raster context is up.
  // A failed or disconnected utility sets this back to false.
  void SetSkipCpuCopy(bool skip);

  // content::WebContentsObserver:
  void WebContentsDestroyed() override;

  void CaptureFrameTexture(CaptureCallback callback);
  void ReleasePending();

 private:
  void OnSharedImageCaptured(CaptureCallback callback,
                             float device_scale_factor,
                             gfx::Size viewport_size_dip,
                             scoped_refptr<gpu::ClientSharedImage> shared_image,
                             viz::ReleaseCallback release_callback);

  void OnBitmapCaptured(CaptureCallback callback,
                        mojom::FrameTexturePtr frame,
                        const viz::CopyOutputBitmapWithMetadata& bitmap_meta);

  raw_ptr<content::WebContents> web_contents_;
  bool skip_cpu_copy_ = false;
  scoped_refptr<gpu::ClientSharedImage> pending_shared_image_;
  viz::ReleaseCallback pending_release_;

  base::WeakPtrFactory<FrameCaptureClient> weak_factory_{this};
};

}  // namespace simulachrome

#endif  // CHROME_BROWSER_SIMULACHROME_FRAME_CAPTURE_CLIENT_H_
