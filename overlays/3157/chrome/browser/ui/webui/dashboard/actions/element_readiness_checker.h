#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_ELEMENT_READINESS_CHECKER_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_ELEMENT_READINESS_CHECKER_H_

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/locator_resolver.h"
#include "content/public/browser/web_contents.h"

class ElementReadinessChecker {
 public:
  using ReadinessCallback = base::OnceCallback<void(bool ready,
                                                     const dashboard::ResolvedElement& element,
                                                     const std::string& error)>;

  explicit ElementReadinessChecker(content::WebContents* web_contents);
  ~ElementReadinessChecker();

  // DEPRECATED: Synchronous version (blocks UI thread). Use WaitForElementReadyAsync instead.
  // The main function that checks if an element is ready for interaction.
  // It will poll the element's state until it's ready or the timeout is
  // reached.
  bool WaitForElementToBeClickable(const dashboard::Element& element,
                                     int timeout_ms);

  // REMOVED: Synchronous version that blocked UI thread.
  // All code now uses WaitForElementReadyAsync() instead.

  // Async version: Waits for element to be ready without blocking.
  // Calls callback with (ready, element, error) when complete or timeout.
  void WaitForElementReadyAsync(const std::vector<dashboard::Locator>& locators,
                                dashboard::SearchScope scope,
                                int timeout_ms,
                                ReadinessCallback callback);

  // Cancel any pending wait
  void Cancel();

 private:
  void OnElementResolved(bool success,
                         const dashboard::ResolvedElement& element,
                         const std::string& error);

  raw_ptr<content::WebContents> web_contents_;
  std::unique_ptr<dashboard::LocatorResolver> resolver_;

  // Async state
  ReadinessCallback async_callback_;
  base::TimeTicks async_start_time_;
  base::TimeDelta async_timeout_;
  dashboard::ResolvedElement async_element_;
  bool cancelled_ = false;

  base::WeakPtrFactory<ElementReadinessChecker> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_ELEMENT_READINESS_CHECKER_H_
