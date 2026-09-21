#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"

#include "base/no_destructor.h"
#include "base/strings/string_util.h"

namespace dashboard {

namespace {

std::string ReadString(const base::Value::Dict* dict,
                       std::string_view key) {
  if (!dict) {
    return std::string();
  }
  const std::string* value = dict->FindString(key);
  return value ? *value : std::string();
}

}  // namespace

ActionExecutionPath ParseActionExecutionPath(std::string_view value,
                                             bool allow_unset) {
  const std::string lowered = base::ToLowerASCII(value);
  if (lowered == "cdp") {
    return ActionExecutionPath::kCdp;
  }
  if (lowered == "native") {
    return ActionExecutionPath::kNative;
  }
  if (allow_unset && (lowered.empty() || lowered == "none" ||
                      lowered == "disabled")) {
    return ActionExecutionPath::kUnset;
  }
  return ActionExecutionPath::kUnset;
}

std::string_view ToActionExecutionPathString(ActionExecutionPath path) {
  switch (path) {
    case ActionExecutionPath::kCdp:
      return "cdp";
    case ActionExecutionPath::kNative:
      return "native";
    case ActionExecutionPath::kUnset:
      return "";
  }
  return "";
}

base::Value::Dict DefaultGlobalSettings() {
  base::Value::Dict s;
  base::Value::Dict anti;
  anti.Set("removeWebdriver", true);
  anti.Set("userAgent", "");
  anti.Set("platform", "");
  anti.Set("languages", base::Value::List());
  anti.Set("hardwareConcurrency", 0);

  base::Value::Dict plugin_spoof;
  plugin_spoof.Set("enabled", false);
  plugin_spoof.Set("randomSelection", false);
  plugin_spoof.Set("selectedPlugins", base::Value::List());
  anti.Set("pluginSpoofing", std::move(plugin_spoof));

  s.Set("antiDetection", std::move(anti));
  s.Set("theme", "dark");

  base::Value::Dict agent;
  agent.Set("provider", "anthropic");
  agent.Set("loopProfile", "glic_task");
  agent.Set("model", "claude-haiku-4-5");
  agent.Set("ollamaEndpoint", "http://127.0.0.1:11434/v1/chat/completions");
  agent.Set("localEndpoint", "http://127.0.0.1:11434/v1/chat/completions");
  agent.Set("keepWarmHeadlessInvestigationTab", true);
  agent.Set("endpoint", "https://api.cerebras.ai/v1/chat/completions");
  base::Value::Dict provider_endpoints;
  provider_endpoints.Set("cerebras",
                         "https://api.cerebras.ai/v1/chat/completions");
  provider_endpoints.Set("gemini",
                         "https://generativelanguage.googleapis.com/v1beta");
  provider_endpoints.Set("openai", "https://api.openai.com/v1/chat/completions");
  provider_endpoints.Set("anthropic", "https://api.anthropic.com/v1/messages");
  agent.Set("providerEndpoints", std::move(provider_endpoints));
  // JSON dialect is better for autonomous task completion (done/click/type).
  agent.Set("responseMode", "json");
  agent.Set("autoStartTaskOnLaunch", true);
  agent.Set("autoStartDelayMs", 5000);
  // Leave empty: baking a site password token into defaults biases every loop.
  agent.Set("autoStartPrompt", "");
  agent.Set("autoSaveScriptOnComplete", true);
  agent.Set("reviewScriptAfterComplete", true);
  agent.Set("maxAutonomousSteps", 20);
  // Local Ollama can take a long time per turn; default ~20 minutes.
  agent.Set("modelStepTimeoutMs", 20 * 60 * 1000);
  s.Set("agent", std::move(agent));

  base::Value::Dict input;
  input.Set("randomDelayEnabled", true);
  input.Set("clickPathJitterPx", 1.5);
  input.Set("clickHoverRadiusPx", 2.0);
  input.Set("clickHoverMoves", 3);
  input.Set("showPlaybackOverlay", true);
  input.Set("loopPreviewEnabled", false);
  s.Set("inputBehavior", std::move(input));

  base::Value::Dict execution;
  execution.Set("primaryPath", "cdp");
  execution.Set("fallbackPath", "");
  s.Set("execution", std::move(execution));

  base::Value::Dict listener_mode;
  listener_mode.Set("runListenersInHeadlessMode", true);
  // When true headless fails, do not silently open a headed background tab.
  listener_mode.Set("fallbackToBackgroundTabOnHeadlessFailure", false);
  s.Set("listenerMode", std::move(listener_mode));

  base::Value::Dict performance;
  performance.Set("aggressivePerformance", false);
  performance.Set("deterministicMode", false);
  // Off by default: CopyFromSurface ring buffer starves headed SPA paint.
  performance.Set("failureReplayEnabled", false);
  s.Set("performance", std::move(performance));

  base::Value::Dict subsystem;
  subsystem.Set("pdfViewerEnabled", true);
  subsystem.Set("extensionsEnabled", true);
  subsystem.Set("printPreviewEnabled", true);
  subsystem.Set("printStubEnabled", false);
  subsystem.Set("safeBrowsingEnabled", true);
  subsystem.Set("remoteDebuggingEnabled", false);
  subsystem.Set("serviceDiscoveryEnabled", true);
  subsystem.Set("geolocationAllowedByDefault", true);
  s.Set("subsystem", std::move(subsystem));
  return s;
}

base::Value::Dict& GlobalSettingsStore() {
  static base::NoDestructor<base::Value::Dict> store(DefaultGlobalSettings());
  return *store;
}

ActionExecutionRouting ReadActionExecutionRouting(
    const base::Value::Dict& settings) {
  ActionExecutionRouting routing;
  const base::Value::Dict* execution = settings.FindDict("execution");
  const base::Value::Dict* input = settings.FindDict("inputBehavior");

  const std::string primary_raw =
      !ReadString(execution, "primaryPath").empty()
          ? ReadString(execution, "primaryPath")
          : !ReadString(execution, "primary").empty()
                ? ReadString(execution, "primary")
                : ReadString(input, "primaryExecutionPath");

  const std::string fallback_raw =
      !ReadString(execution, "fallbackPath").empty()
          ? ReadString(execution, "fallbackPath")
          : !ReadString(execution, "fallback").empty()
                ? ReadString(execution, "fallback")
                : ReadString(input, "fallbackExecutionPath");

  routing.primary = ParseActionExecutionPath(primary_raw, /*allow_unset=*/false);
  if (routing.primary == ActionExecutionPath::kUnset) {
    routing.primary = ActionExecutionPath::kCdp;
  }
  routing.fallback =
      ParseActionExecutionPath(fallback_raw, /*allow_unset=*/true);
  if (routing.fallback == routing.primary) {
    routing.fallback = ActionExecutionPath::kUnset;
  }
  return routing;
}

ActionExecutionRouting GetActionExecutionRouting() {
  return ReadActionExecutionRouting(GlobalSettingsStore());
}

}  // namespace dashboard
