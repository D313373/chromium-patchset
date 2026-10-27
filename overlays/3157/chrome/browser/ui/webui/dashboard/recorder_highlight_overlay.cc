// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/recorder_highlight_overlay.h"

#include <algorithm>
#include <utility>

#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/text_utils.h"
#include "ui/views/view.h"

namespace dashboard {

class RecorderHighlightView : public views::View {
 public:
  RecorderHighlightView() { SetCanProcessEventsWithinSubtree(false); }

  void Update(std::vector<RecorderHighlightOverlay::Box> boxes,
              std::u16string tip,
              gfx::Point tip_anchor) {
    boxes_ = std::move(boxes);
    tip_ = std::move(tip);
    tip_anchor_ = tip_anchor;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    for (const auto& box : boxes_) {
      const bool selected =
          box.style == RecorderHighlightOverlay::Style::kSelected;
      const SkColor stroke = selected ? SkColorSetARGB(242, 48, 200, 255)
                                      : SkColorSetARGB(242, 16, 185, 129);
      gfx::RectF rect(box.rect);
      if (selected) {
        cc::PaintFlags fill;
        fill.setAntiAlias(true);
        fill.setColor(SkColorSetARGB(20, 48, 200, 255));
        canvas->DrawRoundRect(rect, 4, fill);
      }
      cc::PaintFlags border;
      border.setAntiAlias(true);
      border.setStyle(cc::PaintFlags::kStroke_Style);
      border.setStrokeWidth(selected ? 2 : 1);
      border.setColor(stroke);
      rect.Inset(selected ? 1.0f : 0.5f);
      canvas->DrawRoundRect(rect, 4, border);
    }
    if (tip_.empty()) {
      return;
    }
    const gfx::FontList font;
    const int max_width = std::clamp(width() - 16, 120, 520);
    const int text_width =
        std::min(gfx::GetStringWidth(tip_, font), max_width - 16);
    gfx::Rect tip_rect(tip_anchor_.x() + 14, tip_anchor_.y() + 14,
                       text_width + 16, font.GetHeight() + 10);
    tip_rect.AdjustToFit(GetLocalBounds());
    cc::PaintFlags bg;
    bg.setAntiAlias(true);
    bg.setColor(SkColorSetARGB(230, 17, 24, 39));
    canvas->DrawRoundRect(gfx::RectF(tip_rect), 6, bg);
    tip_rect.Inset(gfx::Insets::VH(5, 8));
    canvas->DrawStringRect(tip_, font, SK_ColorWHITE, tip_rect);
  }

 private:
  std::vector<RecorderHighlightOverlay::Box> boxes_;
  std::u16string tip_;
  gfx::Point tip_anchor_;
};

RecorderHighlightOverlay::RecorderHighlightOverlay() = default;

RecorderHighlightOverlay::~RecorderHighlightOverlay() {
  DestroyWidget();
}

void RecorderHighlightOverlay::Show(content::WebContents* web_contents,
                                    std::vector<Box> boxes,
                                    std::u16string tip,
                                    gfx::Point tip_anchor) {
  if (!web_contents || (boxes.empty() && tip.empty())) {
    Hide();
    return;
  }
  Browser* browser = chrome::FindBrowserWithTab(web_contents);
  BrowserView* browser_view =
      browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
  views::Widget* parent = browser_view ? browser_view->GetWidget() : nullptr;
  const gfx::Rect bounds = web_contents->GetContainerBounds();
  if (!parent || bounds.IsEmpty()) {
    Hide();
    return;
  }

  if (!widget_ || !parent_observation_.IsObservingSource(parent)) {
    DestroyWidget();
    views::Widget::InitParams params(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET,
        views::Widget::InitParams::TYPE_POPUP);
    params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
    params.accept_events = false;
    params.activatable = views::Widget::InitParams::Activatable::kNo;
    params.name = "SimulachromeRecorderHighlight";
    params.bounds = bounds;
    params.parent = parent->GetNativeView();
    widget_ = std::make_unique<views::Widget>(std::move(params));
    view_ = widget_->SetContentsView(std::make_unique<RecorderHighlightView>());
    if (widget_->GetLayer()) {
      widget_->GetLayer()->SetFillsBoundsOpaquely(false);
    }
    parent_observation_.Observe(parent);
  } else if (widget_->GetWindowBoundsInScreen() != bounds) {
    widget_->SetBounds(bounds);
  }

  view_->Update(std::move(boxes), std::move(tip), tip_anchor);
  if (!widget_->IsVisible()) {
    widget_->ShowInactive();
  }
}

void RecorderHighlightOverlay::Hide() {
  if (widget_ && widget_->IsVisible()) {
    view_->Update({}, std::u16string(), gfx::Point());
    widget_->Hide();
  }
}

void RecorderHighlightOverlay::OnWidgetDestroying(views::Widget* widget) {
  DestroyWidget();
}

void RecorderHighlightOverlay::DestroyWidget() {
  parent_observation_.Reset();
  view_ = nullptr;
  widget_.reset();
}

}  // namespace dashboard
