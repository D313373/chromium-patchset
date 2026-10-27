// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/simulachrome/simulachrome_web_ui_message_handler.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/string_escape.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/simulachrome/planned_action.h"
#include "chrome/browser/simulachrome/simulachrome_agent_host.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/sessions/content/session_tab_helper.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "url/gurl.h"

namespace simulachrome_webui {
namespace {

constexpr char kMsgExecuteAction[] = "execute_simulachrome_action";
constexpr char kMsgLoadModel[] = "load_simulachrome_model";
constexpr char kMsgSubmitBatch[] = "submit_simulachrome_batch";
constexpr char kMsgCancelBatch[] = "cancel_simulachrome_batch";
constexpr char kMsgCaptureFrame[] = "capture_simulachrome_frame";

// Default pack created by third_party/llama_cpp/scripts/export_ollama_smolvlm2.sh
base::FilePath DefaultModelDir() {
  return base::FilePath(
      FILE_PATH_LITERAL("/Users/matcauthon/simulachrome_models"));
}

bool IsInternalToolingUrl(const GURL& url) {
  if (!url.is_valid()) {
    return false;
  }
  if (url.SchemeIs("chrome") || url.SchemeIs("chrome-untrusted") ||
      url.SchemeIs("devtools")) {
    return true;
  }
  return false;
}

bool LooksLikeJsCallbackId(const std::string& s) {
  if (s.empty() || s.size() > 128) {
    return false;
  }
  // sendWithPromise uses __glic_cb_<ts>_<rand>; parent_bridge rewrites that
  // to __sc_bridge___glic_cb_... when forwarding from the blob: agent iframe.
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '$';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool IsPromiseCallbackId(const std::string& s) {
  return LooksLikeJsCallbackId(s) &&
         (s.rfind("__glic_cb_", 0) == 0 || s.rfind("__sc_bridge_", 0) == 0);
}

}  // namespace

SimulachromeWebUIMessageHandler::SimulachromeWebUIMessageHandler(
    Profile* profile)
    : profile_(profile) {}

SimulachromeWebUIMessageHandler::~SimulachromeWebUIMessageHandler() = default;

void SimulachromeWebUIMessageHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      kMsgExecuteAction,
      base::BindRepeating(
          &SimulachromeWebUIMessageHandler::HandleExecuteSimulachromeAction,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgLoadModel,
      base::BindRepeating(
          &SimulachromeWebUIMessageHandler::HandleLoadSimulachromeModel,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgSubmitBatch,
      base::BindRepeating(
          &SimulachromeWebUIMessageHandler::HandleSubmitSimulachromeBatch,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgCancelBatch,
      base::BindRepeating(
          &SimulachromeWebUIMessageHandler::HandleCancelSimulachromeBatch,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgCaptureFrame,
      base::BindRepeating(
          &SimulachromeWebUIMessageHandler::HandleCaptureSimulachromeFrame,
          base::Unretained(this)));
  // Do NOT AllowJavascript() here — WebUI may not be ready yet (CHECK in
  // CanCallJavascript). Handlers call AllowJavascript() when a message arrives.
  LOG(INFO) << "[SimulachromeWebUI] RegisterMessages "
            << kMsgExecuteAction << ", " << kMsgLoadModel << ", "
            << kMsgSubmitBatch << ", " << kMsgCancelBatch;
}

content::WebContents*
SimulachromeWebUIMessageHandler::ResolveTargetWebContents() {
  if (!profile_) {
    return nullptr;
  }
  Browser* browser = chrome::FindLastActiveWithProfile(profile_);
  if (!browser || !browser->tab_strip_model()) {
    // Fall back to any browser for this profile.
    for (Browser* b : *BrowserList::GetInstance()) {
      if (b && b->profile() == profile_ && b->tab_strip_model()) {
        browser = b;
        break;
      }
    }
  }
  if (!browser || !browser->tab_strip_model()) {
    return nullptr;
  }

  TabStripModel* tabs = browser->tab_strip_model();
  content::WebContents* active = tabs->GetActiveWebContents();
  if (active && !IsInternalToolingUrl(active->GetLastCommittedURL())) {
    return active;
  }
  for (int i = 0; i < tabs->count(); ++i) {
    content::WebContents* wc = tabs->GetWebContentsAt(i);
    if (wc && !IsInternalToolingUrl(wc->GetLastCommittedURL())) {
      return wc;
    }
  }
  return active;  // last resort: even if chrome://
}

content::WebContents* FindTabBySessionId(Profile* profile, int tab_id) {
  if (!profile || tab_id <= 0) {
    return nullptr;
  }
  for (Browser* browser : *BrowserList::GetInstance()) {
    if (!browser || browser->profile() != profile || !browser->tab_strip_model()) {
      continue;
    }
    TabStripModel* tabs = browser->tab_strip_model();
    for (int i = 0; i < tabs->count(); ++i) {
      content::WebContents* wc = tabs->GetWebContentsAt(i);
      if (wc && sessions::SessionTabHelper::IdForTab(wc).id() == tab_id) {
        return wc;
      }
    }
  }
  return nullptr;
}

void SimulachromeWebUIMessageHandler::HandleCaptureSimulachromeFrame(
    const base::Value::List& args) {
  AllowJavascript();
  std::string callback_id;
  const base::Value::Dict* payload = nullptr;
  if (!args.empty() && args[0].is_string() && IsPromiseCallbackId(args[0].GetString())) {
    callback_id = args[0].GetString();
    if (args.size() >= 2 && args[1].is_dict()) {
      payload = &args[1].GetDict();
    }
  }
  content::WebContents* target = nullptr;
  if (payload) {
    std::optional<int> tab_id = payload->FindInt("tab_id");
    if (!tab_id) {
      tab_id = payload->FindInt("tabId");
    }
    if (tab_id) {
      target = FindTabBySessionId(profile_, *tab_id);
    }
  }
  if (!target) {
    target = ResolveTargetWebContents();
  }
  simulachrome::SimulachromeAgentHost* host = EnsureAgentHost(target);
  if (!host) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("error", "no task tab");
    ResolveJsCallback(callback_id, out);
    return;
  }
  host->CaptureViewportImage(base::BindOnce(
      [](base::WeakPtr<SimulachromeWebUIMessageHandler> self,
         std::string callback_id, bool ok, std::string png_base64, int width,
         int height, int css_width, int css_height, std::string source,
         std::string error) {
        if (!self) {
          return;
        }
        base::Value::Dict out;
        if (!ok || png_base64.empty()) {
          out.Set("ok", false);
          out.Set("error", error.empty() ? "screenshot failed" : error);
        } else {
          out.Set("ok", true);
          out.Set("base64", png_base64);
          out.Set("mimeType", "image/png");
          out.Set("source", source);
          if (width > 0) {
            out.Set("widthPixels", width);
          }
          if (height > 0) {
            out.Set("heightPixels", height);
          }
          if (css_width > 0) {
            out.Set("cssWidth", css_width);
          }
          if (css_height > 0) {
            out.Set("cssHeight", css_height);
          }
        }
        self->ResolveJsCallback(callback_id, out);
      },
      weak_factory_.GetWeakPtr(), callback_id));
}

simulachrome::SimulachromeAgentHost*
SimulachromeWebUIMessageHandler::EnsureAgentHost(
    content::WebContents* target) {
  if (!target) {
    return nullptr;
  }
  // Ask the host for its target rather than caching a second raw_ptr here;
  // SimulachromeAgentHost nulls it when the tab goes away.
  if (!agent_host_ || agent_host_->target() != target) {
    agent_host_ =
        std::make_unique<simulachrome::SimulachromeAgentHost>(target);
  }
  return agent_host_.get();
}

void SimulachromeWebUIMessageHandler::HandleExecuteSimulachromeAction(
    const base::Value::List& args) {
  AllowJavascript();

  // Accept either:
  //   [instruction, callbackId?, cssSelector?]  (legacy)
  //   [callbackId, instruction, cssSelector?]   (sendWithPromise / dashboard)
  if (args.empty() || !args[0].is_string()) {
    LOG(ERROR) << "[SimulachromeWebUI] execute_simulachrome_action: "
                  "missing instruction string";
    return;
  }
  std::string instruction;
  std::string callback_id;
  std::string target_selector;
  const std::string a0 = args[0].GetString();
  if (args.size() >= 2 && args[1].is_string() && IsPromiseCallbackId(a0)) {
    callback_id = a0;
    instruction = args[1].GetString();
    if (args.size() >= 3 && args[2].is_string()) {
      target_selector = args[2].GetString();
    }
  } else {
    instruction = a0;
    if (args.size() >= 2 && args[1].is_string()) {
      callback_id = args[1].GetString();
    }
    if (args.size() >= 3 && args[2].is_string()) {
      target_selector = args[2].GetString();
    }
  }
  if (instruction.empty()) {
    LOG(ERROR) << "[SimulachromeWebUI] execute_simulachrome_action: "
                  "empty instruction";
    return;
  }

  content::WebContents* target = ResolveTargetWebContents();
  if (!target) {
    LOG(ERROR) << "[SimulachromeWebUI] no target WebContents";
    if (!callback_id.empty()) {
      base::Value::Dict payload;
      payload.Set("ok", false);
      payload.Set("error", "no_target_web_contents");
      ResolveJsCallback(callback_id, payload);
    }
    return;
  }

  auto* host = EnsureAgentHost(target);
  if (!host) {
    return;
  }

  LOG(INFO) << "[SimulachromeWebUI] RunStep instruction=\"" << instruction
            << "\" selector=\"" << target_selector
            << "\" url=" << target->GetLastCommittedURL();

  // Flow: Strategy2 scrollIntoView (if selector) → 200ms settle → mailbox
  // capture → Mojo ExecuteAction → inject; Strategy1 re-loops on kScroll.
  host->RunStep(
      instruction, target_selector,
      base::BindOnce(&SimulachromeWebUIMessageHandler::OnStepComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback_id)));
}

void SimulachromeWebUIMessageHandler::HandleLoadSimulachromeModel(
    const base::Value::List& args) {
  AllowJavascript();

  // Accept either:
  //   [modelDirPath?, callbackId?]  (legacy)
  //   [callbackId, modelDirPath?]   (sendWithPromise)
  base::FilePath model_dir = DefaultModelDir();
  std::string callback_id;
  if (!args.empty() && args[0].is_string()) {
    const std::string a0 = args[0].GetString();
    if (IsPromiseCallbackId(a0)) {
      callback_id = a0;
      if (args.size() >= 2 && args[1].is_string() &&
          !args[1].GetString().empty()) {
        model_dir = base::FilePath::FromUTF8Unsafe(args[1].GetString());
      }
    } else {
      if (!a0.empty()) {
        model_dir = base::FilePath::FromUTF8Unsafe(a0);
      }
      if (args.size() >= 2 && args[1].is_string()) {
        callback_id = args[1].GetString();
      }
    }
  }

  content::WebContents* target = ResolveTargetWebContents();
  auto* host = EnsureAgentHost(target);
  if (!host) {
    if (!callback_id.empty()) {
      base::Value::Dict payload;
      payload.Set("ok", false);
      payload.Set("error", "no_agent_host");
      ResolveJsCallback(callback_id, payload);
    }
    return;
  }

  host->LoadModel(
      model_dir,
      base::BindOnce(
          [](base::WeakPtr<SimulachromeWebUIMessageHandler> self,
             std::string callback_id, bool ok, const std::string& error) {
            if (!self || callback_id.empty()) {
              return;
            }
            base::Value::Dict payload;
            payload.Set("ok", ok);
            payload.Set("error", error);
            self->ResolveJsCallback(callback_id, payload);
          },
          weak_factory_.GetWeakPtr(), std::move(callback_id)));
}

void SimulachromeWebUIMessageHandler::HandleSubmitSimulachromeBatch(
    const base::Value::List& args) {
  AllowJavascript();

  // [callbackId, batch] (sendWithPromise) or [batch, callbackId?] (legacy).
  std::string callback_id;
  const base::Value* batch_value = nullptr;
  if (!args.empty() && args[0].is_string() &&
      IsPromiseCallbackId(args[0].GetString()) && args.size() >= 2) {
    callback_id = args[0].GetString();
    batch_value = &args[1];
  } else if (!args.empty()) {
    batch_value = &args[0];
    if (args.size() >= 2 && args[1].is_string()) {
      callback_id = args[1].GetString();
    }
  }

  auto reject = [&](const std::string& error, const std::string& detail) {
    LOG(ERROR) << "[SimulachromeWebUI] submit_simulachrome_batch rejected: "
               << error << " " << detail;
    if (callback_id.empty()) {
      return;
    }
    base::Value::Dict payload;
    payload.Set("ok", false);
    payload.Set("error", error);
    if (!detail.empty()) {
      payload.Set("detail", detail);
    }
    ResolveJsCallback(callback_id, payload);
  };

  std::optional<base::Value> parsed;
  if (batch_value && batch_value->is_string()) {
    parsed = base::JSONReader::Read(batch_value->GetString(),
                                    base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    batch_value = parsed ? &*parsed : nullptr;
  }
  if (!batch_value || !batch_value->is_dict()) {
    reject("invalid_batch", "expected {actions:[...]}");
    return;
  }
  const base::Value::Dict& batch = batch_value->GetDict();
  const base::Value::List* actions_list = batch.FindList("actions");
  if (!actions_list) {
    reject("invalid_batch", "missing actions list");
    return;
  }

  // Validate the whole plan before anything runs: a bad step 4 must not leave
  // steps 1-3 already clicked.
  std::vector<simulachrome::PlannedAction> actions;
  std::string parse_error;
  if (!simulachrome::ParsePlannedBatch(*actions_list, &actions,
                                       &parse_error)) {
    reject("invalid_plan", parse_error);
    return;
  }
  simulachrome::PlannedBatchOptions options;
  if (std::optional<bool> stop = batch.FindBool("stop_on_failure")) {
    options.stop_on_failure = *stop;
  }

  content::WebContents* target = ResolveTargetWebContents();
  auto* host = EnsureAgentHost(target);
  if (!host) {
    reject("no_target_web_contents", std::string());
    return;
  }
  LOG(INFO) << "[SimulachromeWebUI] submit batch actions=" << actions.size()
            << " stop_on_failure=" << options.stop_on_failure
            << " url=" << target->GetLastCommittedURL();
  host->SubmitPlannedBatch(
      std::move(actions), options,
      simulachrome::PlannedActionRunner::ActionCallback(),
      base::BindOnce(&SimulachromeWebUIMessageHandler::OnBatchComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback_id)));
}

void SimulachromeWebUIMessageHandler::HandleCancelSimulachromeBatch(
    const base::Value::List& args) {
  if (agent_host_) {
    agent_host_->CancelPlannedBatches();
  }
  if (!args.empty() && args[0].is_string() &&
      IsPromiseCallbackId(args[0].GetString())) {
    base::Value::Dict payload;
    payload.Set("ok", true);
    ResolveJsCallback(args[0].GetString(), payload);
  }
}

void SimulachromeWebUIMessageHandler::OnBatchComplete(
    std::string callback_id,
    simulachrome::PlannedBatchResult result) {
  LOG(INFO) << "[SimulachromeWebUI] batch done ok=" << result.ok
            << " failed_index=" << result.failed_index
            << " results=" << result.results.size();
  if (callback_id.empty()) {
    return;
  }
  ResolveJsCallback(callback_id, result.ToValue());
}

void SimulachromeWebUIMessageHandler::OnStepComplete(
    std::string callback_id,
    bool ok,
    std::string error,
    simulachrome::mojom::AgentActionPtr action) {
  std::string reported_error = error;
  std::string hit_element;
  if (ok) {
    hit_element = std::move(error);
    reported_error.clear();
  } else if (base::StartsWith(error, "trusted_click:",
                              base::CompareCase::SENSITIVE)) {
    const size_t space = error.find(' ');
    if (space != std::string::npos && space + 1 < error.size()) {
      hit_element = error.substr(space + 1);
      reported_error = error.substr(0, space);
    }
  }
  LOG(INFO) << "[SimulachromeWebUI] step done ok=" << ok
            << " err=" << reported_error
            << " hit=" << hit_element
            << " action="
            << (action ? static_cast<int>(action->type) : -1);
  if (callback_id.empty()) {
    return;
  }
  base::Value::Dict payload;
  payload.Set("ok", ok);
  payload.Set("error", reported_error);
  if (!hit_element.empty()) {
    payload.Set("hit_element", hit_element);
  }
  if (action) {
    base::Value::Dict a;
    a.Set("type", static_cast<int>(action->type));
    a.Set("x", action->x);
    a.Set("y", action->y);
    a.Set("payload", action->payload);
    a.Set("confidence", action->confidence);
    payload.Set("action", std::move(a));
  }
  ResolveJsCallback(callback_id, payload);
}

void SimulachromeWebUIMessageHandler::ResolveJsCallback(
    const std::string& callback_id,
    const base::Value::Dict& payload) {
  if (callback_id.empty()) {
    return;
  }
  if (!IsJavascriptAllowed()) {
    AllowJavascript();
  }
  // Only allow simple JS identifiers as callback names.
  for (char c : callback_id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '$';
    if (!ok) {
      LOG(ERROR) << "[SimulachromeWebUI] invalid callback id";
      return;
    }
  }
  // Prefer window[cb] dispatch (same as Dashboard): CallJavascriptFunction on a
  // missing/expired id ReferenceErrors; parent_bridge installs globals on the
  // chrome://glic host that must be invoked by name.
  content::RenderFrameHost* frame =
      web_ui()->GetWebContents()
          ? web_ui()->GetWebContents()->GetPrimaryMainFrame()
          : nullptr;
  if (!frame) {
    return;
  }
  std::string cb_js;
  base::EscapeJSONString(callback_id, /*put_in_quotes=*/true, &cb_js);
  std::string payload_json;
  if (!base::JSONWriter::Write(base::Value(payload.Clone()), &payload_json)) {
    payload_json = "null";
  }
  std::string js = base::StrCat(
      {"try{const __cb=", cb_js,
       ";const __fn=(window&&window[__cb]);if(typeof __fn==='function'){__fn(",
       payload_json, ");}}catch(e){};"});
  frame->ExecuteJavaScript(base::UTF8ToUTF16(js), base::NullCallback());
}

}  // namespace simulachrome_webui
