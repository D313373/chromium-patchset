#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_UNIFIED_PAGE_CONTEXT_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_UNIFIED_PAGE_CONTEXT_H_

#include <vector>

#include "base/functional/callback.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.mojom.h"
#include "components/optimization_guide/content/browser/page_content_proto_provider.h"
#include "components/optimization_guide/proto/features/common_quality_data.pb.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace dashboard {

struct UnifiedPageMapResult {
  UnifiedPageMapResult();
  UnifiedPageMapResult(const UnifiedPageMapResult&) = delete;
  UnifiedPageMapResult(UnifiedPageMapResult&&) noexcept;
  UnifiedPageMapResult& operator=(const UnifiedPageMapResult&) = delete;
  UnifiedPageMapResult& operator=(UnifiedPageMapResult&&) noexcept;
  ~UnifiedPageMapResult();

  bool ok = false;
  std::string error;
  base::Value::Dict page_map;
};

using UnifiedPageMapCallback = base::OnceCallback<void(UnifiedPageMapResult)>;

void FetchUnifiedPageMapSnapshot(
    content::WebContents* web_contents,
    UnifiedPageMapCallback callback,
    mojom::SearchScope scope = mojom::SearchScope::kInteractiveOnly);

// Same Annotated Page Content snapshot, without the script-run gate.
// The Glic task loop is not inside a ScriptExecutor agent run, so
// FetchUnifiedPageMapSnapshot would return page_map_disabled_in_script_mode.
void FetchAgentPageMap(content::WebContents* web_contents,
                       UnifiedPageMapCallback callback);

// Drops any in-flight APC snapshot coalescing so the next fetch captures fresh
// page content (used before locator-sensitive actions).
void InvalidateUnifiedPageMapCache(content::WebContents* web_contents);

void BuildUnifiedPageMapFromAnnotatedPageContent(
    content::WebContents* web_contents,
    optimization_guide::proto::AnnotatedPageContent annotated_page_content,
    const GURL& fallback_url,
    const std::vector<optimization_guide::AIPageContentSelectorEntry>&
        selector_sidecar,
    UnifiedPageMapCallback callback,
    mojom::SearchScope scope = mojom::SearchScope::kInteractiveOnly);

void BuildUnifiedPageMapFromAnnotatedPageContent(
    content::WebContents* web_contents,
    optimization_guide::proto::AnnotatedPageContent annotated_page_content,
    const GURL& fallback_url,
    UnifiedPageMapCallback callback,
    mojom::SearchScope scope = mojom::SearchScope::kInteractiveOnly);

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_UNIFIED_PAGE_CONTEXT_H_
