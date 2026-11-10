// chrome/browser/ui/webui/dashboard/click_handler.cc
#include "chrome/browser/ui/webui/dashboard/click_handler.h"

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/no_destructor.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/text_constants.h"
#include "ui/compositor/layer.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <random>

namespace dashboard {

namespace {

constexpr char kForceWindowActivationSwitch[] =
    "simulachrome-force-window-activation";

void ActivateWebContentsForNativeInput(content::WebContents* web_contents,
                                       bool force_activation) {
  if (!web_contents) {
    return;
  }
  force_activation =
      force_activation ||
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          kForceWindowActivationSwitch);
  if (!force_activation) {
    return;
  }
  web_contents->Focus();
  Browser* browser = chrome::FindBrowserWithTab(web_contents);
  if (!browser) {
    return;
  }
  TabStripModel* tabs = browser->tab_strip_model();
  if (tabs) {
    const int index = tabs->GetIndexOfWebContents(web_contents);
    if (index >= 0 && tabs->GetActiveWebContents() != web_contents) {
      tabs->ActivateTabAt(index);
    }
  }
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (browser_view && browser_view->GetWidget()) {
    browser_view->GetWidget()->Activate();
    return;
  }
  if (browser->window()) {
    browser->window()->Activate();
  }
}

// Random helpers
double RandomDouble(double min, double max) {
  static std::mt19937 rng(std::random_device{}());
  std::uniform_real_distribution<double> dist(min, max);
  return dist(rng);
}

double Lerp(double a, double b, double t) {
  return a + (b - a) * t;
}

std::pair<int, int> ComputeDragLatchPoint(int start_x,
                                          int start_y,
                                          int end_x,
                                          int end_y) {
  const double dx = static_cast<double>(end_x - start_x);
  const double dy = static_cast<double>(end_y - start_y);
  const double distance = std::hypot(dx, dy);
  if (distance < 4.0) {
    return {start_x, start_y};
  }
  const double safe_distance = std::max(1.0, distance);
  const double unit_x = dx / safe_distance;
  const double unit_y = dy / safe_distance;
  const double latch_distance =
      std::clamp(distance * 0.12, 7.0, std::min(18.0, distance - 1.0));
  return {static_cast<int>(std::lround(start_x + unit_x * latch_distance)),
          static_cast<int>(std::lround(start_y + unit_y * latch_distance))};
}

struct ClickCoordinateSnapshot {
  gfx::Point widget_point;
  gfx::PointF root_point;
  gfx::Point screen_from_root;
  gfx::Point screen_from_frame_bounds;
  gfx::Rect frame_view_bounds;
  gfx::Rect root_view_bounds;
  gfx::Rect web_contents_bounds;
  float device_scale_factor = 1.0f;
};

ClickCoordinateSnapshot SnapshotClickCoordinates(
    content::WebContents* web_contents,
    content::RenderWidgetHostView* view,
    int widget_x,
    int widget_y) {
  ClickCoordinateSnapshot snapshot;
  snapshot.widget_point = gfx::Point(widget_x, widget_y);
  snapshot.frame_view_bounds = view ? view->GetViewBounds() : gfx::Rect();
  snapshot.web_contents_bounds =
      web_contents ? web_contents->GetViewBounds() : gfx::Rect();
  snapshot.device_scale_factor =
      view ? view->GetDeviceScaleFactor() : 1.0f;

  content::RenderWidgetHostView* root_view = nullptr;
  if (web_contents && web_contents->GetPrimaryMainFrame() &&
      web_contents->GetPrimaryMainFrame()->GetRenderWidgetHost()) {
    root_view =
        web_contents->GetPrimaryMainFrame()->GetRenderWidgetHost()->GetView();
  }
  snapshot.root_view_bounds = root_view ? root_view->GetViewBounds()
                                        : snapshot.web_contents_bounds;

  if (view) {
    snapshot.root_point =
        view->TransformPointToRootCoordSpaceF(gfx::PointF(widget_x, widget_y));
  }

  // One space only: this widget's GetViewBounds origin + widget DIP.
  // TransformPointToRootCoordSpace + root origin mixed chrome/window offset
  // into screenshot clicks and drew the overlay a pane to the left.
  snapshot.screen_from_frame_bounds = snapshot.frame_view_bounds.origin() +
                                      gfx::Vector2d(widget_x, widget_y);
  snapshot.screen_from_root = snapshot.screen_from_frame_bounds;
  if (!snapshot.root_view_bounds.IsEmpty()) {
    snapshot.root_point = gfx::PointF(
        snapshot.screen_from_frame_bounds.x() - snapshot.root_view_bounds.x(),
        snapshot.screen_from_frame_bounds.y() - snapshot.root_view_bounds.y());
  }
  return snapshot;
}

std::string FormatClickCoordinateSnapshot(
    const ClickCoordinateSnapshot& snapshot) {
  const gfx::Vector2d screen_delta =
      snapshot.screen_from_root - snapshot.screen_from_frame_bounds;
  return base::StringPrintf(
      "widget=(%d,%d) root=(%.1f,%.1f) screen_from_root=(%d,%d) "
      "screen_from_frame=(%d,%d) delta=(%d,%d) frame_bounds=(%d,%d %dx%d) "
      "root_bounds=(%d,%d %dx%d) contents_bounds=(%d,%d %dx%d) dsf=%.2f",
      snapshot.widget_point.x(), snapshot.widget_point.y(),
      snapshot.root_point.x(), snapshot.root_point.y(),
      snapshot.screen_from_root.x(), snapshot.screen_from_root.y(),
      snapshot.screen_from_frame_bounds.x(),
      snapshot.screen_from_frame_bounds.y(), screen_delta.x(), screen_delta.y(),
      snapshot.frame_view_bounds.x(), snapshot.frame_view_bounds.y(),
      snapshot.frame_view_bounds.width(), snapshot.frame_view_bounds.height(),
      snapshot.root_view_bounds.x(), snapshot.root_view_bounds.y(),
      snapshot.root_view_bounds.width(), snapshot.root_view_bounds.height(),
      snapshot.web_contents_bounds.x(), snapshot.web_contents_bounds.y(),
      snapshot.web_contents_bounds.width(),
      snapshot.web_contents_bounds.height(),
      snapshot.device_scale_factor);
}

class NativePlaybackOverlayView : public views::View {
 public:
  NativePlaybackOverlayView() {
    SetCanProcessEventsWithinSubtree(false);
  }

  void UpdatePoint(const gfx::Point& point, bool click_pulse) {
    const base::TimeTicks now = base::TimeTicks::Now();
    last_point_ = point;
    last_event_at_ = now;
    has_point_ = true;

    if (trail_.empty() ||
        std::abs(trail_.back().point.x() - point.x()) > 1 ||
        std::abs(trail_.back().point.y() - point.y()) > 1 || click_pulse) {
      trail_.push_back({point, now});
      while (trail_.size() > 72) {
        trail_.pop_front();
      }
    }
    if (click_pulse) {
      pulses_.push_back({point, now});
    }
    Prune(now);
    if (!animation_timer_.IsRunning()) {
      animation_timer_.Start(
          FROM_HERE, base::Milliseconds(16),
          base::BindRepeating(&NativePlaybackOverlayView::OnAnimationTick,
                              base::Unretained(this)));
    }
    SchedulePaint();
  }

  void UpdateStatus(const std::string& header, const std::string& detail) {
    // Status updates correspond to executor state transitions, not pointer
    // motion. Clear any stale crosshair/trail so the overlay only shows the
    // last actual click path while it is still current.
    has_point_ = false;
    trail_.clear();
    pulses_.clear();
    status_header_ = header;
    status_detail_ = detail;
    status_visible_ = !status_header_.empty() || !status_detail_.empty();
    last_event_at_ = base::TimeTicks::Now();
    if (!animation_timer_.IsRunning()) {
      animation_timer_.Start(
          FROM_HERE, base::Milliseconds(16),
          base::BindRepeating(&NativePlaybackOverlayView::OnAnimationTick,
                              base::Unretained(this)));
    }
    SchedulePaint();
  }

  void ClearStatus() {
    has_point_ = false;
    trail_.clear();
    pulses_.clear();
    status_header_.clear();
    status_detail_.clear();
    status_visible_ = false;
    SchedulePaint();
    if (trail_.empty() && pulses_.empty()) {
      if (views::Widget* w = GetWidget()) {
        w->Hide();
      }
    }
  }

 protected:
  void OnPaint(gfx::Canvas* canvas) override {
    if (!has_point_ && !status_visible_) {
      return;
    }

    const gfx::Rect bounds = GetContentsBounds();
    const SkColor line_color = SkColorSetARGB(120, 0, 255, 255);
    const SkColor trail_color = SkColorSetARGB(180, 0, 255, 255);

    if (has_point_) {
      canvas->FillRect(gfx::Rect(0, last_point_.y(), bounds.width(), 1),
                       line_color);
      canvas->FillRect(gfx::Rect(last_point_.x(), 0, 1, bounds.height()),
                       line_color);
    }

    const base::TimeTicks now = base::TimeTicks::Now();
    for (const auto& dot : trail_) {
      const double age_ms = (now - dot.created_at).InMillisecondsF();
      const double alpha = std::clamp(1.0 - (age_ms / 1500.0), 0.0, 1.0);
      const int a = static_cast<int>(alpha * 160.0);
      if (a <= 0) {
        continue;
      }
      canvas->FillRect(gfx::Rect(dot.point.x() - 1, dot.point.y() - 1, 3, 3),
                       SkColorSetA(trail_color, a));
    }

    for (const auto& pulse : pulses_) {
      const double age_ms = (now - pulse.created_at).InMillisecondsF();
      const double t = std::clamp(age_ms / 320.0, 0.0, 1.0);
      const float radius = static_cast<float>(8.0 + (12.0 * t));
      const int alpha = static_cast<int>((1.0 - t) * 230.0);
      if (alpha <= 0) {
        continue;
      }
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setStyle(cc::PaintFlags::kStroke_Style);
      flags.setStrokeWidth(2.0f);
      flags.setColor(SkColorSetARGB(alpha, 255, 120, 120));
      canvas->DrawCircle(gfx::PointF(pulse.point), radius, flags);
    }

    if (status_visible_) {
      constexpr int kHudWidth = 220;
      constexpr int kHudMargin = 16;
      constexpr int kHudPadding = 10;
      constexpr int kHeaderHeight = 22;
      constexpr int kDetailHeight = 18;
      const bool has_detail = !status_detail_.empty();
      const int hud_height =
          kHudPadding * 2 + kHeaderHeight + (has_detail ? kDetailHeight + 4 : 0);
      gfx::Rect hud(bounds.right() - kHudWidth - kHudMargin, kHudMargin,
                    kHudWidth, hud_height);
      cc::PaintFlags fill_flags;
      fill_flags.setAntiAlias(true);
      fill_flags.setStyle(cc::PaintFlags::kFill_Style);
      fill_flags.setColor(SkColorSetARGB(176, 8, 14, 18));
      canvas->DrawRoundRect(hud, 10, fill_flags);

      cc::PaintFlags stroke_flags;
      stroke_flags.setAntiAlias(true);
      stroke_flags.setStyle(cc::PaintFlags::kStroke_Style);
      stroke_flags.setStrokeWidth(1.0f);
      stroke_flags.setColor(SkColorSetARGB(190, 80, 220, 255));
      canvas->DrawRoundRect(hud, 10, stroke_flags);

      const gfx::Rect header_bounds(hud.x() + kHudPadding, hud.y() + kHudPadding,
                                    hud.width() - kHudPadding * 2,
                                    kHeaderHeight);
      canvas->DrawStringRectWithFlags(
          base::UTF8ToUTF16(status_header_), gfx::FontList(),
          SkColorSetARGB(255, 230, 255, 255), header_bounds,
          gfx::Canvas::TEXT_ALIGN_LEFT);
      if (has_detail) {
        const gfx::Rect detail_bounds(
            hud.x() + kHudPadding, header_bounds.bottom() + 4,
            hud.width() - kHudPadding * 2, kDetailHeight);
        canvas->DrawStringRectWithFlags(
            base::UTF8ToUTF16(status_detail_), gfx::FontList(),
            SkColorSetARGB(235, 170, 220, 235), detail_bounds,
            gfx::Canvas::TEXT_ALIGN_LEFT);
      }
    }
  }

 private:
  struct TrailDot {
    gfx::Point point;
    base::TimeTicks created_at;
  };

  struct Pulse {
    gfx::Point point;
    base::TimeTicks created_at;
  };

  void OnAnimationTick() {
    const base::TimeTicks now = base::TimeTicks::Now();
    Prune(now);
    if ((now - last_event_at_) > base::Milliseconds(1500) && trail_.empty() &&
        pulses_.empty() && !status_visible_) {
      animation_timer_.Stop();
      if (views::Widget* w = GetWidget()) {
        w->Hide();
      }
      return;
    }
    SchedulePaint();
  }

  void Prune(base::TimeTicks now) {
    while (!trail_.empty() &&
           (now - trail_.front().created_at) > base::Milliseconds(1500)) {
      trail_.pop_front();
    }
    while (!pulses_.empty() &&
           (now - pulses_.front().created_at) > base::Milliseconds(320)) {
      pulses_.pop_front();
    }
  }

  gfx::Point last_point_;
  bool has_point_ = false;
  bool status_visible_ = false;
  std::string status_header_;
  std::string status_detail_;
  base::TimeTicks last_event_at_;
  std::deque<TrailDot> trail_;
  std::deque<Pulse> pulses_;
  base::RepeatingTimer animation_timer_;
};

bool CanHostPlaybackOverlay(Browser* browser, BrowserView* browser_view) {
  if (!browser || !browser_view || !browser_view->GetWidget()) {
    return false;
  }
  // OAuth login popups are short-lived. Parenting the overlay there UAF-crashes
  // when the window closes and a later action still updates overlay status.
  if (browser->is_type_popup() || browser->is_type_app_popup()) {
    return false;
  }
  return true;
}

class NativePlaybackOverlayHost : public views::WidgetObserver,
                                  public content::WebContentsObserver {
 public:
  static NativePlaybackOverlayHost& Get() {
    static base::NoDestructor<NativePlaybackOverlayHost> instance;
    return *instance;
  }

  void Update(content::WebContents* web_contents,
              const gfx::Point& screen_point,
              bool click_pulse) {
    if (!web_contents) {
      return;
    }
    Browser* browser = chrome::FindBrowserWithTab(web_contents);
    BrowserView* browser_view =
        browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
    if (!CanHostPlaybackOverlay(browser, browser_view)) {
      return;
    }
    if (!browser_view->GetWidget()->IsVisible()) {
      if (widget_ && widget_->IsVisible()) {
        widget_->Hide();
      }
      return;
    }
    TabStripModel* tabs = browser ? browser->tab_strip_model() : nullptr;
    if (!tabs || tabs->GetActiveWebContents() != web_contents) {
      if (widget_ && widget_->IsVisible()) {
        widget_->Hide();
      }
      return;
    }

    // Bound to the page container only — full client bounds cover the tab
    // strip/toolbar, and on Windows DirectComposition WS_EX_TRANSPARENT is
    // not reliably click-through without WS_EX_LAYERED, so chrome UI dies.
    const gfx::Rect bounds = web_contents->GetContainerBounds();
    if (bounds.IsEmpty()) {
      return;
    }

    EnsureWidget(browser_view, bounds);
    if (!widget_ || !overlay_view_) {
      return;
    }

    if (!widget_->IsVisible()) {
      widget_->ShowInactive();
    }
    TrackSourceWebContents(web_contents);
    StartContextMonitor();
    gfx::Point local_point = screen_point;
    local_point.Offset(-bounds.x(), -bounds.y());
    overlay_view_->UpdatePoint(local_point, click_pulse);
  }

  void UpdateStatus(content::WebContents* web_contents,
                    const std::string& header,
                    const std::string& detail) {
    if (!web_contents) {
      return;
    }
    Browser* browser = chrome::FindBrowserWithTab(web_contents);
    BrowserView* browser_view =
        browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
    if (!CanHostPlaybackOverlay(browser, browser_view)) {
      return;
    }
    if (!browser_view->GetWidget()->IsVisible()) {
      if (widget_ && widget_->IsVisible()) {
        widget_->Hide();
      }
      return;
    }
    TabStripModel* tabs = browser ? browser->tab_strip_model() : nullptr;
    if (!tabs || tabs->GetActiveWebContents() != web_contents) {
      if (widget_ && widget_->IsVisible()) {
        widget_->Hide();
      }
      return;
    }
    const gfx::Rect bounds = web_contents->GetContainerBounds();
    if (bounds.IsEmpty()) {
      return;
    }
    EnsureWidget(browser_view, bounds);
    if (!widget_ || !overlay_view_) {
      return;
    }
    if (!widget_->IsVisible()) {
      widget_->ShowInactive();
    }
    TrackSourceWebContents(web_contents);
    StartContextMonitor();
    overlay_view_->UpdateStatus(header, detail);
  }

  void ClearStatus(content::WebContents* web_contents) {
    if (!widget_ || !overlay_view_) {
      DropSourceWebContents();
      return;
    }
    if (web_contents) {
      // If the overlay is currently keyed to a different tab, ignore.
      if (this->web_contents() && this->web_contents() != web_contents) {
        return;
      }
      Browser* browser = chrome::FindBrowserWithTab(web_contents);
      BrowserView* browser_view =
          browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
      views::Widget* parent =
          browser_view ? browser_view->GetWidget() : nullptr;
      if (parent_widget_ && parent && parent != parent_widget_) {
        return;
      }
    }
    overlay_view_->ClearStatus();
    if (widget_->IsVisible()) {
      widget_->Hide();
    }
    DropSourceWebContents();
  }

 private:
  void TrackSourceWebContents(content::WebContents* web_contents) {
    if (this->web_contents() == web_contents) {
      return;
    }
    Observe(web_contents);
  }

  void DropSourceWebContents() {
    context_monitor_timer_.Stop();
    Observe(nullptr);
  }

  void WebContentsDestroyed() override {
    // Tab closed while overlay timer/status still pointed at it.
    context_monitor_timer_.Stop();
    if (overlay_view_) {
      overlay_view_->ClearStatus();
    }
    if (widget_ && widget_->IsVisible()) {
      widget_->Hide();
    }
    // WebContentsObserver clears web_contents_ after this return.
  }

  void StartContextMonitor() {
    if (context_monitor_timer_.IsRunning()) {
      return;
    }
    context_monitor_timer_.Start(
        FROM_HERE, base::Milliseconds(120),
        base::BindRepeating(&NativePlaybackOverlayHost::ValidateContext,
                            base::Unretained(this)));
  }

  void ValidateContext() {
    if (!widget_ || !widget_->IsVisible()) {
      context_monitor_timer_.Stop();
      return;
    }
    content::WebContents* source = web_contents();
    if (!source) {
      widget_->Hide();
      context_monitor_timer_.Stop();
      return;
    }

    Browser* browser = chrome::FindBrowserWithTab(source);
    BrowserView* browser_view =
        browser ? BrowserView::GetBrowserViewForBrowser(browser) : nullptr;
    TabStripModel* tabs = browser ? browser->tab_strip_model() : nullptr;
    const bool context_valid =
        browser_view && browser_view->GetWidget() &&
        browser_view->GetWidget()->IsVisible() && tabs &&
        tabs->GetActiveWebContents() == source;
    if (!context_valid) {
      widget_->Hide();
      if (overlay_view_) {
        overlay_view_->ClearStatus();
      }
      DropSourceWebContents();
    }
  }

  void EnsureWidget(BrowserView* browser_view, const gfx::Rect& bounds) {
    views::Widget* parent = browser_view ? browser_view->GetWidget() : nullptr;
    if (!parent) {
      return;
    }
    // Compare parent widgets, never a possibly-freed BrowserView*.
    if (parent_widget_ != parent || !widget_) {
      DestroyWidget();

      views::Widget::InitParams params(
          views::Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET,
          views::Widget::InitParams::TYPE_POPUP);
      params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
      params.accept_events = false;
      params.activatable = views::Widget::InitParams::Activatable::kNo;
      params.z_order = ui::ZOrderLevel::kNormal;
      params.name = "SimulachromePlaybackOverlay";
      params.bounds = bounds;
      params.SetParent(parent);

      auto* widget = new views::Widget;
      widget->Init(std::move(params));
      auto overlay_view = std::make_unique<NativePlaybackOverlayView>();
      overlay_view_ = overlay_view.get();
      widget->SetContentsView(std::move(overlay_view));
      if (widget->GetLayer()) {
        widget->GetLayer()->SetFillsBoundsOpaquely(false);
      }
      widget_ = widget;
      browser_view_ = browser_view;
      ObserveWidget(parent, /*is_parent=*/true);
      ObserveWidget(widget_, /*is_parent=*/false);
    } else if (widget_->GetWindowBoundsInScreen() != bounds) {
      widget_->SetBounds(bounds);
    }
  }

  void ObserveWidget(views::Widget* widget, bool is_parent) {
    raw_ptr<views::Widget>& slot = is_parent ? parent_widget_ : widget_;
    if (slot && slot != widget) {
      slot->RemoveObserver(this);
    }
    slot = widget;
    if (slot) {
      slot->AddObserver(this);
    }
  }

  void DestroyWidget() {
    if (parent_widget_) {
      parent_widget_->RemoveObserver(this);
      parent_widget_ = nullptr;
    }
    if (widget_) {
      widget_->RemoveObserver(this);
    }
    overlay_view_ = nullptr;
    // The previous host window (often an OAuth popup) may already be gone.
    browser_view_.ExtractAsDangling();
    DropSourceWebContents();
    if (widget_) {
      widget_->CloseNow();
      widget_ = nullptr;
    }
  }

  void OnWidgetDestroying(views::Widget* widget) override {
    if (widget == parent_widget_) {
      parent_widget_->RemoveObserver(this);
      parent_widget_ = nullptr;
      browser_view_.ExtractAsDangling();
      overlay_view_ = nullptr;
      DropSourceWebContents();
      if (widget_) {
        widget_->RemoveObserver(this);
        widget_ = nullptr;
      }
      return;
    }
    if (widget == widget_) {
      widget_->RemoveObserver(this);
      overlay_view_ = nullptr;
      widget_ = nullptr;
    }
  }

  raw_ptr<BrowserView> browser_view_ = nullptr;
  raw_ptr<views::Widget> parent_widget_ = nullptr;
  raw_ptr<views::Widget> widget_ = nullptr;
  raw_ptr<NativePlaybackOverlayView> overlay_view_ = nullptr;
  base::RepeatingTimer context_monitor_timer_;
};

void ForwardPendingMouseUp(content::GlobalRenderFrameHostId frame_id,
                           int x,
                           int y,
                           int click_count,
                           base::OnceCallback<void(bool)> done) {
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_id);
  if (!frame) {
    if (done) {
      std::move(done).Run(false);
    }
    return;
  }
  content::RenderWidgetHost* rwh = frame->GetRenderWidgetHost();
  if (!rwh) {
    if (done) {
      std::move(done).Run(false);
    }
    return;
  }
  content::RenderWidgetHostView* view = rwh->GetView();
  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(frame);
  gfx::Point screen_point(x, y);
  gfx::Point overlay_root(x, y);
  if (web_contents && view) {
    const ClickCoordinateSnapshot up_snapshot =
        SnapshotClickCoordinates(web_contents, view, x, y);
    screen_point = up_snapshot.screen_from_root;
    overlay_root = gfx::Point(base::ClampRound(up_snapshot.root_point.x()),
                              base::ClampRound(up_snapshot.root_point.y()));
  }

  blink::WebMouseEvent mouse_event(blink::WebInputEvent::Type::kMouseUp,
                                   blink::WebInputEvent::kNoModifiers,
                                   base::TimeTicks::Now());
  mouse_event.button = blink::WebMouseEvent::Button::kLeft;
  mouse_event.click_count = click_count;
  mouse_event.SetPositionInWidget(x, y);
  mouse_event.SetPositionInScreen(screen_point.x(), screen_point.y());
  rwh->ForwardMouseEvent(mouse_event);
  if (web_contents) {
    UpdatePlaybackOverlayCursor(web_contents, overlay_root.x(),
                                overlay_root.y(), /*click_pulse=*/true);
  }
  LOG(INFO) << "[Click] MouseUp sent at (" << x << "," << y << ") on frame "
            << frame;
  if (done) {
    std::move(done).Run(true);
  }
}

}  // namespace

void SetPlaybackOverlayStatus(content::WebContents* web_contents,
                              const std::string& header,
                              const std::string& detail) {
  if (!dashboard::IsPlaybackOverlayEnabled()) {
    return;
  }
  NativePlaybackOverlayHost::Get().UpdateStatus(web_contents, header, detail);
}

void ClearPlaybackOverlayStatus(content::WebContents* web_contents) {
  NativePlaybackOverlayHost::Get().ClearStatus(web_contents);
}

void UpdatePlaybackOverlayCursor(content::WebContents* web_contents,
                                 int x,
                                 int y,
                                 bool click_pulse) {
  if (!dashboard::IsPlaybackOverlayEnabled() || !web_contents) {
    return;
  }
  content::RenderFrameHost* frame = web_contents->GetPrimaryMainFrame();
  if (!frame) {
    return;
  }
  content::RenderWidgetHost* rwh = frame->GetRenderWidgetHost();
  content::RenderWidgetHostView* view = rwh ? rwh->GetView() : nullptr;
  if (!view) {
    return;
  }
  const ClickCoordinateSnapshot snapshot =
      SnapshotClickCoordinates(web_contents, view, x, y);
  NativePlaybackOverlayHost::Get().Update(
      web_contents, snapshot.screen_from_root, click_pulse);
}

bool ConvertFramePointToRootCoordinates(content::WebContents* web_contents,
                                        content::RenderFrameHost* frame,
                                        int frame_x,
                                        int frame_y,
                                        int* out_root_x,
                                        int* out_root_y) {
  if (!out_root_x || !out_root_y || !web_contents || !frame) {
    return false;
  }
  content::RenderWidgetHost* rwh = frame->GetRenderWidgetHost();
  content::RenderWidgetHostView* view = rwh ? rwh->GetView() : nullptr;
  if (!view) {
    return false;
  }
  const ClickCoordinateSnapshot snapshot =
      SnapshotClickCoordinates(web_contents, view, frame_x, frame_y);
  *out_root_x = base::ClampRound(snapshot.root_point.x());
  *out_root_y = base::ClampRound(snapshot.root_point.y());
  return true;
}

// Delivery only — not screenshot mapping. The model already picked a
// viewport DIP from the picture. Captcha widgets live in OOPIFs with their
// own RenderWidgetHost, so the same viewport point must be forwarded to the
// child widget under that pixel or the event never enters the iframe.
bool FindLocalRootForRootPoint(content::WebContents* web_contents,
                               int root_x,
                               int root_y,
                               content::RenderFrameHost** out_frame,
                               int* out_local_x,
                               int* out_local_y) {
  if (!web_contents || !out_frame || !out_local_x || !out_local_y) {
    return false;
  }
  content::RenderFrameHost* best = nullptr;
  int best_area = std::numeric_limits<int>::max();
  int best_x = root_x;
  int best_y = root_y;

  content::RenderFrameHost* main = web_contents->GetPrimaryMainFrame();
  content::RenderWidgetHostView* main_view =
      (main && main->GetRenderWidgetHost()) ? main->GetRenderWidgetHost()->GetView()
                                            : nullptr;
  if (!main_view) {
    return false;
  }
  // One space only: screen DIP. Screenshot and SetPositionInWidget are both
  // relative to the main view origin. Mixing TransformPointToRootCoordSpace
  // with GetViewBounds was shifting OOPIF clicks by the chrome/window offset.
  const gfx::Point screen =
      main_view->GetViewBounds().origin() + gfx::Vector2d(root_x, root_y);

  auto consider = [&](content::RenderFrameHost* rfh) {
    if (!rfh || !rfh->IsActive()) {
      return;
    }
    content::RenderFrameHost* parent = rfh->GetParent();
    if (parent && rfh->GetRenderWidgetHost() == parent->GetRenderWidgetHost()) {
      return;
    }
    content::RenderWidgetHost* rwh = rfh->GetRenderWidgetHost();
    content::RenderWidgetHostView* view = rwh ? rwh->GetView() : nullptr;
    if (!view) {
      return;
    }
    const gfx::Rect bounds = view->GetViewBounds();
    if (bounds.IsEmpty() || !bounds.Contains(screen)) {
      return;
    }
    const int area = std::max(1, bounds.width() * bounds.height());
    if (!best || area < best_area) {
      best = rfh;
      best_area = area;
      best_x = screen.x() - bounds.x();
      best_y = screen.y() - bounds.y();
    }
  };

  consider(main);
  web_contents->ForEachRenderFrameHost(
      [&](content::RenderFrameHost* rfh) { consider(rfh); });

  if (!best) {
    return false;
  }
  *out_frame = best;
  *out_local_x = best_x;
  *out_local_y = best_y;
  return true;
}

ClickHandler::ClickHandler(content::WebContents* web_contents)
    : web_contents_(web_contents),
      cursorX_(-1),
      cursorY_(-1),
      last_overlay_update_(base::TimeTicks()) {}

ClickHandler::~ClickHandler() = default;

bool ClickHandler::ExecuteClickAt(int center_x,
                                  int center_y,
                                  bool use_human_offset,
                                  int click_count,
                                  bool force_activation,
                                  bool preserve_release_point,
                                  std::string* diagnostics,
                                  base::OnceCallback<void(bool)> done) {
  if (!web_contents_) {
    if (done) {
      std::move(done).Run(false);
    }
    return false;
  }
  content::RenderFrameHost* target = web_contents_->GetPrimaryMainFrame();
  int local_x = center_x;
  int local_y = center_y;
  // Always deliver into the widget under this viewport pixel (captcha OOPIF).
  // Overlay stays in main-view DIP so child-local (20,20) cannot draw left.
  if (FindLocalRootForRootPoint(web_contents_, center_x, center_y, &target,
                                &local_x, &local_y) &&
      target) {
    if (target != web_contents_->GetPrimaryMainFrame()) {
      LOG(INFO) << "[Click] Routed root=(" << center_x << "," << center_y
                << ") into child widget local=(" << local_x << "," << local_y
                << ") frame=" << target;
    }
  } else {
    target = web_contents_->GetPrimaryMainFrame();
    local_x = center_x;
    local_y = center_y;
  }
  UpdatePlaybackOverlayCursor(web_contents_, center_x, center_y,
                              /*click_pulse=*/true);
  return ExecuteClickAtInFrame(target, local_x, local_y, use_human_offset,
                               click_count, force_activation,
                               preserve_release_point, diagnostics,
                               std::move(done));
}

void ClickHandler::PerformMoveAndClick(content::RenderFrameHost* frame,
                                       int x,
                                       int y,
                                       int click_count,
                                       bool force_activation,
                                       bool preserve_release_point,
                                       base::OnceCallback<void(bool)> done) {
  // Use the 'unused' focus helper to satisfy the compiler and improve realism
  ActivateWebContentsForNativeInput(web_contents_, force_activation);

  content::RenderWidgetHost* rwh = frame->GetRenderWidgetHost();
  content::RenderWidgetHostView* view = rwh->GetView();
  if (view && (force_activation ||
               base::CommandLine::ForCurrentProcess()->HasSwitch(
                   kForceWindowActivationSwitch))) {
    view->Focus();
  }

  // 1. Initial Movement
  MoveCursorHumanLike(rwh, view, frame, cursorX_, cursorY_, x, y);

  if (click_count <= 0) {
    if (done) {
      std::move(done).Run(true);
    }
    return;
  }

  // 2. Pixel Shift for "micro-jitter"
  int shift_x = 0;
  int shift_y = 0;
  if (!preserve_release_point) {
    shift_x = (std::rand() % 3) - 1;
    shift_y = (std::rand() % 3) - 1;
  }

  const ClickCoordinateSnapshot down_snapshot =
      SnapshotClickCoordinates(web_contents_, view, x, y);

  // 3. MOUSE DOWN
  blink::WebMouseEvent mouse_event(
      blink::WebInputEvent::Type::kMouseDown, blink::WebInputEvent::kNoModifiers,
      base::TimeTicks::Now());
  mouse_event.button = blink::WebMouseEvent::Button::kLeft;
  mouse_event.click_count = click_count;
  mouse_event.SetPositionInWidget(x, y);
  mouse_event.SetPositionInScreen(down_snapshot.screen_from_root.x(),
                                 down_snapshot.screen_from_root.y());

  rwh->ForwardMouseEvent(mouse_event);
  UpdatePlaybackOverlay(frame, x, y, /*click_pulse=*/true);
  LOG(INFO) << "[Click] MouseDown sent at (" << x << "," << y << ") on frame " << frame;

  const int final_x = x + shift_x;
  const int final_y = y + shift_y;
  cursorX_ = final_x;
  cursorY_ = final_y;

  // 4. DWELL then MOUSE UP — never Sleep on the UI / executor sequence.
  const base::TimeDelta dwell =
      base::Milliseconds(60 + (std::rand() % 60));
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ForwardPendingMouseUp, frame->GetGlobalId(), final_x,
                     final_y, click_count, std::move(done)),
      dwell);
}

bool ClickHandler::ExecuteClickAtInFrame(content::RenderFrameHost* frame,
                                         int x,
                                         int y,
                                         bool use_human_offset,
                                         int click_count,
                                         bool force_activation,
                                         bool preserve_release_point,
                                         std::string* diagnostics,
                                         base::OnceCallback<void(bool)> done) {
  if (!frame || !frame->GetRenderWidgetHost() || !web_contents_) {
    if (diagnostics) *diagnostics = "Invalid frame, host, or web_contents";
    if (done) {
      std::move(done).Run(false);
    }
    return false;
  }

  // 1. Force Browser/OS Window Activation & Focus
  // We use the helper function here to satisfy the -Wunused-function check.
  ActivateWebContentsForNativeInput(web_contents_, force_activation);

  content::RenderWidgetHost* rwh = frame->GetRenderWidgetHost();
  content::RenderWidgetHostView* view = rwh->GetView();
  if (view && (force_activation ||
               base::CommandLine::ForCurrentProcess()->HasSwitch(
                   kForceWindowActivationSwitch))) {
    view->Focus();
  }

  // 2. Maintain Cursor State
  if (cursorX_ < 0 || cursorY_ < 0) {
    cursorX_ = x;
    cursorY_ = y;
  }

  // 3. Collect Diagnostics
  // We call FormatClickCoordinateSnapshot here to satisfy the other unused-function check.
  if (diagnostics) {
    if (view) {
      const ClickCoordinateSnapshot snapshot =
          SnapshotClickCoordinates(web_contents_, view, x, y);
      *diagnostics = FormatClickCoordinateSnapshot(snapshot);
    } else {
      *diagnostics = base::StringPrintf("Clicked coords=(%d,%d) in frame=%p (no view)",
                                       x, y, (void*)frame);
    }
  }

  // 4. Perform the Click Sequence
  PerformMoveAndClick(frame, x, y, click_count, force_activation,
                      preserve_release_point, std::move(done));

  return true;
}
// MAKE SURE NO STRAY CODE FROM THE OLD VERSION EXISTS BELOW THIS LINE

bool ClickHandler::ExecuteDragAt(int from_x,
                                 int from_y,
                                 int to_x,
                                 int to_y,
                                 bool use_human_offset,
                                 int steps) {
  if (!web_contents_)
    return false;
  return ExecuteDragAtInFrame(web_contents_->GetPrimaryMainFrame(), from_x,
                              from_y, to_x, to_y, use_human_offset, steps);
}

bool ClickHandler::ExecuteDragAtInFrame(content::RenderFrameHost* frame,
                                        int from_x,
                                        int from_y,
                                        int to_x,
                                        int to_y,
                                        bool use_human_offset,
                                        int steps) {
  if (!web_contents_ || !frame)
    return false;

  content::RenderWidgetHost* rwh = frame->GetRenderWidgetHost();
  if (!rwh)
    return false;

  auto* rwhv = rwh->GetView();
  if (!rwhv)
    return false;

  int start_x =
      from_x + (use_human_offset ? static_cast<int>(RandomDouble(-3, 3)) : 0);
  int start_y =
      from_y + (use_human_offset ? static_cast<int>(RandomDouble(-3, 3)) : 0);
  int end_x =
      to_x + (use_human_offset ? static_cast<int>(RandomDouble(-3, 3)) : 0);
  int end_y =
      to_y + (use_human_offset ? static_cast<int>(RandomDouble(-3, 3)) : 0);
  start_x = std::max(0, start_x);
  start_y = std::max(0, start_y);
  end_x = std::max(0, end_x);
  end_y = std::max(0, end_y);

  steps = std::clamp(steps, 2, 80);

  if (cursorX_ < 0 || cursorY_ < 0) {
    cursorX_ = start_x;
    cursorY_ = start_y;
  }

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          kForceWindowActivationSwitch)) {
    rwhv->Focus();
  }

  MoveCursorHumanLike(rwh, rwhv, frame, cursorX_, cursorY_, start_x, start_y);
  cursorX_ = start_x;
  cursorY_ = start_y;

  const ClickCoordinateSnapshot start_snapshot =
      SnapshotClickCoordinates(web_contents_, rwhv, start_x, start_y);
  blink::WebMouseEvent mouse_down(
      blink::WebInputEvent::Type::kMouseDown,
      blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now());
  mouse_down.button = blink::WebMouseEvent::Button::kLeft;
  mouse_down.SetPositionInWidget(start_x, start_y);
  mouse_down.SetPositionInScreen(start_snapshot.screen_from_root.x(),
                                 start_snapshot.screen_from_root.y());
  mouse_down.click_count = 1;
  rwh->ForwardMouseEvent(mouse_down);
  UpdatePlaybackOverlay(frame, start_x, start_y, /*click_pulse=*/true);

  int prev_x = start_x;
  int prev_y = start_y;
  int path_start_x = start_x;
  int path_start_y = start_y;

  const auto latch_point = ComputeDragLatchPoint(start_x, start_y, end_x, end_y);
  if (latch_point.first != start_x || latch_point.second != start_y) {
    const int latch_x = std::max(0, latch_point.first);
    const int latch_y = std::max(0, latch_point.second);
    const ClickCoordinateSnapshot latch_snapshot =
        SnapshotClickCoordinates(web_contents_, rwhv, latch_x, latch_y);
    blink::WebMouseEvent latch_event(
        blink::WebInputEvent::Type::kMouseMove,
        blink::WebInputEvent::kLeftButtonDown, base::TimeTicks::Now());
    latch_event.button = blink::WebMouseEvent::Button::kLeft;
    latch_event.SetPositionInWidget(latch_x, latch_y);
    latch_event.SetPositionInScreen(latch_snapshot.screen_from_root.x(),
                                    latch_snapshot.screen_from_root.y());
    latch_event.movement_x = latch_x - prev_x;
    latch_event.movement_y = latch_y - prev_y;
    rwh->ForwardMouseEvent(latch_event);
    UpdatePlaybackOverlay(frame, latch_x, latch_y, /*click_pulse=*/false);
    prev_x = latch_x;
    prev_y = latch_y;
    path_start_x = latch_x;
    path_start_y = latch_y;
  }

  const double path_jitter = std::max(0.0, dashboard::GetClickPathJitterPx());
  for (int i = 1; i <= steps; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(steps);
    int xi = static_cast<int>(std::round(Lerp(path_start_x, end_x, t)));
    int yi = static_cast<int>(std::round(Lerp(path_start_y, end_y, t)));
    if (i < steps && path_jitter > 0.0) {
      xi += static_cast<int>(RandomDouble(-path_jitter, path_jitter));
      yi += static_cast<int>(RandomDouble(-path_jitter, path_jitter));
    }
    xi = std::max(0, xi);
    yi = std::max(0, yi);
    const ClickCoordinateSnapshot move_snapshot =
        SnapshotClickCoordinates(web_contents_, rwhv, xi, yi);
    blink::WebMouseEvent move_event(
        blink::WebInputEvent::Type::kMouseMove,
        blink::WebInputEvent::kLeftButtonDown, base::TimeTicks::Now());
    move_event.button = blink::WebMouseEvent::Button::kLeft;
    move_event.SetPositionInWidget(xi, yi);
    move_event.SetPositionInScreen(move_snapshot.screen_from_root.x(),
                                   move_snapshot.screen_from_root.y());
    move_event.movement_x = xi - prev_x;
    move_event.movement_y = yi - prev_y;
    rwh->ForwardMouseEvent(move_event);
    UpdatePlaybackOverlay(frame, xi, yi, /*click_pulse=*/false);
    prev_x = xi;
    prev_y = yi;
  }

  const ClickCoordinateSnapshot end_snapshot =
      SnapshotClickCoordinates(web_contents_, rwhv, end_x, end_y);
  blink::WebMouseEvent mouse_up = mouse_down;
  mouse_up.SetType(blink::WebInputEvent::Type::kMouseUp);
  mouse_up.SetPositionInWidget(end_x, end_y);
  mouse_up.SetPositionInScreen(end_snapshot.screen_from_root.x(),
                               end_snapshot.screen_from_root.y());
  mouse_up.movement_x = end_x - prev_x;
  mouse_up.movement_y = end_y - prev_y;
  rwh->ForwardMouseEvent(mouse_up);
  UpdatePlaybackOverlay(frame, end_x, end_y, /*click_pulse=*/true);

  cursorX_ = end_x;
  cursorY_ = end_y;
  return true;
}

void ClickHandler::MoveCursorHumanLike(content::RenderWidgetHost* rwh,
                                       content::RenderWidgetHostView* view,
                                       content::RenderFrameHost* frame,
                                       int x0,
                                       int y0,
                                       int x1,
                                       int y1) {
  if (!rwh || !view)
    return;

  // Compute distance and steps; vary steps by distance (humans take more steps for longer moves).
  const double dx = static_cast<double>(x1 - x0);
  const double dy = static_cast<double>(y1 - y0);
  const double dist = std::hypot(dx, dy);
  int steps = std::clamp(static_cast<int>(dist / 15.0), 5, 15);

  auto ease = [](double t) { return (t < 0.5) ? (2.0 * t * t) : (1.0 - std::pow(-2.0 * t + 2.0, 2.0) / 2.0); };

  int prev_x = x0;
  int prev_y = y0;
  for (int i = 1; i <= steps; ++i) {
    const double t = static_cast<double>(i) / steps;
    const double et = ease(t);
    // Micro tremor ~ 10Hz-like at small amplitude
    const double tremor = 0.4 * std::sin(i * 0.9);
    double step_jitter = dashboard::GetClickPathJitterPx();
    int xi = static_cast<int>(Lerp(x0, x1, et) + RandomDouble(-step_jitter, step_jitter) + tremor);
    int yi = static_cast<int>(Lerp(y0, y1, et) + RandomDouble(-step_jitter, step_jitter) + tremor);

    const ClickCoordinateSnapshot move_snapshot =
        SnapshotClickCoordinates(web_contents_, view, xi, yi);

    blink::WebMouseEvent move_event(
        blink::WebInputEvent::Type::kMouseMove, blink::WebInputEvent::kNoModifiers,
        base::TimeTicks::Now());
    move_event.SetPositionInWidget(xi, yi);
    move_event.SetPositionInScreen(move_snapshot.screen_from_root.x(),
                                   move_snapshot.screen_from_root.y());
    move_event.movement_x = xi - prev_x;
    move_event.movement_y = yi - prev_y;
    rwh->ForwardMouseEvent(move_event);
    UpdatePlaybackOverlay(frame, xi, yi, /*click_pulse=*/false);
    prev_x = xi; prev_y = yi;
  }
}

void ClickHandler::UpdatePlaybackOverlay(content::RenderFrameHost* frame,
                                         int x,
                                         int y,
                                         bool click_pulse) {
  if (!dashboard::IsPlaybackOverlayEnabled() || !frame || !web_contents_) {
    return;
  }

  const base::TimeTicks now = base::TimeTicks::Now();
  if (!click_pulse && !last_overlay_update_.is_null() &&
      (now - last_overlay_update_) < base::Milliseconds(16)) {
    return;
  }
  last_overlay_update_ = now;

  content::RenderWidgetHost* rwh = frame->GetRenderWidgetHost();
  content::RenderWidgetHostView* view = rwh ? rwh->GetView() : nullptr;
  if (!view) {
    return;
  }
  const ClickCoordinateSnapshot snapshot =
      SnapshotClickCoordinates(web_contents_, view, x, y);
  NativePlaybackOverlayHost::Get().Update(
      web_contents_, snapshot.screen_from_root, click_pulse);
}

}  // namespace dashboard
