// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/headless_listener_controller.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"

#if BUILDFLAG(IS_POSIX)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <tuple>

#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/files/file.h"
#include "base/files/file_descriptor_watcher_posix.h"
#include "base/files/file_util.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_ipc.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/common/chrome_switches.h"
#include "content/public/common/content_switches.h"
#include "ui/gfx/switches.h"
#endif  // BUILDFLAG(IS_POSIX)

namespace dashboard {
namespace {

std::map<Profile*, HeadlessListenerController*>& ControllersByProfile() {
  static base::NoDestructor<std::map<Profile*, HeadlessListenerController*>>
      map;
  return *map;
}

#if BUILDFLAG(IS_POSIX)

base::FilePath SharedHeadlessUserDataDir() {
  if (const char* env = std::getenv("HEADLESS_SHARED_PROFILE"); env && *env) {
    return base::FilePath::FromUTF8Unsafe(env);
  }
  base::FilePath sock = DefaultHeadlessControlSocketPath();
  if (sock.empty()) {
    return base::FilePath();
  }
  return sock.DirName().AppendASCII("headless_shared");
}

struct LaunchChildResult {
  bool ok = false;
  std::string error;
  base::Process process;
};

void ClearStaleProcessSingletonFiles(const base::FilePath& user_data) {
  // Orphaned Singleton* files make the next launch hand off to a dead PID and
  // exit before HeadlessListenerAgent can bind control.sock.
  for (const char* name :
       {"SingletonLock", "SingletonCookie", "SingletonSocket"}) {
    const base::FilePath path = user_data.AppendASCII(name);
    if (base::PathExists(path)) {
      LOG(WARNING) << "[HeadlessController] Clearing stale " << name << " at "
                   << path;
      base::DeleteFile(path);
    }
  }
}

void CopyFileWithSqliteSidecars(const base::FilePath& src,
                                const base::FilePath& dst) {
  if (!base::PathExists(src) || !base::CreateDirectory(dst.DirName())) {
    return;
  }
  std::ignore = base::CopyFile(src, dst);
  for (const char* suffix : {"-wal", "-shm", "-journal"}) {
    const base::FilePath side_src = base::FilePath(src.value() + suffix);
    if (base::PathExists(side_src)) {
      std::ignore =
          base::CopyFile(side_src, base::FilePath(dst.value() + suffix));
    }
  }
}

// Mirror the old Python _warm_sync: copy session state from the headed profile
// so headless listeners resume logged-in sessions.
void WarmSyncHeadlessSessionFromHeadedBlocking(
    const base::FilePath& headed_profile_dir,
    const base::FilePath& headless_user_data) {
  if (headed_profile_dir.empty() || headless_user_data.empty()) {
    return;
  }
  const base::FilePath dst_default =
      headless_user_data.AppendASCII("Default");
  if (!base::CreateDirectory(dst_default) ||
      !base::CreateDirectory(dst_default.AppendASCII("Network"))) {
    LOG(WARNING) << "[HeadlessController] Failed creating Default under "
                 << headless_user_data;
    return;
  }

  CopyFileWithSqliteSidecars(headed_profile_dir.AppendASCII("Cookies"),
                             dst_default.AppendASCII("Cookies"));
  CopyFileWithSqliteSidecars(
      headed_profile_dir.AppendASCII("Network").AppendASCII("Cookies"),
      dst_default.AppendASCII("Network").AppendASCII("Cookies"));

  const base::FilePath src_ls =
      headed_profile_dir.AppendASCII("Local Storage");
  const base::FilePath dst_ls = dst_default.AppendASCII("Local Storage");
  if (base::DirectoryExists(src_ls)) {
    base::DeletePathRecursively(dst_ls);
    std::ignore = base::CopyDirectory(src_ls, dst_ls, /*recursive=*/true);
  }

  const base::FilePath src_nap =
      headed_profile_dir.AppendASCII("Network Action Predictor");
  const base::FilePath dst_nap =
      dst_default.AppendASCII("Network Action Predictor");
  if (base::PathExists(src_nap)) {
    if (base::DirectoryExists(src_nap)) {
      base::DeletePathRecursively(dst_nap);
      std::ignore = base::CopyDirectory(src_nap, dst_nap, /*recursive=*/true);
    } else {
      CopyFileWithSqliteSidecars(src_nap, dst_nap);
    }
  }
  LOG(INFO) << "[HeadlessController] Warm-synced session state from "
            << headed_profile_dir << " -> " << headless_user_data;
}

LaunchChildResult LaunchHeadlessChildBlocking(
    base::FilePath browser,
    base::FilePath socket_path,
    base::FilePath user_data,
    base::FilePath scripts_dir,
    base::FilePath headed_profile_dir) {
  LaunchChildResult out;
  if (socket_path.empty() || user_data.empty()) {
    out.error = "Missing headless paths.";
    return out;
  }
  if (!base::CreateDirectory(socket_path.DirName())) {
    out.error = "Failed to create Headless control directory.";
    return out;
  }
  if (!base::CreateDirectory(user_data)) {
    out.error = "Failed to create shared headless profile dir.";
    return out;
  }
  ClearStaleProcessSingletonFiles(user_data);
  // Stale control socket from a previous crash can make bind() fail.
  base::DeleteFile(socket_path);
  WarmSyncHeadlessSessionFromHeadedBlocking(headed_profile_dir, user_data);

  base::CommandLine cmd(browser);
  cmd.AppendSwitchASCII(switches::kHeadless, "new");
  cmd.AppendSwitch(switches::kHeadlessListener);
  cmd.AppendSwitchASCII(switches::kRemoteDebuggingPort, "9222");
  cmd.AppendSwitchASCII("remote-debugging-address", "127.0.0.1");
  cmd.AppendSwitchASCII("remote-allow-origins", "*");
  cmd.AppendSwitch(switches::kNoFirstRun);
  cmd.AppendSwitch("disable-service-worker");
  cmd.AppendSwitch(switches::kNoStartupWindow);
  cmd.AppendSwitchPath(switches::kUserDataDir, user_data);
  cmd.AppendSwitchASCII("window-size", "1600,2200");
  cmd.AppendArg("about:blank");

  base::LaunchOptions options;
  options.current_directory = browser.DirName();
  options.environment[kHeadlessControlSocketEnv] = socket_path.AsUTF8Unsafe();
  options.environment["DASHBOARD_SCRIPTS_DIR"] = scripts_dir.AsUTF8Unsafe();
  // Avoid a second dock tile for the same Chromium.app when the child runs
  // headless / --no-startup-window (macOS treats siblings as the same app).
  options.environment["LSUIElement"] = "1";

  const base::FilePath log_path =
      socket_path.DirName().AppendASCII("headless_listener_launch.log");
  base::File log_file(
      log_path, base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_APPEND |
                    base::File::FLAG_WRITE);
  if (log_file.IsValid()) {
    options.fds_to_remap.emplace_back(log_file.GetPlatformFile(),
                                      STDERR_FILENO);
  }

  LOG(INFO) << "[HeadlessController] Launching: " << cmd.GetCommandLineString();
  out.process = base::LaunchProcess(cmd, options);
  if (!out.process.IsValid()) {
    out.error = "Failed to launch headless Chromium.";
    return out;
  }
  out.ok = true;
  return out;
}

}  // namespace

class HeadlessListenerController::Impl {
 public:
  explicit Impl(Profile* profile) : profile_(profile) {}

  void SetOwner(HeadlessListenerController* owner) { owner_ = owner; }

  ~Impl() {
    Disconnect();
    if (child_.IsValid()) {
      child_.Terminate(0, /*wait=*/false);
      child_.Close();
    }
  }

  void SetTriggerCallback(TriggerCallback callback) {
    trigger_callback_ = std::move(callback);
  }

  void EnsureReady(base::OnceCallback<void(bool, std::string)> done) {
    if (client_fd_ >= 0) {
      std::move(done).Run(true, std::string());
      return;
    }
    ready_waiters_.push_back(std::move(done));
    if (launch_in_progress_) {
      return;
    }
    launch_in_progress_ = true;

    socket_path_ = DefaultHeadlessControlSocketPath();
    if (socket_path_.empty()) {
      FinishReady(false, "No control socket path.");
      return;
    }

    // Child already live from a prior launch: just reconnect.
    if (child_.IsValid()) {
      connect_attempts_ = 0;
      TryConnect();
      return;
    }

    const base::FilePath browser =
        base::CommandLine::ForCurrentProcess()->GetProgram();
    const base::FilePath user_data = SharedHeadlessUserDataDir();
    const base::FilePath scripts_dir = ScriptStore::ScriptsDir(profile_);
    const base::FilePath headed_profile =
        profile_ ? profile_->GetPath() : base::FilePath();

    // CreateDirectory / LaunchProcess are MayBlock — never on the UI thread
    // (DCHECK_IS_ON / DCHECK_ALWAYS_ON builds abort via AssertBlockingAllowed).
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
         base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::BindOnce(&LaunchHeadlessChildBlocking, browser, socket_path_,
                       user_data, scripts_dir, headed_profile),
        base::BindOnce(&Impl::OnChildLaunchFinished,
                       weak_factory_.GetWeakPtr()));
  }

  void StartListener(const std::string& script_id, ResponseCallback done) {
    SendCommand("start_listener", script_id, std::move(done));
  }

  void StopListener(const std::string& script_id, ResponseCallback done) {
    SendCommand("stop_listener", script_id, std::move(done));
  }

  void ListActive(ResponseCallback done) {
    SendCommand("list_active", std::string(), std::move(done));
  }

  void Warm(ResponseCallback done) {
    SendCommand("warm", std::string(), std::move(done));
  }

  void ClearPendingTriggers(ResponseCallback done) {
    SendCommand("clear_pending_triggers", std::string(), std::move(done));
  }

  void InvestigatePage(base::Value::Dict payload, ResponseCallback done) {
    EnsureReady(base::BindOnce(
        [](base::WeakPtr<Impl> self, base::Value::Dict payload,
           ResponseCallback done, bool ok, std::string error) {
          if (!self) {
            return;
          }
          if (!ok) {
            base::Value::Dict response;
            response.Set("error", error.empty()
                                     ? "Headless control channel not ready."
                                     : error);
            std::move(done).Run(false, std::move(response));
            return;
          }
          self->SendInvestigatePage(std::move(payload), std::move(done));
        },
        weak_factory_.GetWeakPtr(), std::move(payload), std::move(done)));
  }

 private:
  void OnChildLaunchFinished(LaunchChildResult result) {
    if (!result.ok) {
      FinishReady(false, result.error.empty()
                             ? "Failed to launch headless Chromium."
                             : result.error);
      return;
    }
    child_ = std::move(result.process);
    connect_attempts_ = 0;
    TryConnect();
  }

  void TryConnect() {
    // Chromium headless startup + agent bind can take well over 10s on a cold
    // shared profile; allow ~45s before failing the headed start request.
    constexpr int kMaxAttempts = 180;
    if (connect_attempts_++ >= kMaxAttempts) {
      FinishReady(false, "Timed out waiting for headless control socket.");
      return;
    }
    const int fd = HANDLE_EINTR(socket(AF_UNIX, SOCK_STREAM, 0));
    if (fd < 0) {
      ScheduleReconnect();
      return;
    }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    const std::string path = socket_path_.AsUTF8Unsafe();
    if (path.size() >= sizeof(addr.sun_path)) {
      IGNORE_EINTR(close(fd));
      FinishReady(false, "Control socket path too long.");
      return;
    }
    UNSAFE_TODO(memcpy(addr.sun_path, path.data(), path.size()));
    if (HANDLE_EINTR(connect(fd, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr))) != 0) {
      IGNORE_EINTR(close(fd));
      ScheduleReconnect();
      return;
    }
    client_fd_ = fd;
    WatchClient();
    // Handshake.
    SendCommand(
        "warm", std::string(),
        base::BindOnce(
            [](base::WeakPtr<Impl> self, bool ok, base::Value::Dict) {
              if (!self) {
                return;
              }
              self->FinishReady(ok, ok ? std::string()
                                       : "Headless warm handshake failed.");
            },
            weak_factory_.GetWeakPtr()));
  }

  void ScheduleReconnect() {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Impl::TryConnect, weak_factory_.GetWeakPtr()),
        base::Milliseconds(250));
  }

  void FinishReady(bool ok, std::string error) {
    launch_in_progress_ = false;
    std::vector<base::OnceCallback<void(bool, std::string)>> waiters;
    waiters.swap(ready_waiters_);
    for (auto& waiter : waiters) {
      std::move(waiter).Run(ok, error);
    }
  }

  void Disconnect() {
    client_watcher_.reset();
    if (client_fd_ >= 0) {
      IGNORE_EINTR(close(client_fd_));
      client_fd_ = -1;
    }
  }

  void WatchClient() {
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
      LOG(WARNING) << "[HeadlessController] read failed: " << error;
      Disconnect();
      for (auto& [id, cb] : pending_) {
        std::move(cb).Run(false, MakeIpcErrorResponse(id, error));
      }
      pending_.clear();
      return;
    }
    const std::string type =
        msg->FindString("type") ? *msg->FindString("type") : std::string();
    if (type == "trigger" ||
        (msg->FindString("action") &&
         *msg->FindString("action") == "trigger")) {
      if (trigger_callback_) {
        trigger_callback_.Run(std::move(*msg));
      }
      return;
    }
    const std::string request_id = msg->FindString("request_id")
                                       ? *msg->FindString("request_id")
                                       : std::string();
    auto it = pending_.find(request_id);
    if (it == pending_.end()) {
      return;
    }
    ResponseCallback cb = std::move(it->second);
    pending_.erase(it);
    const bool ok = msg->FindBool("ok").value_or(false);
    std::move(cb).Run(ok, std::move(*msg));
  }

  void SendCommand(const std::string& type,
                   const std::string& script_id,
                   ResponseCallback done) {
    EnsureReady(base::BindOnce(
        [](base::WeakPtr<Impl> self, std::string type, std::string script_id,
           ResponseCallback done, bool ok, std::string error) {
          if (!self) {
            return;
          }
          if (!ok) {
            std::move(done).Run(false, MakeIpcErrorResponse("", error));
            return;
          }
          self->SendCommandReady(type, script_id, std::move(done));
        },
        weak_factory_.GetWeakPtr(), type, script_id, std::move(done)));
  }

  void SendCommandReady(const std::string& type,
                        const std::string& script_id,
                        ResponseCallback done) {
    if (client_fd_ < 0) {
      std::move(done).Run(false,
                          MakeIpcErrorResponse("", "Not connected to headless."));
      return;
    }
    const std::string request_id =
        base::Uuid::GenerateRandomV4().AsLowercaseString();
    base::Value::Dict req = MakeIpcRequest(type, request_id);
    if (!script_id.empty()) {
      req.Set("script_id", script_id);
    }
    std::string error;
    if (!WriteFramedJsonMessage(client_fd_, req, &error)) {
      std::move(done).Run(false, MakeIpcErrorResponse(request_id, error));
      return;
    }
    pending_[request_id] = std::move(done);
  }

  void SendInvestigatePage(base::Value::Dict payload, ResponseCallback done) {
    if (client_fd_ < 0) {
      std::move(done).Run(false,
                          MakeIpcErrorResponse("", "Not connected to headless."));
      return;
    }
    const std::string request_id =
        base::Uuid::GenerateRandomV4().AsLowercaseString();
    base::Value::Dict req = MakeIpcRequest("investigate_page", request_id);
    for (auto it : payload) {
      req.Set(it.first, it.second.Clone());
    }
    std::string error;
    if (!WriteFramedJsonMessage(client_fd_, req, &error)) {
      std::move(done).Run(false, MakeIpcErrorResponse(request_id, error));
      return;
    }
    pending_[request_id] = std::move(done);
  }

  raw_ptr<Profile> profile_ = nullptr;
  raw_ptr<HeadlessListenerController> owner_ = nullptr;
  base::FilePath socket_path_;
  base::Process child_;
  int client_fd_ = -1;
  int connect_attempts_ = 0;
  bool launch_in_progress_ = false;
  TriggerCallback trigger_callback_;
  std::vector<base::OnceCallback<void(bool, std::string)>> ready_waiters_;
  std::map<std::string, ResponseCallback> pending_;
  std::unique_ptr<base::FileDescriptorWatcher::Controller> client_watcher_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

#else  // !BUILDFLAG(IS_POSIX)

class HeadlessListenerController::Impl {
 public:
  explicit Impl(Profile* profile) : profile_(profile) {}
  void SetOwner(HeadlessListenerController* owner) { owner_ = owner; }

  void SetTriggerCallback(TriggerCallback callback) {
    trigger_callback_ = std::move(callback);
  }

  void EnsureReady(base::OnceCallback<void(bool, std::string)> done) {
    std::move(done).Run(
        false,
        "Native headless control channel is POSIX-only in this tree; Windows "
        "named-pipe transport is not wired yet.");
  }

  void StartListener(const std::string&, ResponseCallback done) {
    base::Value::Dict response;
    response.Set(
        "error",
        "Headless listeners are not available on Windows yet (named-pipe IPC "
        "pending).");
    std::move(done).Run(false, std::move(response));
  }

  void StopListener(const std::string&, ResponseCallback done) {
    base::Value::Dict response;
    response.Set("ok", true);
    std::move(done).Run(true, std::move(response));
  }

  void ListActive(ResponseCallback done) {
    base::Value::Dict response;
    response.Set("listeners", base::Value::List());
    std::move(done).Run(true, std::move(response));
  }

  void Warm(ResponseCallback done) {
    EnsureReady(base::BindOnce(
        [](ResponseCallback done, bool ok, std::string error) {
          base::Value::Dict response;
          if (!ok) {
            response.Set("error", std::move(error));
          }
          std::move(done).Run(ok, std::move(response));
        },
        std::move(done)));
  }

  void ClearPendingTriggers(ResponseCallback done) {
    base::Value::Dict response;
    response.Set("ok", true);
    std::move(done).Run(true, std::move(response));
  }

  void InvestigatePage(base::Value::Dict, ResponseCallback done) {
    base::Value::Dict response;
    response.Set(
        "error",
        "Headless investigate_page is not available on Windows yet "
        "(named-pipe IPC pending).");
    std::move(done).Run(false, std::move(response));
  }

 private:
  raw_ptr<Profile> profile_ = nullptr;
  raw_ptr<HeadlessListenerController> owner_ = nullptr;
  TriggerCallback trigger_callback_;
};

#endif  // BUILDFLAG(IS_POSIX)

HeadlessListenerController* HeadlessListenerController::GetForProfile(
    Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  auto& map = ControllersByProfile();
  auto it = map.find(profile);
  if (it != map.end()) {
    return it->second;
  }
  // Lazily created by callers that own unique_ptr; GetForProfile only looks up.
  return nullptr;
}

HeadlessListenerController::HeadlessListenerController(Profile* profile)
    : impl_(std::make_unique<Impl>(profile)) {
  impl_->SetOwner(this);
  ControllersByProfile()[profile] = this;
}

HeadlessListenerController::~HeadlessListenerController() {
  for (auto it = ControllersByProfile().begin();
       it != ControllersByProfile().end();) {
    if (it->second == this) {
      it = ControllersByProfile().erase(it);
    } else {
      ++it;
    }
  }
}

void HeadlessListenerController::SetTriggerCallback(TriggerCallback callback) {
  impl_->SetTriggerCallback(std::move(callback));
}

void HeadlessListenerController::EnsureReady(
    base::OnceCallback<void(bool ok, std::string error)> done) {
  impl_->EnsureReady(std::move(done));
}

void HeadlessListenerController::StartListener(const std::string& script_id,
                                               ResponseCallback done) {
  impl_->StartListener(script_id, std::move(done));
}

void HeadlessListenerController::StopListener(const std::string& script_id,
                                              ResponseCallback done) {
  impl_->StopListener(script_id, std::move(done));
}

void HeadlessListenerController::ListActive(ResponseCallback done) {
  impl_->ListActive(std::move(done));
}

void HeadlessListenerController::Warm(ResponseCallback done) {
  impl_->Warm(std::move(done));
}

void HeadlessListenerController::ClearPendingTriggers(ResponseCallback done) {
  impl_->ClearPendingTriggers(std::move(done));
}

void HeadlessListenerController::InvestigatePage(base::Value::Dict payload,
                                                 ResponseCallback done) {
  impl_->InvestigatePage(std::move(payload), std::move(done));
}

}  // namespace dashboard
