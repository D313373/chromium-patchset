// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_RECORDER_HIGHLIGHT_OVERLAY_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_RECORDER_HIGHLIGHT_OVERLAY_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

namespace content {
class WebContents;
}

namespace dashboard {

class RecorderHighlightView;

// Click-through browser widget that outlines page elements for the recorder
// (loop candidates, guided scrape picks). Drawn outside the page, so nothing
// is added to the DOM.
class RecorderHighlightOverlay : public views::WidgetObserver {
 public:
  enum class Style { kCandidate, kSelected };
  struct Box {
    gfx::Rect rect;  // Web contents view DIPs.
    Style style = Style::kCandidate;
  };

  RecorderHighlightOverlay();
  RecorderHighlightOverlay(const RecorderHighlightOverlay&) = delete;
  RecorderHighlightOverlay& operator=(const RecorderHighlightOverlay&) = delete;
  ~RecorderHighlightOverlay() override;

  void Show(content::WebContents* web_contents,
            std::vector<Box> boxes,
            std::u16string tip = std::u16string(),
            gfx::Point tip_anchor = gfx::Point());
  void Hide();

  // views::WidgetObserver:
  void OnWidgetDestroying(views::Widget* widget) override;

 private:
  void DestroyWidget();

  std::unique_ptr<views::Widget> widget_;
  raw_ptr<RecorderHighlightView> view_ = nullptr;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      parent_observation_{this};
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_RECORDER_HIGHLIGHT_OVERLAY_H_
