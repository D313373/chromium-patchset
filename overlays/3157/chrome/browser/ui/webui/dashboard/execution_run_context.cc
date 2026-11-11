#include "chrome/browser/ui/webui/dashboard/execution_run_context.h"

#include "base/command_line.h"
#include "chrome/common/chrome_switches.h"

namespace dashboard {

namespace {

ExecutionRunContext* GetContextStorage() {
  static thread_local ExecutionRunContext context;
  return &context;
}

}  // namespace

ExecutionRunContext& ExecutionRunContext::Get() {
  return *GetContextStorage();
}

void ExecutionRunContext::Set(bool is_agent_mode, bool force_native_execution) {
  is_agent_mode_ = is_agent_mode;
  force_native_execution_ = force_native_execution;
}

bool ExecutionRunContext::ShouldUseUnifiedPageMap() const {
  if (is_agent_mode_) {
    return true;
  }
  // The headless listener process has no Glic panel, but agents and listeners
  // there still need the same page map the headed browser builds.
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  return command_line && command_line->HasSwitch(switches::kHeadlessListener);
}

void ExecutionRunContext::Clear() {
  is_agent_mode_ = false;
  force_native_execution_ = false;
  // Intentionally leave listen_oneshot_depth_ intact so a nested onMatch
  // ScriptExecutor Clear() cannot unlock another listen→onMatch cycle.
}

}  // namespace dashboard
