#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_LISTEN_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_LISTEN_ACTION_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/locator_resolver.h"
#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include "content/public/browser/global_routing_id.h"
#include "third_party/re2/src/re2/re2.h"

class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace dashboard {
class LocatorResolver;
}  // namespace dashboard

// Native listener backend. This implementation evaluates conditions against
// repeated unified page-map snapshots rather than installing renderer-side JS
// observers.
class ListenAction : public dashboard::actions::Action {
 public:
  explicit ListenAction(Profile* profile);
  ~ListenAction() override;

  ListenAction(const ListenAction&) = delete;
  ListenAction& operator=(const ListenAction&) = delete;

  bool SetParams(const base::Value::Dict& params) override;
  void Execute(content::WebContents* web_contents, Callback callback) override;

 private:
  void StartNativeListen(content::WebContents* web_contents);
  void PollNativeListenerState();
  void ScheduleNextNativePoll();
  void OnListenerWatchTriggered();
  void OnNativePageMapCaptured(dashboard::UnifiedPageMapResult result);
  void StartResolverPageMapFallback(const std::string& reason);
  void OnResolverPageMapFallbackReady(base::Value::Dict resolver_page_map);
  void StartResolverLocatorFallback(const std::string& reason);
  void OnResolverLocatorFallbackReady(bool success,
                                      const dashboard::ResolvedElement& element,
                                      const std::string& error);
  void Finish(bool success, const std::string& message);
  void FinishAfterMatch(const std::string& match_payload);
  void StartOneShotOnMatch(const std::string& match_payload);
  void OnOnMatchScriptLoaded(std::string match_payload,
                             bool ok,
                             base::Value::Dict script);

  std::vector<dashboard::Locator> locators_;
  std::optional<content::GlobalRenderFrameHostId> frame_hint_;
  std::string condition_;
  std::string text_;
  bool text_use_regex_ = false;
  std::string attribute_;
  bool attribute_use_regex_ = false;
  std::string ax_role_hint_;
  std::string ax_name_hint_;
  std::string interactive_ancestor_role_hint_;
  std::string on_match_script_id_;
  int timeout_ms_ = 60000;
  int sample_interval_ms_ = 250;
  uint64_t listener_watch_registration_id_ = 0;
  bool listener_watch_active_ = false;
  raw_ptr<content::WebContents> web_contents_ = nullptr;
  base::TimeTicks start_time_;
  int poll_count_ = 0;
  bool native_baseline_initialized_ = false;
  bool native_prev_visible_ = false;
  int native_baseline_child_count_ = 0;
  bool resolver_page_map_fallback_attempted_ = false;
  bool resolver_locator_fallback_active_ = false;
  std::unique_ptr<dashboard::PageMapExtractor> resolver_page_map_extractor_;
  std::unique_ptr<dashboard::LocatorResolver> resolver_locator_fallback_;
  std::unique_ptr<RE2> cached_text_regex_;
  std::unique_ptr<RE2> cached_attribute_regex_;
  Callback callback_;

  base::WeakPtrFactory<ListenAction> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_LISTEN_ACTION_H_
