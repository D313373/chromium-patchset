// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/auto_reset.h"
#include "base/base64.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"
#include "components/os_crypt/sync/os_crypt.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/webui/dashboard/recorder_controller.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace {

bool IsDashboardOrInternalToolingUrl(const GURL& url) {
  if (!url.is_valid()) {
    return false;
  }
  // Agents must never act on the dashboard (or other chrome:// tooling) tab —
  // that is what was navigating chrome://dashboard to Google.
  if (url.SchemeIs("chrome") || url.SchemeIs("chrome-untrusted") ||
      url.SchemeIs("devtools")) {
    return true;
  }
  return false;
}

std::string NormalizeAgentChatProvider(std::string provider) {
  provider = base::ToLowerASCII(
      base::TrimWhitespaceASCII(provider, base::TRIM_ALL));
  if (provider == "ollama") {
    // Legacy settings name → generic local OpenAI-compatible provider.
    return "local";
  }
  if (provider == "cerebras" || provider == "gemini" || provider == "openai" ||
      provider == "anthropic" || provider == "local") {
    return provider;
  }
  return "anthropic";
}

std::string RequiredApiKeyNameForChatProvider(std::string_view provider) {
  if (provider == "cerebras") {
    return "CEREBRAS_API_KEY";
  }
  if (provider == "gemini") {
    return "GEMINI_API_KEY";
  }
  if (provider == "openai") {
    return "OPENAI_API_KEY";
  }
  if (provider == "anthropic") {
    return "ANTHROPIC_API_KEY";
  }
  if (provider == "local") {
    // Optional. Many local servers accept empty auth.
    return "LOCAL_API_KEY";
  }
  return std::string();
}

std::string DecodeSecretIniValueForChat(const std::string& encoded_or_plain) {
  std::string cipher;
  if (!base::Base64Decode(encoded_or_plain, &cipher)) {
    return encoded_or_plain;
  }
  std::string plain;
  if (!OSCrypt::DecryptString(cipher, &plain)) {
    return encoded_or_plain;
  }
  return plain;
}

// Must run on a MayBlock thread (PathExists / ReadFileToString).
std::string ResolveChatProviderApiKeyFromPath(base::FilePath profile_path,
                                              std::string key_name) {
  if (profile_path.empty() || key_name.empty()) {
    return std::string();
  }
  const base::FilePath secrets_path =
      profile_path.AppendASCII("SimulachromeData").AppendASCII("secrets.ini");
  if (base::PathExists(secrets_path)) {
    std::string contents;
    if (base::ReadFileToString(secrets_path, &contents)) {
      for (const auto& raw :
           base::SplitStringPiece(contents, "\n", base::KEEP_WHITESPACE,
                                  base::SPLIT_WANT_ALL)) {
        std::string line(raw);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        std::string trimmed = line;
        base::TrimWhitespaceASCII(trimmed, base::TRIM_ALL, &trimmed);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
          continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
          continue;
        }
        std::string key = line.substr(0, eq);
        base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
        if (key.size() >= 2 && key.front() == '{' && key.back() == '}') {
          key = key.substr(1, key.size() - 2);
          base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
        }
        if (key != key_name) {
          continue;
        }
        std::string raw_value = line.substr(eq + 1);
        base::TrimWhitespaceASCII(raw_value, base::TRIM_ALL, &raw_value);
        const std::string decoded = DecodeSecretIniValueForChat(raw_value);
        if (!decoded.empty()) {
          return decoded;
        }
      }
    }
  }
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> from_env = env->GetVar(key_name);
  if (from_env.has_value() && !from_env->empty()) {
    return *from_env;
  }
  return std::string();
}

content::WebContents* FindPreferredAgentTargetWebContents(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  Browser* browser = chrome::FindLastActiveWithProfile(profile);
  if (!browser || !browser->tab_strip_model()) {
    return nullptr;
  }
  TabStripModel* tabs = browser->tab_strip_model();
  content::WebContents* active = tabs->GetActiveWebContents();
  if (active &&
      !IsDashboardOrInternalToolingUrl(active->GetLastCommittedURL())) {
    return active;
  }
  for (int i = 0; i < tabs->count(); ++i) {
    content::WebContents* wc = tabs->GetWebContentsAt(i);
    if (wc &&
        !IsDashboardOrInternalToolingUrl(wc->GetLastCommittedURL())) {
      return wc;
    }
  }
  return nullptr;
}

content::WebContents* EnsureAgentTargetWebContents(Profile* profile) {
  if (content::WebContents* existing =
          FindPreferredAgentTargetWebContents(profile)) {
    return existing;
  }
  Browser* browser = chrome::FindLastActiveWithProfile(profile);
  if (!browser) {
    return nullptr;
  }
  NavigateParams np(browser, GURL("about:blank"),
                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  np.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&np);
  return np.navigated_or_inserted_contents;
}

std::optional<int> ParseIntLikeValue(const base::Value* value) {
  if (!value) {
    return std::nullopt;
  }
  if (value->is_int()) {
    return value->GetInt();
  }
  if (value->is_double()) {
    return static_cast<int>(value->GetDouble());
  }
  if (value->is_string()) {
    int parsed = 0;
    if (base::StringToInt(value->GetString(), &parsed)) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::optional<int> ReadRequestedTabId(const base::Value::Dict& payload) {
  if (std::optional<int> tab_id = ParseIntLikeValue(payload.Find("tab_id"));
      tab_id.has_value()) {
    return tab_id;
  }
  if (std::optional<int> tab_id = ParseIntLikeValue(payload.Find("tabId"));
      tab_id.has_value()) {
    return tab_id;
  }
  if (const base::Value::Dict* options = payload.FindDict("options")) {
    if (std::optional<int> tab_id =
            ParseIntLikeValue(options->Find("tab_id"));
        tab_id.has_value()) {
      return tab_id;
    }
    if (std::optional<int> tab_id = ParseIntLikeValue(options->Find("tabId"));
        tab_id.has_value()) {
      return tab_id;
    }
  }
  return std::nullopt;
}

content::WebContents* FindWebContentsForTabId(Profile* profile, int tab_id) {
  if (!profile || tab_id <= 0) {
    return nullptr;
  }
  for (Browser* browser : *BrowserList::GetInstance()) {
    if (!browser || browser->profile() != profile) {
      continue;
    }
    TabStripModel* tab_strip = browser->tab_strip_model();
    if (!tab_strip) {
      continue;
    }
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      if (!wc) {
        continue;
      }
      if (sessions::SessionTabHelper::IdForTab(wc).id() == tab_id) {
        return wc;
      }
    }
  }
  return nullptr;
}


// Convert OpenAI-style multimodal content (string or list with image_url) into
// Anthropic Messages API content blocks (text + base64 image).
bool ParseDataImageUrl(const std::string& url,
                       std::string* media_type,
                       std::string* data) {
  if (!media_type || !data) {
    return false;
  }
  static constexpr std::string_view kPrefix = "data:";
  if (!base::StartsWith(url, kPrefix)) {
    return false;
  }
  const size_t comma = url.find(',');
  if (comma == std::string::npos || comma <= kPrefix.size()) {
    return false;
  }
  const std::string meta = url.substr(kPrefix.size(), comma - kPrefix.size());
  *data = url.substr(comma + 1);
  if (data->empty()) {
    return false;
  }
  const size_t semi = meta.find(';');
  *media_type = semi == std::string::npos ? meta : meta.substr(0, semi);
  base::TrimWhitespaceASCII(*media_type, base::TRIM_ALL, media_type);
  return !media_type->empty();
}

base::Value::List BuildAnthropicContentParts(const base::Value& content_value) {
  base::Value::List parts;
  auto append_text = [&parts](const std::string& text) {
    if (text.empty()) {
      return;
    }
    base::Value::Dict text_part;
    text_part.Set("type", "text");
    text_part.Set("text", text);
    parts.Append(std::move(text_part));
  };
  auto append_image = [&parts](const std::string& media_type,
                               const std::string& data) {
    if (media_type.empty() || data.empty()) {
      return;
    }
    base::Value::Dict source;
    source.Set("type", "base64");
    source.Set("media_type", media_type);
    source.Set("data", data);
    base::Value::Dict image_part;
    image_part.Set("type", "image");
    image_part.Set("source", std::move(source));
    parts.Append(std::move(image_part));
  };

  if (content_value.is_string()) {
    append_text(content_value.GetString());
    return parts;
  }
  if (!content_value.is_list()) {
    return parts;
  }
  for (const auto& part_value : content_value.GetList()) {
    if (part_value.is_string()) {
      append_text(part_value.GetString());
      continue;
    }
    if (!part_value.is_dict()) {
      continue;
    }
    const base::Value::Dict& part = part_value.GetDict();
    const std::string* type = part.FindString("type");
    const std::string type_str = type ? *type : std::string();
    if (type_str == "text" || type_str.empty()) {
      if (const std::string* text = part.FindString("text")) {
        append_text(*text);
      }
      continue;
    }
    if (type_str == "image_url") {
      const base::Value::Dict* image_url = part.FindDict("image_url");
      const std::string* url =
          image_url ? image_url->FindString("url") : part.FindString("url");
      if (!url) {
        continue;
      }
      std::string media_type;
      std::string data;
      if (ParseDataImageUrl(*url, &media_type, &data)) {
        append_image(media_type, data);
      }
      continue;
    }
    if (type_str == "image") {
      const base::Value::Dict* source = part.FindDict("source");
      if (!source) {
        continue;
      }
      const std::string* media_type = source->FindString("media_type");
      const std::string* data = source->FindString("data");
      if (media_type && data) {
        append_image(*media_type, *data);
      }
    }
  }
  return parts;
}

}  // namespace

void DashboardUIMessageHandler::HandleAgentGetActivePageInfo(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::Dict out;
  // Prefer the last active browser. This avoids returning the dashboard
  // WebUI itself when it is not the tab the user is actually acting on.
  if (Browser* browser = chrome::FindLastActiveWithProfile(profile_)) {
    content::WebContents* wc = browser->tab_strip_model()->GetActiveWebContents();
    if (wc) {
      out.Set("url", wc->GetLastCommittedURL().spec());
      out.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
      const int tab_id = sessions::SessionTabHelper::IdForTab(wc).id();
      if (tab_id > 0) {
        out.Set("tab_id", tab_id);
        out.Set("tabId", tab_id);
      }
    }
  }
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandlePerformAgentAction(
    const base::Value::List& args) {
  // Enter agent mode for the duration of this execution.
  base::AutoReset<bool> agent_mode_guard(&is_agent_mode_, true);
  std::string cb;
  if (args.size() > 0 && args[0].is_string())
    cb = args[0].GetString();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;

  base::Value::Dict out;
  if (!payload) {
    out.Set("ok", false);
    out.Set("error", "missing payload");
    ResolveCallback(cb, out);
    return;
  }

  base::Value::List timeline;
  bool has_timeline = false;

  if (const base::Value::List* provided = payload->FindList("timeline")) {
    timeline = provided->Clone();
    has_timeline = !timeline.empty();
  } else if (const base::Value::List* actions = payload->FindList("actions")) {
    timeline = actions->Clone();
    has_timeline = !timeline.empty();
  } else if (const base::Value::Dict* action_dict = payload->FindDict("action")) {
    timeline.Append(action_dict->Clone());
    has_timeline = true;
  } else if (const base::Value::Dict* step_dict = payload->FindDict("step")) {
    timeline.Append(step_dict->Clone());
    has_timeline = true;
  } else if (const std::string* action_name = payload->FindString("action")) {
    base::Value::Dict action_entry;
    action_entry.Set("action", *action_name);
    if (const base::Value::Dict* params = payload->FindDict("params"))
      action_entry.Set("params", params->Clone());
    timeline.Append(std::move(action_entry));
    has_timeline = true;
  }

  if (!has_timeline) {
    out.Set("ok", false);
    out.Set("error", "missing action or timeline");
    ResolveCallback(cb, out);
    return;
  }

  auto read_bool_option =
      [&](const char* key, const char* alt_key, bool default_value) -> bool {
    if (std::optional<bool> direct = payload->FindBool(key);
        direct.has_value()) {
      return *direct;
    }
    if (std::optional<bool> direct_alt = payload->FindBool(alt_key);
        direct_alt.has_value()) {
      return *direct_alt;
    }
    if (const base::Value::Dict* options = payload->FindDict("options")) {
      if (std::optional<bool> from_options = options->FindBool(key);
          from_options.has_value()) {
        return *from_options;
      }
      if (std::optional<bool> from_options_alt = options->FindBool(alt_key);
          from_options_alt.has_value()) {
        return *from_options_alt;
      }
    }
    return default_value;
  };
  const std::optional<int> requested_tab_id = ReadRequestedTabId(*payload);
  content::WebContents* target_wc = requested_tab_id.has_value()
                                        ? FindWebContentsForTabId(profile_,
                                                                  *requested_tab_id)
                                        : nullptr;
  if (!target_wc) {
    target_wc = EnsureAgentTargetWebContents(profile_);
  } else if (IsDashboardOrInternalToolingUrl(
                 target_wc->GetLastCommittedURL())) {
    // Explicit tab_id pointed at dashboard/tooling — retarget instead of
    // destroying the WebUI.
    LOG(WARNING) << "[Agent] Refusing tooling tab_id="
                 << (requested_tab_id.has_value() ? *requested_tab_id : -1)
                 << " url=" << target_wc->GetLastCommittedURL().spec()
                 << "; opening/using a non-dashboard task tab";
    target_wc = EnsureAgentTargetWebContents(profile_);
  }
  if (!target_wc) {
    out.Set("ok", false);
    if (requested_tab_id.has_value()) {
      out.Set("error",
              base::StringPrintf("no tab found for tab_id=%d",
                                 *requested_tab_id));
    } else {
      out.Set("error", "no active tab");
    }
    ResolveCallback(cb, out);
    return;
  }

  const int resolved_tab_id = sessions::SessionTabHelper::IdForTab(target_wc).id();
  LOG(INFO) << "[Agent] Starting timeline run"
            << " tab_id=" << resolved_tab_id
            << " url=" << target_wc->GetLastCommittedURL().spec()
            << " requested_tab_id="
            << (requested_tab_id.has_value()
                    ? base::NumberToString(*requested_tab_id)
                    : "none")
            << " actions=" << timeline.size()
            << " wait_for_completion="
            << read_bool_option("wait_for_completion", "waitForCompletion", false)
            << " force_native="
            << read_bool_option("force_native_execution", "forceNativeExecution",
                                false);

  dashboard::ScriptExecutionService* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (!exec_service) {
    out.Set("ok", false);
    out.Set("error", "execution service unavailable");
    ResolveCallback(cb, out);
    return;
  }
  const bool wait_for_completion =
      read_bool_option("wait_for_completion", "waitForCompletion", false);
  const bool force_cdp_execution =
      read_bool_option("force_cdp_execution", "forceCdpExecution", false);
  const bool force_native_execution =
      read_bool_option("force_native_execution", "forceNativeExecution", false);
  dashboard::ScriptExecutionService::RunOptions run_options;
  run_options.force_cdp_execution = force_cdp_execution;
  run_options.force_native_execution = force_native_execution;
  run_options.is_agent_mode =
      read_bool_option("is_agent_mode", "isAgentMode", is_agent_mode_);
  // Optional: include a screenshot of current tab state before queuing.
  bool want_screenshot = false;
  std::optional<bool> b = payload->FindBool("includeScreenshot");
  if (b.has_value())
    want_screenshot = b.value();

  if (want_screenshot && wait_for_completion) {
    out.Set("ok", false);
    out.Set("error",
            "wait_for_completion is not supported when includeScreenshot=true");
    ResolveCallback(cb, out);
    return;
  }

  if (want_screenshot) {
    RecorderController* rc = RecorderController::GetActive();
    if (!rc)
      rc = recorder_.get();
    if (rc) {
      rc->CaptureScreenshot(base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self,
             std::string cb_id,
             std::optional<int> tab_id_hint,
             base::Value::List actions,
             std::string base64_png) {
            if (!self)
              return;
            dashboard::ScriptExecutionService* exec_service =
                dashboard::ScriptExecutionServiceFactory::GetForProfile(
                    self->profile_);
            if (exec_service) {
              content::WebContents* wc =
                  tab_id_hint.has_value()
                      ? FindWebContentsForTabId(self->profile_, *tab_id_hint)
                      : nullptr;
              if (!wc ||
                  IsDashboardOrInternalToolingUrl(wc->GetLastCommittedURL())) {
                wc = EnsureAgentTargetWebContents(self->profile_);
              }
              if (wc)
                exec_service->StartAdHocRun(actions, wc);
            }
            base::Value::Dict res;
            res.Set("ok", true);
            res.Set("queued", true);
            res.Set("count", static_cast<int>(actions.size()));
            if (!base64_png.empty())
              res.Set("screenshot", base::StrCat(
                                        {"data:image/png;base64,", base64_png}));
            self->ResolveCallback(cb_id, res);
          },
          weak_ptr_factory_.GetWeakPtr(), cb, requested_tab_id, timeline.Clone()));
      return;
    }
  }

  if (wait_for_completion) {
    auto execution_id_ref = std::make_shared<std::string>();
    std::string execution_id = exec_service->StartAdHocRunWithOptions(
        timeline, target_wc, run_options,
        base::BindOnce(
            [](base::WeakPtr<DashboardUIMessageHandler> self,
               std::string cb_id,
               std::shared_ptr<std::string> run_id_ref,
               bool success,
               int failed_step,
               std::string message) {
              if (!self) {
                return;
              }
              base::Value::Dict res;
              res.Set("ok", success);
              res.Set("success", success);
              res.Set("completed", true);
              res.Set("queued", false);
              if (run_id_ref && !run_id_ref->empty()) {
                res.Set("execution_id", *run_id_ref);
              }
              if (failed_step >= 0) {
                res.Set("failed_step", failed_step);
              }
              if (!message.empty()) {
                res.Set("message", message);
              }
              self->ResolveCallback(cb_id, res);
            },
            weak_ptr_factory_.GetWeakPtr(), cb, execution_id_ref));
    if (execution_id.empty()) {
      out.Set("ok", false);
      out.Set("error", "failed to queue ad-hoc run");
      ResolveCallback(cb, out);
      return;
    }
    *execution_id_ref = execution_id;
    return;
  }

  const std::string execution_id =
      exec_service->StartAdHocRunWithOptions(timeline, target_wc, run_options);
  out.Set("ok", !execution_id.empty());
  out.Set("queued", !execution_id.empty());
  out.Set("count", static_cast<int>(timeline.size()));
  if (!execution_id.empty()) {
    out.Set("execution_id", execution_id);
  } else {
    out.Set("error", "failed to queue ad-hoc run");
  }
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentCallAction(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const std::string action =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                               : std::string();
  const base::Value::Dict* params =
      (args.size() > 2 && args[2].is_dict()) ? &args[2].GetDict() : nullptr;
  const base::Value::Dict* options =
      (args.size() > 3 && args[3].is_dict()) ? &args[3].GetDict() : nullptr;
  base::Value::Dict payload;
  if (!action.empty())
    payload.Set("action", action);
  if (params)
    payload.Set("params", params->Clone());
  if (options)
    payload.Set("options", options->Clone());
  base::Value::List forwarded_args;
  forwarded_args.Append(cb);
  forwarded_args.Append(std::move(payload));
  HandlePerformAgentAction(forwarded_args);
}

void DashboardUIMessageHandler::HandleAgentRunTimeline(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::List forwarded_args;
  forwarded_args.Append(cb);
  if (args.size() > 1 && args[1].is_dict()) {
    base::Value::Dict payload = args[1].GetDict().Clone();
    if (!payload.contains("is_agent_mode") &&
        !payload.contains("isAgentMode")) {
      payload.Set("is_agent_mode", false);
    }
    forwarded_args.Append(std::move(payload));
    HandlePerformAgentAction(forwarded_args);
    return;
  }
  const base::Value::List* timeline =
      (args.size() > 1 && args[1].is_list()) ? &args[1].GetList() : nullptr;
  base::Value::Dict payload;
  if (timeline) {
    payload.Set("timeline", timeline->Clone());
  }
  payload.Set("is_agent_mode", false);
  forwarded_args.Append(std::move(payload));
  HandlePerformAgentAction(forwarded_args);
}

void DashboardUIMessageHandler::HandleAgentRefreshPageMap(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::Dict out;
  RecorderController* rc = RecorderController::GetActive();
  if (!rc && recorder_)
    rc = recorder_.get();
  // Allow PageMap refresh even when recording has never been started by
  // lazily creating a RecorderController bound to the active browser.
  if (!rc) {
    Browser* browser = chrome::FindLastActiveWithProfile(profile_);
    BrowserView* bv = browser ? BrowserView::GetBrowserViewForBrowser(browser)
                              : nullptr;
    if (bv) {
      if (!recorder_)
        recorder_ = std::make_unique<RecorderController>();
      rc = recorder_.get();
      RecorderController::SetActive(rc);
      // No need to enter recording/passive mode here; snapshot capture will
      // lazily attach to the active tab as needed.
    }
  }
  if (!rc) {
    out.Set("ok", false);
    out.Set("error", "recorder not active");
    ResolveCallback(cb, out);
    return;
  }
  rc->CapturePageMap(base::BindOnce(
      [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb_id,
         base::Value::Dict pagemap) {
        if (!self)
          return;
        base::Value::Dict result;
        result.Set("ok", true);
        result.Set("pagemap", pagemap.Clone());
        self->ResolveCallback(cb_id, result);
      },
      weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandleAgentReloadPage(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::Dict out;
  Browser* browser = chrome::FindLastActiveWithProfile(profile_);
  content::WebContents* wc =
      browser ? browser->tab_strip_model()->GetActiveWebContents() : nullptr;
  if (!wc) {
    out.Set("ok", false);
    out.Set("error", "no active tab");
    ResolveCallback(cb, out);
    return;
  }
  wc->GetController().Reload(content::ReloadType::NORMAL, false);
  out.Set("ok", true);
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentInspectElement(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;

  base::Value::Dict out;
  int index = -1;
  if (payload) {
    if (auto idx = payload->FindInt("index"))
      index = *idx;
  }

  if (index < 0) {
    out.Set("ok", false);
    out.Set("error", "missing or invalid index");
    ResolveCallback(cb, out);
    return;
  }

  RecorderController* rc = RecorderController::GetActive();
  if (!rc && recorder_)
    rc = recorder_.get();
  // Make inspect_element usable even when the recorder has not been put into
  // recording mode yet by lazily creating / attaching a controller.
  if (!rc) {
    Browser* browser = chrome::FindLastActiveWithProfile(profile_);
    BrowserView* bv = browser ? BrowserView::GetBrowserViewForBrowser(browser)
                              : nullptr;
    if (bv) {
      if (!recorder_)
        recorder_ = std::make_unique<RecorderController>();
      rc = recorder_.get();
      RecorderController::SetActive(rc);
    }
  }
  if (!rc) {
    out.Set("ok", false);
    out.Set("error", "recorder not active");
    ResolveCallback(cb, out);
    return;
  }

  auto report = rc->BuildElementReportForIndex(index);
  if (!report) {
    out.Set("ok", false);
    out.Set("error", "no page_map element or element out of range");
    ResolveCallback(cb, out);
    return;
  }

  out.Set("ok", true);
  out.Set("index", index);
  out.Set("report", std::move(*report));
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentScreenshot(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::Dict out;
  RecorderController* rc = RecorderController::GetActive();
  if (!rc)
    rc = recorder_.get();
  if (!rc) {
    out.Set("ok", false);
    out.Set("error", "recorder not active");
    ResolveCallback(cb, out);
    return;
  }
  rc->CaptureScreenshot(base::BindOnce(
      [](base::WeakPtr<DashboardUIMessageHandler> self,
         std::string cb_id,
         std::string base64_png) {
        if (!self)
          return;
        base::Value::Dict res;
        if (!base64_png.empty()) {
          res.Set("ok", true);
          res.Set("image",
                  base::StrCat({"data:image/png;base64,", base64_png}));
        } else {
          res.Set("ok", false);
          res.Set("error", "screenshot failed");
        }
        self->ResolveCallback(cb_id, res);
      },
      weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandleAgentOllamaChat(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;

  base::Value::Dict early;
  if (!payload) {
    early.Set("ok", false);
    early.Set("error", "missing_payload");
    ResolveCallback(cb, early);
    return;
  }

  auto read_string = [](const base::Value::Dict& dict,
                        std::string_view primary,
                        std::string_view alternate) -> std::string {
    if (const std::string* value = dict.FindString(primary)) {
      return *value;
    }
    if (const std::string* value = dict.FindString(alternate)) {
      return *value;
    }
    return std::string();
  };

  std::string raw_provider = read_string(*payload, "provider", "provider");
  // Empty / legacy "ollama" → prefer Settings-configured provider.
  if (raw_provider.empty() ||
      base::EqualsCaseInsensitiveASCII(raw_provider, "ollama")) {
    if (const base::Value::Dict* agent =
            dashboard::GlobalSettingsStore().FindDict("agent")) {
      if (const std::string* configured = agent->FindString("provider")) {
        raw_provider = *configured;
      }
    }
  }
  std::string provider = NormalizeAgentChatProvider(raw_provider);

  std::string endpoint = read_string(*payload, "endpoint", "endpoint");
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }

  std::string model = read_string(*payload, "model", "model");
  const std::string system_prompt =
      read_string(*payload, "system_prompt", "systemPrompt");
  int max_tokens = 1024;
  if (const std::optional<int> configured = payload->FindInt("max_tokens")) {
    max_tokens = std::clamp(*configured, 256, 4096);
  } else if (const std::optional<int> configured_alt =
                 payload->FindInt("maxTokens")) {
    max_tokens = std::clamp(*configured_alt, 256, 4096);
  }

  base::Value::List chat_messages;
  if (const base::Value::List* messages = payload->FindList("messages")) {
    for (const auto& item : *messages) {
      if (item.is_dict()) {
        chat_messages.Append(item.Clone());
      }
    }
  }

  const std::string api_key_name = [&]() {
    std::string from_payload =
        read_string(*payload, "api_key_name", "apiKeyName");
    if (!from_payload.empty()) {
      return from_payload;
    }
    return RequiredApiKeyNameForChatProvider(provider);
  }();
  // secrets.ini IO is blocking; never do it on the UI thread. Local servers
  // often need no key, but LOCAL_API_KEY is still resolved when present.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ResolveChatProviderApiKeyFromPath, profile_->GetPath(),
                     api_key_name),
      base::BindOnce(&DashboardUIMessageHandler::StartAgentOllamaChatRequest,
                     weak_ptr_factory_.GetWeakPtr(), cb, provider, endpoint,
                     model, system_prompt, max_tokens,
                     std::move(chat_messages), api_key_name));
}

void DashboardUIMessageHandler::StartAgentOllamaChatRequest(
    std::string cb,
    std::string provider,
    std::string endpoint,
    std::string model,
    std::string system_prompt,
    int max_tokens,
    base::Value::List chat_messages,
    std::string api_key_name,
    std::string api_key) {
  base::Value::Dict early;
  if (provider != "local" && api_key.empty()) {
    early.Set("ok", false);
    early.Set("error",
              base::StrCat({"missing_api_key:", api_key_name,
                            " (set in Data > Secrets)"}));
    ResolveCallback(cb, early);
    return;
  }

  base::Value::Dict body;
  GURL request_url;
  auto request = std::make_unique<network::ResourceRequest>();
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader("Content-Type", "application/json");
  request->headers.SetHeader("Accept", "application/json");

  if (provider == "anthropic") {
    if (endpoint.empty()) {
      endpoint = "https://api.anthropic.com/v1/messages";
    }
    if (model.empty() || model == "claude-haiku-4-5-20251001" ||
        model == "claude-3-5-haiku-latest") {
      model = "claude-haiku-4-5";
    } else if (model == "claude-sonnet-4-20250514" ||
               model == "claude-3-5-sonnet-latest") {
      model = "claude-sonnet-4-5";
    }
    body.Set("model", model);
    body.Set("max_tokens", max_tokens);
    body.Set("temperature", 0);
    if (!system_prompt.empty()) {
      body.Set("system", system_prompt);
    }
    base::Value::List anthropic_messages;
    for (const auto& item : chat_messages) {
      if (!item.is_dict()) {
        continue;
      }
      const base::Value::Dict& src = item.GetDict();
      const std::string* role = src.FindString("role");
      if (!role || *role == "system") {
        continue;
      }
      const base::Value* content_value = src.Find("content");
      if (!content_value) {
        continue;
      }
      base::Value::List parts = BuildAnthropicContentParts(*content_value);
      if (parts.empty()) {
        continue;
      }
      base::Value::Dict msg;
      msg.Set("role", *role);
      msg.Set("content", std::move(parts));
      anthropic_messages.Append(std::move(msg));
    }
    body.Set("messages", std::move(anthropic_messages));
    request_url = GURL(endpoint);
    request->headers.SetHeader("x-api-key", api_key);
    request->headers.SetHeader("anthropic-version", "2023-06-01");
    request->headers.SetHeader("User-Agent", "SimulachromeDashboard/1.0");
  } else if (provider == "openai" || provider == "cerebras" ||
             provider == "local") {
    if (endpoint.empty()) {
      if (provider == "cerebras") {
        endpoint = "https://api.cerebras.ai/v1/chat/completions";
      } else if (provider == "local") {
        endpoint = "http://127.0.0.1:11434/v1/chat/completions";
      } else {
        endpoint = "https://api.openai.com/v1/chat/completions";
      }
    }
    // Accept base URLs like http://host:port/v1 and append chat completions.
    if (provider == "local") {
      if (base::EndsWith(endpoint, "/v1", base::CompareCase::SENSITIVE)) {
        endpoint = base::StrCat({endpoint, "/chat/completions"});
      } else if (!base::EndsWith(endpoint, "/chat/completions",
                                 base::CompareCase::SENSITIVE) &&
                 !base::EndsWith(endpoint, "/completions",
                                 base::CompareCase::SENSITIVE)) {
        if (base::EndsWith(endpoint, "/", base::CompareCase::SENSITIVE)) {
          endpoint = base::StrCat({endpoint, "v1/chat/completions"});
        } else {
          endpoint = base::StrCat({endpoint, "/v1/chat/completions"});
        }
      }
    }
    if (model.empty()) {
      if (provider == "cerebras") {
        model = "qwen-3-235b-a22b-instruct-2507";
      } else if (provider == "local") {
        model = "local-model";
      } else {
        model = "gpt-4.1-mini";
      }
    }
    base::Value::List request_messages;
    if (!system_prompt.empty()) {
      base::Value::Dict system_message;
      system_message.Set("role", "system");
      system_message.Set("content", system_prompt);
      request_messages.Append(std::move(system_message));
    }
    for (const auto& item : chat_messages) {
      request_messages.Append(item.Clone());
    }
    body.Set("model", model);
    body.Set("temperature", 0);
    body.Set("max_tokens", max_tokens);
    body.Set("messages", std::move(request_messages));
    request_url = GURL(endpoint);
    if (!api_key.empty()) {
      request->headers.SetHeader("Authorization",
                                 base::StrCat({"Bearer ", api_key}));
    }
    request->headers.SetHeader("User-Agent", "SimulachromeDashboard/1.0");
  } else if (provider == "gemini") {
    if (endpoint.empty()) {
      endpoint = "https://generativelanguage.googleapis.com/v1beta";
    }
    if (model.empty()) {
      model = "gemini-2.5-flash";
    }
    base::Value::List contents;
    for (const auto& item : chat_messages) {
      if (!item.is_dict()) {
        continue;
      }
      const base::Value::Dict& src = item.GetDict();
      const std::string* role = src.FindString("role");
      const std::string* content = src.FindString("content");
      if (!role || !content || *role == "system") {
        continue;
      }
      base::Value::Dict entry;
      entry.Set("role", *role == "assistant" ? "model" : "user");
      base::Value::List parts;
      base::Value::Dict text_part;
      text_part.Set("text", *content);
      parts.Append(std::move(text_part));
      entry.Set("parts", std::move(parts));
      contents.Append(std::move(entry));
    }
    body.Set("contents", std::move(contents));
    if (!system_prompt.empty()) {
      base::Value::Dict system_instruction;
      base::Value::List parts;
      base::Value::Dict text_part;
      text_part.Set("text", system_prompt);
      parts.Append(std::move(text_part));
      system_instruction.Set("parts", std::move(parts));
      body.Set("systemInstruction", std::move(system_instruction));
    }
    base::Value::Dict generation;
    generation.Set("temperature", 0);
    generation.Set("maxOutputTokens", max_tokens);
    body.Set("generationConfig", std::move(generation));
    request_url = GURL(base::StrCat(
        {endpoint, "/models/", model, ":generateContent"}));
    request->headers.SetHeader("x-goog-api-key", api_key);
    request->headers.SetHeader("User-Agent", "SimulachromeDashboard/1.0");
  } else {
    early.Set("ok", false);
    early.Set("error", base::StrCat({"unsupported_provider:", provider}));
    ResolveCallback(cb, early);
    return;
  }

  std::string body_json;
  if (!base::JSONWriter::Write(body, &body_json) || body_json.empty()) {
    early.Set("ok", false);
    early.Set("error", "failed_to_build_request_body");
    ResolveCallback(cb, early);
    return;
  }

  if (!request_url.is_valid()) {
    early.Set("ok", false);
    early.Set("error", "invalid_provider_endpoint");
    ResolveCallback(cb, early);
    return;
  }
  request->url = request_url;

  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(request),
                                       TRAFFIC_ANNOTATION_FOR_TESTS);
  loader->AttachStringForUpload(body_json, "application/json");
  loader->SetAllowHttpErrorResults(true);
  int timeout_ms = provider == "local" ? 20 * 60 * 1000 : 90 * 1000;
  if (const base::Value::Dict* agent =
          dashboard::GlobalSettingsStore().FindDict("agent")) {
    if (const std::optional<int> configured =
            agent->FindInt("modelStepTimeoutMs")) {
      timeout_ms = std::clamp(*configured, 30 * 1000, 60 * 60 * 1000);
    }
  }
  loader->SetTimeoutDuration(base::Milliseconds(timeout_ms));

  network::SimpleURLLoader* loader_raw = loader.get();
  auto* partition = profile_->GetDefaultStoragePartition();
  loader_raw->DownloadToString(
      partition->GetURLLoaderFactoryForBrowserProcess().get(),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             std::unique_ptr<network::SimpleURLLoader> loader,
             std::unique_ptr<std::string> response_body) {
            if (!self) {
              return;
            }
            base::Value::Dict out;
            int http_status = 0;
            if (loader && loader->ResponseInfo() &&
                loader->ResponseInfo()->headers) {
              http_status = loader->ResponseInfo()->headers->response_code();
            }
            if (!response_body) {
              out.Set("ok", false);
              out.Set("http_status", http_status);
              out.Set("error",
                      base::StrCat(
                          {"network_error:",
                           base::NumberToString(loader ? loader->NetError()
                                                       : net::ERR_FAILED)}));
              self->ResolveCallback(cb, out);
              return;
            }
            out.Set("ok", http_status >= 200 && http_status < 300);
            out.Set("http_status", http_status);
            out.Set("body", *response_body);
            if (http_status < 200 || http_status >= 300) {
              out.Set("error", *response_body);
            }
            self->ResolveCallback(cb, out);
          },
          weak_ptr_factory_.GetWeakPtr(), cb, std::move(loader)),
      4 * 1024 * 1024);
}
