#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_TYPE_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_TYPE_HANDLER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/element_readiness_checker.h"
#include "chrome/browser/ui/webui/dashboard/actions/locator_resolver.h"

namespace content {
class RenderFrameHost;
class RenderWidgetHost;
class RenderWidgetHostView;
class WebContents;
}  // namespace content

class TypeHandler {
 public:
  using TypeCallback = base::OnceCallback<void(
      bool success,
      const dashboard::ResolvedElement& element,
      const std::string& error)>;

  explicit TypeHandler(content::WebContents* web_contents);
  ~TypeHandler();

  void ExecuteTypeAsync(const std::vector<dashboard::Locator>& locators,
                        const std::string& text_to_type,
                        int wait_timeout_ms,
                        TypeCallback callback);

 private:
  dashboard::ResolvedElement BuildTypingElementSnapshot(
      const dashboard::ResolvedElement& element) const;
  void OnTypeTargetReady(const std::string& text,
                         TypeCallback callback,
                         bool ready,
                         const dashboard::ResolvedElement& element,
                         const std::string& error);
  void ContinueWithNativeTyping(dashboard::ResolvedElement element,
                                const std::string& text,
                                TypeCallback callback);
  void OnFocusClickDone(dashboard::ResolvedElement element,
                        std::string text,
                        TypeCallback callback,
                        bool success);
  content::RenderFrameHost* ResolveTypingTargetFrame(
      const dashboard::ResolvedElement& element) const;
  void TypeCharacterNativeAsync(dashboard::ResolvedElement element,
                                const std::string& text,
                                size_t char_index,
                                TypeCallback callback);
  bool DispatchNativeKeyPress(content::RenderWidgetHost* rwh,
                              content::RenderWidgetHostView* rwhv,
                              char c) const;

  raw_ptr<content::WebContents> web_contents_;
  std::unique_ptr<ElementReadinessChecker> checker_;
  base::WeakPtrFactory<TypeHandler> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_TYPE_HANDLER_H_
