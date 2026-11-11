#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CLICK_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CLICK_ACTION_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/element_readiness_checker.h"

class ClickAction : public dashboard::actions::Action {
 public:
  explicit ClickAction(Profile* profile);
  ~ClickAction() override;

  // Action implementations
  void Execute(content::WebContents* web_contents, Callback callback) override;
  bool SetParams(const base::Value::Dict& params) override;

 private:
  void OnElementReady(content::WebContents* web_contents,
                      Callback callback,
                      bool ready,
                      const dashboard::ResolvedElement& element,
                      const std::string& error);
  void OnNativeClickDone(Callback callback,
                         std::string ok_message,
                         std::string fail_message,
                         bool success);
  void OnTrustedClickDone(content::WebContents* web_contents,
                          dashboard::ResolvedElement element,
                          Callback callback,
                          std::string ok_message,
                          bool success);

  std::vector<dashboard::Locator> locators_;
  // Optional execution hints captured during recording. These are not treated
  // as user-facing locators (rec log) but can dramatically reduce frame/shadow
  // traversal during resolution.
  std::string frame_id_hint_;
  std::string shadow_chain_hint_;
  int wait_timeout_ms_ = 5000;
  bool use_human_offset_ = true;
  // True only when the guest explicitly sent humanOffset=false with a
  // screenshot point. Locator clicks that also carry an element center must
  // not take this path.
  bool screenshot_point_ = false;
  bool prefer_center_point_ = false;
  bool allow_href_navigation_fallback_ = true;
  bool force_activation_ = false;
  int click_count_ = 1;
  // Optional direct coordinate injection (bypasses locator resolution).
  bool has_direct_coords_ = false;
  int direct_x_ = 0;
  int direct_y_ = 0;

  // Heap-allocated helper for async operations
  std::unique_ptr<ElementReadinessChecker> checker_;

  base::WeakPtrFactory<ClickAction> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_CLICK_ACTION_H_
