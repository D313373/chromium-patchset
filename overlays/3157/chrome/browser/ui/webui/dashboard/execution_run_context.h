#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_EXECUTION_RUN_CONTEXT_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_EXECUTION_RUN_CONTEXT_H_

#include <string>

namespace dashboard {

// Thread-local run flags for the active ScriptExecutor run. Script mode in the
// headed browser keeps locator-only resolution. Agent mode, and every run in
// the headless listener process, uses the Annotated Page Content page map.
class ExecutionRunContext {
 public:
  static ExecutionRunContext& Get();

  void Set(bool is_agent_mode, bool force_native_execution);
  void Clear();

  bool is_agent_mode() const { return is_agent_mode_; }
  bool force_native_execution() const { return force_native_execution_; }

  bool ShouldUseUnifiedPageMap() const;

  // Depth of one-shot listen→onMatch nesting inside a normal (non-listener)
  // script run. ListenerManager persistent sessions never use this path.
  // Kept out of Clear() so nested ScriptExecutor runs do not reset it.
  int listen_oneshot_depth() const { return listen_oneshot_depth_; }
  void set_listen_oneshot_depth(int depth) { listen_oneshot_depth_ = depth; }

  ExecutionRunContext() = default;
  ~ExecutionRunContext() = default;

 private:
  bool is_agent_mode_ = false;
  bool force_native_execution_ = false;
  int listen_oneshot_depth_ = 0;
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_EXECUTION_RUN_CONTEXT_H_
