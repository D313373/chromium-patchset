#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_NATIVE_LOCATOR_MATCHES_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_NATIVE_LOCATOR_MATCHES_H_

#include <vector>

#include "base/functional/callback.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.mojom.h"

namespace content {
class WebContents;
}

namespace dashboard {

// Asks the renderer resolver in every live frame of the primary page for all
// elements matching any of `locators`. Runs on Blink's C++ DOM APIs; no page
// script, isolated world or DevTools session is involved.
//
// The reply is a page map ({"frames":[{"frameId","elements":[...]}]}) with
// bounds in root-widget coordinates, so it can be fed to
// ExtractPageMapCandidates / FindMatchingPageMapCandidates and the scrape
// extractors unchanged. Empty frames are omitted.
void CollectNativeLocatorMatches(
    content::WebContents* web_contents,
    const std::vector<Locator>& locators,
    mojom::SearchScope scope,
    int max_per_frame,
    base::OnceCallback<void(base::Value::Dict page_map)> callback);

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_NATIVE_LOCATOR_MATCHES_H_
