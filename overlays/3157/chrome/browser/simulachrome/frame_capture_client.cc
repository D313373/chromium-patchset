// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/frame_capture_client.h"

#include <cstring>

#include "base/logging.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "gpu/command_buffer/client/client_shared_image.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/sync_token.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace simulachrome {
namespace {

std::vector<uint8_t> SkBitmapToRgbaBytes(const SkBitmap& bitmap) {
  std::vector<uint8_t> out;
  if (bitmap.drawsNothing()) {
    return out;
  }
  const SkImageInfo rgba_info =
      SkImageInfo::Make(bitmap.width(), bitmap.height(), kRGBA_8888_SkColorType,
                        kPremul_SkAlphaType, bitmap.refColorSpace());
  const size_t nbytes =
      static_cast<size_t>(bitmap.width()) * bitmap.height() * 4;
  out.resize(nbytes);
  SkPixmap dst(rgba_info, out.data(),
               static_cast<size_t>(bitmap.width()) * 4);
  // SkBitmap::copyTo was removed; readPixels handles color-type conversion.
  if (!bitmap.readPixels(dst)) {
    out.clear();
  }
  return out;
}

}  // namespace

FrameCaptureClient::FrameCaptureClient(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      web_contents_(web_contents) {}

FrameCaptureClient::~FrameCaptureClient() {
  ReleasePending();
}

void FrameCaptureClient::WebContentsDestroyed() {
  // raw_ptr dangling detection traps at shutdown if this pointer outlives
  // the tab. Clear it here, while the allocation is still live, and drop the
  // shared image so the GPU resource is not leaked either.
  ReleasePending();
  web_contents_ = nullptr;
}

void FrameCaptureClient::SetSkipCpuCopy(bool skip) {
  skip_cpu_copy_ = skip;
}

void FrameCaptureClient::ReleasePending() {
  pending_shared_image_.reset();
  if (pending_release_) {
    std::move(pending_release_).Run(gpu::SyncToken(), /*is_lost=*/false);
  }
}

void FrameCaptureClient::CaptureFrameTexture(CaptureCallback callback) {
  if (!web_contents_) {
    std::move(callback).Run(nullptr, "no_web_contents");
    return;
  }

  content::RenderWidgetHostView* view = web_contents_->GetRenderWidgetHostView();
  if (!view) {
    std::move(callback).Run(nullptr, "no_rwhv");
    return;
  }
  if (!view->IsSurfaceAvailableForCopy()) {
    std::move(callback).Run(nullptr, "surface_unavailable");
    return;
  }

  ReleasePending();

  const float dsf = view->GetDeviceScaleFactor();
  const gfx::Size dip_size = view->GetViewBounds().size();

  view->CopySharedImageFromSurface(
      gfx::Rect(), gfx::Size(),
      base::BindOnce(&FrameCaptureClient::OnSharedImageCaptured,
                     weak_factory_.GetWeakPtr(), std::move(callback), dsf,
                     dip_size));
}

void FrameCaptureClient::OnSharedImageCaptured(
    CaptureCallback callback,
    float device_scale_factor,
    gfx::Size viewport_size_dip,
    scoped_refptr<gpu::ClientSharedImage> shared_image,
    viz::ReleaseCallback release_callback) {
  auto frame = mojom::FrameTexture::New();
  frame->device_scale_factor = device_scale_factor;
  frame->viewport_size_dip = viewport_size_dip;

  if (shared_image) {
    pending_shared_image_ = shared_image;
    pending_release_ = std::move(release_callback);
    frame->mailbox = shared_image->mailbox();
    frame->sync_token = shared_image->creation_sync_token();
    frame->size_px = shared_image->size();
    LOG(INFO) << "[SimulachromeCapture] SharedImage mailbox ok size_px="
              << frame->size_px.ToString();
  } else {
    LOG(WARNING) << "[SimulachromeCapture] SharedImage copy failed; "
                    "will try bitmap-only path";
  }

  // The utility reads this mailbox itself. CopyFromSurface would ship a second
  // full-frame RGBA blob through mojo for a path nothing consumes.
  if (skip_cpu_copy_ && shared_image) {
    LOG(INFO) << "[SimulachromeCapture] mailbox readback active; "
                 "skipping CPU copy";
    std::move(callback).Run(std::move(frame), std::string());
    return;
  }

  content::RenderWidgetHostView* view =
      web_contents_ ? web_contents_->GetRenderWidgetHostView() : nullptr;
  if (!view) {
    if (frame->mailbox.IsZero()) {
      std::move(callback).Run(nullptr, "shared_image_and_view_failed");
      return;
    }
    // Mailbox-only; utility needs RasterInterface to read it.
    std::move(callback).Run(std::move(frame), std::string());
    return;
  }

  // CPU RGBA for sandboxed utility (no RasterInterface). Same viewport.
  view->CopyFromSurface(
      gfx::Rect(), frame->size_px.IsEmpty() ? gfx::Size() : frame->size_px,
      base::BindOnce(&FrameCaptureClient::OnBitmapCaptured,
                     weak_factory_.GetWeakPtr(), std::move(callback),
                     std::move(frame)));
}

void FrameCaptureClient::OnBitmapCaptured(
    CaptureCallback callback,
    mojom::FrameTexturePtr frame,
    const viz::CopyOutputBitmapWithMetadata& bitmap_meta) {
  const SkBitmap& bitmap = bitmap_meta.bitmap;
  if (!bitmap.drawsNothing()) {
    if (frame->size_px.IsEmpty()) {
      frame->size_px = gfx::Size(bitmap.width(), bitmap.height());
    }
    frame->rgba_pixels = SkBitmapToRgbaBytes(bitmap);
    LOG(INFO) << "[SimulachromeCapture] CPU rgba_pixels="
              << frame->rgba_pixels.size()
              << " (" << bitmap.width() << "x" << bitmap.height() << ")";
  } else {
    LOG(WARNING) << "[SimulachromeCapture] bitmap copy empty";
  }

  if (frame->mailbox.IsZero() && frame->rgba_pixels.empty()) {
    std::move(callback).Run(nullptr, "capture_failed");
    return;
  }
  std::move(callback).Run(std::move(frame), std::string());
}

}  // namespace simulachrome
