// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/headless_listener_agent.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
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
#include "chrome/browser/ui/webui/dashboard/headless_listener_ipc.h"
#include "chrome/browser/ui/webui/dashboard/listener_manager.h"
#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/common/chrome_switches.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
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

std::string BuildInvestigationSnapshotJs(int max_links,
                                         int max_inputs,
                                         int max_buttons,
                                         int max_text_chars) {
  base::Value::Dict cfg;
  cfg.Set("maxLinks", std::max(1, max_links));
  cfg.Set("maxInputs", std::max(1, max_inputs));
  cfg.Set("maxButtons", std::max(1, max_buttons));
  cfg.Set("maxTextChars", std::max(200, max_text_chars));
  std::string cfg_json;
  if (!base::JSONWriter::Write(cfg, &cfg_json)) {
    cfg_json = "{\"maxLinks\":16,\"maxInputs\":16,\"maxButtons\":16,"
               "\"maxTextChars\":1400}";
  }
  return "(function(){"
         "const cfg=" +
         cfg_json +
         ";"
         "const norm=(s)=>String(s||'').replace(/\\s+/g,' ').trim();"
         "const clip=(s,n)=>{const t=norm(s);return t.length>n?t.slice(0,n):t;};"
         "const links=[];"
         "try{const list=Array.from(document.querySelectorAll('a[href]'));"
         "for(let i=0;i<list.length&&links.length<cfg.maxLinks;i++){"
         "const el=list[i];"
         "const href=String(el.getAttribute('href')||el.href||'').trim();"
         "if(!href)continue;"
         "links.push({href,text:clip(el.innerText||el.textContent||'',140)});}"
         "}catch(e){}"
         "const inputs=[];"
         "try{const list=Array.from(document.querySelectorAll("
         "'input,textarea,select'));"
         "for(let i=0;i<list.length&&inputs.length<cfg.maxInputs;i++){"
         "const el=list[i];"
         "inputs.push({tag:String(el.tagName||'').toLowerCase(),"
         "type:String(el.getAttribute&&el.getAttribute('type')||''),"
         "name:String(el.getAttribute&&el.getAttribute('name')||''),"
         "id:String(el.getAttribute&&el.getAttribute('id')||''),"
         "placeholder:String(el.getAttribute&&el.getAttribute('placeholder')"
         "||''),"
         "aria:String(el.getAttribute&&el.getAttribute('aria-label')||'')});}"
         "}catch(e){}"
         "const buttons=[];"
         "try{const list=Array.from(document.querySelectorAll("
         "'button,input[type=\"submit\"],input[type=\"button\"],"
         "[role=\"button\"]'));"
         "for(let i=0;i<list.length&&buttons.length<cfg.maxButtons;i++){"
         "const el=list[i];"
         "buttons.push({text:clip(el.innerText||el.textContent||"
         "el.getAttribute('value')||'',140),"
         "id:String(el.getAttribute&&el.getAttribute('id')||''),"
         "className:String(el.getAttribute&&el.getAttribute('class')||'')});}"
         "}catch(e){}"
         "let elementCount=0;"
         "try{elementCount=Number(document.getElementsByTagName('*').length||0);}"
         "catch(e){}"
         "const title=String(document.title||'');"
         "const inner=clip((document.body&&(document.body.innerText||"
         "document.body.textContent))||'',cfg.maxTextChars);"
         "return {pageUrl:String(location&&location.href||''),title,"
         "readyState:String(document.readyState||''),elementCount,links,"
         "inputs,buttons,innerTextExcerpt:inner};"
         "})()";
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
                     Browser* browser,
                     base::Value::Dict request,
                     FinishCallback done)
      : profile_(profile),
        browser_(browser),
        request_(std::move(request)),
        done_(std::move(done)) {
    start_url_ = FindStringAny(request_, {"url", "start_url", "startUrl"});
    if (start_url_.empty()) {
      start_url_ = "about:blank";
    }
    close_target_ =
        FindBoolAny(request_, {"close_target", "closeTarget"}, true);
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
    if (const base::Value::Dict* probe = request_.FindDict("probe")) {
      probe_ = probe->Clone();
    }
    target_id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
  }

  ~InvestigateSession() override { load_timer_.Stop(); settle_timer_.Stop(); }

  void Start() {
    if (!profile_ || !browser_ || !browser_->tab_strip_model()) {
      FinishError("No browser available for investigation.");
      return;
    }
    const GURL url = GURL(start_url_).is_valid() ? GURL(start_url_)
                                                 : GURL("about:blank");
    NavigateParams np(browser_, url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    np.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
    Navigate(&np);
    content::WebContents* wc = np.navigated_or_inserted_contents;
    if (!wc) {
      FinishError("Failed to create investigation tab.");
      return;
    }
    Observe(wc);
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
    content::RenderFrameHost* rfh = wc->GetPrimaryMainFrame();
    if (!rfh) {
      FinishError("No render frame for investigation snapshot.");
      return;
    }
    rfh->ExecuteJavaScript(
        base::UTF8ToUTF16(BuildInvestigationSnapshotJs(
            max_links_, max_inputs_, max_buttons_, max_text_chars_)),
        base::BindOnce(&InvestigateSession::OnSnapshotJs,
                       weak_factory_.GetWeakPtr()));
  }

  void OnSnapshotJs(base::Value result) {
    if (finished_) {
      return;
    }
    ApplySnapshot(std::move(result));
    RunProbe();
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
    }
    payload_.Set("implemented", true);
    payload_.Set("command", "investigate_page");
    payload_.Set("target_id", target_id_);
    payload_.Set("url", url);
    payload_.Set("title", title);
    payload_.Set("ready_state", ready_state);
    payload_.Set("element_count", element_count);
    payload_.Set("inner_text_excerpt", inner_text);
    payload_.Set("links", std::move(links));
    payload_.Set("inputs", std::move(inputs));
    payload_.Set("buttons", std::move(buttons));
  }

  void RunProbe() {
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
    extractor_ = std::make_unique<PageMapExtractor>();
    extractor_->Run(web_contents(),
                    base::BindOnce(&InvestigateSession::OnPageMap,
                                   weak_factory_.GetWeakPtr()),
                    /*greedy=*/false, /*is_agent_mode=*/true,
                    /*timeout_ms=*/4000);
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
      const std::vector<Locator> converted = ConvertLocators(*locators);
      const std::vector<NativePageMapCandidate> matches =
          FindMatchingPageMapCandidates(page_map, converted, 8);
      base::Value::Dict result;
      result.Set("found", !matches.empty());
      result.Set("match_count", static_cast<int>(matches.size()));
      if (!matches.empty()) {
        result.Set("tag", matches[0].tag);
        result.Set("text", matches[0].text);
        result.Set("visible", matches[0].visible);
        result.Set("clickable", matches[0].clickable);
        result.Set("css_selector", matches[0].css_selector);
        result.Set("accessibility_name", matches[0].accessibility_name);
        result.Set("matchValue", matches[0].text);
        base::Value::List all;
        for (const NativePageMapCandidate& match : matches) {
          all.Append(CandidateToProbeDict(match));
        }
        result.Set("matches", std::move(all));
      }
      probe_out.Set("result", std::move(result));
    }
    payload_.Set("locator_probe", std::move(probe_out));
    extractor_.reset();
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
    if (!close_target_ || !browser_ || !web_contents()) {
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
  raw_ptr<Browser> browser_ = nullptr;
  base::Value::Dict request_;
  base::Value::Dict payload_;
  base::Value::Dict probe_;
  base::Value::List actions_;
  FinishCallback done_;
  std::string start_url_;
  std::string target_id_;
  bool close_target_ = true;
  bool include_screenshot_ = false;
  bool include_image_base64_ = false;
  bool finished_ = false;
  bool load_seen_ = false;
  int max_links_ = 16;
  int max_inputs_ = 16;
  int max_buttons_ = 16;
  int max_text_chars_ = 1400;
  int settle_ms_ = 500;
  std::unique_ptr<PageMapExtractor> extractor_;
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
    if (client_fd_ >= 0) {
      IGNORE_EINTR(close(client_fd_));
      client_watcher_.reset();
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

  void StartInvestigate(std::string request_id, base::Value::Dict msg) {
    if (investigate_) {
      pending_investigates_.emplace_back(std::move(request_id), std::move(msg));
      return;
    }
    Browser* browser = EnsureBrowser();
    if (!browser) {
      ReplyError(request_id, "No browser available for investigation.");
      return;
    }
    investigate_ = std::make_unique<InvestigateSession>(
        profile_, browser, std::move(msg),
        base::BindOnce(&Impl::OnInvestigateFinished, weak_factory_.GetWeakPtr(),
                       request_id));
    investigate_->Start();
  }

  void OnInvestigateFinished(std::string request_id,
                             bool ok,
                             base::Value::Dict payload) {
    investigate_.reset();
    if (ok) {
      ReplyOk(request_id, std::move(payload));
    } else {
      const std::string error = payload.FindString("error")
                                    ? *payload.FindString("error")
                                    : "investigate_page failed.";
      Send(MakeIpcErrorResponse(request_id, error));
    }
    if (!pending_investigates_.empty()) {
      auto [next_id, next_msg] = std::move(pending_investigates_.front());
      pending_investigates_.pop_front();
      StartInvestigate(std::move(next_id), std::move(next_msg));
    }
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
    content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
    if (!view || !view->IsSurfaceAvailableForCopy()) {
      ReplyError(request_id, "Listener surface unavailable for screenshot.");
      return;
    }
    const std::string current_url = wc->GetLastCommittedURL().spec();
    view->CopyFromSurface(
        gfx::Rect(), gfx::Size(),
        base::BindOnce(&Impl::OnListenerScreenshot, weak_factory_.GetWeakPtr(),
                       request_id, script_id, current_url));
  }

  void OnListenerScreenshot(std::string request_id,
                            std::string script_id,
                            std::string current_url,
                            const viz::CopyOutputBitmapWithMetadata& result) {
    if (result.bitmap.drawsNothing()) {
      ReplyError(request_id, "Captured empty screenshot.");
      return;
    }
    std::optional<std::vector<uint8_t>> png =
        gfx::PNGCodec::EncodeBGRASkBitmap(result.bitmap,
                                          /*discard_transparency=*/false);
    if (!png || png->empty()) {
      ReplyError(request_id, "Failed to encode screenshot.");
      return;
    }
    const std::string image_base64 =
        base::Base64Encode(base::span<const uint8_t>(*png));
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
  std::deque<std::pair<std::string, base::Value::Dict>> pending_investigates_;
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
