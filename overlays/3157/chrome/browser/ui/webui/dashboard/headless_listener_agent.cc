// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/headless_listener_agent.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)
#include <deque>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/flat_map.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/json/json_writer.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/actions/native_page_map_matcher.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_devtools_client.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_ipc.h"
#include "chrome/browser/ui/webui/dashboard/listener_manager.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/headless_script_runner.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/common/chrome_switches.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/referrer.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "base/files/file_descriptor_watcher_posix.h"
#include "base/files/scoped_file.h"
#include "base/posix/eintr_wrapper.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/win/scoped_handle.h"
#endif
#endif  // BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)

namespace dashboard {

#if BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)
namespace {

bool IsHeadlessListenerProcess() {
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kHeadlessListener);
}

struct AgentPrepareResult {
  bool ok = false;
  std::string error;
  base::FilePath socket_path;
  base::Value::Dict scripts;
};

AgentPrepareResult PrepareAgentFilesystemBlocking(Profile* profile) {
  AgentPrepareResult out;
  out.socket_path = DefaultHeadlessControlSocketPath();
  if (out.socket_path.empty()) {
    out.error = "No control socket path.";
    return out;
  }
#if BUILDFLAG(IS_WIN)
  // Named pipes are not filesystem objects; skip directory/socket cleanup.
#else
  if (!base::CreateDirectory(out.socket_path.DirName())) {
    out.error = "Failed to create control socket directory.";
    return out;
  }
  base::DeleteFile(out.socket_path);
#endif
  out.scripts = ScriptStore::LoadAll(profile);
  out.ok = true;
  return out;
}

std::string FindStringAny(const base::Value::Dict& dict,
                          std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    if (const std::string* value = dict.FindString(key);
        value && !value->empty()) {
      return *value;
    }
  }
  return std::string();
}

int FindIntAny(const base::Value::Dict& dict,
               std::initializer_list<const char*> keys,
               int fallback) {
  for (const char* key : keys) {
    if (std::optional<int> value = dict.FindInt(key)) {
      return *value;
    }
    if (std::optional<double> value = dict.FindDouble(key)) {
      return static_cast<int>(*value);
    }
  }
  return fallback;
}

bool FindBoolAny(const base::Value::Dict& dict,
                 std::initializer_list<const char*> keys,
                 bool fallback) {
  for (const char* key : keys) {
    if (std::optional<bool> value = dict.FindBool(key)) {
      return *value;
    }
  }
  return fallback;
}

std::string ActionName(const base::Value::Dict& action) {
  const std::string name = FindStringAny(action, {"action", "name", "type"});
  return name;
}

bool ActionsAreOnlyNavigateAndWait(const base::Value::List& actions) {
  if (actions.empty()) {
    return true;
  }
  for (const base::Value& item : actions) {
    if (!item.is_dict()) {
      return false;
    }
    const std::string name = ActionName(item.GetDict());
    if (name != "navigate" && name != "wait") {
      return false;
    }
  }
  return true;
}

std::string ClipContextText(std::string text, size_t limit) {
  text = base::CollapseWhitespaceASCII(text, true);
  if (text.size() > limit) {
    text.resize(limit);
  }
  return text;
}

bool ContextContains(std::string_view haystack, std::string_view needle) {
  return base::ToLowerASCII(haystack).find(needle) != std::string::npos;
}

// Project the same Annotated Page Content page map the headed agent uses into
// the investigate_page fields listeners and the model already read.
base::Value::Dict InvestigationSnapshotFromPageMap(
    const base::Value::Dict& page_map,
    content::WebContents* web_contents,
    int max_links,
    int max_inputs,
    int max_buttons,
    int max_text_chars) {
  const size_t link_limit = static_cast<size_t>(std::max(1, max_links));
  const size_t input_limit = static_cast<size_t>(std::max(1, max_inputs));
  const size_t button_limit = static_cast<size_t>(std::max(1, max_buttons));
  const size_t text_limit =
      static_cast<size_t>(std::max(200, max_text_chars));

  base::Value::List links;
  base::Value::List inputs;
  base::Value::List buttons;
  base::Value::List tables;
  base::Value::List sample_rows;
  std::string excerpt;
  int element_count = 0;

  auto visit = [&](const base::Value::Dict& element) {
    ++element_count;
    const std::string tag = element.FindString("tag")
                                ? base::ToLowerASCII(*element.FindString("tag"))
                                : std::string();
    const std::string role =
        element.FindString("role")
            ? base::ToLowerASCII(*element.FindString("role"))
            : std::string();
    const std::string type = element.FindString("type")
                                 ? base::ToLowerASCII(*element.FindString("type"))
                                 : std::string();
    const std::string text = ClipContextText(
        element.FindString("text") ? *element.FindString("text") : std::string(),
        140);
    const std::string id =
        element.FindString("idAttr") ? *element.FindString("idAttr") : std::string();
    const std::string name = element.FindString("nameAttr")
                                 ? *element.FindString("nameAttr")
                                 : std::string();
    const std::string class_name = ClipContextText(
        element.FindString("classAttr") ? *element.FindString("classAttr")
                                        : std::string(),
        80);
    const std::string placeholder = element.FindString("placeholder")
                                        ? *element.FindString("placeholder")
                                        : std::string();
    const std::string ax_name = element.FindString("accessibilityName")
                                    ? *element.FindString("accessibilityName")
                                    : std::string();
    std::string href = id.empty() ? std::string() : "#" + id;
    if (href.empty()) {
      href = name;
    }

    if (excerpt.size() < text_limit && !text.empty() &&
        !ContextContains(type, "password")) {
      if (!excerpt.empty()) {
        excerpt.append("\n");
      }
      excerpt.append(text);
      if (excerpt.size() > text_limit) {
        excerpt.resize(text_limit);
      }
    }

    const bool is_row = tag == "tr" || ContextContains(role, "row") ||
                        ContextContains(type, "table_row");
    const bool is_table =
        !is_row && (tag == "table" || ContextContains(type, "content_attribute_table"));
    const bool is_button = tag == "button" || ContextContains(role, "button") ||
                           ContextContains(type, "submit") ||
                           ContextContains(type, "form_control_type_button");
    const bool is_input =
        !is_button &&
        (tag == "input" || tag == "textarea" || tag == "select" ||
         ContextContains(type, "form_control") ||
         ContextContains(role, "textbox") || ContextContains(role, "combobox"));
    const bool is_link = tag == "a" || ContextContains(role, "link") ||
                         ContextContains(type, "anchor");

    if (is_row && sample_rows.size() < 3 && !text.empty()) {
      sample_rows.Append(text);
    }
    if (is_table && tables.size() < 6) {
      tables.Append(base::Value::Dict()
                        .Set("id", id)
                        .Set("className", class_name)
                        .Set("trCount", 0)
                        .Set("suggestedRowSelector",
                             id.empty() ? "table tbody tr"
                                        : "#" + id + " tbody tr"));
    } else if (is_input && inputs.size() < input_limit) {
      inputs.Append(base::Value::Dict()
                        .Set("tag", tag)
                        .Set("type", type)
                        .Set("name", name)
                        .Set("id", id)
                        .Set("placeholder", placeholder)
                        .Set("aria", ax_name));
    } else if (is_button && buttons.size() < button_limit) {
      buttons.Append(base::Value::Dict()
                         .Set("text", text.empty() ? ax_name : text)
                         .Set("id", id)
                         .Set("className", class_name));
    } else if (is_link && links.size() < link_limit &&
               (!text.empty() || !href.empty())) {
      links.Append(base::Value::Dict().Set("href", href).Set("text", text));
    }
  };

  if (const base::Value::List* frames = page_map.FindList("frames")) {
    for (const base::Value& frame_value : *frames) {
      const base::Value::Dict* frame = frame_value.GetIfDict();
      if (!frame) {
        continue;
      }
      if (const base::Value::List* elements = frame->FindList("elements")) {
        for (const base::Value& element_value : *elements) {
          if (const base::Value::Dict* element = element_value.GetIfDict()) {
            visit(*element);
          }
        }
      }
      if (const base::Value::List* shadows = frame->FindList("shadowRoots")) {
        for (const base::Value& shadow_value : *shadows) {
          const base::Value::Dict* shadow = shadow_value.GetIfDict();
          if (!shadow) {
            continue;
          }
          if (const base::Value::List* elements = shadow->FindList("elements")) {
            for (const base::Value& element_value : *elements) {
              if (const base::Value::Dict* element = element_value.GetIfDict()) {
                visit(*element);
              }
            }
          }
        }
      }
    }
  }
  if (!sample_rows.empty()) {
    const int row_count = static_cast<int>(sample_rows.size());
    if (tables.empty()) {
      tables.Append(base::Value::Dict()
                        .Set("id", "")
                        .Set("className", "")
                        .Set("trCount", row_count)
                        .Set("sampleRows", std::move(sample_rows))
                        .Set("suggestedRowSelector", "table tbody tr"));
    } else if (tables[0].is_dict()) {
      tables[0].GetDict().Set("sampleRows", std::move(sample_rows));
      tables[0].GetDict().Set("trCount", row_count);
    }
  }

  base::Value::Dict out;
  out.Set("source", "apc");
  if (web_contents) {
    out.Set("pageUrl", web_contents->GetLastCommittedURL().spec());
    out.Set("title", base::UTF16ToUTF8(web_contents->GetTitle()));
    out.Set("readyState", web_contents->IsLoading() ? "loading" : "complete");
  } else if (const std::string* url = page_map.FindString("url")) {
    out.Set("pageUrl", *url);
  }
  out.Set("elementCount", element_count);
  out.Set("links", std::move(links));
  out.Set("inputs", std::move(inputs));
  out.Set("buttons", std::move(buttons));
  out.Set("tables", std::move(tables));
  out.Set("innerTextExcerpt", std::move(excerpt));
  return out;
}

base::Value::Dict CandidateToProbeDict(const NativePageMapCandidate& c) {
  base::Value::Dict d;
  d.Set("tag", c.tag);
  d.Set("text", c.text);
  d.Set("visible", c.visible);
  d.Set("clickable", c.clickable);
  d.Set("focusable", c.focusable);
  d.Set("editable", c.editable);
  d.Set("disabled", c.disabled);
  d.Set("css_selector", c.css_selector);
  d.Set("xpath", c.xpath);
  d.Set("accessibility_name", c.accessibility_name);
  d.Set("role", c.role);
  d.Set("id_attr", c.id_attr);
  return d;
}

class InvestigateSession : public content::WebContentsObserver {
 public:
  using FinishCallback =
      base::OnceCallback<void(bool ok, base::Value::Dict payload)>;

  InvestigateSession(Profile* profile,
                     Dashboard* dashboard,
                     Browser* browser,
                     base::Value::Dict request,
                     FinishCallback done)
      : profile_(profile),
        dashboard_(dashboard),
        browser_(browser),
        request_(std::move(request)),
        done_(std::move(done)) {
    start_url_ = FindStringAny(request_, {"url", "start_url", "startUrl"});
    if (start_url_.empty()) {
      start_url_ = "about:blank";
    }
    // Keep the investigation tab by default so follow-up probes can reuse it.
    // Callers that want a one-shot tab set close_target/closeTarget true.
    close_target_ =
        FindBoolAny(request_, {"close_target", "closeTarget"}, false);
    include_screenshot_ = FindBoolAny(
        request_, {"include_screenshot", "includeScreenshot"}, false);
    include_image_base64_ = FindBoolAny(
        request_, {"include_image_base64", "includeImageBase64"}, false);
    max_links_ = FindIntAny(request_, {"max_links", "maxLinks"}, 16);
    max_inputs_ = FindIntAny(request_, {"max_inputs", "maxInputs"}, 16);
    max_buttons_ = FindIntAny(request_, {"max_buttons", "maxButtons"}, 16);
    max_text_chars_ =
        FindIntAny(request_, {"max_text_chars", "maxTextChars"}, 1400);
    settle_ms_ = FindIntAny(request_, {"settle_ms", "settleMs"}, 500);
    if (const base::Value::List* actions = request_.FindList("actions")) {
      actions_ = actions->Clone();
    }
    // url alone must mean "go there": synthesize navigate+wait when no
    // actions were provided for an http(s) target.
    const GURL start_gurl(start_url_);
    if (actions_.empty() && start_gurl.is_valid() &&
        start_gurl.SchemeIsHTTPOrHTTPS()) {
      base::Value::Dict nav;
      nav.Set("action", "navigate");
      base::Value::Dict nav_params;
      nav_params.Set("url", start_url_);
      nav.Set("params", std::move(nav_params));
      actions_.Append(std::move(nav));
      base::Value::Dict wait;
      wait.Set("action", "wait");
      base::Value::Dict wait_params;
      wait_params.Set("waitType", "time");
      wait_params.Set("time", 800);
      wait_params.Set("timeout", 0);
      wait_params.Set("locators", base::Value::List());
      wait.Set("params", std::move(wait_params));
      actions_.Append(std::move(wait));
    }
    if (const base::Value::Dict* probe = request_.FindDict("probe")) {
      probe_ = probe->Clone();
    }
    target_id_ = FindStringAny(request_, {"target_id", "targetId"});
    if (target_id_.empty()) {
      target_id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
    }
    // Pre-snapshot KEY / saved script: key_script_id preferred, else script_id
    // when the caller is asking for a gated investigate (url present).
    key_script_id_ = FindStringAny(request_, {"key_script_id", "keyScriptId"});
    if (key_script_id_.empty() && start_url_ != "about:blank") {
      key_script_id_ = FindStringAny(request_, {"script_id", "scriptId"});
    }
  }

  const std::string& target_id() const { return target_id_; }
  bool close_target() const { return close_target_; }

  // When close_target is false, leave the tab for the next probe.
  content::WebContents* ReleaseKeptWebContents() {
    if (close_target_ || !web_contents()) {
      return nullptr;
    }
    content::WebContents* wc = web_contents();
    Observe(nullptr);
    return wc;
  }

  ~InvestigateSession() override { load_timer_.Stop(); settle_timer_.Stop(); }

  void Start(content::WebContents* reuse_wc = nullptr) {
    if (!profile_ || !browser_ || !browser_->tab_strip_model()) {
      FinishError("No browser available for investigation.");
      return;
    }
    content::WebContents* wc = reuse_wc;
    const GURL url = GURL(start_url_).is_valid() ? GURL(start_url_)
                                                 : GURL("about:blank");
    if (wc) {
      Observe(wc);
      // Reuse an existing investigation tab; navigate if a real URL was given.
      if (url.is_valid() && url.SchemeIsHTTPOrHTTPS() &&
          wc->GetLastCommittedURL() != url) {
        wc->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                    std::string());
      }
    } else {
      NavigateParams np(browser_, url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
      np.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
      Navigate(&np);
      wc = np.navigated_or_inserted_contents;
      if (!wc) {
        FinishError("Failed to create investigation tab.");
        return;
      }
      Observe(wc);
    }
    load_timer_.Start(FROM_HERE, base::Seconds(30), this,
                      &InvestigateSession::OnLoadTimeout);
    if (!wc->IsLoading()) {
      AfterLoad();
    }
  }

 private:
  void DidStopLoading() override { AfterLoad(); }

  void WebContentsDestroyed() override {
    Observe(nullptr);
    if (!finished_) {
      FinishError("Investigation tab was destroyed.");
    }
  }

  void OnLoadTimeout() {
    if (load_seen_) {
      return;
    }
    LOG(WARNING) << "[HeadlessAgent] investigate_page load timed out url="
                 << start_url_;
    AfterLoad();
  }

  void AfterLoad() {
    if (load_seen_ || finished_) {
      return;
    }
    load_seen_ = true;
    load_timer_.Stop();
    if (!web_contents()) {
      FinishError("Investigation tab gone after load.");
      return;
    }
    if (!key_script_id_.empty() && !key_ran_) {
      RunKeyScriptThenContinue();
      return;
    }
    ContinueAfterKey();
  }

  void RunKeyScriptThenContinue() {
    key_ran_ = true;
    if (!dashboard_ || !web_contents()) {
      ContinueAfterKey();
      return;
    }
    std::string error;
    base::Value::List timeline =
        LoadExpandedScriptTimeline(dashboard_, key_script_id_, &error);
    if (timeline.empty()) {
      payload_.Set("key_script_ok", false);
      payload_.Set("key_script_error",
                   error.empty() ? "KEY script empty." : error);
      ContinueAfterKey();
      return;
    }
    const std::string id = StartNativeAdHocRun(
        profile_, timeline, web_contents(),
        base::BindOnce(&InvestigateSession::OnKeyScriptDone,
                       weak_factory_.GetWeakPtr()));
    if (id.empty()) {
      payload_.Set("key_script_ok", false);
      payload_.Set("key_script_error", "Failed to start KEY script.");
      ContinueAfterKey();
    }
  }

  void OnKeyScriptDone(bool success, int failed_step, std::string message) {
    payload_.Set("key_script_id", key_script_id_);
    payload_.Set("key_script_ok", success);
    if (!success) {
      if (!message.empty()) {
        payload_.Set("key_script_error", message);
      }
      payload_.Set("key_script_failed_step", failed_step);
    }
    ContinueAfterKey();
  }

  void ContinueAfterKey() {
    if (finished_) {
      return;
    }
    if (!web_contents()) {
      FinishError("Investigation tab gone after KEY script.");
      return;
    }
    if (!ActionsAreOnlyNavigateAndWait(actions_)) {
      RunActions();
      return;
    }
    ScheduleSnapshot();
  }

  void RunActions() {
    ScriptExecutionService* exec =
        ScriptExecutionServiceFactory::GetForProfile(profile_);
    if (!exec || !web_contents()) {
      ScheduleSnapshot();
      return;
    }
    ScriptExecutionService::RunOptions options;
    options.force_native_execution = true;
    const std::string id = exec->StartAdHocRunWithOptions(
        actions_, web_contents(), options,
        base::BindOnce(&InvestigateSession::OnActionsDone,
                       weak_factory_.GetWeakPtr()));
    if (id.empty()) {
      payload_.Set("actions_error", "Failed to start investigation actions.");
      ScheduleSnapshot();
    }
  }

  void OnActionsDone(bool success, int failed_step, std::string message) {
    if (!success) {
      payload_.Set("actions_ok", false);
      if (!message.empty()) {
        payload_.Set("actions_error", message);
      }
      payload_.Set("actions_failed_step", failed_step);
    } else {
      payload_.Set("actions_ok", true);
    }
    ScheduleSnapshot();
  }

  void ScheduleSnapshot() {
    const int delay_ms = std::clamp(settle_ms_, 0, 5000);
    if (delay_ms <= 0) {
      CaptureSnapshot();
      return;
    }
    settle_timer_.Start(FROM_HERE, base::Milliseconds(delay_ms), this,
                        &InvestigateSession::CaptureSnapshot);
  }

  void CaptureSnapshot() {
    if (finished_) {
      return;
    }
    content::WebContents* wc = web_contents();
    if (!wc) {
      FinishError("Investigation tab gone before snapshot.");
      return;
    }
    FetchAgentPageMap(
        wc, base::BindOnce(&InvestigateSession::OnAgentPageMap,
                           weak_factory_.GetWeakPtr()));
  }

  void OnAgentPageMap(UnifiedPageMapResult result) {
    if (finished_) {
      return;
    }
    base::Value::Dict page_map;
    base::Value::Dict snap;
    if (!result.ok) {
      snap.Set("contextError", result.error);
    } else {
      page_map = std::move(result.page_map);
      snap = InvestigationSnapshotFromPageMap(page_map, web_contents(),
                                             max_links_, max_inputs_,
                                             max_buttons_, max_text_chars_);
    }
    ApplySnapshot(base::Value(std::move(snap)));
    RunProbe(std::move(page_map));
  }

  void ApplySnapshot(base::Value result) {
    content::WebContents* wc = web_contents();
    std::string url = start_url_;
    std::string title;
    if (wc) {
      url = wc->GetLastCommittedURL().spec();
      title = base::UTF16ToUTF8(wc->GetTitle());
    }
    int element_count = 0;
    std::string ready_state;
    std::string inner_text;
    base::Value::List links;
    base::Value::List inputs;
    base::Value::List buttons;
    base::Value::List tables;
    if (result.is_dict()) {
      const base::Value::Dict& snap = result.GetDict();
      if (const std::string* page_url = snap.FindString("pageUrl")) {
        if (!page_url->empty()) {
          url = *page_url;
        }
      }
      if (const std::string* snap_title = snap.FindString("title")) {
        title = *snap_title;
      }
      if (const std::string* rs = snap.FindString("readyState")) {
        ready_state = *rs;
      }
      if (std::optional<int> int_count = snap.FindInt("elementCount")) {
        element_count = *int_count;
      } else if (std::optional<double> double_count =
                     snap.FindDouble("elementCount")) {
        element_count = static_cast<int>(*double_count);
      }
      if (const std::string* excerpt = snap.FindString("innerTextExcerpt")) {
        inner_text = *excerpt;
      }
      if (const base::Value::List* v = snap.FindList("links")) {
        links = v->Clone();
      }
      if (const base::Value::List* v = snap.FindList("inputs")) {
        inputs = v->Clone();
      }
      if (const base::Value::List* v = snap.FindList("buttons")) {
        buttons = v->Clone();
      }
      if (const base::Value::List* v = snap.FindList("tables")) {
        tables = v->Clone();
      }
      if (const std::string* source = snap.FindString("source")) {
        payload_.Set("context_source", *source);
      }
      if (const std::string* context_error = snap.FindString("contextError")) {
        payload_.Set("context_error", *context_error);
      }
    }
    payload_.Set("implemented", true);
    payload_.Set("command", "investigate_page");
    payload_.Set("target_id", target_id_);
    payload_.Set("close_target", close_target_);
    payload_.Set("url", url);
    payload_.Set("title", title);
    payload_.Set("ready_state", ready_state);
    payload_.Set("element_count", element_count);
    payload_.Set("inner_text_excerpt", inner_text);
    payload_.Set("links", std::move(links));
    payload_.Set("inputs", std::move(inputs));
    payload_.Set("buttons", std::move(buttons));
    payload_.Set("tables", std::move(tables));
  }

  void RunProbe(base::Value::Dict page_map) {
    if (probe_.empty()) {
      MaybeScreenshot();
      return;
    }
    const base::Value::List* locators = probe_.FindList("locators");
    if (!locators || locators->empty() || !web_contents()) {
      base::Value::Dict probe_out = probe_.Clone();
      base::Value::Dict result;
      result.Set("found", false);
      result.Set("match_count", 0);
      probe_out.Set("result", std::move(result));
      payload_.Set("locator_probe", std::move(probe_out));
      MaybeScreenshot();
      return;
    }
    OnPageMap(std::move(page_map));
  }

  void OnPageMap(base::Value::Dict page_map) {
    base::Value::Dict probe_out;
    const std::string condition =
        FindStringAny(probe_, {"condition"}).empty()
            ? "visible"
            : FindStringAny(probe_, {"condition"});
    const std::string needle =
        FindStringAny(probe_, {"text", "needle", "value"});
    const std::string attribute = FindStringAny(probe_, {"attribute"});
    probe_out.Set("condition", condition);
    probe_out.Set("needle", needle);
    probe_out.Set("attribute", attribute);
    probe_out.Set("text_use_regex",
                  FindBoolAny(probe_, {"textUseRegex", "text_use_regex"},
                              false));
    probe_out.Set("attribute_use_regex",
                  FindBoolAny(probe_,
                              {"attributeUseRegex", "attribute_use_regex"},
                              false));
    if (const base::Value::List* locators = probe_.FindList("locators")) {
      probe_out.Set("locators", locators->Clone());
      base::Value::List per_locator;
      bool any_found = false;
      NativePageMapCandidate first_match;
      bool have_first = false;
      base::Value::List all_matches;
      for (const base::Value& loc_val : *locators) {
        base::Value::List one;
        one.Append(loc_val.Clone());
        std::vector<Locator> converted = ConvertLocators(one);
        std::vector<NativePageMapCandidate> matches =
            FindMatchingPageMapCandidates(page_map, converted, 4);
        std::string matched_via;
        // Page maps often omit idAttr; ID locators still resolve via #id CSS.
        if (matches.empty()) {
          if (const base::Value::Dict* loc_dict = loc_val.GetIfDict()) {
            const std::string* type = loc_dict->FindString("type");
            const std::string* value = loc_dict->FindString("value");
            if (type && value && !value->empty() &&
                base::EqualsCaseInsensitiveASCII(*type, "ID")) {
              base::Value::List css_one;
              base::Value::Dict css_loc;
              css_loc.Set("type", "CSS_SELECTOR");
              std::string css_val = *value;
              if (!base::StartsWith(css_val, "#")) {
                css_val = "#" + css_val;
              }
              css_loc.Set("value", css_val);
              css_one.Append(std::move(css_loc));
              matches = FindMatchingPageMapCandidates(
                  page_map, ConvertLocators(css_one), 4);
              if (!matches.empty()) {
                matched_via = "css_id_fallback";
              }
            }
          }
        }
        base::Value::Dict entry;
        if (const base::Value::Dict* loc_dict = loc_val.GetIfDict()) {
          if (const std::string* t = loc_dict->FindString("type")) {
            entry.Set("type", *t);
          }
          if (const std::string* v = loc_dict->FindString("value")) {
            entry.Set("value", *v);
          }
        }
        entry.Set("found", !matches.empty());
        entry.Set("match_count", static_cast<int>(matches.size()));
        if (!matched_via.empty()) {
          entry.Set("matched_via", matched_via);
        }
        if (!matches.empty()) {
          any_found = true;
          entry.Set("sample", CandidateToProbeDict(matches[0]));
          if (!have_first) {
            first_match = matches[0];
            have_first = true;
          }
          for (const NativePageMapCandidate& match : matches) {
            all_matches.Append(CandidateToProbeDict(match));
          }
        }
        per_locator.Append(std::move(entry));
      }
      probe_out.Set("per_locator", std::move(per_locator));
      // Aggregate OR result for older consumers (no second full page-map scan).
      base::Value::Dict result;
      result.Set("found", any_found);
      result.Set("match_count", static_cast<int>(all_matches.size()));
      if (have_first) {
        result.Set("tag", first_match.tag);
        result.Set("text", first_match.text);
        result.Set("visible", first_match.visible);
        result.Set("clickable", first_match.clickable);
        result.Set("css_selector", first_match.css_selector);
        result.Set("accessibility_name", first_match.accessibility_name);
        result.Set("matchValue", first_match.text);
      }
      // Cap match samples — model prompt uses per_locator, not the full list.
      if (!all_matches.empty()) {
        base::Value::List capped;
        for (size_t i = 0; i < all_matches.size() && i < 4; ++i) {
          capped.Append(all_matches[i].Clone());
        }
        result.Set("matches", std::move(capped));
      }
      probe_out.Set("result", std::move(result));
    }
    payload_.Set("locator_probe", std::move(probe_out));
    MaybeScreenshot();
  }

  void MaybeScreenshot() {
    if (!include_screenshot_ || !web_contents()) {
      FinishOk();
      return;
    }
    content::RenderWidgetHostView* view =
        web_contents()->GetRenderWidgetHostView();
    if (!view || !view->IsSurfaceAvailableForCopy()) {
      payload_.Set("screenshot_path", "");
      payload_.Set("screenshot_image_base64", "");
      FinishOk();
      return;
    }
    view->CopyFromSurface(
        gfx::Rect(), gfx::Size(),
        base::BindOnce(&InvestigateSession::OnScreenshot,
                       weak_factory_.GetWeakPtr()));
  }

  void OnScreenshot(const viz::CopyOutputBitmapWithMetadata& result) {
    payload_.Set("screenshot_path", "");
    if (result.bitmap.drawsNothing()) {
      payload_.Set("screenshot_image_base64", "");
      FinishOk();
      return;
    }
    std::optional<std::vector<uint8_t>> png =
        gfx::PNGCodec::EncodeBGRASkBitmap(result.bitmap,
                                          /*discard_transparency=*/false);
    if (!png || png->empty() || !include_image_base64_) {
      payload_.Set("screenshot_image_base64", "");
      FinishOk();
      return;
    }
    payload_.Set("screenshot_image_base64",
                 base::Base64Encode(base::span<const uint8_t>(*png)));
    FinishOk();
  }

  void FinishOk() {
    // Never report success on about:blank when the caller asked for a real URL.
    const GURL requested(start_url_);
    content::WebContents* wc = web_contents();
    const GURL actual = wc ? wc->GetLastCommittedURL() : GURL();
    if (requested.is_valid() && requested.SchemeIsHTTPOrHTTPS()) {
      if (!actual.is_valid() || actual.IsAboutBlank() ||
          !actual.SchemeIsHTTPOrHTTPS()) {
        FinishError(
            "Investigation never reached the requested page (still blank). "
            "url=" +
            start_url_);
        return;
      }
    }
    payload_.Set("ok", true);
    Finish(true, std::move(payload_));
  }

  void FinishError(const std::string& error) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("implemented", true);
    out.Set("command", "investigate_page");
    out.Set("error", error);
    if (!start_url_.empty()) {
      out.Set("url", start_url_);
    }
    Finish(false, std::move(out));
  }

  void Finish(bool ok, base::Value::Dict payload) {
    if (finished_) {
      return;
    }
    finished_ = true;
    load_timer_.Stop();
    settle_timer_.Stop();
    CloseTabIfNeeded();
    if (done_) {
      std::move(done_).Run(ok, std::move(payload));
    }
  }

  void CloseTabIfNeeded() {
    if (!close_target_) {
      // Tab is retained for reuse; ReleaseKeptWebContents() clears observation.
      return;
    }
    if (!browser_ || !web_contents()) {
      Observe(nullptr);
      return;
    }
    TabStripModel* tabs = browser_->tab_strip_model();
    content::WebContents* wc = web_contents();
    Observe(nullptr);
    if (!tabs) {
      return;
    }
    const int index = tabs->GetIndexOfWebContents(wc);
    if (index >= 0) {
      tabs->CloseWebContentsAt(index, TabCloseTypes::CLOSE_NONE);
    }
  }

  raw_ptr<Profile> profile_ = nullptr;
  raw_ptr<Dashboard> dashboard_ = nullptr;
  raw_ptr<Browser> browser_ = nullptr;
  base::Value::Dict request_;
  base::Value::Dict payload_;
  base::Value::Dict probe_;
  base::Value::List actions_;
  FinishCallback done_;
  std::string start_url_;
  std::string target_id_;
  std::string key_script_id_;
  bool key_ran_ = false;
  bool close_target_ = false;
  bool include_screenshot_ = false;
  bool include_image_base64_ = false;
  bool finished_ = false;
  bool load_seen_ = false;
  int max_links_ = 16;
  int max_inputs_ = 16;
  int max_buttons_ = 16;
  int max_text_chars_ = 1400;
  int settle_ms_ = 500;
  base::OneShotTimer load_timer_;
  base::OneShotTimer settle_timer_;
  base::WeakPtrFactory<InvestigateSession> weak_factory_{this};
};

}  // namespace

class HeadlessListenerAgent::Impl {
 public:
  Impl() = default;
  ~Impl() { Shutdown(); }

  void MaybeStartForProfile(Profile* profile) {
    if (!IsHeadlessListenerProcess() || !profile || started_ ||
        start_requested_) {
      return;
    }
    start_requested_ = true;
    profile_ = profile;
    // Defer past BrowserMainLoop startup (blocking disallowed) and do
    // CreateDirectory / ScriptStore I/O on a MayBlock worker.
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&Impl::StartAfterStartup, weak_factory_.GetWeakPtr()));
  }

  void StartAfterStartup() {
    if (!profile_ || started_) {
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
         base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::BindOnce(&PrepareAgentFilesystemBlocking, profile_),
        base::BindOnce(&Impl::OnFilesystemPrepared,
                       weak_factory_.GetWeakPtr()));
  }

  void OnFilesystemPrepared(AgentPrepareResult prepared) {
    if (!profile_ || started_) {
      return;
    }
    if (!prepared.ok) {
      LOG(ERROR) << "[HeadlessAgent] " << prepared.error;
      start_requested_ = false;
      return;
    }
    socket_path_ = std::move(prepared.socket_path);
    dashboard_ = std::make_unique<Dashboard>();
    dashboard_->ReplaceAllScripts(std::move(prepared.scripts));
    listener_manager_ =
        std::make_unique<ListenerManager>(profile_, dashboard_.get());
    listener_manager_->SetRemoteTriggerDispatchCallback(base::BindRepeating(
        &Impl::OnRemoteTrigger, weak_factory_.GetWeakPtr()));

    if (!EnsureBrowser()) {
      start_requested_ = false;
      LOG(ERROR) << "[HeadlessAgent] Failed to create Browser.";
      return;
    }
    if (!BindAndListen()) {
      start_requested_ = false;
      return;
    }
    started_ = true;
    LOG(INFO) << "[HeadlessAgent] Listening on " << socket_path_;
    WatchAccept();
  }

  void Shutdown() {
#if BUILDFLAG(IS_POSIX)
    accept_watcher_.reset();
    client_watcher_.reset();
    if (client_fd_ >= 0) {
      IGNORE_EINTR(close(client_fd_));
      client_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
      IGNORE_EINTR(close(listen_fd_));
      listen_fd_ = -1;
    }
    if (!socket_path_.empty()) {
      // Best-effort; shutdown can run after BrowserMain teardown.
      base::ThreadPool::PostTask(
          FROM_HERE,
          {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
          base::BindOnce(base::IgnoreResult(&base::DeleteFile), socket_path_));
    }
#elif BUILDFLAG(IS_WIN)
    if (pipe_.is_valid()) {
      CancelIoEx(pipe_.Get(), nullptr);
      pipe_.Close();
    }
#endif
    listener_manager_.reset();
    dashboard_.reset();
    investigate_.reset();
    resume_op_.reset();
    execute_busy_ = false;
    run_script_busy_ = false;
    pending_agent_ops_.clear();
    retained_investigate_wc_ = nullptr;
    retained_target_id_.clear();
    started_ = false;
    start_requested_ = false;
  }

 private:
  Browser* EnsureBrowser() {
    if (Browser* existing = chrome::FindLastActiveWithProfile(profile_)) {
      return existing;
    }
    Browser::CreateParams params(profile_, /*user_gesture=*/false);
    params.omit_from_session_restore = true;
    Browser* created = Browser::Create(params);
    LOG(INFO) << "[HeadlessAgent] Created Browser for listener tabs.";
    return created;
  }

  bool BindAndListen() {
#if BUILDFLAG(IS_POSIX)
    listen_fd_ = HANDLE_EINTR(socket(AF_UNIX, SOCK_STREAM, 0));
    if (listen_fd_ < 0) {
      PLOG(ERROR) << "[HeadlessAgent] socket()";
      return false;
    }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    const std::string path = socket_path_.AsUTF8Unsafe();
    if (path.size() >= sizeof(addr.sun_path)) {
      LOG(ERROR) << "[HeadlessAgent] socket path too long";
      IGNORE_EINTR(close(listen_fd_));
      listen_fd_ = -1;
      return false;
    }
    UNSAFE_TODO(memcpy(addr.sun_path, path.data(), path.size()));
    if (HANDLE_EINTR(bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                          sizeof(addr))) != 0) {
      PLOG(ERROR) << "[HeadlessAgent] bind()";
      IGNORE_EINTR(close(listen_fd_));
      listen_fd_ = -1;
      return false;
    }
    if (HANDLE_EINTR(listen(listen_fd_, 1)) != 0) {
      PLOG(ERROR) << "[HeadlessAgent] listen()";
      IGNORE_EINTR(close(listen_fd_));
      listen_fd_ = -1;
      return false;
    }
    return true;
#elif BUILDFLAG(IS_WIN)
    io_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
         base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN});
    ui_runner_ = base::SequencedTaskRunner::GetCurrentDefault();
    return true;
#else
    return false;
#endif
  }

  void WatchAccept() {
#if BUILDFLAG(IS_POSIX)
    if (listen_fd_ < 0) {
      return;
    }
    accept_watcher_ = base::FileDescriptorWatcher::WatchReadable(
        listen_fd_,
        base::BindRepeating(&Impl::OnAcceptable, weak_factory_.GetWeakPtr()));
#elif BUILDFLAG(IS_WIN)
    if (!io_runner_) {
      return;
    }
    io_runner_->PostTask(FROM_HERE,
                         base::BindOnce(&Impl::AcceptPipeOnIo,
                                        base::Unretained(this),
                                        weak_factory_.GetWeakPtr()));
#endif
  }

#if BUILDFLAG(IS_POSIX)
  void OnAcceptable() {
    const int fd = HANDLE_EINTR(accept(listen_fd_, nullptr, nullptr));
    if (fd < 0) {
      PLOG(WARNING) << "[HeadlessAgent] accept()";
      return;
    }
    // Only the headed controller may own this channel. A second client used
    // to steal the fd and crash the agent; refuse extras.
    if (client_fd_ >= 0) {
      LOG(WARNING) << "[HeadlessAgent] Refusing extra control-socket client.";
      IGNORE_EINTR(close(fd));
      return;
    }
    client_fd_ = fd;
    LOG(INFO) << "[HeadlessAgent] Headed controller connected.";
    WatchClient();
  }

  void WatchClient() {
    if (client_fd_ < 0) {
      return;
    }
    client_watcher_ = base::FileDescriptorWatcher::WatchReadable(
        client_fd_,
        base::BindRepeating(&Impl::OnClientReadable,
                            weak_factory_.GetWeakPtr()));
  }

  void OnClientReadable() {
    if (client_fd_ < 0) {
      return;
    }
    std::string error;
    std::optional<base::Value::Dict> msg =
        ReadFramedJsonMessage(client_fd_, &error);
    if (!msg) {
      LOG(WARNING) << "[HeadlessAgent] read failed: " << error;
      client_watcher_.reset();
      IGNORE_EINTR(close(client_fd_));
      client_fd_ = -1;
      return;
    }
    HandleMessage(std::move(*msg));
  }
#elif BUILDFLAG(IS_WIN)
  void AcceptPipeOnIo(base::WeakPtr<Impl> ui_self) {
    const std::wstring name = socket_path_.value();
    HANDLE pipe = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024,
        64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      LOG(ERROR) << "[HeadlessAgent] CreateNamedPipe failed error="
                 << GetLastError();
      return;
    }
    const BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
      LOG(ERROR) << "[HeadlessAgent] ConnectNamedPipe failed error="
                 << GetLastError();
      CloseHandle(pipe);
      return;
    }
    pipe_.Set(pipe);
    LOG(INFO) << "[HeadlessAgent] Headed controller connected via named pipe.";
    ReadLoopOnIo(std::move(ui_self));
  }

  void ReadLoopOnIo(base::WeakPtr<Impl> ui_self) {
    while (pipe_.is_valid()) {
      std::string error;
      std::optional<base::Value::Dict> msg =
          ReadFramedJsonMessage(pipe_.Get(), &error);
      if (!msg) {
        LOG(WARNING) << "[HeadlessAgent] named-pipe read failed: " << error;
        pipe_.Close();
        return;
      }
      ui_runner_->PostTask(
          FROM_HERE, base::BindOnce(&Impl::HandleMessage, ui_self,
                                    std::move(*msg)));
    }
  }

  void WriteOnIo(base::Value::Dict msg) {
    if (!pipe_.is_valid()) {
      return;
    }
    std::string error;
    if (!WriteFramedJsonMessage(pipe_.Get(), msg, &error)) {
      LOG(WARNING) << "[HeadlessAgent] named-pipe write failed: " << error;
    }
  }
#endif

  void HandleMessage(base::Value::Dict msg) {
    const std::string type =
        msg.FindString("type") ? *msg.FindString("type") : std::string();
    const std::string request_id = msg.FindString("request_id")
                                       ? *msg.FindString("request_id")
                                       : std::string();
    const std::string script_id =
        msg.FindString("script_id") ? *msg.FindString("script_id")
                                    : std::string();

    // Refresh scripts from disk so headed edits are visible. LoadAll is
    // MayBlock — keep a snapshot refreshed off-thread; use last known scripts
    // for this request to avoid UI-thread DCHECK aborts.
    EnsureBrowser();

    if (type == "ping" || type == "warm") {
      ReplyOk(request_id, base::Value::Dict());
      // Kick a background refresh for subsequent commands.
      ScheduleScriptRefresh();
      return;
    }
    if (type == "start_listener") {
      ScheduleScriptRefresh();
      std::string error;
      const bool ok =
          listener_manager_ &&
          listener_manager_->StartListener(script_id, &error);
      if (!ok) {
        ReplyError(request_id, error.empty() ? "StartListener failed." : error);
        return;
      }
      base::Value::Dict payload;
      payload.Set("mode", "headless");
      payload.Set("script_id", script_id);
      ReplyOk(request_id, std::move(payload));
      return;
    }
    if (type == "stop_listener") {
      std::string error;
      const bool ok =
          listener_manager_ &&
          listener_manager_->StopListener(script_id, &error);
      if (!ok) {
        ReplyError(request_id, error.empty() ? "StopListener failed." : error);
        return;
      }
      ReplyOk(request_id, base::Value::Dict());
      return;
    }
    if (type == "list_active") {
      base::Value::List listeners;
      if (listener_manager_) {
        for (const auto& info : listener_manager_->GetActiveListeners()) {
          base::Value::Dict d;
          d.Set("script_id", info.script_id);
          d.Set("script_name", info.script_name);
          d.Set("is_active", info.is_active);
          d.Set("status", info.status);
          d.Set("status_detail", info.status_detail);
          d.Set("current_url", info.current_url);
          d.Set("mode", "headless");
          listeners.Append(std::move(d));
        }
      }
      base::Value::Dict payload;
      payload.Set("listeners", std::move(listeners));
      ReplyOk(request_id, std::move(payload));
      return;
    }
    if (type == "clear_pending_triggers") {
      // Triggers are pushed live; nothing queued in-agent. Ack for API parity.
      ReplyOk(request_id, base::Value::Dict());
      return;
    }
    if (type == "investigate_page") {
      StartInvestigate(request_id, std::move(msg));
      return;
    }
    if (type == "execute_actions") {
      StartExecuteActions(request_id, std::move(msg));
      return;
    }
    if (type == "run_script") {
      StartRunScript(request_id, std::move(msg));
      return;
    }
    if (type == "resume_from_step") {
      StartResumeFromStep(request_id, std::move(msg));
      return;
    }
    if (type == "refresh") {
      HandleRefresh(request_id, script_id);
      return;
    }
    if (type == "capture_screenshot") {
      HandleCaptureScreenshot(request_id, script_id);
      return;
    }
    ReplyError(request_id, "Unknown command: " + type);
  }

  content::WebContents* ResolveRetainedTarget(const std::string& want_tid) {
    Browser* browser = EnsureBrowser();
    if (!browser || want_tid.empty()) {
      return nullptr;
    }
    if (want_tid == retained_target_id_ && retained_investigate_wc_) {
      TabStripModel* tabs = browser->tab_strip_model();
      if (tabs &&
          tabs->GetIndexOfWebContents(retained_investigate_wc_) !=
              TabStripModel::kNoTab) {
        return retained_investigate_wc_;
      }
      retained_investigate_wc_ = nullptr;
      retained_target_id_.clear();
    }
    return nullptr;
  }

  content::WebContents* EnsureInvestigationTarget(std::string* target_id_inout,
                                                  std::string* error) {
    Browser* browser = EnsureBrowser();
    if (!browser || !browser->tab_strip_model()) {
      if (error) {
        *error = "No browser available.";
      }
      return nullptr;
    }
    std::string tid = target_id_inout ? *target_id_inout : std::string();
    content::WebContents* wc = ResolveRetainedTarget(tid);
    if (wc) {
      if (target_id_inout) {
        *target_id_inout = retained_target_id_;
      }
      return wc;
    }
    NavigateParams np(browser, GURL("about:blank"),
                      ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    np.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
    Navigate(&np);
    wc = np.navigated_or_inserted_contents;
    if (!wc) {
      if (error) {
        *error = "Failed to create investigation tab.";
      }
      return nullptr;
    }
    if (tid.empty()) {
      tid = base::Uuid::GenerateRandomV4().AsLowercaseString();
    }
    retained_investigate_wc_ = wc;
    retained_target_id_ = tid;
    if (target_id_inout) {
      *target_id_inout = tid;
    }
    return wc;
  }

  bool AgentOpBusy() const {
    return investigate_ || execute_busy_ || run_script_busy_ || resume_op_;
  }

  void StartExecuteActions(std::string request_id, base::Value::Dict msg) {
    if (AgentOpBusy()) {
      pending_agent_ops_.emplace_back("execute_actions", std::move(request_id),
                                      std::move(msg));
      return;
    }
    const base::Value::List* actions = msg.FindList("actions");
    if (!actions || actions->empty()) {
      ReplyError(request_id, "execute_actions requires a non-empty actions list.");
      return;
    }
    std::string tid = FindStringAny(msg, {"target_id", "targetId"});
    std::string error;
    content::WebContents* wc = EnsureInvestigationTarget(&tid, &error);
    if (!wc) {
      ReplyError(request_id, error.empty() ? "No investigation target." : error);
      return;
    }
    const bool return_context =
        FindBoolAny(msg, {"return_context", "returnContext"}, true);
    execute_busy_ = true;
    const std::string run_id = StartNativeAdHocRun(
        profile_, *actions, wc,
        base::BindOnce(&Impl::OnExecuteActionsDone, weak_factory_.GetWeakPtr(),
                       request_id, tid, return_context));
    if (run_id.empty()) {
      execute_busy_ = false;
      ReplyError(request_id, "Failed to start execute_actions.");
      DrainPendingAgentOps();
    }
  }

  void OnExecuteActionsDone(std::string request_id,
                            std::string target_id,
                            bool return_context,
                            bool success,
                            int failed_step,
                            std::string message) {
    execute_busy_ = false;
    base::Value::Dict payload;
    payload.Set("command", "execute_actions");
    payload.Set("target_id", target_id);
    payload.Set("actions_ok", success);
    if (!success) {
      payload.Set("actions_error", message);
      payload.Set("actions_failed_step", failed_step);
    }
    content::WebContents* wc = ResolveRetainedTarget(target_id);
    if (wc) {
      payload.Set("url", wc->GetLastCommittedURL().spec());
      payload.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
    }
    if (return_context && wc) {
      CaptureSlimContextThenReply(std::move(request_id), std::move(payload), wc,
                                  success);
      return;
    }
    if (success) {
      ReplyOk(request_id, std::move(payload));
    } else {
      payload.Set("error", message.empty() ? "execute_actions failed." : message);
      Send(MakeIpcErrorResponse(request_id, payload.FindString("error")
                                                ? *payload.FindString("error")
                                                : "execute_actions failed."));
    }
    DrainPendingAgentOps();
  }

  void StartRunScript(std::string request_id, base::Value::Dict msg) {
    if (AgentOpBusy()) {
      pending_agent_ops_.emplace_back("run_script", std::move(request_id),
                                      std::move(msg));
      return;
    }
    // resume_from_step may arrive folded into run_script.
    if (msg.Find("resume_from_step") || msg.Find("resumeFromStep") ||
        msg.Find("step_index") || msg.Find("stepIndex") || msg.Find("step")) {
      if (msg.Find("resume_from_step") || msg.Find("resumeFromStep") ||
          msg.FindBool("resume").value_or(false)) {
        StartResumeFromStep(std::move(request_id), std::move(msg));
        return;
      }
    }
    const std::string script_id =
        FindStringAny(msg, {"script_id", "scriptId", "id"});
    if (script_id.empty()) {
      ReplyError(request_id, "run_script requires script_id.");
      return;
    }
    if (!dashboard_) {
      ReplyError(request_id, "Dashboard unavailable.");
      return;
    }
    std::string error;
    base::Value::List timeline =
        LoadExpandedScriptTimeline(dashboard_.get(), script_id, &error);
    if (timeline.empty()) {
      ReplyError(request_id, error.empty() ? "Script empty." : error);
      return;
    }
    std::string tid = FindStringAny(msg, {"target_id", "targetId"});
    content::WebContents* wc = EnsureInvestigationTarget(&tid, &error);
    if (!wc) {
      ReplyError(request_id, error.empty() ? "No investigation target." : error);
      return;
    }
    const bool return_context =
        FindBoolAny(msg, {"return_context", "returnContext"}, true);
    run_script_busy_ = true;
    const std::string run_id = StartNativeScriptRun(
        profile_, script_id, timeline, wc,
        base::BindOnce(&Impl::OnRunScriptDone, weak_factory_.GetWeakPtr(),
                       request_id, tid, script_id, return_context));
    if (run_id.empty()) {
      run_script_busy_ = false;
      ReplyError(request_id, "Failed to start run_script.");
      DrainPendingAgentOps();
    }
  }

  void OnRunScriptDone(std::string request_id,
                       std::string target_id,
                       std::string script_id,
                       bool return_context,
                       bool success,
                       int failed_step,
                       std::string message) {
    run_script_busy_ = false;
    base::Value::Dict payload;
    payload.Set("command", "run_script");
    payload.Set("target_id", target_id);
    payload.Set("script_id", script_id);
    payload.Set("ok", success);
    if (!success) {
      payload.Set("error", message);
      payload.Set("failed_step", failed_step);
    }
    content::WebContents* wc = ResolveRetainedTarget(target_id);
    if (wc) {
      payload.Set("url", wc->GetLastCommittedURL().spec());
      payload.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
    }
    if (return_context && wc) {
      CaptureSlimContextThenReply(std::move(request_id), std::move(payload), wc,
                                  success);
      return;
    }
    if (success) {
      ReplyOk(request_id, std::move(payload));
    } else {
      ReplyError(request_id, message.empty() ? "run_script failed." : message);
    }
    DrainPendingAgentOps();
  }

  void StartResumeFromStep(std::string request_id, base::Value::Dict msg) {
    if (AgentOpBusy()) {
      pending_agent_ops_.emplace_back("resume_from_step", std::move(request_id),
                                      std::move(msg));
      return;
    }
    const std::string script_id =
        FindStringAny(msg, {"script_id", "scriptId", "id"});
    if (script_id.empty()) {
      ReplyError(request_id, "resume_from_step requires script_id.");
      return;
    }
    int step_index = FindIntAny(msg, {"step_index", "stepIndex", "step",
                                      "resume_from_step", "resumeFromStep"},
                                0);
    std::string tid = FindStringAny(msg, {"target_id", "targetId"});
    std::string error;
    content::WebContents* wc = EnsureInvestigationTarget(&tid, &error);
    if (!wc) {
      ReplyError(request_id, error.empty() ? "No investigation target." : error);
      return;
    }
    const bool return_context =
        FindBoolAny(msg, {"return_context", "returnContext"}, true);
    auto flow = std::make_unique<ResumeFromStepWithKeyFallback>(
        profile_, dashboard_.get(), script_id, step_index, wc,
        base::BindOnce(&Impl::OnResumeFromStepDone, weak_factory_.GetWeakPtr(),
                       request_id, tid, script_id, step_index, return_context));
    if (!flow->Start(&error)) {
      ReplyError(request_id, error.empty() ? "resume_from_step failed to start."
                                           : error);
      return;
    }
    resume_op_ = std::move(flow);
  }

  void OnResumeFromStepDone(std::string request_id,
                            std::string target_id,
                            std::string script_id,
                            int step_index,
                            bool return_context,
                            bool success,
                            int failed_step,
                            std::string message) {
    resume_op_.reset();
    base::Value::Dict payload;
    payload.Set("command", "resume_from_step");
    payload.Set("target_id", target_id);
    payload.Set("script_id", script_id);
    payload.Set("step_index", step_index);
    payload.Set("ok", success);
    if (!success) {
      payload.Set("error", message);
      payload.Set("failed_step", failed_step);
    }
    content::WebContents* wc = ResolveRetainedTarget(target_id);
    if (wc) {
      payload.Set("url", wc->GetLastCommittedURL().spec());
      payload.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
    }
    if (return_context && wc) {
      CaptureSlimContextThenReply(std::move(request_id), std::move(payload), wc,
                                  success);
      return;
    }
    if (success) {
      ReplyOk(request_id, std::move(payload));
    } else {
      ReplyError(request_id,
                 message.empty() ? "resume_from_step failed." : message);
    }
    DrainPendingAgentOps();
  }

  void CaptureSlimContextThenReply(std::string request_id,
                                    base::Value::Dict payload,
                                    content::WebContents* wc,
                                    bool overall_ok) {
    FetchAgentPageMap(
        wc, base::BindOnce(&Impl::OnSlimPageMap, weak_factory_.GetWeakPtr(),
                           std::move(request_id), std::move(payload), overall_ok,
                           wc->GetWeakPtr()));
  }

  void OnSlimPageMap(std::string request_id,
                     base::Value::Dict payload,
                     bool overall_ok,
                     base::WeakPtr<content::WebContents> web_contents,
                     UnifiedPageMapResult result) {
    base::Value snap;
    if (result.ok) {
      snap = base::Value(InvestigationSnapshotFromPageMap(
          result.page_map, web_contents.get(), 16, 16, 16, 1400));
    } else {
      base::Value::Dict error_snap;
      error_snap.Set("contextError", result.error);
      snap = base::Value(std::move(error_snap));
    }
    OnSlimContext(std::move(request_id), std::move(payload), overall_ok,
                  std::move(snap));
  }

  void OnSlimContext(std::string request_id,
                       base::Value::Dict payload,
                       bool overall_ok,
                       base::Value result) {
    if (result.is_dict()) {
      const base::Value::Dict& snap = result.GetDict();
      if (const std::string* page_url = snap.FindString("pageUrl")) {
        payload.Set("url", *page_url);
      }
      if (const std::string* title = snap.FindString("title")) {
        payload.Set("title", *title);
      }
      if (const std::string* rs = snap.FindString("readyState")) {
        payload.Set("ready_state", *rs);
      }
      if (const std::string* excerpt = snap.FindString("innerTextExcerpt")) {
        payload.Set("inner_text_excerpt", *excerpt);
      }
      if (const base::Value::List* v = snap.FindList("links")) {
        payload.Set("links", v->Clone());
      }
      if (const base::Value::List* v = snap.FindList("inputs")) {
        payload.Set("inputs", v->Clone());
      }
      if (const base::Value::List* v = snap.FindList("buttons")) {
        payload.Set("buttons", v->Clone());
      }
      if (const base::Value::List* v = snap.FindList("tables")) {
        payload.Set("tables", v->Clone());
      }
      if (std::optional<int> count = snap.FindInt("elementCount")) {
        payload.Set("element_count", *count);
      }
      if (const std::string* source = snap.FindString("source")) {
        payload.Set("context_source", *source);
      }
      if (const std::string* context_error = snap.FindString("contextError")) {
        payload.Set("context_error", *context_error);
      }
    }
    payload.Set("context_status", "FULL");
    if (overall_ok) {
      ReplyOk(request_id, std::move(payload));
    } else {
      const std::string* err = payload.FindString("error");
      ReplyError(request_id, err ? *err : "operation failed.");
    }
    DrainPendingAgentOps();
  }

  void DrainPendingAgentOps() {
    if (AgentOpBusy() || pending_agent_ops_.empty()) {
      return;
    }
    auto [type, next_id, next_msg] = std::move(pending_agent_ops_.front());
    pending_agent_ops_.pop_front();
    if (type == "investigate_page") {
      StartInvestigate(std::move(next_id), std::move(next_msg));
    } else if (type == "execute_actions") {
      StartExecuteActions(std::move(next_id), std::move(next_msg));
    } else if (type == "run_script") {
      StartRunScript(std::move(next_id), std::move(next_msg));
    } else if (type == "resume_from_step") {
      StartResumeFromStep(std::move(next_id), std::move(next_msg));
    }
  }

  void StartInvestigate(std::string request_id, base::Value::Dict msg) {
    if (AgentOpBusy()) {
      pending_agent_ops_.emplace_back("investigate_page", std::move(request_id),
                                      std::move(msg));
      return;
    }
    if (investigate_) {
      pending_agent_ops_.emplace_back("investigate_page", std::move(request_id),
                                      std::move(msg));
      return;
    }
    Browser* browser = EnsureBrowser();
    if (!browser) {
      ReplyError(request_id, "No browser available for investigation.");
      return;
    }
    content::WebContents* reuse_wc = nullptr;
    std::string want_tid;
    if (const std::string* tid = msg.FindString("target_id")) {
      want_tid = *tid;
    } else if (const std::string* camel_tid = msg.FindString("targetId")) {
      want_tid = *camel_tid;
    }
    if (!want_tid.empty() && want_tid == retained_target_id_ &&
        retained_investigate_wc_) {
      // Confirm the tab is still in the strip.
      TabStripModel* tabs = browser->tab_strip_model();
      if (tabs &&
          tabs->GetIndexOfWebContents(retained_investigate_wc_) !=
              TabStripModel::kNoTab) {
        reuse_wc = retained_investigate_wc_;
      } else {
        retained_investigate_wc_ = nullptr;
        retained_target_id_.clear();
      }
    }
    investigate_ = std::make_unique<InvestigateSession>(
        profile_, dashboard_.get(), browser, std::move(msg),
        base::BindOnce(&Impl::OnInvestigateFinished, weak_factory_.GetWeakPtr(),
                       request_id));
    investigate_->Start(reuse_wc);
  }

  void OnInvestigateFinished(std::string request_id,
                             bool ok,
                             base::Value::Dict payload) {
    content::WebContents* keep_wc = nullptr;
    std::string keep_tid;
    bool close = true;
    if (investigate_) {
      close = investigate_->close_target();
      keep_tid = investigate_->target_id();
      keep_wc = investigate_->ReleaseKeptWebContents();
    }
    investigate_.reset();
    if (!close && keep_wc) {
      retained_investigate_wc_ = keep_wc;
      retained_target_id_ = keep_tid;
      payload.Set("target_id", keep_tid);
      payload.Set("reused_target", true);
    } else {
      retained_investigate_wc_ = nullptr;
      retained_target_id_.clear();
    }
    if (ok) {
      ReplyOk(request_id, std::move(payload));
    } else {
      const std::string error = payload.FindString("error")
                                    ? *payload.FindString("error")
                                    : "investigate_page failed.";
      Send(MakeIpcErrorResponse(request_id, error));
    }
    DrainPendingAgentOps();
  }

  void HandleRefresh(const std::string& request_id,
                     const std::string& script_id) {
    content::WebContents* wc =
        listener_manager_ ? listener_manager_->GetListenerWebContents(script_id)
                          : nullptr;
    if (!wc) {
      ReplyError(request_id, "Listener is not active in the headless process.");
      return;
    }
    wc->GetController().Reload(content::ReloadType::NORMAL,
                               /*check_for_repost=*/false);
    base::Value::Dict payload;
    payload.Set("script_id", script_id);
    payload.Set("mode", "headless");
    payload.Set("current_url", wc->GetLastCommittedURL().spec());
    ReplyOk(request_id, std::move(payload));
  }

  void HandleCaptureScreenshot(const std::string& request_id,
                               const std::string& script_id) {
    content::WebContents* wc =
        listener_manager_ ? listener_manager_->GetListenerWebContents(script_id)
                          : nullptr;
    if (!wc) {
      ReplyError(request_id, "Listener is not active in the headless process.");
      return;
    }
    const std::string current_url = wc->GetLastCommittedURL().spec();
    dashboard::DashboardDevToolsClient::CapturePngBase64(
        wc, base::BindOnce(&Impl::OnListenerScreenshot,
                           weak_factory_.GetWeakPtr(), request_id, script_id,
                           current_url));
  }

  void OnListenerScreenshot(std::string request_id,
                            std::string script_id,
                            std::string current_url,
                            bool ok,
                            std::string error,
                            std::string image_base64) {
    if (!ok || image_base64.empty()) {
      ReplyError(request_id, error.empty() ? "Captured empty screenshot."
                                           : error);
      return;
    }
    base::Value::Dict payload;
    payload.Set("script_id", script_id);
    payload.Set("success", true);
    payload.Set("image_base64", image_base64);
    payload.Set("current_url", current_url);
    payload.Set("ts_ms", static_cast<double>(
                             base::Time::Now().InMillisecondsSinceUnixEpoch()));
    ReplyOk(request_id, std::move(payload));
  }

  void ReplyOk(const std::string& request_id, base::Value::Dict payload) {
    Send(MakeIpcOkResponse(request_id, std::move(payload)));
  }

  void ReplyError(const std::string& request_id, const std::string& error) {
    Send(MakeIpcErrorResponse(request_id, error));
  }

  void Send(base::Value::Dict msg) {
#if BUILDFLAG(IS_POSIX)
    if (client_fd_ < 0) {
      return;
    }
    std::string error;
    if (!WriteFramedJsonMessage(client_fd_, msg, &error)) {
      LOG(WARNING) << "[HeadlessAgent] write failed: " << error;
    }
#elif BUILDFLAG(IS_WIN)
    if (!io_runner_) {
      return;
    }
    io_runner_->PostTask(FROM_HERE, base::BindOnce(&Impl::WriteOnIo,
                                                   base::Unretained(this),
                                                   std::move(msg)));
#endif
  }

  void ScheduleScriptRefresh() {
    if (!profile_ || !dashboard_ || refresh_in_flight_) {
      return;
    }
    refresh_in_flight_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
         base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::BindOnce(&ScriptStore::LoadAll, profile_),
        base::BindOnce(
            [](base::WeakPtr<Impl> self, base::Value::Dict scripts) {
              if (!self) {
                return;
              }
              self->refresh_in_flight_ = false;
              if (self->dashboard_) {
                self->dashboard_->ReplaceAllScripts(std::move(scripts));
              }
            },
            weak_factory_.GetWeakPtr()));
  }

  void OnRemoteTrigger(
      const std::string& listener_script_id,
      const std::string& trigger_script_id,
      const base::flat_map<std::string, std::string>& token_map,
      const std::string& preferred_start_url,
      const std::string& detail) {
    base::Value::Dict event;
    event.Set("type", "trigger");
    event.Set("action", "trigger");
    event.Set("listener_script_id", listener_script_id);
    event.Set("trigger_script_id", trigger_script_id);
    if (!preferred_start_url.empty()) {
      event.Set("match_url", preferred_start_url);
    }
    if (!detail.empty()) {
      event.Set("detail", detail);
    }
    base::Value::Dict tokens;
    for (const auto& [k, v] : token_map) {
      tokens.Set(k, v);
      if (k == "listener.match.url" && preferred_start_url.empty() &&
          !v.empty()) {
        event.Set("match_url", v);
      }
      if (k == "listener.match.value" || k == "listener.match.text") {
        event.Set("matchValue", v);
        event.Set("text", v);
      }
    }
    event.Set("tokens", std::move(tokens));
    event.Set("ts_ms", static_cast<double>(
                           base::Time::Now().InMillisecondsSinceUnixEpoch()));
    Send(std::move(event));
  }

  raw_ptr<Profile> profile_ = nullptr;
  base::FilePath socket_path_;
  std::unique_ptr<Dashboard> dashboard_;
  std::unique_ptr<ListenerManager> listener_manager_;
#if BUILDFLAG(IS_POSIX)
  int listen_fd_ = -1;
  int client_fd_ = -1;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> accept_watcher_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> client_watcher_;
#elif BUILDFLAG(IS_WIN)
  base::win::ScopedHandle pipe_;
  scoped_refptr<base::SequencedTaskRunner> io_runner_;
  scoped_refptr<base::SequencedTaskRunner> ui_runner_;
#endif
  bool started_ = false;
  bool start_requested_ = false;
  bool refresh_in_flight_ = false;
  std::unique_ptr<InvestigateSession> investigate_;
  bool execute_busy_ = false;
  bool run_script_busy_ = false;
  std::unique_ptr<ResumeFromStepWithKeyFallback> resume_op_;
  std::deque<std::tuple<std::string, std::string, base::Value::Dict>>
      pending_agent_ops_;
  // Retained when close_target=false so follow-up investigate_page calls can
  // reuse the same headless tab (target_id).
  raw_ptr<content::WebContents> retained_investigate_wc_ = nullptr;
  std::string retained_target_id_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

#else  // !BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_WIN)

class HeadlessListenerAgent::Impl {
 public:
  void MaybeStartForProfile(Profile*) {}
  void Shutdown() {}
};

#endif  // BUILDFLAG(IS_POSIX) || BUILDFLAG(IS_WIN)

HeadlessListenerAgent& HeadlessListenerAgent::GetInstance() {
  static base::NoDestructor<HeadlessListenerAgent> instance;
  return *instance;
}

HeadlessListenerAgent::HeadlessListenerAgent()
    : impl_(std::make_unique<Impl>()) {}

HeadlessListenerAgent::~HeadlessListenerAgent() = default;

void HeadlessListenerAgent::MaybeStartForProfile(Profile* profile) {
  impl_->MaybeStartForProfile(profile);
}

void HeadlessListenerAgent::Shutdown() {
  impl_->Shutdown();
}

}  // namespace dashboard
