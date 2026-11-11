// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ANTI_DETECTION_CONTROLLER_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ANTI_DETECTION_CONTROLLER_H_

#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class WebContents;
}

namespace dashboard {

// Applies anti-detection settings through browser APIs. Nothing here attaches
// DevTools or runs script in the page: a patched navigator is a stronger
// signal than the value it was papering over.
class AntiDetectionController : public content::WebContentsObserver {
 public:
  explicit AntiDetectionController(content::WebContents* web_contents);
  ~AntiDetectionController() override;

  // `userAgent`, when non-empty, becomes the tab's user-agent override and is
  // paired with this browser's real client hints. `languages`, when non-empty,
  // sets the profile Accept-Language preference. Other historical keys
  // (webdriver, platform, hardwareConcurrency, pluginSpoofing, dark reader)
  // are ignored.
  void InjectAntiDetectionScripts(const base::Value::Dict& settings);
  void InjectAntiDetectionScripts(const base::Value& settings);

 private:
  void Apply(const base::Value::Dict& settings);
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ANTI_DETECTION_CONTROLLER_H_
