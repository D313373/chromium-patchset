// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/anti_detection_controller.h"

#include <string>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "chrome/browser/profiles/profile.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/language/core/browser/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"

namespace dashboard {

namespace {

bool IsValidHeaderValue(const std::string& value) {
  return value.find('\0') == std::string::npos &&
         value.find('\r') == std::string::npos &&
         value.find('\n') == std::string::npos;
}

}  // namespace

AntiDetectionController::AntiDetectionController(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents) {}

AntiDetectionController::~AntiDetectionController() = default;

void AntiDetectionController::InjectAntiDetectionScripts(
    const base::Value::Dict& settings) {
  Apply(settings);
}

void AntiDetectionController::InjectAntiDetectionScripts(
    const base::Value& settings) {
  if (const base::Value::Dict* dict = settings.GetIfDict()) {
    Apply(*dict);
    return;
  }
  VLOG(1) << "[AntiDetection] Expected a dictionary of settings.";
}

void AntiDetectionController::Apply(const base::Value::Dict& settings) {
  content::WebContents* wc = web_contents();
  if (!wc) {
    return;
  }

  // A custom UA is only a win when the request header and navigator.userAgent
  // change together, and client hints stay the real ones. Sending no client
  // hints, or a JS-defined navigator.userAgent, is the more common tell.
  blink::UserAgentOverride ua_override;
  if (const std::string* ua = settings.FindString("userAgent")) {
    const std::string trimmed(
        base::TrimWhitespaceASCII(*ua, base::TRIM_ALL));
    if (!trimmed.empty() && IsValidHeaderValue(trimmed)) {
      ua_override.ua_string_override = trimmed;
      ua_override.ua_metadata_override =
          embedder_support::GetUserAgentMetadata();
    }
  }
  const bool overriding = !ua_override.ua_string_override.empty();
  wc->SetUserAgentOverride(ua_override, overriding);
  if (content::NavigationEntry* entry =
          wc->GetController().GetLastCommittedEntry()) {
    entry->SetIsOverridingUserAgent(overriding);
  }

  // Accept-Language is the profile preference Chrome already uses for both the
  // request header and navigator.languages. An empty list leaves it alone.
  const base::Value::List* languages = settings.FindList("languages");
  if (!languages || languages->empty()) {
    return;
  }
  std::vector<std::string> tags;
  for (const base::Value& value : *languages) {
    if (!value.is_string()) {
      continue;
    }
    const std::string tag(
        base::TrimWhitespaceASCII(value.GetString(), base::TRIM_ALL));
    if (tag.empty() || !IsValidHeaderValue(tag) ||
        tag.find(',') != std::string::npos) {
      continue;
    }
    tags.push_back(tag);
  }
  if (tags.empty()) {
    return;
  }
  Profile* profile = Profile::FromBrowserContext(wc->GetBrowserContext());
  if (!profile || !profile->GetPrefs()) {
    return;
  }
  profile->GetPrefs()->SetString(language::prefs::kAcceptLanguages,
                                 base::JoinString(tags, ","));
}

}  // namespace dashboard
