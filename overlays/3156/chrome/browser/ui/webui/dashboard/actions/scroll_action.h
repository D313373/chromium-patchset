#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_SCROLL_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_SCROLL_ACTION_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/element_readiness_checker.h"

class Profile;

namespace content {
class WebContents;
}

class ScrollAction : public dashboard::actions::Action {
 public:
  explicit ScrollAction(Profile* profile);
  ~ScrollAction() override;

  bool SetParams(const base::Value::Dict& params) override;
  void Execute(content::WebContents* web_contents, Callback callback) override;

 private:
  void OnElementReady(content::WebContents* web_contents,
                      Callback callback,
                      bool ready,
                      const dashboard::ResolvedElement& element,
                      const std::string& error);
  void ScrollMainDocument(content::WebContents* web_contents,
                          Callback callback);
  void DispatchTrustedWheelScroll(content::WebContents* web_contents,
                                  int target_x,
                                  int target_y,
                                  double target_height,
                                  double target_width,
                                  Callback callback);
  void ComputeScrollDeltas(double target_height,
                           double target_width,
                           double* out_delta_x,
                           double* out_delta_y) const;

  bool use_distance_ = false;
  double distance_ = 0.0;  // Vertical scroll delta in pixels.
  double percent_y_ = 0.0;  // Vertical scroll percentage (0-100)
  double percent_x_ = 0.0;  // Horizontal scroll percentage (0-100)
  std::string element_type_;  // "window" or "element"
  std::vector<dashboard::Locator> locators_;  // For element scrolling

  std::unique_ptr<ElementReadinessChecker> checker_;
  base::WeakPtrFactory<ScrollAction> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_SCROLL_ACTION_H_
