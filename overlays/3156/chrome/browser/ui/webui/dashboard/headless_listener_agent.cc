// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/headless_listener_agent.h"

#include <memory>
#include <string>
#include <utility>

#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <vector>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/flat_map.h"
#include "base/files/file_descriptor_watcher_posix.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/functional/bind.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_ipc.h"
#include "chrome/browser/ui/webui/dashboard/listener_manager.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/common/chrome_switches.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#endif  // BUILDFLAG(IS_POSIX)

namespace dashboard {

#if BUILDFLAG(IS_POSIX)
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
  if (!base::CreateDirectory(out.socket_path.DirName())) {
    out.error = "Failed to create control socket directory.";
    return out;
  }
  base::DeleteFile(out.socket_path);
  out.scripts = ScriptStore::LoadAll(profile);
  out.ok = true;
  return out;
}

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

    EnsureBrowser();
    if (!BindAndListen()) {
      start_requested_ = false;
      return;
    }
    started_ = true;
    LOG(INFO) << "[HeadlessAgent] Listening on " << socket_path_;
    WatchAccept();
  }

  void Shutdown() {
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
    listener_manager_.reset();
    dashboard_.reset();
    started_ = false;
    start_requested_ = false;
  }

 private:
  void EnsureBrowser() {
    if (chrome::FindLastActiveWithProfile(profile_)) {
      return;
    }
    Browser::CreateParams params(profile_, /*user_gesture=*/false);
    params.omit_from_session_restore = true;
    Browser::Create(params);
    LOG(INFO) << "[HeadlessAgent] Created Browser for listener tabs.";
  }

  bool BindAndListen() {
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
  }

  void WatchAccept() {
    if (listen_fd_ < 0) {
      return;
    }
    accept_watcher_ = base::FileDescriptorWatcher::WatchReadable(
        listen_fd_,
        base::BindRepeating(&Impl::OnAcceptable, weak_factory_.GetWeakPtr()));
  }

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
      // Contract placeholder: headed sends {type, request_id, url, actions?,
      // probe?}. Full ScriptExecutionService investigation is the next IPC
      // bite; acknowledge the channel + echo URL so callers can detect ready.
      base::Value::Dict payload;
      payload.Set("implemented", false);
      payload.Set(
          "note",
          "investigate_page IPC accepted; page automation follow-up pending.");
      if (const std::string* url = msg.FindString("url")) {
        payload.Set("url", *url);
      }
      ReplyOk(request_id, std::move(payload));
      return;
    }
    ReplyError(request_id, "Unknown command: " + type);
  }

  void ReplyOk(const std::string& request_id, base::Value::Dict payload) {
    Send(MakeIpcOkResponse(request_id, std::move(payload)));
  }

  void ReplyError(const std::string& request_id, const std::string& error) {
    Send(MakeIpcErrorResponse(request_id, error));
  }

  void Send(base::Value::Dict msg) {
    if (client_fd_ < 0) {
      return;
    }
    std::string error;
    if (!WriteFramedJsonMessage(client_fd_, msg, &error)) {
      LOG(WARNING) << "[HeadlessAgent] write failed: " << error;
    }
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
  int listen_fd_ = -1;
  int client_fd_ = -1;
  bool started_ = false;
  bool start_requested_ = false;
  bool refresh_in_flight_ = false;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> accept_watcher_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> client_watcher_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

#else  // !BUILDFLAG(IS_POSIX)

class HeadlessListenerAgent::Impl {
 public:
  void MaybeStartForProfile(Profile*) {}
  void Shutdown() {}
};

#endif  // BUILDFLAG(IS_POSIX)

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
