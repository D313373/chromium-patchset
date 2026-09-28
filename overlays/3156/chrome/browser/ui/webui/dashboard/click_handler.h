// chrome/browser/ui/webui/dashboard/click_handler.h
#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_CLICK_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_CLICK_HANDLER_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "content/public/browser/web_contents.h"

namespace content {
class RenderFrameHost;
class RenderWidgetHost;
class RenderWidgetHostView;
}

namespace dashboard {

void SetPlaybackOverlayStatus(content::WebContents* web_contents,
                              const std::string& header,
                              const std::string& detail = std::string());
void ClearPlaybackOverlayStatus(content::WebContents* web_contents);
void UpdatePlaybackOverlayCursor(content::WebContents* web_contents,
                                 int x,
                                 int y,
                                 bool click_pulse);
bool ConvertFramePointToRootCoordinates(content::WebContents* web_contents,
                                        content::RenderFrameHost* frame,
                                        int frame_x,
                                        int frame_y,
                                        int* out_root_x,
                                        int* out_root_y);

class ClickHandler {
 public:
  explicit ClickHandler(content::WebContents* web_contents);
  ~ClickHandler();

  // Injects a trusted click. Mouse-down is sent immediately; mouse-up is
  // posted after a short dwell so the UI thread is never blocked. `done`
  // runs exactly once when provided: false if the sequence could not start,
  // otherwise after mouse-up (or immediately for hover-only / click_count
  // <= 0).
  bool ExecuteClickAt(int center_x,
                      int center_y,
                      bool use_human_offset,
                      int click_count,
                      bool force_activation = false,
                      bool preserve_release_point = false,
                      std::string* diagnostics = nullptr,
                      base::OnceCallback<void(bool)> done = {});

  // Like ExecuteClickAt(), but targets a specific frame widget. Use when the
  // resolved coordinates are in an iframe's client space (OOPIF-safe).
  bool ExecuteClickAtInFrame(content::RenderFrameHost* frame,
                             int x,
                             int y,
                             bool use_human_offset,
                             int click_count,
                             bool force_activation,
                             bool preserve_release_point,
                             std::string* diagnostics,
                             base::OnceCallback<void(bool)> done = {});

  // Drag from one point to another using trusted mouse events.
  bool ExecuteDragAt(int from_x,
                     int from_y,
                     int to_x,
                     int to_y,
                     bool use_human_offset,
                     int steps);

  // Like ExecuteDragAt(), but injects into a specific frame widget.
  bool ExecuteDragAtInFrame(content::RenderFrameHost* frame,
                            int from_x,
                            int from_y,
                            int to_x,
                            int to_y,
                            bool use_human_offset,
                            int steps);

 private:
  // Move the synthetic cursor from (x0,y0) to (x1,y1) in a human-like fashion
  // by emitting a small number of intermediate mousemove events. This is
  // synchronous but non-blocking (no sleeps).
  void PerformMoveAndClick(content::RenderFrameHost* frame,
                           int x,
                           int y,
                           int click_count,
                           bool force_activation,
                           bool preserve_release_point,
                           base::OnceCallback<void(bool)> done);
  
  void MoveCursorHumanLike(content::RenderWidgetHost* rwh,
                           content::RenderWidgetHostView* view,
                           content::RenderFrameHost* frame,
                           int x0,
                           int y0,
                           int x1,
                           int y1);
  
  

  
  
  
  void UpdatePlaybackOverlay(content::RenderFrameHost* frame,
                             int x,
                             int y,
                             bool click_pulse);

  // cached WebContents (not owned)
  raw_ptr<content::WebContents, DanglingUntriaged> web_contents_;

  // cached cursor position — preserved across clicks. Negative values mean "not initialized yet".
  int cursorX_;
  int cursorY_;
  base::TimeTicks last_overlay_update_;
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_CLICK_HANDLER_H_
