#include "chrome/browser/ui/webui/dashboard/actions/glic_prompt_action.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_split.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/containers/span.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "base/uuid.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/actions/element_readiness_checker.h"
#include "chrome/browser/ui/webui/dashboard/agent_model_contract.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/recorder_controller.h"
#include "chrome/browser/ui/webui/dashboard/script_schema.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "components/os_crypt/sync/os_crypt.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "net/http/http_response_headers.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "ui/gfx/geometry/rect_f.h"

namespace dashboard {
namespace actions {

namespace {

constexpr char kDefaultAgentProvider[] = "ollama";
constexpr char kDefaultDashboardModel[] = "qwen3-coder-next";
constexpr char kDefaultCerebrasModel[] = "qwen-3-235b-a22b-instruct-2507";
constexpr char kDefaultGeminiModel[] = "gemini-2.5-pro";
constexpr char kDefaultOpenAIModel[] = "gpt-4.1";
constexpr char kDefaultGrokModel[] = "grok-4";
constexpr char kDefaultAnthropicModel[] = "claude-haiku-4-5";
constexpr char kDefaultOllamaEndpoint[] = "http://127.0.0.1:11434";
constexpr char kDefaultCerebrasEndpoint[] =
    "https://api.cerebras.ai/v1/chat/completions";
constexpr char kDefaultGeminiEndpoint[] =
    "https://generativelanguage.googleapis.com/v1beta";
constexpr char kDefaultOpenAIEndpoint[] =
    "https://api.openai.com/v1/chat/completions";
constexpr char kDefaultGrokEndpoint[] =
    "https://api.x.ai/v1/chat/completions";
constexpr char kDefaultAnthropicEndpoint[] =
    "https://api.anthropic.com/v1/messages";
constexpr char kHostedClientUserAgent[] = "SimulachromeDashboard/1.0";
constexpr int kMaxResponseBytes = 4 * 1024 * 1024;
constexpr int kDefaultMaxAutonomousAgentSteps = 40;
constexpr int kMaxAutonomousFailureStreak = 2;
constexpr int kMaxPlainTextRetries = 1;
constexpr size_t kMaxFinalResponseChars = 1200u;
constexpr size_t kChoiceMenuMaxElements = 8u;
constexpr size_t kMaxChoiceBatchSteps = 5u;
constexpr base::TimeDelta kAgentPageReadyPollInterval = base::Milliseconds(250);
constexpr int kMaxAgentPageReadyPollAttempts = 80;
constexpr double kAgentReadyLoadProgressThreshold = 0.995;
constexpr base::TimeDelta kAgentRecentNavigationGrace = base::Seconds(10);
constexpr char kChoiceStructuredAnswerPattern[] =
    "^[A-I](?:\\d+)?(?:-\\d+)?(?:=(?:'[^'\\n]*'|\"[^\"\\n]*\"|[^,\\n]+))?"
    "(?:\\s*,\\s*[A-I](?:\\d+)?(?:-\\d+)?(?:=(?:'[^'\\n]*'|\"[^\"\\n]*\"|"
    "[^,\\n]+))?){0,4}$";

int MaxAutonomousAgentSteps() {
  const base::Value::Dict* agent =
      dashboard::GlobalSettingsStore().FindDict("agent");
  if (agent) {
    if (const std::optional<int> value = agent->FindInt("maxAutonomousSteps")) {
      return std::clamp(*value, 5, 100);
    }
  }
  return kDefaultMaxAutonomousAgentSteps;
}

bool AgentBoolSetting(std::string_view key, bool default_value) {
  const base::Value::Dict* agent =
      dashboard::GlobalSettingsStore().FindDict("agent");
  if (agent) {
    if (const std::optional<bool> value = agent->FindBool(key)) {
      return *value;
    }
  }
  return default_value;
}

int AgentIntSetting(std::string_view key, int default_value, int min_value,
                    int max_value) {
  const base::Value::Dict* agent =
      dashboard::GlobalSettingsStore().FindDict("agent");
  if (agent) {
    if (const std::optional<int> value = agent->FindInt(key)) {
      return std::clamp(*value, min_value, max_value);
    }
  }
  return std::clamp(default_value, min_value, max_value);
}

base::TimeDelta ModelStepTimeout() {
  // ~20 minutes by default so locally hosted models can finish a turn.
  return base::Milliseconds(AgentIntSetting(
      "modelStepTimeoutMs", /*default_value=*/20 * 60 * 1000,
      /*min_value=*/60 * 1000, /*max_value=*/60 * 60 * 1000));
}

base::Value::List ResolveFocusAreaMatches(const base::Value::List& focus_areas,
                                          const base::Value::Dict& page_map);
std::string BuildFocusAreasBlock(const base::Value::List& focus_areas,
                                 const base::Value::List& matched_areas);
base::Value::List BuildRankedPromptCandidates(const std::string& page_url,
                                              const base::Value::Dict& page_map,
                                              const base::Value::List& matched_areas);
std::string BuildCandidateBatchBlock(const base::Value::List& candidates,
                                     size_t batch_index);
base::Value::Dict BuildPromptPageMapForUrl(const std::string& page_url,
                                           const base::Value::Dict& page_map);
std::string BuildSiteSpecificPromptNote(const std::string& page_url);
std::string BuildCapabilityNote(std::string_view model_name,
                                bool screenshot_attached);
bool IsVisionCapableModel(std::string_view model_name);
std::string BuildChoiceTurnPrompt(const std::string& goal,
                                  const std::string& turn_note,
                                  std::string_view model_name,
                                  bool screenshot_attached,
                                  const std::string& page_title,
                                  const std::string& page_url,
                                  const base::Value::List& focus_areas,
                                  const base::Value::List& focus_matches,
                                  bool focus_only_context,
                                  int context_batch_index,
                                  const base::Value::List& choice_menu,
                                  bool controls_menu_active,
                                  bool allow_screenshot,
                                  const std::string& delta_note);
base::Value::List BuildChoiceMenuItems(const std::string& page_url,
                                       const base::Value::Dict& page_map,
                                       const base::Value::List& focus_areas,
                                       int context_batch_index);
std::string BuildSaveScriptPrompt(const std::string& goal,
                                  const base::Value::List& task_actions);
std::string BuildChoiceFollowupTurnNote(const base::Value::Dict& command,
                                        const base::Value::Dict& result,
                                        int step,
                                        int max_steps);
base::Value::List BuildLocatorsFromElementSelectors(
    const base::Value::Dict& element);

bool ShouldWaitForAgentPageReady(content::WebContents* web_contents,
                                 int wait_attempt) {
  if (!web_contents || wait_attempt >= kMaxAgentPageReadyPollAttempts) {
    return false;
  }
  if (web_contents->IsLoading() &&
      web_contents->GetLoadProgress() < kAgentReadyLoadProgressThreshold) {
    return true;
  }
  content::NavigationEntry* entry =
      web_contents->GetController().GetLastCommittedEntry();
  if (!entry) {
    return false;
  }
  const base::Time navigation_time = entry->GetTimestamp();
  if (navigation_time.is_null()) {
    return false;
  }
  // Sites often lie about readiness; keep a fixed post-navigation grace.
  return (base::Time::Now() - navigation_time) < kAgentRecentNavigationGrace;
}

bool IsSafeAgentLocatorType(std::string_view type) {
  return !base::EqualsCaseInsensitiveASCII(type, "VISUAL_COORDS");
}

base::Value::List SanitizeLocatorsForAgentExecution(
    const base::Value::List& locators) {
  base::Value::List sanitized;
  for (const base::Value& locator_value : locators) {
    const base::Value::Dict* locator = locator_value.GetIfDict();
    const std::string* type = locator ? locator->FindString("type") : nullptr;
    const std::string* value = locator ? locator->FindString("value") : nullptr;
    if (!type || !value || value->empty() || !IsSafeAgentLocatorType(*type)) {
      continue;
    }
    sanitized.Append(locator->Clone());
  }
  return sanitized;
}

std::optional<gfx::RectF> ReadBounds(const base::Value::Dict& element) {
  const base::Value::Dict* bounds = element.FindDict("bounds");
  if (!bounds) {
    return std::nullopt;
  }
  const float x = static_cast<float>(bounds->FindDouble("x").value_or(0.0));
  const float y = static_cast<float>(bounds->FindDouble("y").value_or(0.0));
  const float w = static_cast<float>(bounds->FindDouble("w").value_or(0.0));
  const float h = static_cast<float>(bounds->FindDouble("h").value_or(0.0));
  if (w <= 1.0f || h <= 1.0f) {
    return std::nullopt;
  }
  return gfx::RectF(x, y, w, h);
}

std::string NormalizePromptText(std::string text, size_t max_chars = 360u) {
  text = base::CollapseWhitespaceASCII(text, true);
  base::TrimWhitespaceASCII(text, base::TRIM_ALL, &text);
  if (text.empty()) {
    return std::string();
  }
  std::string normalized =
      std::string(base::TruncateUTF8ToByteSize(text, max_chars));
  if (text.size() <= max_chars) {
    return normalized;
  }
  normalized.append("\n... [context excerpt truncated]");
  return normalized;
}

bool LooksLikeInstructionalTypeText(std::string_view text) {
  const std::string lowered = base::ToLowerASCII(text);
  static constexpr const char* kInstructionNeedles[] = {
      "element i labeled",
      "element i named",
      "element number",
      "target element",
      "into the element",
      "copy the reply",
      "choose the best next",
      "multiple choice question",
      "current menu",
      "locators",
      "focus area",
      "reply-box",
  };
  for (const char* needle : kInstructionNeedles) {
    if (base::Contains(lowered, needle)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> FindInstructionEchoTypedText(
    const base::Value::Dict& command) {
  const auto find_from_action = [](const base::Value::Dict& action)
      -> std::optional<std::string> {
    const std::string* action_name = action.FindString("action");
    const base::Value::Dict* params = action.FindDict("params");
    const std::string* text = params ? params->FindString("text") : nullptr;
    if (!action_name || !text ||
        !base::EqualsCaseInsensitiveASCII(*action_name, "type")) {
      return std::nullopt;
    }
    if (LooksLikeInstructionalTypeText(*text)) {
      return *text;
    }
    return std::nullopt;
  };

  const std::string* command_name = command.FindString("command");
  const base::Value::Dict* payload = command.FindDict("payload");
  if (!command_name || !payload) {
    return std::nullopt;
  }
  if (*command_name == "call_action") {
    return find_from_action(*payload);
  }
  if (*command_name != "run_timeline") {
    return std::nullopt;
  }
  const base::Value::List* timeline = payload->FindList("timeline");
  if (!timeline) {
    return std::nullopt;
  }
  for (const base::Value& action_value : *timeline) {
    const base::Value::Dict* action = action_value.GetIfDict();
    if (!action) {
      continue;
    }
    if (std::optional<std::string> invalid = find_from_action(*action)) {
      return invalid;
    }
  }
  return std::nullopt;
}

constexpr char kChoiceAgentSystemPrompt[] = R"(You are a browser action chooser.
Choose the single best next browser move.
Do not explain your reasoning.
Do not emit markdown.
When the transport requires structured output, put only the requested choice payload in the required field.)";

bool UrlHostMatches(const std::string& url, std::string_view host);

bool DictContainsAnySubstring(const base::Value::Dict& dict,
                              base::span<const char* const> needles);

std::string FindProminentOverlaySummary(const base::Value::Dict& page_map);

base::FilePath AgentPromptTraceDir(Profile* profile) {
  return profile->GetPath()
      .AppendASCII("SimulachromeData")
      .AppendASCII("AgentPromptRuns");
}

std::pair<base::FilePath, base::FilePath> BuildAgentTracePaths(
    Profile* profile,
    const base::Value::Dict& trace) {
  const std::string* trace_id = trace.FindString("trace_id");
  const base::FilePath trace_dir = AgentPromptTraceDir(profile);
  return {
      trace_dir.AppendASCII(
          base::StrCat({trace_id ? *trace_id : "agent_trace", ".json"})),
      trace_dir.AppendASCII("latest.json"),
  };
}

void SaveTraceToDisk(base::FilePath path, base::Value::Dict trace) {
  base::CreateDirectory(path.DirName());
  std::string json;
  if (!base::JSONWriter::WriteWithOptions(
          trace, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json)) {
    return;
  }
  base::WriteFile(path, json);
}

void ScheduleTraceWrite(base::FilePath trace_path,
                        base::FilePath latest_trace_path,
                        base::Value::Dict trace,
                        base::TaskShutdownBehavior shutdown_behavior) {
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE, shutdown_behavior},
      base::BindOnce(&SaveTraceToDisk, trace_path, trace.Clone()));
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE, shutdown_behavior},
      base::BindOnce(&SaveTraceToDisk, latest_trace_path, std::move(trace)));
}

std::string TruncateForPrompt(std::string text, size_t max_chars) {
  std::string sanitized =
      std::string(base::TruncateUTF8ToByteSize(text, max_chars));
  if (text.size() <= max_chars) {
    return sanitized;
  }
  sanitized.append("\n... [context excerpt truncated]");
  return sanitized;
}

std::string SanitizeUtf8ForValue(std::string_view text, size_t max_chars) {
  return std::string(base::TruncateUTF8ToByteSize(text, max_chars));
}

std::string TrimmedStringValue(const base::Value::Dict& dict,
                               std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const std::string* value = dict.FindString(key);
    if (value && !base::TrimWhitespaceASCII(*value, base::TRIM_ALL).empty()) {
      return *value;
    }
  }
  return std::string();
}

std::string ElementSummaryText(const base::Value::Dict& element) {
  return TrimmedStringValue(
      element,
      {"text", "name", "value", "label", "accessibility_name",
       "accessibilityName", "ax_name", "axName"});
}

base::Value::List CollectDescendantTextHints(const base::Value::Dict& element,
                                             const base::Value::Dict& page_map,
                                             size_t max_hints) {
  base::Value::List hints;
  if (max_hints == 0u) {
    return hints;
  }
  const std::optional<gfx::RectF> parent_bounds = ReadBounds(element);
  if (!parent_bounds.has_value()) {
    return hints;
  }
  const base::Value::List* frames = page_map.FindList("frames");
  if (!frames) {
    return hints;
  }

  const std::string parent_id = TrimmedStringValue(element, {"id"});
  std::set<std::string> seen;
  std::vector<std::pair<float, std::string>> candidates;
  for (const base::Value& frame_value : *frames) {
    const base::Value::Dict* frame = frame_value.GetIfDict();
    const base::Value::List* elements = frame ? frame->FindList("elements")
                                              : nullptr;
    if (!elements) {
      continue;
    }
    for (const base::Value& child_value : *elements) {
      const base::Value::Dict* child = child_value.GetIfDict();
      if (!child) {
        continue;
      }
      const std::string child_id = TrimmedStringValue(*child, {"id"});
      if (!parent_id.empty() && child_id == parent_id) {
        continue;
      }
      const std::optional<gfx::RectF> child_bounds = ReadBounds(*child);
      if (!child_bounds.has_value()) {
        continue;
      }
      if (!parent_bounds->Contains(child_bounds->CenterPoint()) ||
          child_bounds->width() >= parent_bounds->width() ||
          child_bounds->height() >= parent_bounds->height()) {
        continue;
      }
      std::string text = NormalizePromptText(ElementSummaryText(*child), 120u);
      if (text.empty()) {
        continue;
      }
      if (!seen.insert(text).second) {
        continue;
      }
      candidates.emplace_back(child_bounds->width() * child_bounds->height(),
                              std::move(text));
    }
  }

  std::stable_sort(
      candidates.begin(), candidates.end(),
      [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& candidate : candidates) {
    hints.Append(candidate.second);
    if (hints.size() >= max_hints) {
      break;
    }
  }
  return hints;
}

std::optional<std::string> BuildCoarsePositionLabel(const base::Value::Dict& element) {
  const base::Value::Dict* bounds = element.FindDict("bounds");
  if (!bounds) {
    return std::nullopt;
  }
  const double x = bounds->FindDouble("x").value_or(0.0);
  const double y = bounds->FindDouble("y").value_or(0.0);
  const double w = bounds->FindDouble("w").value_or(0.0);
  const double h = bounds->FindDouble("h").value_or(0.0);
  if (w <= 0.0 || h <= 0.0) {
    return std::nullopt;
  }
  const double cx = x + w / 2.0;
  const double cy = y + h / 2.0;
  const std::string horizontal =
      cx < 360.0 ? "left" : (cx > 960.0 ? "right" : "center");
  const std::string vertical =
      cy < 320.0 ? "top" : (cy > 900.0 ? "bottom" : "middle");
  return base::StrCat({vertical, "-", horizontal});
}

base::Value::List BuildSemanticHintsFromLocators(const base::Value::Dict& element) {
  base::Value::List hints;
  const base::Value::Dict* selectors = element.FindDict("selectors");
  const base::Value::List* locators =
      selectors ? selectors->FindList("locators") : nullptr;
  if (!locators) {
    return hints;
  }
  std::set<std::string> seen;
  for (const base::Value& locator_value : *locators) {
    const base::Value::Dict* locator = locator_value.GetIfDict();
    if (!locator) {
      continue;
    }
    const std::string* type = locator->FindString("type");
    const std::string* value = locator->FindString("value");
    if (!type || !value || value->empty()) {
      continue;
    }
    std::string hint;
    if (base::EqualsCaseInsensitiveASCII(*type, "ID")) {
      hint = base::StrCat({"id=", SanitizeUtf8ForValue(*value, 80u)});
    } else if (base::EqualsCaseInsensitiveASCII(*type, "CLASS_NAME")) {
      hint = base::StrCat({"class=", TruncateForPrompt(*value, 48u)});
    } else if (base::EqualsCaseInsensitiveASCII(*type, "NAME")) {
      hint = base::StrCat({"name=", SanitizeUtf8ForValue(*value, 80u)});
    } else if (base::EqualsCaseInsensitiveASCII(*type, "ACCESSIBILITY_NAME")) {
      hint = base::StrCat({"ax=", TruncateForPrompt(*value, 60u)});
    } else if (base::EqualsCaseInsensitiveASCII(*type, "TEXT_CONTENT")) {
      hint = base::StrCat({"text=", TruncateForPrompt(*value, 60u)});
    }
    if (hint.empty() || !seen.insert(hint).second) {
      continue;
    }
    hints.Append(hint);
    if (hints.size() >= 4u) {
      break;
    }
  }
  return hints;
}

base::Value::List BuildSemanticHintsFromLocatorList(
    const base::Value::List& locators) {
  base::Value::List hints;
  std::set<std::string> seen;
  for (const base::Value& locator_value : locators) {
    const base::Value::Dict* locator = locator_value.GetIfDict();
    if (!locator) {
      continue;
    }
    const std::string* type = locator->FindString("type");
    const std::string* value = locator->FindString("value");
    if (!type || !value || value->empty()) {
      continue;
    }
    std::string hint;
    if (base::EqualsCaseInsensitiveASCII(*type, "ID")) {
      hint = base::StrCat({"id=", SanitizeUtf8ForValue(*value, 80u)});
    } else if (base::EqualsCaseInsensitiveASCII(*type, "CLASS_NAME")) {
      hint = base::StrCat({"class=", TruncateForPrompt(*value, 36u)});
    } else if (base::EqualsCaseInsensitiveASCII(*type, "NAME")) {
      hint = base::StrCat({"name=", SanitizeUtf8ForValue(*value, 80u)});
    } else if (base::EqualsCaseInsensitiveASCII(*type, "ACCESSIBILITY_NAME")) {
      hint = base::StrCat({"ax=", TruncateForPrompt(*value, 48u)});
    } else if (base::EqualsCaseInsensitiveASCII(*type, "TEXT_CONTENT")) {
      hint = base::StrCat({"text=", TruncateForPrompt(*value, 48u)});
    } else if (base::EqualsCaseInsensitiveASCII(*type, "PLACEHOLDER")) {
      hint = base::StrCat({"placeholder=", TruncateForPrompt(*value, 48u)});
    }
    if (hint.empty() || !seen.insert(hint).second) {
      continue;
    }
    hints.Append(hint);
    if (hints.size() >= 3u) {
      break;
    }
  }
  return hints;
}

bool LocatorMatchesCandidate(const base::Value::Dict& locator,
                             const base::Value::Dict& element) {
  const std::string* raw_type = locator.FindString("type");
  const std::string* raw_value = locator.FindString("value");
  if (!raw_type || !raw_value || raw_value->empty()) {
    return false;
  }

  const std::string type = base::ToUpperASCII(*raw_type);
  const std::string value = *raw_value;
  const std::string lowered_value = base::ToLowerASCII(value);
  const base::Value::Dict* selectors = element.FindDict("selectors");
  const base::Value::List* candidate_locators =
      selectors ? selectors->FindList("locators") : nullptr;

  auto candidate_locator_matches = [&](std::string_view wanted_type) {
    if (!candidate_locators) {
      return false;
    }
    for (const base::Value& candidate_value : *candidate_locators) {
      const base::Value::Dict* candidate = candidate_value.GetIfDict();
      if (!candidate) {
        continue;
      }
      const std::string* candidate_type = candidate->FindString("type");
      const std::string* candidate_locator_value = candidate->FindString("value");
      if (!candidate_type || !candidate_locator_value) {
        continue;
      }
      if (base::EqualsCaseInsensitiveASCII(*candidate_type, wanted_type) &&
          *candidate_locator_value == value) {
        return true;
      }
    }
    return false;
  };

  if (candidate_locator_matches(type)) {
    return true;
  }

  if (type == "CSS_SELECTOR") {
    if (selectors) {
      if (const std::string* css = selectors->FindString("css");
          css && *css == value) {
        return true;
      }
    }
  } else if (type == "XPATH") {
    if (selectors) {
      if (const std::string* xpath = selectors->FindString("xpath");
          xpath && *xpath == value) {
        return true;
      }
    }
  } else if (type == "TAG_NAME") {
    if (const std::string* tag = element.FindString("tag");
        tag && base::EqualsCaseInsensitiveASCII(*tag, value)) {
      return true;
    }
  } else if (type == "CLASS_NAME") {
    if (candidate_locators) {
      for (const base::Value& candidate_value : *candidate_locators) {
        const base::Value::Dict* candidate = candidate_value.GetIfDict();
        if (!candidate) {
          continue;
        }
        const std::string* candidate_type = candidate->FindString("type");
        const std::string* candidate_locator_value = candidate->FindString("value");
        if (!candidate_type || !candidate_locator_value) {
          continue;
        }
        if (!base::EqualsCaseInsensitiveASCII(*candidate_type, "CLASS_NAME")) {
          continue;
        }
        if (base::Contains(base::ToLowerASCII(*candidate_locator_value),
                           lowered_value)) {
          return true;
        }
      }
    }
  }

  return false;
}

base::Value::Dict BuildPromptCandidateSummary(const base::Value::Dict& element,
                                              const base::Value::Dict& locator,
                                              const base::Value::Dict* page_map) {
  base::Value::Dict summary;
  if (const std::string* id = element.FindString("id")) {
    summary.Set("snapshotId", *id);
  }
  if (const std::string* role = element.FindString("role"); role && !role->empty()) {
    summary.Set("role", *role);
  }
  if (const std::string* ancestor_role =
          element.FindString("interactiveAncestorRole");
      ancestor_role && !ancestor_role->empty()) {
    summary.Set("interactiveAncestorRole", *ancestor_role);
  }
  if (const std::string* tag = element.FindString("tag"); tag && !tag->empty()) {
    summary.Set("tag", *tag);
  }
  if (const std::string* type = element.FindString("type");
      type && !type->empty()) {
    summary.Set("elementType", *type);
  }
  if (const std::string* accessibility_name =
          element.FindString("accessibilityName");
      accessibility_name && !accessibility_name->empty()) {
    summary.Set("accessibilityName",
                TruncateForPrompt(*accessibility_name, 120u));
  }
  if (const std::string text = ElementSummaryText(element); !text.empty()) {
    summary.Set("text", TruncateForPrompt(text, 240u));
  }
  if (page_map) {
    base::Value::List child_text_hints =
        CollectDescendantTextHints(element, *page_map, 3u);
    if (!child_text_hints.empty()) {
      summary.Set("childTextHints", std::move(child_text_hints));
    }
  }
  if (std::optional<std::string> position = BuildCoarsePositionLabel(element)) {
    summary.Set("coarsePosition", *position);
  }
  base::Value::List semantic_hints = BuildSemanticHintsFromLocators(element);
  if (!semantic_hints.empty()) {
    summary.Set("semanticHints", std::move(semantic_hints));
  }
  base::Value::List locators = BuildLocatorsFromElementSelectors(element);
  if (locators.empty() && !locator.empty()) {
    locators.Append(locator.Clone());
  }
  if (!locators.empty()) {
    summary.Set("locators", std::move(locators));
  }
  return summary;
}

base::Value::List ResolveFocusLocatorMatches(const base::Value::List& locators,
                                             const base::Value::Dict& page_map) {
  base::Value::List matches;
  if (locators.empty()) {
    return matches;
  }
  const base::Value::List* frames = page_map.FindList("frames");
  if (!frames) {
    return matches;
  }

  std::set<std::string> seen_ids;
  for (const base::Value& locator_value : locators) {
    const base::Value::Dict* locator = locator_value.GetIfDict();
    if (!locator) {
      continue;
    }
    for (const base::Value& frame_value : *frames) {
      const base::Value::Dict* frame = frame_value.GetIfDict();
      if (!frame) {
        continue;
      }
      const base::Value::List* elements = frame->FindList("elements");
      if (!elements) {
        continue;
      }
      for (const base::Value& element_value : *elements) {
        const base::Value::Dict* element = element_value.GetIfDict();
        if (!element || !LocatorMatchesCandidate(*locator, *element)) {
          continue;
        }
        const std::string snapshot_id = TrimmedStringValue(*element, {"id"});
        if (!snapshot_id.empty() && !seen_ids.insert(snapshot_id).second) {
          continue;
        }
        matches.Append(
            BuildPromptCandidateSummary(*element, *locator, &page_map));
        if (matches.size() >= 12u) {
          return matches;
        }
      }
    }
  }

  return matches;
}

bool ElementLooksRelevantForXPrompt(const base::Value::Dict& element) {
  static constexpr const char* kPreferredNeedles[] = {
      "tweettextarea",
      "tweetbutton",
      "reply",
      "composer",
      "drafteditor",
      "contenteditable",
      "textbox",
      "postbutton",
  };
  if (DictContainsAnySubstring(element, kPreferredNeedles)) {
    return true;
  }
  const std::string* role = element.FindString("role");
  if (role && base::EqualsCaseInsensitiveASCII(*role, "AX_ROLE_TEXT_FIELD")) {
    return true;
  }
  const std::string* ancestor_role = element.FindString("interactiveAncestorRole");
  return ancestor_role &&
         base::EqualsCaseInsensitiveASCII(*ancestor_role, "textbox");
}

base::Value::List FlattenFocusAreas(const base::Value::List& focus_areas) {
  base::Value::List flattened;
  for (const base::Value& area_value : focus_areas) {
    const base::Value::Dict* area = area_value.GetIfDict();
    const base::Value::List* locators =
        area ? area->FindList("locators") : nullptr;
    if (!locators) {
      continue;
    }
    for (const base::Value& locator_value : *locators) {
      flattened.Append(locator_value.Clone());
    }
  }
  return flattened;
}

base::Value::Dict BuildFocusAreaFallbackSummary(const base::Value::Dict& area) {
  base::Value::Dict summary;
  if (const std::string* label = area.FindString("label");
      label && !label->empty()) {
    summary.Set("label", *label);
  }
  if (const std::string* ax_role = area.FindString("ax_role");
      ax_role && !ax_role->empty()) {
    summary.Set("role", *ax_role);
  } else if (const std::string* ancestor_role =
                 area.FindString("interactive_ancestor_role");
             ancestor_role && !ancestor_role->empty()) {
    summary.Set("interactiveAncestorRole", *ancestor_role);
  }
  if (const std::string* ax_name = area.FindString("ax_name");
      ax_name && !ax_name->empty()) {
    summary.Set("accessibilityName", TruncateForPrompt(*ax_name, 120u));
  }
  if (const base::Value::List* locators = area.FindList("locators");
      locators && !locators->empty()) {
    base::Value::List semantic_hints = BuildSemanticHintsFromLocatorList(*locators);
    if (!semantic_hints.empty()) {
      summary.Set("semanticHints", std::move(semantic_hints));
    }
    base::Value::List sanitized = SanitizeLocatorsForAgentExecution(*locators);
    if (!sanitized.empty()) {
      summary.Set("locators", std::move(sanitized));
    }
  }
  summary.Set("focusAreaFallback", true);
  return summary;
}

base::Value::List ResolveFocusAreaMatches(const base::Value::List& focus_areas,
                                          const base::Value::Dict& page_map) {
  base::Value::List resolved_areas;
  for (const base::Value& area_value : focus_areas) {
    const base::Value::Dict* area = area_value.GetIfDict();
    const base::Value::List* locators =
        area ? area->FindList("locators") : nullptr;
    if (!locators || locators->empty()) {
      continue;
    }
    base::Value::Dict resolved_area;
    if (const std::string* label = area->FindString("label");
        label && !label->empty()) {
      resolved_area.Set("label", *label);
    }
    resolved_area.Set("locators", locators->Clone());
    resolved_area.Set("matchedElements",
                      ResolveFocusLocatorMatches(*locators, page_map));
    resolved_areas.Append(std::move(resolved_area));
  }
  return resolved_areas;
}

std::string BuildFocusAreasBlock(const base::Value::List& focus_areas,
                                 const base::Value::List& matched_areas) {
  if (focus_areas.empty()) {
    return std::string();
  }
  base::Value::List compact_areas;
  for (const base::Value& area_value : focus_areas) {
    const base::Value::Dict* area = area_value.GetIfDict();
    if (!area) {
      continue;
    }
    base::Value::Dict compact;
    if (const std::string* label = area->FindString("label");
        label && !label->empty()) {
      compact.Set("label", *label);
    }
    if (const base::Value::List* locators = area->FindList("locators");
        locators && !locators->empty()) {
      base::Value::List hints = BuildSemanticHintsFromLocatorList(*locators);
      if (!hints.empty()) {
        compact.Set("semanticHints", std::move(hints));
      }
    }

    const base::Value::Dict* matched_area = nullptr;
    if (const std::string* label = area->FindString("label");
        label && !label->empty()) {
      for (const base::Value& matched_value : matched_areas) {
        const base::Value::Dict* candidate = matched_value.GetIfDict();
        if (!candidate) {
          continue;
        }
        const std::string* candidate_label = candidate->FindString("label");
        if (candidate_label && *candidate_label == *label) {
          matched_area = candidate;
          break;
        }
      }
    }

    const base::Value::List* matched_elements =
        matched_area ? matched_area->FindList("matchedElements") : nullptr;
    const base::Value::Dict* best_match =
        (matched_elements && !matched_elements->empty())
            ? matched_elements->front().GetIfDict()
            : nullptr;
    if (best_match) {
      if (const std::string* snapshot_id = best_match->FindString("snapshotId");
          snapshot_id && !snapshot_id->empty()) {
        compact.Set("snapshotId", *snapshot_id);
      }
      if (const std::string* role = best_match->FindString("role");
          role && !role->empty()) {
        compact.Set("role", *role);
      }
      const std::string text = ElementSummaryText(*best_match);
      if (!text.empty()) {
        compact.Set("text", TruncateForPrompt(text, 96u));
      }
      if (std::optional<std::string> coarse_position =
              BuildCoarsePositionLabel(*best_match);
          coarse_position.has_value()) {
        compact.Set("coarsePosition", *coarse_position);
      }
      base::Value::List semantic_hints = BuildSemanticHintsFromLocators(*best_match);
      if (!semantic_hints.empty()) {
        compact.Set("matchedHints", std::move(semantic_hints));
      }
      if (const base::Value::List* child_text_hints =
              best_match->FindList("childTextHints");
          child_text_hints && !child_text_hints->empty()) {
        compact.Set("childTextHints", child_text_hints->Clone());
      }
    }

    compact_areas.Append(std::move(compact));
  }

  std::string serialized = "[]";
  if (!base::JSONWriter::Write(compact_areas, &serialized) || serialized.empty()) {
    serialized = "[]";
  }
  return base::StrCat({
      "Agent focus areas (prioritize these elements/regions when planning and "
      "acting):\n",
      TruncateForPrompt(std::move(serialized), 1600u),
      "\n\n",
  });
}

std::string BuildPrimaryGoalBlock(const std::string& prompt) {
  return base::StrCat(
      {"Primary user goal. Do not drift away from this task unless it is "
       "already solved or impossible with the available tools:\n",
       prompt, "\n\n"});
}

std::string BuildFocusOnlyContextNote(bool focus_only_context,
                                      int context_batch_index) {
  if (!focus_only_context) {
    return std::string();
  }
  if (context_batch_index > 0) {
    return base::StringPrintf(
        "Context mode: focus-only. You were given the user-specified "
        "elements/areas plus heuristically ranked extra candidates, batch %d.\n\n",
        context_batch_index);
  }
  return "Context mode: focus-only. You were given only the user-specified elements/areas of interest plus minimal page metadata. If you need broader context, issue request_more_context. If you need visual grounding, issue request_screenshot.\n\n";
}

std::string BuildMinimalPageHeaderBlock(const std::string& page_title,
                                        const std::string& page_url) {
  return base::StrCat({"Current Page: ", page_title, " (", page_url,
                       ")\n\n"});
}

std::string CandidateIdentityKey(const base::Value::Dict& candidate) {
  if (const std::string* snapshot = candidate.FindString("snapshotId");
      snapshot && !snapshot->empty()) {
    return *snapshot;
  }
  if (const std::string* id = candidate.FindString("id"); id && !id->empty()) {
    return base::StrCat({"id:", *id});
  }
  std::string css;
  std::string role;
  std::string name;
  if (const base::Value::Dict* selectors = candidate.FindDict("selectors")) {
    if (const std::string* s = selectors->FindString("css")) {
      css = *s;
    }
  }
  if (const std::string* r = candidate.FindString("role")) {
    role = *r;
  }
  if (const std::string* n = candidate.FindString("name")) {
    name = *n;
  } else if (const std::string* t = candidate.FindString("text")) {
    name = *t;
  }
  return base::StrCat({role, "|", name, "|", css});
}

std::set<std::string> CollectCandidateIdentityKeys(
    const base::Value::List& candidates) {
  std::set<std::string> keys;
  for (const base::Value& value : candidates) {
    const base::Value::Dict* candidate = value.GetIfDict();
    if (!candidate) {
      continue;
    }
    const std::string key = CandidateIdentityKey(*candidate);
    if (!key.empty()) {
      keys.insert(key);
    }
  }
  return keys;
}

std::string BuildContextDigest(const std::string& page_url,
                               const std::string& page_title,
                               const std::set<std::string>& candidate_ids) {
  std::string joined;
  for (const std::string& id : candidate_ids) {
    if (!joined.empty()) {
      joined.push_back('\n');
    }
    joined.append(id);
  }
  return base::StrCat({page_url, "\n", page_title, "\n", joined});
}

std::string BuildUnchangedContextNote(const std::string& page_title,
                                      const std::string& page_url) {
  return base::StrCat(
      {BuildMinimalPageHeaderBlock(page_title, page_url),
       "Context status: UNCHANGED since last payload (same URL + same "
       "interactive candidates). Do not re-read a full context. Continue "
       "from prior targets. Call look only if you expect new controls, an "
       "overlay/captcha appeared, or navigation occurred.\n\n"});
}

std::string BuildContextDeltaNote(const std::string& page_title,
                                  const std::string& page_url,
                                  const base::Value::List& candidates,
                                  const std::set<std::string>& previous_ids) {
  base::Value::List added;
  for (const base::Value& value : candidates) {
    const base::Value::Dict* candidate = value.GetIfDict();
    if (!candidate) {
      continue;
    }
    const std::string key = CandidateIdentityKey(*candidate);
    if (key.empty() || previous_ids.contains(key)) {
      continue;
    }
    added.Append(value.Clone());
    if (added.size() >= 8u) {
      break;
    }
  }
  std::string removed_note;
  size_t removed = 0;
  for (const std::string& prev : previous_ids) {
    bool still_present = false;
    for (const base::Value& value : candidates) {
      const base::Value::Dict* candidate = value.GetIfDict();
      if (candidate && CandidateIdentityKey(*candidate) == prev) {
        still_present = true;
        break;
      }
    }
    if (!still_present) {
      ++removed;
    }
  }
  if (removed > 0) {
    removed_note = base::StrCat(
        {"Removed/missing prior targets: ", base::NumberToString(removed),
         ".\n"});
  }
  std::string added_block;
  if (!added.empty()) {
    std::string serialized = "[]";
    base::JSONWriter::Write(added, &serialized);
    added_block = base::StrCat(
        {"New interactive candidates:\n",
         TruncateForPrompt(std::move(serialized), 3500u), "\n"});
  } else if (removed_note.empty()) {
    return BuildUnchangedContextNote(page_title, page_url);
  }
  return base::StrCat(
      {BuildMinimalPageHeaderBlock(page_title, page_url),
       "Context status: DELTA (same URL; interactive set changed).\n",
       removed_note, added_block,
       "Prior context still applies for unchanged targets.\n\n"});
}

// Builds page context for a model turn, preferring UNCHANGED/DELTA over a full
// candidate dump when the URL is stable and the interactive set barely moved.
std::string BuildAgentTurnContextCompact(
    const std::string& prompt,
    const std::string& selected_model,
    bool screenshot_attached,
    const std::string& page_title,
    const std::string& page_url,
    const base::Value::Dict& current_page_map,
    const base::Value::List& focus_areas,
    bool focus_only_context,
    int context_batch_index,
    bool include_page_context,
    bool force_full_context,
    const std::string& previous_context_url,
    const std::string& previous_context_digest,
    const std::set<std::string>& previous_candidate_ids,
    std::string* out_digest,
    std::set<std::string>* out_candidate_ids) {
  if (!include_page_context) {
    if (out_digest) {
      out_digest->clear();
    }
    if (out_candidate_ids) {
      out_candidate_ids->clear();
    }
    return BuildPrimaryGoalBlock(prompt) +
           BuildCapabilityNote(selected_model, screenshot_attached);
  }

  base::Value::Dict prompt_page_map =
      current_page_map.empty()
          ? base::Value::Dict()
          : BuildPromptPageMapForUrl(page_url, current_page_map);
  const base::Value::List focus_matches =
      ResolveFocusAreaMatches(focus_areas, prompt_page_map);
  const base::Value::List ranked_candidates =
      BuildRankedPromptCandidates(page_url, prompt_page_map, focus_matches);
  const std::set<std::string> candidate_ids =
      CollectCandidateIdentityKeys(ranked_candidates);
  const std::string digest =
      BuildContextDigest(page_url, page_title, candidate_ids);
  if (out_digest) {
    *out_digest = digest;
  }
  if (out_candidate_ids) {
    *out_candidate_ids = candidate_ids;
  }

  const bool url_changed =
      !previous_context_url.empty() && previous_context_url != page_url;
  const bool first_payload = previous_context_digest.empty();
  const bool digest_unchanged =
      !previous_context_digest.empty() && previous_context_digest == digest;

  const std::string prefix =
      BuildPrimaryGoalBlock(prompt) +
      BuildFocusOnlyContextNote(focus_only_context, context_batch_index) +
      BuildCapabilityNote(selected_model, screenshot_attached) +
      BuildSiteSpecificPromptNote(page_url) +
      BuildFocusAreasBlock(focus_areas, focus_matches);

  if (!force_full_context && !first_payload && !url_changed &&
      digest_unchanged) {
    return prefix + BuildUnchangedContextNote(page_title, page_url);
  }

  if (!force_full_context && !first_payload && !url_changed &&
      !previous_candidate_ids.empty()) {
    return prefix + BuildContextDeltaNote(page_title, page_url,
                                          ranked_candidates,
                                          previous_candidate_ids);
  }

  const bool include_candidate_batch =
      !focus_only_context || context_batch_index > 0;
  return prefix + BuildMinimalPageHeaderBlock(page_title, page_url) +
         (url_changed ? "Context status: FULL (URL changed).\n\n"
                      : "Context status: FULL.\n\n") +
         (include_candidate_batch
              ? BuildCandidateBatchBlock(
                    ranked_candidates,
                    static_cast<size_t>(context_batch_index > 0
                                            ? context_batch_index - 1
                                            : 0))
              : std::string());
}

std::string BuildChoiceMenuEntryJson(base::Value::Dict entry) {
  std::string serialized = "{}";
  if (!base::JSONWriter::Write(entry, &serialized) || serialized.empty()) {
    return "{}";
  }
  return TruncateForPrompt(std::move(serialized), 700u);
}

base::Value::List BuildLocatorsFromSelectorSummary(const base::Value::Dict& item) {
  base::Value::List locators;
  if (const base::Value::List* existing = item.FindList("locators")) {
    return SanitizeLocatorsForAgentExecution(*existing);
  }
  const base::Value::Dict* selectors = item.FindDict("selectors");
  if (!selectors) {
    return locators;
  }
  if (const std::string* css = selectors->FindString("css");
      css && !css->empty()) {
    base::Value::Dict locator;
    locator.Set("type", "CSS_SELECTOR");
    locator.Set("value", *css);
    locators.Append(std::move(locator));
  }
  if (const std::string* xpath = selectors->FindString("xpath");
      xpath && !xpath->empty()) {
    base::Value::Dict locator;
    locator.Set("type", "XPATH");
    locator.Set("value", *xpath);
    locators.Append(std::move(locator));
  }
  return SanitizeLocatorsForAgentExecution(locators);
}

base::Value::List BuildChoiceMenuItems(const std::string& page_url,
                                       const base::Value::Dict& page_map,
                                       const base::Value::List& focus_areas,
                                       int context_batch_index) {
  base::Value::List menu;
  if (page_map.empty()) {
    return menu;
  }

  const base::Value::Dict prompt_page_map =
      BuildPromptPageMapForUrl(page_url, page_map);
  const base::Value::List focus_matches =
      ResolveFocusAreaMatches(focus_areas, prompt_page_map);
  const base::Value::List ranked_candidates =
      BuildRankedPromptCandidates(page_url, prompt_page_map, focus_matches);
  std::set<std::string> seen_keys;
  int choice_number = 1;

  const auto append_item = [&](base::Value::Dict item,
                               const std::string& source,
                               const std::string& label,
                               const base::Value::List* locator_override) {
    if (menu.size() >= kChoiceMenuMaxElements) {
      return;
    }
    std::string dedupe_key =
        item.FindString("snapshotId") ? *item.FindString("snapshotId")
                                       : std::string();
    if (dedupe_key.empty()) {
      std::string serialized;
      base::JSONWriter::Write(item, &serialized);
      dedupe_key = serialized;
    }
    if (!seen_keys.insert(dedupe_key).second) {
      return;
    }
    if (!label.empty()) {
      item.Set("label", label);
    }
    item.Set("source", source);
    base::Value::List locators =
        locator_override ? SanitizeLocatorsForAgentExecution(*locator_override)
                         : BuildLocatorsFromSelectorSummary(item);
    if (locators.empty()) {
      locators = BuildLocatorsFromSelectorSummary(item);
    }
    if (!locators.empty()) {
      item.Set("locators", std::move(locators));
    }
    item.Set("choiceNumber", choice_number++);
    menu.Append(std::move(item));
  };

  size_t focus_count = 0u;
  if (context_batch_index <= 0) {
    for (const base::Value& area_value : focus_matches) {
      const base::Value::Dict* area = area_value.GetIfDict();
      const base::Value::List* matched =
          area ? area->FindList("matchedElements") : nullptr;
      const std::string label =
          area && area->FindString("label") ? *area->FindString("label")
                                            : std::string();
      const base::Value::List* area_locators =
          area ? area->FindList("locators") : nullptr;
      if (matched && !matched->empty()) {
        for (const base::Value& match_value : *matched) {
          const base::Value::Dict* match = match_value.GetIfDict();
          if (!match) {
            continue;
          }
          append_item(match->Clone(), "focus", label, area_locators);
          ++focus_count;
          if (menu.size() >= kChoiceMenuMaxElements) {
            return menu;
          }
        }
        continue;
      }
      if (area) {
        append_item(BuildFocusAreaFallbackSummary(*area), "focus_area", label,
                    area_locators);
        ++focus_count;
        if (menu.size() >= kChoiceMenuMaxElements) {
          return menu;
        }
      }
    }
  }

  const size_t first_batch_candidate_capacity =
      focus_count >= kChoiceMenuMaxElements
          ? 0u
          : (kChoiceMenuMaxElements - focus_count);
  const size_t candidate_offset =
      context_batch_index <= 0
          ? 0u
          : (first_batch_candidate_capacity +
             static_cast<size_t>(context_batch_index - 1) *
                 kChoiceMenuMaxElements);
  size_t candidate_index = 0u;
  for (const base::Value& candidate_value : ranked_candidates) {
    const base::Value::Dict* candidate = candidate_value.GetIfDict();
    if (!candidate) {
      continue;
    }
    if (candidate_index++ < candidate_offset) {
      continue;
    }
    append_item(candidate->Clone(), "candidate", std::string(), nullptr);
    if (menu.size() >= kChoiceMenuMaxElements) {
      break;
    }
  }
  return menu;
}

std::string BuildChoiceActionMenu(bool controls_menu_active, bool allow_screenshot) {
  if (!controls_menu_active) {
    return "Primary options:\n"
           "A : click target element as A<number>\n"
           "B : hover target element as B<number>\n"
           "C : type literal final text into target element as C<number>='text to insert'\n"
           "D : press a key as D='Enter'\n"
           "E : wait as E='1000'\n"
           "F : request more target candidates\n"
           "G : refresh page context\n"
           "H : open secondary controls\n"
           "I : finish as I='solved: summary' or I='blocked: reason'\n\n";
  }
  std::string menu =
      "Secondary controls:\n"
      "A : reload the current page\n"
      "B : inspect one element as B<number>\n"
      "C : save useful recent task actions as a script\n"
      "D : double click target element as D<number>\n"
      "E : scrape target element text as E<number>\n"
      "F : drag between two targets as F<from>-<to>\n"
      "G : go back to primary options\n";
  if (allow_screenshot) {
    menu = base::StrCat(
        {menu, "H : request screenshot as H<number> or H='fullPage'\n"});
  }
  return base::StrCat(
      {menu,
       "I : finish as I='solved: summary' or I='blocked: reason'\n\n"});
}

std::string BuildChoiceElementMenu(const base::Value::List& choice_menu) {
  if (choice_menu.empty()) {
    return "Element choices:\n<none>\n\n";
  }
  std::string out = "Element choices:\n";
  for (const base::Value& item_value : choice_menu) {
    const base::Value::Dict* item = item_value.GetIfDict();
    if (!item) {
      continue;
    }
    const int choice_number = item->FindInt("choiceNumber").value_or(0);
    if (choice_number <= 0) {
      continue;
    }
    base::Value::Dict rendered = item->Clone();
    rendered.Remove("choiceNumber");
    rendered.Remove("locators");
    rendered.Remove("source");
    out = base::StrCat(
        {out, base::NumberToString(choice_number), " : ",
         BuildChoiceMenuEntryJson(std::move(rendered)), "\n"});
  }
  out.push_back('\n');
  return out;
}

std::string BuildChoiceDeltaNote(const std::string& previous_page_title,
                                 const std::string& previous_page_url,
                                 const base::Value::List& previous_choice_menu,
                                 const std::string& current_page_title,
                                 const std::string& current_page_url,
                                 const base::Value::List& current_choice_menu) {
  std::string out;
  if (!previous_page_url.empty() || !previous_page_title.empty()) {
    if (previous_page_url != current_page_url ||
        previous_page_title != current_page_title) {
      out = base::StrCat(
          {out, "Context delta: page changed from ", previous_page_title, " (",
           previous_page_url, ") to ", current_page_title, " (", current_page_url,
           ").\n"});
    }
  }
  std::set<std::string> previous_ids;
  for (const base::Value& item_value : previous_choice_menu) {
    const base::Value::Dict* item = item_value.GetIfDict();
    const std::string* snapshot_id = item ? item->FindString("snapshotId") : nullptr;
    if (snapshot_id && !snapshot_id->empty()) {
      previous_ids.insert(*snapshot_id);
    }
  }
  base::Value::List new_targets;
  for (const base::Value& item_value : current_choice_menu) {
    const base::Value::Dict* item = item_value.GetIfDict();
    const std::string* snapshot_id = item ? item->FindString("snapshotId") : nullptr;
    if (snapshot_id && previous_ids.contains(*snapshot_id)) {
      continue;
    }
    new_targets.Append(item_value.Clone());
    if (new_targets.size() >= 3u) {
      break;
    }
  }
  if (!new_targets.empty()) {
    out = base::StrCat({out, "New likely targets:\n", BuildChoiceElementMenu(new_targets)});
  }
  return out;
}

std::string BuildChoiceTurnPrompt(const std::string& goal,
                                  const std::string& turn_note,
                                  std::string_view model_name,
                                  bool screenshot_attached,
                                  const std::string& page_title,
                                  const std::string& page_url,
                                  const base::Value::List& focus_areas,
                                  const base::Value::List& focus_matches,
                                  bool focus_only_context,
                                  int context_batch_index,
                                  const base::Value::List& choice_menu,
                                  bool controls_menu_active,
                                  bool allow_screenshot,
                                  const std::string& delta_note) {
  return base::StrCat(
      {"In a hypothetical situation where a user was trying to:\n",
       goal,
       "\n\nChoose the single best next browser move.\n\n",
       BuildFocusOnlyContextNote(focus_only_context, context_batch_index),
       BuildCapabilityNote(model_name, screenshot_attached),
       BuildSiteSpecificPromptNote(page_url),
       BuildFocusAreasBlock(focus_areas, focus_matches),
       BuildMinimalPageHeaderBlock(page_title, page_url),
       delta_note.empty() ? std::string() : base::StrCat({"Recent context delta:\n", delta_note, "\n"}),
       "Turn-specific context:\n",
       turn_note,
       "\n\n",
       BuildChoiceActionMenu(controls_menu_active, allow_screenshot),
       BuildChoiceElementMenu(choice_menu),
       "Rules:\n"
       "- Return only the decision payload for the multiple choice question.\n"
       "- No prose, no reasoning, no markdown.\n"
       "- You may return a short batch only when the sequence is obvious and does not require refresh, reload, screenshot, inspection, or branching.\n"
       "- If batching, separate steps with commas, for example A1,C2='hello',D='Tab',C3='secret',D='Enter'.\n"
       "- For C<number>='text', the value must be the literal final user-visible text to insert. Never restate instructions, element labels, target descriptions, or analysis notes.\n"
       "- If the action needs an element, append the element number directly.\n"
       "- If the current menu is insufficient, choose F/G/H from the primary menu or G from the secondary menu.\n"});
}

std::string BuildSaveScriptPrompt(const std::string& goal,
                                  const base::Value::List& task_actions) {
  std::string actions_block = "Attempted task actions:\n";
  for (size_t i = 0; i < task_actions.size(); ++i) {
    const base::Value::Dict* action = task_actions[i].GetIfDict();
    if (!action) {
      continue;
    }
    std::string rendered = "{}";
    base::JSONWriter::Write(*action, &rendered);
    actions_block = base::StrCat(
        {actions_block, base::NumberToString(static_cast<int>(i)), " : ",
         TruncateForPrompt(std::move(rendered), 420u), "\n"});
  }
  return base::StrCat(
      {"You are cleaning up a task run into a reusable script.\n"
       "Primary user goal:\n",
       goal,
       "\n\nPrune mistakes, retries, and dead ends. Keep only the useful action indexes that should become a reusable script.\n"
       "Return only the selected indexes and a short script name.\n\n",
       actions_block});
}

std::string BuildChoiceFollowupTurnNote(const base::Value::Dict& command,
                                        const base::Value::Dict& result,
                                        int step,
                                        int max_steps) {
  std::string command_json = "{}";
  std::string result_json = "{}";
  base::JSONWriter::Write(command, &command_json);
  base::JSONWriter::Write(result, &result_json);
  return base::StrCat(
      {"Autonomous continuation step ",
       base::NumberToString(step + 1),
       "/",
       base::NumberToString(max_steps),
       ".\nLast command:\n",
       TruncateForPrompt(std::move(command_json), 700u),
       "\n\nCommand result:\n",
       TruncateForPrompt(std::move(result_json), 900u),
       "\n\nContinue working toward the primary user goal.\n"});
}

std::string BuildJsonOnlyRetryPrompt(const std::string& prompt,
                                     const std::string& previous_response) {
  return base::StrCat(
      {"Primary user goal (restated verbatim):\n",
       prompt,
       "\n\nPrevious reply was not one executable JSON move.\n"
       "Reply with exactly one action object, e.g.\n"
       "{\"action\":\"click\",\"target_id\":1} or {\"action\":\"complete\","
       "\"status\":\"solved\",\"summary\":\"...\"}\n"
       "Plain text only if truly blocked.\n\n"
       "Previous reply:\n",
       TruncateForPrompt(previous_response, 800u), "\n\n"});
}

std::string BuildChoiceOnlyRetryPrompt(const std::string& previous_response) {
  return base::StrCat(
      {"Your previous reply did not match the required choice format.\n"
       "Reply with one valid choice payload only, such as A2, C3='text', "
       "D='Enter', G, H, or I='blocked: reason'.\n"
       "If you choose C, return only the literal final text to insert. Do not type instructions about what should be typed.\n\n"
       "Previous reply:\n",
       TruncateForPrompt(previous_response, 500u), "\n\n"});
}

std::string TryRepairTruncatedJsonObject(const std::string& input) {
  std::string repaired =
      std::string(base::TrimWhitespaceASCII(input, base::TRIM_ALL));
  if (repaired.empty() || repaired.front() != '{') {
    return std::string();
  }
  int brace_balance = 0;
  int bracket_balance = 0;
  bool in_string = false;
  bool escape = false;
  for (char ch : repaired) {
    if (escape) {
      escape = false;
      continue;
    }
    if (ch == '\\') {
      escape = true;
      continue;
    }
    if (ch == '"') {
      in_string = !in_string;
      continue;
    }
    if (in_string) {
      continue;
    }
    if (ch == '{') {
      ++brace_balance;
    } else if (ch == '}') {
      --brace_balance;
    } else if (ch == '[') {
      ++bracket_balance;
    } else if (ch == ']') {
      --bracket_balance;
    }
  }
  if (in_string) {
    repaired.push_back('"');
  }
  while (bracket_balance > 0) {
    repaired.push_back(']');
    --bracket_balance;
  }
  while (brace_balance > 0) {
    repaired.push_back('}');
    --brace_balance;
  }
  return repaired;
}

constexpr size_t kContextCandidateBatchSize = 5u;

int ScoreElementForPromptContext(const std::string& page_url,
                                 const base::Value::Dict& element) {
  int score = 0;
  const bool clickable = element.FindBool("clickable").value_or(false);
  const bool focusable = element.FindBool("focusable").value_or(false);
  const std::string role = TrimmedStringValue(element, {"role"});
  const std::string ancestor_role =
      TrimmedStringValue(element, {"interactiveAncestorRole"});
  const std::string tag = TrimmedStringValue(element, {"tag"});
  const std::string text = base::ToLowerASCII(ElementSummaryText(element));

  if (clickable) score += 20;
  if (focusable) score += 14;
  if (base::Contains(base::ToLowerASCII(role), "button") ||
      base::Contains(base::ToLowerASCII(ancestor_role), "button")) {
    score += 24;
  }
  if (base::Contains(base::ToLowerASCII(role), "link") ||
      base::Contains(base::ToLowerASCII(ancestor_role), "link")) {
    score += 8;
  }
  if (base::Contains(base::ToLowerASCII(role), "text") ||
      base::Contains(base::ToLowerASCII(role), "field") ||
      base::Contains(base::ToLowerASCII(ancestor_role), "textbox")) {
    score += 26;
  }
  if (base::EqualsCaseInsensitiveASCII(tag, "button")) score += 18;
  if (base::EqualsCaseInsensitiveASCII(tag, "input") ||
      base::EqualsCaseInsensitiveASCII(tag, "textarea")) {
    score += 20;
  }

  static constexpr const char* kGenericNeedles[] = {
      "reply", "post", "send", "submit", "comment", "composer", "textbox",
      "editor", "draft", "text", "input", "message",
  };
  if (DictContainsAnySubstring(element, kGenericNeedles)) {
    score += 18;
  }

  if (UrlHostMatches(page_url, "x.com") || UrlHostMatches(page_url, "www.x.com")) {
    if (ElementLooksRelevantForXPrompt(element)) {
      score += 40;
    }
    if (base::Contains(text, "reply") || base::Contains(text, "post")) {
      score += 10;
    }
  }

  if (score == 0 && !clickable && !focusable) {
    score -= 25;
  }
  return score;
}

std::set<std::string> CollectFocusMatchedSnapshotIds(
    const base::Value::List& matched_areas) {
  std::set<std::string> ids;
  for (const base::Value& area_value : matched_areas) {
    const base::Value::Dict* area = area_value.GetIfDict();
    const base::Value::List* matches = area ? area->FindList("matchedElements")
                                            : nullptr;
    if (!matches) {
      continue;
    }
    for (const base::Value& match_value : *matches) {
      const base::Value::Dict* match = match_value.GetIfDict();
      const std::string* snapshot_id =
          match ? match->FindString("snapshotId") : nullptr;
      if (snapshot_id && !snapshot_id->empty()) {
        ids.insert(*snapshot_id);
      }
    }
  }
  return ids;
}

base::Value::List BuildRankedPromptCandidates(const std::string& page_url,
                                              const base::Value::Dict& page_map,
                                              const base::Value::List& matched_areas) {
  base::Value::List candidates;
  const base::Value::List* frames = page_map.FindList("frames");
  if (!frames) {
    return candidates;
  }
  const std::set<std::string> excluded_ids =
      CollectFocusMatchedSnapshotIds(matched_areas);
  std::vector<std::pair<int, base::Value::Dict>> scored;
  std::set<std::string> seen_ids;
  for (const base::Value& frame_value : *frames) {
    const base::Value::Dict* frame = frame_value.GetIfDict();
    if (!frame) {
      continue;
    }
    const base::Value::List* elements = frame->FindList("elements");
    if (!elements) {
      continue;
    }
    for (const base::Value& element_value : *elements) {
      const base::Value::Dict* element = element_value.GetIfDict();
      if (!element) {
        continue;
      }
      const std::string snapshot_id = TrimmedStringValue(*element, {"id"});
      if (!snapshot_id.empty()) {
        if (excluded_ids.find(snapshot_id) != excluded_ids.end() ||
            !seen_ids.insert(snapshot_id).second) {
          continue;
        }
      }
      const int score = ScoreElementForPromptContext(page_url, *element);
      if (score <= 0) {
        continue;
      }
      base::Value::Dict summary =
          BuildPromptCandidateSummary(*element, base::Value::Dict(), &page_map);
      summary.Set("heuristicScore", score);
      scored.emplace_back(score, std::move(summary));
    }
  }

  std::stable_sort(scored.begin(), scored.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  for (auto& [score, summary] : scored) {
    candidates.Append(std::move(summary));
  }
  return candidates;
}

std::string BuildCandidateBatchBlock(const base::Value::List& candidates,
                                     size_t batch_index) {
  if (candidates.empty()) {
    return std::string();
  }
  const size_t offset = batch_index * kContextCandidateBatchSize;
  if (offset >= candidates.size()) {
    return "Additional ranked candidates: none remaining.\n\n";
  }

  base::Value::List batch;
  const size_t end = std::min(candidates.size(), offset + kContextCandidateBatchSize);
  for (size_t i = offset; i < end; ++i) {
    batch.Append(candidates[i].Clone());
  }
  std::string serialized = "[]";
  if (!base::JSONWriter::Write(batch, &serialized) || serialized.empty()) {
    serialized = "[]";
  }
  return base::StrCat(
      {"Ranked extra context candidates (batch ",
       base::NumberToString(batch_index + 1),
       ", showing ",
       base::NumberToString(offset + 1),
       "-",
       base::NumberToString(end),
       " of ",
       base::NumberToString(candidates.size()),
       "):\n",
       TruncateForPrompt(std::move(serialized), 5000u),
       "\n\n"});
}

bool UrlHostMatches(const std::string& url, std::string_view host) {
  const GURL gurl(url);
  if (!gurl.is_valid()) {
    return false;
  }
  return base::EqualsCaseInsensitiveASCII(gurl.host(), host);
}

bool DictContainsAnySubstring(const base::Value::Dict& dict,
                              base::span<const char* const> needles) {
  for (const auto item : dict) {
    const std::string& key = item.first;
    if (std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
          return !key.empty() &&
                 base::Contains(base::ToLowerASCII(key),
                                base::ToLowerASCII(std::string(needle)));
        })) {
      return true;
    }
    const base::Value& value = item.second;
    if (value.is_string()) {
      const std::string lowered = base::ToLowerASCII(value.GetString());
      if (std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
            return base::Contains(lowered,
                                  base::ToLowerASCII(std::string(needle)));
          })) {
        return true;
      }
    } else if (value.is_dict()) {
      if (DictContainsAnySubstring(value.GetDict(), needles)) {
        return true;
      }
    } else if (value.is_list()) {
      for (const base::Value& entry : value.GetList()) {
        if (entry.is_string()) {
          const std::string lowered = base::ToLowerASCII(entry.GetString());
          if (std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
                return base::Contains(lowered,
                                      base::ToLowerASCII(std::string(needle)));
              })) {
            return true;
          }
        } else if (entry.is_dict() &&
                   DictContainsAnySubstring(entry.GetDict(), needles)) {
          return true;
        }
      }
    }
  }
  return false;
}

base::Value::Dict BuildPromptPageMapForUrl(const std::string& page_url,
                                           const base::Value::Dict& page_map) {
  if (!UrlHostMatches(page_url, "x.com") && !UrlHostMatches(page_url, "www.x.com")) {
    return page_map.Clone();
  }

  static constexpr const char* kExcludedNeedles[] = {
      "grokdrawer",
      "chat-drawer-main",
      "grokdrawerheader",
      "chat drawer",
      "pin-code-input-container",
      "pin-title",
  };
  base::Value::Dict filtered = page_map.Clone();
  base::Value::List* frames = filtered.FindList("frames");
  if (!frames) {
    return filtered;
  }

  for (base::Value& frame_value : *frames) {
    if (!frame_value.is_dict()) {
      continue;
    }
    base::Value::Dict& frame = frame_value.GetDict();
    base::Value::List* elements = frame.FindList("elements");
    if (!elements) {
      continue;
    }

    base::Value::List kept;
    for (base::Value& element_value : *elements) {
      if (!element_value.is_dict()) {
        continue;
      }
      const base::Value::Dict& element = element_value.GetDict();
      if (DictContainsAnySubstring(element, kExcludedNeedles)) {
        continue;
      }
      kept.Append(element.Clone());
    }
    frame.Set("elements", std::move(kept));
  }

  base::Value::Dict* metadata = filtered.FindDict("metadata");
  if (!metadata) {
    filtered.Set("metadata", base::Value::Dict());
    metadata = filtered.FindDict("metadata");
  }
  metadata->Set("promptProfile", "x.com");
  metadata->Set("promptFilter", "exclude_grok_drawer_noise");
  return filtered;
}

std::string BuildSiteSpecificPromptNote(const std::string& page_url) {
  if (UrlHostMatches(page_url, "x.com") || UrlHostMatches(page_url, "www.x.com")) {
    return R"(Site-specific guidance for x.com:
- Ignore Grok drawers, side drawers, chat drawers, and unrelated overlays unless the user explicitly asked about them.
- Focus on the main tweet/article thread and reply flow for the current post.
- Reply composers on X are often contenteditable textboxes or rich editors, not plain textarea elements.
- If available, prefer the reply composer anchor [data-testid="tweetTextarea_0_label"] over unrelated text inputs or side-panel controls.
- If the goal is to comment/reply, first target the reply affordance for the active tweet, then refresh page context, then target the composer/editor, type the reply, and finally target the Post/Reply submit button.
- When typing a reply or post, type only the final user-visible reply text. Do not type task instructions, element labels, or target descriptions into the composer.
- Prefer tweet/reply/composer related elements over generic page chrome or side panels.

)";
  }
  return std::string();
}

std::string NormalizeOllamaEndpoint(std::string endpoint) {
  endpoint = base::TrimWhitespaceASCII(endpoint, base::TRIM_ALL);
  if (endpoint.empty()) {
    return kDefaultOllamaEndpoint;
  }
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  if (!endpoint.starts_with("http://") && !endpoint.starts_with("https://")) {
    endpoint = base::StrCat({"http://", endpoint});
  }
  return endpoint.empty() ? std::string(kDefaultOllamaEndpoint)
                          : endpoint;
}

std::string NormalizeHostedEndpoint(std::string endpoint,
                                    std::string_view fallback) {
  endpoint = base::TrimWhitespaceASCII(endpoint, base::TRIM_ALL);
  if (endpoint.empty()) {
    endpoint = std::string(fallback);
  }
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }
  if (!endpoint.starts_with("http://") && !endpoint.starts_with("https://")) {
    endpoint = base::StrCat({"https://", endpoint});
  }
  return endpoint.empty() ? std::string(fallback) : endpoint;
}

std::string NormalizeAgentProvider(std::string provider) {
  provider = base::ToLowerASCII(
      base::TrimWhitespaceASCII(provider, base::TRIM_ALL));
  if (provider == "cerebras" || provider == "gemini" || provider == "openai" ||
      provider == "anthropic" || provider == "grok") {
    return provider;
  }
  return kDefaultAgentProvider;
}

bool IsOllamaProvider(std::string_view provider) {
  return provider == "ollama";
}

bool IsOpenAICompatibleProvider(std::string_view provider) {
  return provider == "cerebras" || provider == "openai" || provider == "grok";
}

bool IsGeminiProvider(std::string_view provider) {
  return provider == "gemini";
}

bool IsAnthropicProvider(std::string_view provider) {
  return provider == "anthropic";
}

std::string RequiredApiKeyNameForProvider(std::string_view provider) {
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
  if (provider == "grok") {
    return "XAI_API_KEY";
  }
  return std::string();
}

std::string DefaultEndpointForProvider(std::string_view provider) {
  if (provider == "cerebras") {
    return kDefaultCerebrasEndpoint;
  }
  if (provider == "gemini") {
    return kDefaultGeminiEndpoint;
  }
  if (provider == "openai") {
    return kDefaultOpenAIEndpoint;
  }
  if (provider == "grok") {
    return kDefaultGrokEndpoint;
  }
  if (provider == "anthropic") {
    return kDefaultAnthropicEndpoint;
  }
  return kDefaultOllamaEndpoint;
}

std::string DefaultModelForProvider(std::string_view provider) {
  if (provider == "cerebras") {
    return kDefaultCerebrasModel;
  }
  if (provider == "gemini") {
    return kDefaultGeminiModel;
  }
  if (provider == "openai") {
    return kDefaultOpenAIModel;
  }
  if (provider == "grok") {
    return kDefaultGrokModel;
  }
  if (provider == "anthropic") {
    return kDefaultAnthropicModel;
  }
  return kDefaultDashboardModel;
}

std::string NormalizeDataKey(std::string key) {
  base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  if (key.size() >= 2 && key.front() == '{' && key.back() == '}') {
    key = key.substr(1, key.size() - 2);
    base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  }
  return key;
}

std::string DecodeSecretIniValue(const std::string& encoded_or_plain) {
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

std::optional<std::string> ReadSecretFromIni(const base::FilePath& path,
                                             std::string_view key) {
  if (path.empty() || !base::PathExists(path)) {
    return std::nullopt;
  }
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return std::nullopt;
  }
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
    const std::string parsed_key = NormalizeDataKey(line.substr(0, eq));
    if (parsed_key != key) {
      continue;
    }
    return DecodeSecretIniValue(line.substr(eq + 1));
  }
  return std::nullopt;
}

std::string ResolveProviderApiKey(const base::FilePath& profile_path,
                                  std::string_view provider) {
  const std::string key_name = RequiredApiKeyNameForProvider(provider);
  if (key_name.empty()) {
    return std::string();
  }
  if (!profile_path.empty()) {
    const base::FilePath secrets_path =
        profile_path.AppendASCII("SimulachromeData").AppendASCII("secrets.ini");
    std::optional<std::string> from_secret =
        ReadSecretFromIni(secrets_path, key_name);
    if (from_secret.has_value() && !from_secret->empty()) {
      return *from_secret;
    }
  }

  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> from_env = env->GetVar(key_name);
  if (from_env.has_value() && !from_env->empty()) {
    return *from_env;
  }
  return std::string();
}

std::string BuildGeminiGenerateContentEndpoint(std::string endpoint,
                                               std::string_view model) {
  endpoint = NormalizeHostedEndpoint(endpoint, kDefaultGeminiEndpoint);
  const std::string selected_model =
      model.empty() ? std::string(kDefaultGeminiModel) : std::string(model);
  return base::StrCat({endpoint, "/models/", selected_model, ":generateContent"});
}

bool UsesChoiceResponseFormat(const base::Value::Dict& config) {
  if (const std::string* response_mode = config.FindString("responseMode");
      response_mode && !response_mode->empty()) {
    return base::EqualsCaseInsensitiveASCII(*response_mode, "choice");
  }
  return true;
}

base::Value::Dict ReadConfiguredDashboardAgentConfigFromScriptsDir(
    const base::FilePath& scripts_dir,
    const base::FilePath& profile_path) {
  base::Value::Dict config;
  config.Set("provider", std::string(kDefaultAgentProvider));
  config.Set("model", std::string(kDefaultDashboardModel));
  config.Set("endpoint", DefaultEndpointForProvider(kDefaultAgentProvider));
  config.Set("responseMode", "choice");
  if (!scripts_dir.empty()) {
    const base::FilePath path = scripts_dir.AppendASCII("global_settings.json");
    std::string json;
    if (base::ReadFileToString(path, &json)) {
      std::optional<base::Value> parsed = base::JSONReader::Read(
          json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
      if (parsed && parsed->is_dict()) {
        const base::Value::Dict* agent = parsed->GetDict().FindDict("agent");
        if (agent) {
          const std::string provider = NormalizeAgentProvider(
              agent->FindString("provider")
                  ? *agent->FindString("provider")
                  : std::string());
          config.Set("provider", provider);
          config.Set("model", DefaultModelForProvider(provider));
          config.Set("endpoint", DefaultEndpointForProvider(provider));
          if (const std::string* model = agent->FindString("model");
              model && !model->empty()) {
            config.Set("model", *model);
          }
          if (IsOllamaProvider(provider)) {
            if (const std::string* endpoint = agent->FindString("ollamaEndpoint");
                endpoint && !endpoint->empty()) {
              config.Set("endpoint", NormalizeOllamaEndpoint(*endpoint));
            }
          } else {
            bool endpoint_from_provider_map = false;
            if (const base::Value::Dict* provider_endpoints =
                    agent->FindDict("providerEndpoints")) {
              if (const std::string* endpoint =
                      provider_endpoints->FindString(provider);
                  endpoint && !endpoint->empty()) {
                config.Set("endpoint",
                           NormalizeHostedEndpoint(
                               *endpoint, DefaultEndpointForProvider(provider)));
                endpoint_from_provider_map = true;
              }
            }
            if (!endpoint_from_provider_map) {
              if (const std::string* endpoint = agent->FindString("endpoint");
                  endpoint && !endpoint->empty()) {
                config.Set(
                    "endpoint",
                    NormalizeHostedEndpoint(
                        *endpoint, DefaultEndpointForProvider(provider)));
              }
            }
          }
          if (const std::string* response_mode = agent->FindString("responseMode");
              response_mode && !response_mode->empty()) {
            config.Set("responseMode", *response_mode);
          }
        }
      }
    }
  }

  const std::string provider =
      NormalizeAgentProvider(config.FindString("provider")
                                 ? *config.FindString("provider")
                                 : std::string());
  const std::string required_key = RequiredApiKeyNameForProvider(provider);
  if (!required_key.empty()) {
    config.Set("apiKeyName", required_key);
    std::string api_key = ResolveProviderApiKey(profile_path, provider);
    if (!api_key.empty()) {
      config.Set("apiKey", std::move(api_key));
    }
  }

  return config;
}

bool IsVisionCapableModel(std::string_view model_name) {
  const std::string lower = base::ToLowerASCII(model_name);
  return lower.find("vision") != std::string::npos ||
         lower.find("-vl") != std::string::npos ||
         lower.find(":vl") != std::string::npos ||
         lower.find("qwen-vl") != std::string::npos ||
         lower.find("qwen2-vl") != std::string::npos ||
         lower.find("qwen2.5-vl") != std::string::npos ||
         lower.find("llava") != std::string::npos ||
         lower.find("minicpm-v") != std::string::npos ||
         lower.find("internvl") != std::string::npos ||
         lower.find("phi-3-vision") != std::string::npos ||
         lower.find("moondream") != std::string::npos ||
         lower.find("gemma3") != std::string::npos ||
         lower.find("gemma4") != std::string::npos;
}

std::string BuildCapabilityNote(std::string_view model_name,
                                bool screenshot_attached) {
  if (IsVisionCapableModel(model_name)) {
    return screenshot_attached
               ? "Model capabilities: vision-capable model with a screenshot attached for this turn.\n\n"
               : "Model capabilities: vision-capable model, but no screenshot was attached for this turn. Do not claim to inspect page images you were not given. If visual evidence is required, issue request_screenshot sparingly and prefer a targeted screenshot request over a full-page one.\n\n";
  }
  return "Model capabilities: text-only session. You do not have image viewing capability in this turn. Do not claim to inspect screenshots or page images.\n\n";
}

std::string ExtractModelResponseText(const base::Value::Dict& response) {
  if (const base::Value::Dict* message = response.FindDict("message")) {
    if (const std::string* content = message->FindString("content")) {
      return *content;
    }
  }
  if (const std::string* response_text = response.FindString("response")) {
    return *response_text;
  }
  if (const std::string* delta = response.FindString("delta")) {
    return *delta;
  }
  return std::string();
}

std::optional<base::Value::Dict> ExtractJsonCommandFromText(
    const std::string& text) {
  const auto try_parse = [](const std::string& candidate)
      -> std::optional<base::Value::Dict> {
    std::optional<base::Value> parsed =
        base::JSONReader::Read(candidate, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    if (!parsed || !parsed->is_dict()) {
      return std::nullopt;
    }
    const base::Value::Dict& dict = parsed->GetDict();
    const std::string* command = dict.FindString("command");
    if (command && !command->empty()) {
      return dict.Clone();
    }
    const std::string* do_name = dict.FindString("do");
    if (do_name && !do_name->empty()) {
      return dict.Clone();
    }
    const std::string* action = dict.FindString("action");
    if (action && !action->empty()) {
      return dict.Clone();
    }
    return std::nullopt;
  };
  const auto try_parse_with_repair = [&](const std::string& candidate)
      -> std::optional<base::Value::Dict> {
    if (std::optional<base::Value::Dict> parsed = try_parse(candidate)) {
      return parsed;
    }
    const std::string repaired = TryRepairTruncatedJsonObject(candidate);
    if (repaired.empty() || repaired == candidate) {
      return std::nullopt;
    }
    return try_parse(repaired);
  };

  size_t search_pos = 0;
  while (true) {
    const size_t block_start = text.find("```json", search_pos);
    if (block_start == std::string::npos) {
      break;
    }
    const size_t payload_start = text.find('\n', block_start);
    if (payload_start == std::string::npos) {
      break;
    }
    const size_t block_end = text.find("```", payload_start + 1);
    if (block_end == std::string::npos) {
      break;
    }
    const std::string candidate(std::string(base::TrimWhitespaceASCII(
        text.substr(payload_start + 1, block_end - payload_start - 1),
        base::TRIM_ALL)));
    if (std::optional<base::Value::Dict> parsed = try_parse_with_repair(candidate)) {
      return std::optional<base::Value::Dict>(std::move(*parsed));
    }
    search_pos = block_end + 3;
  }

  return try_parse_with_repair(
      std::string(base::TrimWhitespaceASCII(text, base::TRIM_ALL)));
}

std::string ExtractOpenAICompatibleResponseText(const base::Value::Dict& response) {
  const base::Value::List* choices = response.FindList("choices");
  if (!choices || choices->empty()) {
    return std::string();
  }
  const base::Value::Dict* first = choices->front().GetIfDict();
  const base::Value::Dict* message = first ? first->FindDict("message") : nullptr;
  if (!message) {
    return std::string();
  }
  if (const std::string* content = message->FindString("content")) {
    return *content;
  }
  const base::Value::List* content_parts = message->FindList("content");
  if (!content_parts) {
    return std::string();
  }
  std::string combined;
  for (const base::Value& part_value : *content_parts) {
    const base::Value::Dict* part = part_value.GetIfDict();
    const std::string* text = part ? part->FindString("text") : nullptr;
    if (!text || text->empty()) {
      continue;
    }
    if (!combined.empty()) {
      combined.push_back('\n');
    }
    combined.append(*text);
  }
  return combined;
}

std::string ExtractGeminiResponseText(const base::Value::Dict& response) {
  const base::Value::List* candidates = response.FindList("candidates");
  if (!candidates || candidates->empty()) {
    return std::string();
  }
  const base::Value::Dict* first = candidates->front().GetIfDict();
  const base::Value::Dict* content = first ? first->FindDict("content") : nullptr;
  const base::Value::List* parts = content ? content->FindList("parts") : nullptr;
  if (!parts) {
    return std::string();
  }
  std::string combined;
  for (const base::Value& part_value : *parts) {
    const base::Value::Dict* part = part_value.GetIfDict();
    const std::string* text = part ? part->FindString("text") : nullptr;
    if (!text || text->empty()) {
      continue;
    }
    if (!combined.empty()) {
      combined.push_back('\n');
    }
    combined.append(*text);
  }
  return combined;
}

std::string ExtractAnthropicResponseText(const base::Value::Dict& response) {
  const base::Value::List* content = response.FindList("content");
  if (!content) {
    return std::string();
  }
  std::string combined;
  for (const base::Value& part_value : *content) {
    const base::Value::Dict* part = part_value.GetIfDict();
    const std::string* type = part ? part->FindString("type") : nullptr;
    if (!part || (type && *type != "text")) {
      continue;
    }
    const std::string* text = part->FindString("text");
    if (!text || text->empty()) {
      continue;
    }
    if (!combined.empty()) {
      combined.push_back('\n');
    }
    combined.append(*text);
  }
  return combined;
}

base::Value::Dict BuildChoiceTransportSchema(bool allow_pattern) {
  base::Value::Dict schema;
  schema.Set("type", "object");
  base::Value::Dict properties;
  base::Value::Dict answer;
  answer.Set("type", "string");
  if (allow_pattern) {
    answer.Set("pattern", kChoiceStructuredAnswerPattern);
  }
  properties.Set("answer", std::move(answer));
  schema.Set("properties", std::move(properties));
  base::Value::List required;
  required.Append("answer");
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

base::Value::Dict BuildSaveScriptTransportSchema() {
  base::Value::Dict schema;
  schema.Set("type", "object");
  base::Value::Dict properties;
  base::Value::Dict indexes;
  indexes.Set("type", "array");
  indexes.Set("minItems", 1);
  indexes.Set("uniqueItems", true);
  base::Value::Dict index_items;
  index_items.Set("type", "integer");
  index_items.Set("minimum", 0);
  indexes.Set("items", std::move(index_items));
  properties.Set("indexes", std::move(indexes));
  base::Value::Dict name;
  name.Set("type", "string");
  name.Set("minLength", 1);
  name.Set("maxLength", 80);
  properties.Set("name", std::move(name));
  schema.Set("properties", std::move(properties));
  base::Value::List required;
  required.Append("indexes");
  required.Append("name");
  schema.Set("required", std::move(required));
  schema.Set("additionalProperties", false);
  return schema;
}

std::optional<base::Value::Dict> ReadJsonDictFromText(const std::string& text) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(text, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    return std::nullopt;
  }
  return parsed->GetDict().Clone();
}

const base::Value::Dict* FindChoiceMenuItemByNumber(const base::Value::List& menu,
                                                    int choice_number) {
  if (choice_number <= 0) {
    return nullptr;
  }
  for (const base::Value& item_value : menu) {
    const base::Value::Dict* item = item_value.GetIfDict();
    if (!item) {
      continue;
    }
    if (item->FindInt("choiceNumber").value_or(0) == choice_number) {
      return item;
    }
  }
  return nullptr;
}

std::string NormalizeChoiceReply(std::string text) {
  text = std::string(base::TrimWhitespaceASCII(text, base::TRIM_ALL));
  if (text.empty()) {
    return text;
  }
  if (std::optional<base::Value::Dict> json_dict = ReadJsonDictFromText(text)) {
    if (const std::string* answer = json_dict->FindString("answer")) {
      return std::string(base::TrimWhitespaceASCII(*answer, base::TRIM_ALL));
    }
  }
  if (text.starts_with("```")) {
    const size_t first_newline = text.find('\n');
    if (first_newline != std::string::npos) {
      text = text.substr(first_newline + 1);
    }
    const size_t fence_end = text.find("```");
    if (fence_end != std::string::npos) {
      text = text.substr(0, fence_end);
    }
  }
  const size_t first_newline = text.find('\n');
  if (first_newline != std::string::npos) {
    text = text.substr(0, first_newline);
  }
  text = std::string(base::TrimWhitespaceASCII(text, base::TRIM_ALL));
  if (text.size() >= 2u &&
      ((text.front() == '\'' && text.back() == '\'') ||
       (text.front() == '"' && text.back() == '"') ||
       (text.front() == '`' && text.back() == '`'))) {
    text = text.substr(1, text.size() - 2);
  }
  return std::string(base::TrimWhitespaceASCII(text, base::TRIM_ALL));
}

std::vector<std::string> SplitChoiceSteps(const std::string& reply) {
  std::vector<std::string> out;
  std::string current;
  bool in_single = false;
  bool in_double = false;
  for (char ch : reply) {
    if (ch == '\'' && !in_double) {
      in_single = !in_single;
    } else if (ch == '"' && !in_single) {
      in_double = !in_double;
    }
    if (ch == ',' && !in_single && !in_double) {
      std::string piece = std::string(base::TrimWhitespaceASCII(
          current, base::TRIM_ALL));
      if (!piece.empty()) {
        out.push_back(std::move(piece));
      }
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  std::string tail = std::string(base::TrimWhitespaceASCII(current, base::TRIM_ALL));
  if (!tail.empty()) {
    out.push_back(std::move(tail));
  }
  return out;
}

std::optional<base::Value::Dict> ParseSaveScriptSelectionReply(
    const std::string& reply,
    size_t action_count) {
  if (std::optional<base::Value::Dict> json_dict = ReadJsonDictFromText(reply)) {
    const base::Value::List* indexes_list = json_dict->FindList("indexes");
    const std::string* name = json_dict->FindString("name");
    if (!indexes_list || !name || name->empty()) {
      return std::nullopt;
    }
    base::Value::List indexes;
    std::set<int> seen;
    for (const base::Value& value : *indexes_list) {
      const std::optional<int> maybe_index = value.GetIfInt();
      if (!maybe_index.has_value() || *maybe_index < 0 ||
          static_cast<size_t>(*maybe_index) >= action_count ||
          !seen.insert(*maybe_index).second) {
        return std::nullopt;
      }
      indexes.Append(*maybe_index);
    }
    if (indexes.empty()) {
      return std::nullopt;
    }
    base::Value::Dict command;
    command.Set("command", "save_task_script");
    base::Value::Dict payload;
    payload.Set("indexes", std::move(indexes));
    payload.Set("name", *name);
    command.Set("payload", std::move(payload));
    return command;
  }
  if (!base::StartsWith(reply, "SAVE[", base::CompareCase::INSENSITIVE_ASCII)) {
    return std::nullopt;
  }
  const size_t bracket_end = reply.find(']');
  if (bracket_end == std::string::npos || bracket_end <= 5u) {
    return std::nullopt;
  }
  const std::string index_block = reply.substr(5, bracket_end - 5);
  const std::vector<std::string> index_tokens =
      base::SplitString(index_block, ",", base::TRIM_WHITESPACE,
                        base::SPLIT_WANT_NONEMPTY);
  base::Value::List indexes;
  std::set<int> seen;
  for (const std::string& token : index_tokens) {
    int index = -1;
    if (!base::StringToInt(token, &index) || index < 0 ||
        static_cast<size_t>(index) >= action_count || !seen.insert(index).second) {
      return std::nullopt;
    }
    indexes.Append(index);
  }
  if (indexes.empty()) {
    return std::nullopt;
  }
  std::string name = "Saved Agent Script";
  const size_t name_pos = base::ToLowerASCII(reply).find("name=");
  if (name_pos != std::string::npos) {
    name = std::string(base::TrimWhitespaceASCII(
        reply.substr(name_pos + 5), base::TRIM_ALL));
    if (name.size() >= 2u &&
        ((name.front() == '\'' && name.back() == '\'') ||
         (name.front() == '"' && name.back() == '"'))) {
      name = name.substr(1, name.size() - 2);
    }
    if (name.empty()) {
      name = "Saved Agent Script";
    }
  }
  base::Value::Dict command;
  command.Set("command", "save_task_script");
  base::Value::Dict payload;
  payload.Set("indexes", std::move(indexes));
  payload.Set("name", name);
  command.Set("payload", std::move(payload));
  return command;
}

std::optional<base::Value::Dict> ExtractChoiceCommandFromText(
    const std::string& text,
    const base::Value::List& choice_menu,
    bool controls_menu_active,
    bool save_script_menu_active,
    bool allow_screenshot,
    size_t task_action_count) {
  const std::string reply = NormalizeChoiceReply(text);
  if (reply.empty()) {
    return std::nullopt;
  }
  if (save_script_menu_active) {
    return ParseSaveScriptSelectionReply(reply, task_action_count);
  }

  const std::vector<std::string> raw_steps = SplitChoiceSteps(reply);
  if (raw_steps.empty()) {
    return std::nullopt;
  }
  if (raw_steps.size() > kMaxChoiceBatchSteps) {
    return std::nullopt;
  }
  if (controls_menu_active && raw_steps.size() > 1u) {
    return std::nullopt;
  }

  base::Value::List timeline;
  auto build_action = [&](std::string action_name, base::Value::Dict params) {
    base::Value::Dict action;
    action.Set("action", std::move(action_name));
    action.Set("params", std::move(params));
    return action;
  };

  for (size_t step_index = 0; step_index < raw_steps.size(); ++step_index) {
    const std::string& step_text = raw_steps[step_index];
    size_t pos = 0;
    while (pos < step_text.size() && base::IsAsciiWhitespace(step_text[pos])) {
      ++pos;
    }
    if (pos >= step_text.size() || !base::IsAsciiAlpha(step_text[pos])) {
      return std::nullopt;
    }
    const char action_letter =
        static_cast<char>(base::ToUpperASCII(step_text[pos]));
    ++pos;
    while (pos < step_text.size() && base::IsAsciiWhitespace(step_text[pos])) {
      ++pos;
    }

    int element_number = 0;
    while (pos < step_text.size() && base::IsAsciiDigit(step_text[pos])) {
      element_number = element_number * 10 + (step_text[pos] - '0');
      ++pos;
    }
    int secondary_element_number = 0;
    if (controls_menu_active && pos < step_text.size() && step_text[pos] == '-') {
      ++pos;
      while (pos < step_text.size() && base::IsAsciiDigit(step_text[pos])) {
        secondary_element_number =
            secondary_element_number * 10 + (step_text[pos] - '0');
        ++pos;
      }
    }
    while (pos < step_text.size() && base::IsAsciiWhitespace(step_text[pos])) {
      ++pos;
    }

    std::string value;
    if (pos < step_text.size()) {
      if (step_text[pos] != '=') {
        return std::nullopt;
      }
      ++pos;
      value = std::string(
          base::TrimWhitespaceASCII(step_text.substr(pos), base::TRIM_ALL));
      if (value.size() >= 2u &&
          ((value.front() == '\'' && value.back() == '\'') ||
           (value.front() == '"' && value.back() == '"'))) {
        value = value.substr(1, value.size() - 2);
      } else if (!value.empty() &&
                 (value.front() == '\'' || value.front() == '"')) {
        value.erase(value.begin());
      }
    }

    const base::Value::Dict* target =
        element_number > 0 ? FindChoiceMenuItemByNumber(choice_menu, element_number)
                           : nullptr;
    const base::Value::Dict* secondary_target =
        secondary_element_number > 0
            ? FindChoiceMenuItemByNumber(choice_menu, secondary_element_number)
            : nullptr;
    const auto add_target_locators = [&](base::Value::Dict* params,
                                         const base::Value::Dict* item) {
      if (!params || !item) {
        return false;
      }
      const base::Value::List* locators = item->FindList("locators");
      if (!locators || locators->empty()) {
        return false;
      }
      params->Set("locators", locators->Clone());
      return true;
    };

    if (!controls_menu_active) {
      switch (action_letter) {
        case 'A': {
          if (!target) {
            return std::nullopt;
          }
          base::Value::Dict params;
          if (!add_target_locators(&params, target)) {
            return std::nullopt;
          }
          timeline.Append(build_action("click", std::move(params)));
          continue;
        }
        case 'B': {
          if (!target) {
            return std::nullopt;
          }
          base::Value::Dict params;
          if (!add_target_locators(&params, target)) {
            return std::nullopt;
          }
          timeline.Append(build_action("hover", std::move(params)));
          continue;
        }
        case 'C': {
          if (!target || value.empty()) {
            return std::nullopt;
          }
          base::Value::Dict params;
          params.Set("text", value);
          if (!add_target_locators(&params, target)) {
            return std::nullopt;
          }
          timeline.Append(build_action("type", std::move(params)));
          continue;
        }
        case 'D': {
          if (value.empty()) {
            return std::nullopt;
          }
          base::Value::Dict params;
          params.Set("key", value);
          timeline.Append(build_action("key_press", std::move(params)));
          continue;
        }
        case 'E': {
          base::Value::Dict params;
          int wait_ms = 1000;
          if (!value.empty() && !base::StringToInt(value, &wait_ms)) {
            return std::nullopt;
          }
          params.Set("waitType", "time");
          params.Set("time", wait_ms);
          timeline.Append(build_action("wait", std::move(params)));
          continue;
        }
        case 'F': {
          if (raw_steps.size() > 1u) {
            return std::nullopt;
          }
          base::Value::Dict command;
          command.Set("command", "request_more_context");
          command.Set("payload", base::Value::Dict());
          return command;
        }
        case 'G': {
          if (raw_steps.size() > 1u) {
            return std::nullopt;
          }
          base::Value::Dict command;
          command.Set("command", "refresh_pagemap");
          command.Set("payload", base::Value::Dict());
          return command;
        }
        case 'H': {
          if (raw_steps.size() > 1u) {
            return std::nullopt;
          }
          base::Value::Dict command;
          command.Set("command", "request_more_controls");
          command.Set("payload", base::Value::Dict());
          return command;
        }
        case 'I': {
          if (raw_steps.size() > 1u) {
            return std::nullopt;
          }
          base::Value::Dict payload;
          std::string status = "solved";
          std::string summary = value;
          if (base::StartsWith(summary, "blocked:",
                               base::CompareCase::INSENSITIVE_ASCII)) {
            status = "blocked";
            summary = std::string(base::TrimWhitespaceASCII(
                summary.substr(std::string("blocked:").size()), base::TRIM_ALL));
          } else if (base::StartsWith(summary, "solved:",
                                      base::CompareCase::INSENSITIVE_ASCII)) {
            summary = std::string(base::TrimWhitespaceASCII(
                summary.substr(std::string("solved:").size()), base::TRIM_ALL));
          }
          payload.Set("status", status);
          payload.Set("summary", summary.empty() ? "Finished." : summary);
          base::Value::Dict command;
          command.Set("command", "agent_finish");
          command.Set("payload", std::move(payload));
          return command;
        }
        default:
          return std::nullopt;
      }
    }

    switch (action_letter) {
      case 'A': {
        base::Value::Dict command;
        command.Set("command", "reload_page");
        command.Set("payload", base::Value::Dict());
        return command;
      }
      case 'B': {
        if (!target) {
          return std::nullopt;
        }
        base::Value::Dict command;
        command.Set("command", "inspect_element");
        base::Value::Dict payload;
        if (const std::string* snapshot_id = target->FindString("snapshotId");
            snapshot_id && !snapshot_id->empty()) {
          payload.Set("snapshotId", *snapshot_id);
        }
        if (const base::Value::List* locators = target->FindList("locators")) {
          payload.Set("locators", locators->Clone());
        }
        command.Set("payload", std::move(payload));
        return command;
      }
      case 'C': {
        base::Value::Dict command;
        command.Set("command", "save_task_script");
        command.Set("payload", base::Value::Dict());
        return command;
      }
      case 'D': {
        if (!target) {
          return std::nullopt;
        }
        base::Value::Dict params;
        params.Set("clickCount", 2);
        if (!add_target_locators(&params, target)) {
          return std::nullopt;
        }
        timeline.Append(build_action("click", std::move(params)));
        break;
      }
      case 'E': {
        if (!target) {
          return std::nullopt;
        }
        base::Value::Dict params;
        if (!add_target_locators(&params, target)) {
          return std::nullopt;
        }
        params.Set("variableName", "__agent_scraped_text");
        params.Set("dataType", "text");
        timeline.Append(build_action("scrape_element", std::move(params)));
        break;
      }
      case 'F': {
        if (!target || !secondary_target) {
          return std::nullopt;
        }
        base::Value::Dict params;
        if (!add_target_locators(&params, target)) {
          return std::nullopt;
        }
        const base::Value::List* to_locators = secondary_target->FindList("locators");
        if (!to_locators || to_locators->empty()) {
          return std::nullopt;
        }
        params.Set("fromLocators", params.FindList("locators")->Clone());
        params.Remove("locators");
        params.Set("toLocators", to_locators->Clone());
        timeline.Append(build_action("drag", std::move(params)));
        break;
      }
      case 'G': {
        base::Value::Dict command;
        command.Set("command", "request_primary_options");
        command.Set("payload", base::Value::Dict());
        return command;
      }
      case 'H': {
        if (!allow_screenshot) {
          return std::nullopt;
        }
        base::Value::Dict command;
        command.Set("command", "request_screenshot");
        base::Value::Dict payload;
        if (target) {
          if (const std::string* snapshot_id = target->FindString("snapshotId");
              snapshot_id && !snapshot_id->empty()) {
            payload.Set("snapshotId", *snapshot_id);
          } else if (const base::Value::List* locators = target->FindList("locators");
                     locators && !locators->empty()) {
            payload.Set("locators", locators->Clone());
          } else {
            return std::nullopt;
          }
        } else if (base::EqualsCaseInsensitiveASCII(value, "fullPage")) {
          payload.Set("fullPage", true);
        } else {
          return std::nullopt;
        }
        command.Set("payload", std::move(payload));
        return command;
      }
      case 'I': {
        base::Value::Dict payload;
        std::string status = "solved";
        std::string summary = value;
        if (base::StartsWith(summary, "blocked:",
                             base::CompareCase::INSENSITIVE_ASCII)) {
          status = "blocked";
          summary = std::string(base::TrimWhitespaceASCII(
              summary.substr(std::string("blocked:").size()), base::TRIM_ALL));
        } else if (base::StartsWith(summary, "solved:",
                                    base::CompareCase::INSENSITIVE_ASCII)) {
          summary = std::string(base::TrimWhitespaceASCII(
              summary.substr(std::string("solved:").size()), base::TRIM_ALL));
        }
        payload.Set("status", status);
        payload.Set("summary", summary.empty() ? "Finished." : summary);
        base::Value::Dict command;
        command.Set("command", "agent_finish");
        command.Set("payload", std::move(payload));
        return command;
      }
      default:
        return std::nullopt;
    }
  }
  if (timeline.empty()) {
    return std::nullopt;
  }
  if (timeline.size() == 1u) {
    base::Value::Dict command;
    command.Set("command", "call_action");
    command.Set("payload", timeline.front().GetDict().Clone());
    return command;
  }
  base::Value::Dict command;
  command.Set("command", "run_timeline");
  base::Value::Dict payload;
  payload.Set("timeline", std::move(timeline));
  command.Set("payload", std::move(payload));
  return command;
}

base::Value::List BuildLocatorsFromElementSelectors(const base::Value::Dict& element) {
  base::Value::List locators;
  const base::Value::Dict* selectors = element.FindDict("selectors");
  if (!selectors) {
    return locators;
  }
  if (const std::string* css = selectors->FindString("css");
      css && !css->empty()) {
    base::Value::Dict locator;
    locator.Set("type", "CSS_SELECTOR");
    locator.Set("value", *css);
    locators.Append(std::move(locator));
  }
  if (const std::string* xpath = selectors->FindString("xpath");
      xpath && !xpath->empty()) {
    base::Value::Dict locator;
    locator.Set("type", "XPATH");
    locator.Set("value", *xpath);
    locators.Append(std::move(locator));
  }
  return SanitizeLocatorsForAgentExecution(locators);
}

const base::Value::Dict* FindFocusAreaByLabel(const base::Value::List& focus_areas,
                                              const std::string& label) {
  if (label.empty()) {
    return nullptr;
  }
  const std::string lowered_label = base::ToLowerASCII(label);
  const base::Value::Dict* partial_match = nullptr;
  for (const base::Value& area_value : focus_areas) {
    const base::Value::Dict* area = area_value.GetIfDict();
    const std::string* area_label = area ? area->FindString("label") : nullptr;
    if (!area_label) {
      continue;
    }
    const std::string lowered_area_label = base::ToLowerASCII(*area_label);
    if (lowered_area_label == lowered_label) {
      return area;
    }
    if (!partial_match &&
        (base::Contains(lowered_area_label, lowered_label) ||
         base::Contains(lowered_label, lowered_area_label))) {
      partial_match = area;
    }
  }
  return partial_match;
}

const base::Value::Dict* FindElementBySnapshotId(const base::Value::Dict& page_map,
                                                 const std::string& snapshot_id) {
  const base::Value::List* frames = page_map.FindList("frames");
  if (!frames || snapshot_id.empty()) {
    return nullptr;
  }
  for (const base::Value& frame_value : *frames) {
    const base::Value::Dict* frame = frame_value.GetIfDict();
    const base::Value::List* elements = frame ? frame->FindList("elements") : nullptr;
    if (!elements) {
      continue;
    }
    for (const base::Value& element_value : *elements) {
      const base::Value::Dict* element = element_value.GetIfDict();
      if (!element) {
        continue;
      }
      const std::string* id = element->FindString("id");
      if (id && *id == snapshot_id) {
        return element;
      }
    }
  }
  return nullptr;
}

const base::Value::Dict* FindElementByLocators(const base::Value::Dict& page_map,
                                               const base::Value::List& locators) {
  const base::Value::List* frames = page_map.FindList("frames");
  if (!frames || locators.empty()) {
    return nullptr;
  }
  for (const base::Value& frame_value : *frames) {
    const base::Value::Dict* frame = frame_value.GetIfDict();
    const base::Value::List* elements = frame ? frame->FindList("elements")
                                              : nullptr;
    if (!elements) {
      continue;
    }
    for (const base::Value& element_value : *elements) {
      const base::Value::Dict* element = element_value.GetIfDict();
      if (!element) {
        continue;
      }
      for (const base::Value& locator_value : locators) {
        const base::Value::Dict* locator = locator_value.GetIfDict();
        if (locator && LocatorMatchesCandidate(*locator, *element)) {
          return element;
        }
      }
    }
  }
  return nullptr;
}

std::optional<base::Value::Dict> ResolveElementFromCommandPayload(
    const base::Value::Dict& payload,
    const base::Value::List& focus_areas,
    const base::Value::Dict& page_map) {
  const base::Value::Dict* element = nullptr;
  for (const char* key :
       {"snapshotId", "snapshot_id", "elementId", "element_id", "id"}) {
    if (const std::string* snapshot_id = payload.FindString(key);
        snapshot_id && !snapshot_id->empty()) {
      element = FindElementBySnapshotId(page_map, *snapshot_id);
      if (element) {
        return element->Clone();
      }
    }
  }
  if (const base::Value::List* locators = payload.FindList("locators");
      locators && !locators->empty()) {
    element = FindElementByLocators(page_map, *locators);
    if (element) {
      return element->Clone();
    }
  }
  if (const std::string* label = payload.FindString("label");
      label && !label->empty()) {
    if (const base::Value::Dict* area = FindFocusAreaByLabel(focus_areas, *label)) {
      if (const base::Value::List* locators = area->FindList("locators")) {
        element = FindElementByLocators(page_map, *locators);
        if (element) {
          return element->Clone();
        }
      }
    }
  }
  return std::nullopt;
}

std::optional<gfx::RectF> ReadElementBounds(const base::Value::Dict& element) {
  const base::Value::Dict* bounds = element.FindDict("bounds");
  if (!bounds) {
    return std::nullopt;
  }
  const float x = static_cast<float>(bounds->FindDouble("x").value_or(0.0));
  const float y = static_cast<float>(bounds->FindDouble("y").value_or(0.0));
  const float w = static_cast<float>(bounds->FindDouble("w").value_or(0.0));
  const float h = static_cast<float>(bounds->FindDouble("h").value_or(0.0));
  if (w <= 1.0f || h <= 1.0f) {
    return std::nullopt;
  }
  return gfx::RectF(x, y, w, h);
}

std::string BuildElementInspectionNote(const base::Value::Dict& element) {
  base::Value::Dict summary =
      BuildPromptCandidateSummary(element, base::Value::Dict(), nullptr);
  std::string summary_json = "{}";
  base::JSONWriter::Write(summary, &summary_json);
  return base::StrCat(
      {"Inspected element details:\n",
       TruncateForPrompt(std::move(summary_json), 1200u),
       "\n\nUse this only if it materially changes the next decision.\n"});
}

bool ShouldOfferScreenshotOption(std::string_view model_name,
                                 const std::string& goal,
                                 const base::Value::Dict& page_map,
                                 const base::Value::List& focus_areas,
                                 const base::Value::List& choice_menu,
                                 bool screenshot_attached) {
  if (screenshot_attached || !IsVisionCapableModel(model_name)) {
    return false;
  }
  if (choice_menu.empty()) {
    return true;
  }
  const std::string lowered_goal = base::ToLowerASCII(goal);
  static constexpr const char* kVisualNeedles[] = {
      "image", "screenshot", "look", "see", "visual", "layout", "color",
      "icon", "chart", "diagram", "canvas", "video",
  };
  for (const char* needle : kVisualNeedles) {
    if (base::Contains(lowered_goal, needle)) {
      return true;
    }
  }
  if (!focus_areas.empty()) {
    return false;
  }
  return !FindProminentOverlaySummary(page_map).empty();
}

base::Value::Dict BuildRecordedTaskAction(const base::Value::Dict& action,
                                          int task_action_index,
                                          int turn_index,
                                          const std::string& execution,
                                          bool from_batch) {
  base::Value::Dict recorded = action.Clone();
  recorded.Set("taskActionIndex", task_action_index);
  recorded.Set("turnIndex", turn_index);
  recorded.Set("execution", execution);
  recorded.Set("fromBatch", from_batch);
  return recorded;
}

base::Value::List BuildSavedTimelineFromIndexes(const base::Value::List& task_actions,
                                                const base::Value::List& indexes) {
  base::Value::List timeline;
  for (const base::Value& index_value : indexes) {
    const std::optional<int> maybe_index = index_value.GetIfInt();
    if (!maybe_index.has_value() || *maybe_index < 0 ||
        static_cast<size_t>(*maybe_index) >= task_actions.size()) {
      continue;
    }
    const base::Value::Dict* recorded = task_actions[*maybe_index].GetIfDict();
    const std::string* action_name = recorded ? recorded->FindString("action") : nullptr;
    if (!recorded || !action_name || action_name->empty()) {
      continue;
    }
    base::Value::Dict action;
    action.Set("action", *action_name);
    if (const base::Value::Dict* params = recorded->FindDict("params")) {
      action.Set("params", params->Clone());
    } else {
      action.Set("params", base::Value::Dict());
    }
    timeline.Append(std::move(action));
  }
  return timeline;
}

base::Value::List BuildSucceededTimeline(const base::Value::List& task_actions) {
  base::Value::List indexes;
  for (size_t i = 0; i < task_actions.size(); ++i) {
    const base::Value::Dict* recorded = task_actions[i].GetIfDict();
    if (!recorded) {
      continue;
    }
    const std::string* execution = recorded->FindString("execution");
    if (execution && base::EqualsCaseInsensitiveASCII(*execution, "succeeded")) {
      indexes.Append(static_cast<int>(i));
    }
  }
  return BuildSavedTimelineFromIndexes(task_actions, indexes);
}

std::string BuildPostTaskScriptReviewPrompt(const std::string& goal,
                                            const base::Value::List& task_actions,
                                            const std::string& draft_script_name) {
  return base::StrCat(
      {"The primary user goal is complete. A draft reusable script named '",
       draft_script_name,
       "' was auto-saved from successful steps only.\n"
       "You may prune mistake/retry steps now. Do not invent new browser moves.\n"
       "Reply with either:\n"
       "{\"action\":\"save_task_script\",\"indexes\":[0,2,3],\"name\":\"",
       draft_script_name,
       "\"}\n"
       "or {\"action\":\"complete\",\"status\":\"solved\",\"summary\":"
       "\"keep draft as-is\"}.\n\n",
       BuildSaveScriptPrompt(goal, task_actions)});
}

std::string SanitizeSavedScriptName(std::string name) {
  name = std::string(base::TrimWhitespaceASCII(name, base::TRIM_ALL));
  if (name.empty()) {
    return "Saved Agent Script";
  }
  if (name.size() > 80u) {
    name.resize(80u);
  }
  return name;
}

base::Value::Dict BuildScreenshotRequestMetadata(
    const base::Value::Dict* element,
    const base::Value::Dict& payload) {
  base::Value::Dict metadata;
  metadata.Set("scope", element ? "targeted" : "viewport");
  if (payload.FindBool("fullPage").value_or(false)) {
    metadata.Set("requested_full_page", true);
  }
  if (!element) {
    return metadata;
  }
  if (const std::string* id = element->FindString("id"); id && !id->empty()) {
    metadata.Set("snapshotId", *id);
  }
  if (const std::string* label = payload.FindString("label");
      label && !label->empty()) {
    metadata.Set("label", *label);
  }
  if (const std::string* text = element->FindString("text");
      text && !text->empty()) {
    metadata.Set("text", TruncateForPrompt(*text, 120u));
  } else if (const std::string* ax_name =
                 element->FindString("accessibilityName");
             ax_name && !ax_name->empty()) {
    metadata.Set("text", TruncateForPrompt(*ax_name, 120u));
  }
  if (const std::string* role = element->FindString("role");
      role && !role->empty()) {
    metadata.Set("role", *role);
  }
  return metadata;
}

std::pair<std::optional<gfx::RectF>, base::Value::Dict> ResolveScreenshotRequest(
    const base::Value::Dict& payload,
    const base::Value::List& focus_areas,
    const base::Value::Dict& page_map) {
  if (payload.FindBool("fullPage").value_or(false)) {
    return {std::nullopt, BuildScreenshotRequestMetadata(nullptr, payload)};
  }

  const base::Value::Dict* element = nullptr;
  for (const char* key :
       {"snapshotId", "snapshot_id", "elementId", "element_id", "id"}) {
    if (const std::string* snapshot_id = payload.FindString(key);
        snapshot_id && !snapshot_id->empty()) {
      element = FindElementBySnapshotId(page_map, *snapshot_id);
      if (element) {
        break;
      }
    }
  }
  if (!element) {
    if (const std::string* label = payload.FindString("label");
        label && !label->empty()) {
      if (const base::Value::Dict* area = FindFocusAreaByLabel(focus_areas, *label)) {
        if (const base::Value::List* locators = area->FindList("locators")) {
          element = FindElementByLocators(page_map, *locators);
        }
      }
    }
  }
  if (!element) {
    if (const base::Value::List* locators = payload.FindList("locators")) {
      element = FindElementByLocators(page_map, *locators);
    }
  }
  if (!element && payload.empty() && focus_areas.size() == 1u) {
    const base::Value::Dict* area = focus_areas.front().GetIfDict();
    const base::Value::List* locators = area ? area->FindList("locators")
                                             : nullptr;
    if (locators) {
      element = FindElementByLocators(page_map, *locators);
    }
  }

  base::Value::Dict metadata = BuildScreenshotRequestMetadata(element, payload);
  if (!element) {
    return {std::nullopt, std::move(metadata)};
  }
  return {ReadElementBounds(*element), std::move(metadata)};
}


base::Value::Dict ResolveElementIdTargetsInCommand(
    const base::Value::Dict& command,
    const base::Value::Dict& page_map) {
  base::Value::Dict resolved = command.Clone();
  base::Value::Dict* payload = resolved.FindDict("payload");
  if (!payload) {
    return resolved;
  }
  base::Value::Dict* params = payload->FindDict("params");
  if (!params) {
    return resolved;
  }
  const base::Value::List* locators = params->FindList("locators");
  if (locators && !locators->empty()) {
    return resolved;
  }
  std::string snapshot_id;
  if (std::optional<int> target_id = params->FindInt("target_id")) {
    snapshot_id = base::NumberToString(*target_id);
  }
  for (const char* key : {"element_id", "elementId", "snapshot_id", "snapshotId", "id"}) {
    if (!snapshot_id.empty()) {
      break;
    }
    if (const std::string* value = params->FindString(key);
        value && !value->empty()) {
      snapshot_id = *value;
      break;
    }
  }
  if (snapshot_id.empty()) {
    return resolved;
  }
  const base::Value::Dict* element = FindElementBySnapshotId(page_map, snapshot_id);
  if (!element) {
    return resolved;
  }
  base::Value::List resolved_locators = BuildLocatorsFromElementSelectors(*element);
  if (resolved_locators.empty()) {
    return resolved;
  }
  params->Set("locators", std::move(resolved_locators));
  params->Remove("target_id");
  params->Remove("element_id");
  params->Remove("elementId");
  params->Remove("snapshot_id");
  params->Remove("snapshotId");
  params->Remove("id");
  return resolved;
}

base::Value::Dict ResolveFocusAreaLabelTargetsInCommand(
    const base::Value::Dict& command,
    const base::Value::List& focus_areas) {
  base::Value::Dict resolved = command.Clone();
  base::Value::Dict* payload = resolved.FindDict("payload");
  if (!payload) {
    return resolved;
  }
  const std::string* action_name = payload->FindString("action");
  base::Value::Dict* params = payload->FindDict("params");
  if (!params) {
    return resolved;
  }
  const base::Value::List* locators = params->FindList("locators");
  if (locators && !locators->empty()) {
    return resolved;
  }

  const std::string* label = params->FindString("label");
  if (!label || label->empty()) {
    return resolved;
  }
  const base::Value::Dict* area = FindFocusAreaByLabel(focus_areas, *label);
  if (!area) {
    return resolved;
  }
  const base::Value::List* area_locators = area->FindList("locators");
  if (!area_locators || area_locators->empty()) {
    return resolved;
  }
  base::Value::List sanitized = SanitizeLocatorsForAgentExecution(*area_locators);
  if (sanitized.empty()) {
    return resolved;
  }
  params->Set("locators", std::move(sanitized));
  if (action_name && base::EqualsCaseInsensitiveASCII(*action_name, "scrape_element")) {
    if (!params->Find("variableName")) {
      params->Set("variableName", "__agent_scraped_text");
    }
    if (!params->Find("dataType")) {
      params->Set("dataType", "text");
    }
  }
  params->Remove("label");
  return resolved;
}

bool ActionLikelyChangesPageFocusOrInteractiveState(const std::string& action) {
  // Only auto-refresh after navigation-class actions. Typing username→password
  // →submit must NOT pull a fresh APC payload every turn; the model can call
  // look when it needs an update (captcha/overlay already visible on first
  // context is enough until URL/DOM identity changes).
  const std::string lower = base::ToLowerASCII(action);
  return lower == "navigate" || lower == "goto" || lower == "open" ||
         lower == "open_url" || lower == "reload" || lower == "reload_page" ||
         lower == "back" || lower == "forward";
}

bool TimelineLikelyChangesPageFocusOrInteractiveState(
    const base::Value::Dict& payload) {
  const base::Value::List* timeline = payload.FindList("timeline");
  if (!timeline) {
    timeline = payload.FindList("actions");
  }
  if (!timeline) {
    return false;
  }
  for (const base::Value& action_value : *timeline) {
    const base::Value::Dict* action = action_value.GetIfDict();
    if (!action) {
      continue;
    }
    const std::string* name = action->FindString("action");
    if (name && ActionLikelyChangesPageFocusOrInteractiveState(*name)) {
      return true;
    }
  }
  return false;
}

std::optional<gfx::RectF> FindViewportBounds(const base::Value::Dict& page_map) {
  const base::Value::List* frames = page_map.FindList("frames");
  if (!frames) {
    return std::nullopt;
  }
  std::optional<gfx::RectF> best;
  for (const base::Value& frame_value : *frames) {
    const base::Value::Dict* frame = frame_value.GetIfDict();
    if (!frame) {
      continue;
    }
    const base::Value::List* elements = frame->FindList("elements");
    if (!elements) {
      continue;
    }
    for (const base::Value& element_value : *elements) {
      const base::Value::Dict* element = element_value.GetIfDict();
      const base::Value::Dict* bounds = element ? element->FindDict("bounds")
                                                : nullptr;
      if (!bounds) {
        continue;
      }
      const double x = bounds->FindDouble("x").value_or(0.0);
      const double y = bounds->FindDouble("y").value_or(0.0);
      const double w = bounds->FindDouble("w").value_or(0.0);
      const double h = bounds->FindDouble("h").value_or(0.0);
      if (w <= 0.0 || h <= 0.0) {
        continue;
      }
      gfx::RectF rect(x, y, w, h);
      if (!best.has_value() || rect.width() * rect.height() >
                                   best->width() * best->height()) {
        best = rect;
      }
    }
  }
  return best;
}

std::string BuildOverlayLabel(const base::Value::Dict& element) {
  for (const char* key : {"accessibilityName", "text", "type", "role", "tag"}) {
    if (const std::string* value = element.FindString(key);
        value && !value->empty()) {
      return TruncateForPrompt(*value, 120u);
    }
  }
  if (const base::Value::Dict* selectors = element.FindDict("selectors")) {
    for (const char* key : {"css", "resourcePath", "xpath"}) {
      if (const std::string* value = selectors->FindString(key);
          value && !value->empty()) {
        return TruncateForPrompt(*value, 120u);
      }
    }
  }
  return "unknown overlay";
}

std::string FindProminentOverlaySummary(const base::Value::Dict& page_map) {
  const std::optional<gfx::RectF> viewport = FindViewportBounds(page_map);
  if (!viewport.has_value() || viewport->width() <= 0.0f ||
      viewport->height() <= 0.0f) {
    return std::string();
  }

  const float viewport_center_x = viewport->x() + viewport->width() / 2.0f;
  const float viewport_center_y = viewport->y() + viewport->height() / 2.0f;
  const base::Value::List* frames = page_map.FindList("frames");
  if (!frames) {
    return std::string();
  }

  static constexpr const char* kOverlayNeedles[] = {
      "dialog", "modal", "popover", "sheet", "keyboard shortcuts",
      "shortcut help", "keyboard_shortcuts", "drawer", "menu",
  };

  const base::Value::Dict* best_element = nullptr;
  float best_score = 0.0f;
  for (const base::Value& frame_value : *frames) {
    const base::Value::Dict* frame = frame_value.GetIfDict();
    if (!frame) {
      continue;
    }
    const base::Value::List* elements = frame->FindList("elements");
    if (!elements) {
      continue;
    }
    for (const base::Value& element_value : *elements) {
      const base::Value::Dict* element = element_value.GetIfDict();
      const base::Value::Dict* bounds = element ? element->FindDict("bounds")
                                                : nullptr;
      if (!element || !bounds) {
        continue;
      }
      const float x = bounds->FindDouble("x").value_or(0.0);
      const float y = bounds->FindDouble("y").value_or(0.0);
      const float w = bounds->FindDouble("w").value_or(0.0);
      const float h = bounds->FindDouble("h").value_or(0.0);
      if (w <= viewport->width() * 0.18f || h <= viewport->height() * 0.12f) {
        continue;
      }
      const float center_x = x + w / 2.0f;
      const float center_y = y + h / 2.0f;
      const float dx = std::abs(center_x - viewport_center_x);
      const float dy = std::abs(center_y - viewport_center_y);
      const bool centered = dx <= viewport->width() * 0.28f &&
                            dy <= viewport->height() * 0.28f;
      const bool overlay_like = DictContainsAnySubstring(*element, kOverlayNeedles);
      if (!centered && !overlay_like) {
        continue;
      }
      float score = (w * h) / (viewport->width() * viewport->height());
      if (overlay_like) {
        score += 1.0f;
      }
      if (score > best_score) {
        best_score = score;
        best_element = element;
      }
    }
  }

  if (!best_element || best_score < 0.15f) {
    return std::string();
  }
  return BuildOverlayLabel(*best_element);
}

std::string BuildAutonomousFollowupPrompt(const base::Value::Dict& command,
                                          const base::Value::Dict& result,
                                          int step,
                                          int max_steps,
                                          bool include_page_context,
                                          const std::string& page_context) {
  std::string command_json = "{}";
  std::string result_json = "{}";
  base::JSONWriter::Write(command, &command_json);
  base::JSONWriter::Write(result, &result_json);
  command_json = TruncateForPrompt(std::move(command_json), 700u);
  result_json = TruncateForPrompt(std::move(result_json), 1200u);

  std::string followup = base::StringPrintf(
      "Autonomous continuation step %d/%d.\n"
      "Last command:\n%s\n\n"
      "Command result:\n%s\n\n"
      "Continue the primary goal. If solved or blocked, plain text only. "
      "Else one JSON command.\n\n",
      step + 1, max_steps, command_json.c_str(), result_json.c_str());
  if (result.FindBool("significant_mutation").value_or(false)) {
    followup = base::StrCat(
        {"Important: page state changed significantly. Re-plan from updated "
         "context.\n\n",
         followup});
  }
  if (include_page_context && !page_context.empty()) {
    followup = page_context + followup;
  }
  return followup;
}

std::string BuildFinalMessage(const std::string& prefix,
                              const base::FilePath& trace_path,
                              const std::string& detail) {
  std::string message = prefix;
  if (!detail.empty()) {
    message = base::StrCat({message, ": ", detail});
  }
  return base::StrCat(
      {message, "\n[Agent Trace] ", trace_path.AsUTF8Unsafe()});
}

std::string ClipForFinalMessage(const std::string& text) {
  return TruncateForPrompt(text, kMaxFinalResponseChars);
}

std::string BuildHttpFailureMessage(int http_status,
                                    const std::string& status_text,
                                    const std::string& response_body) {
  std::string message = base::StrCat(
      {"Dashboard model request failed: HTTP ",
       base::NumberToString(http_status)});
  if (!status_text.empty()) {
    message = base::StrCat({message, " ", status_text});
  }
  const std::string trimmed_body =
      std::string(base::TrimWhitespaceASCII(response_body, base::TRIM_ALL));
  if (!trimmed_body.empty()) {
    message = base::StrCat(
        {message, ": ", ClipForFinalMessage(trimmed_body)});
  }
  return message;
}

}  // namespace

GlicPromptAction::GlicPromptAction(Profile* profile) : Action(profile) {}

GlicPromptAction::~GlicPromptAction() {
  if (!pending_callback_.has_value() || trace_.empty()) {
    return;
  }
  RecordTraceCompletion(
      /*success=*/false,
      "Agent run aborted during teardown before normal finalization.");
  const auto [trace_path, latest_trace_path] =
      BuildAgentTracePaths(profile_, trace_);
  ScheduleTraceWrite(std::move(trace_path), std::move(latest_trace_path),
                     trace_.Clone(),
                     base::TaskShutdownBehavior::BLOCK_SHUTDOWN);
}

namespace {

bool ActionWouldLeaveOrSpawnTab(const base::Value::Dict& action) {
  const std::string* kind = action.FindString("action");
  if (!kind || kind->empty()) {
    return false;
  }
  return *kind == "navigate" || *kind == "goto" || *kind == "open" ||
         *kind == "open_url" || *kind == "call_script";
}

bool TimelineWouldLeaveOrSpawnTab(const base::Value::List& timeline) {
  for (const base::Value& item : timeline) {
    const base::Value::Dict* action = item.GetIfDict();
    if (action && ActionWouldLeaveOrSpawnTab(*action)) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool GlicPromptAction::SetParams(const base::Value::Dict& params) {
  const std::string* prompt = params.FindString("prompt");
  if (!prompt) {
    return false;
  }
  prompt_ = *prompt;
  std::string trimmed = prompt_;
  base::TrimWhitespaceASCII(trimmed, base::TRIM_ALL, &trimmed);
  if (trimmed.empty()) {
    return false;
  }
  prompt_ = std::move(trimmed);
  focus_only_context_ = params.FindBool("focusOnlyContext").value_or(false);
  stay_on_current_tab_ =
      params.FindBool("stayOnCurrentTab")
          .value_or(params.FindBool("stay_on_current_tab").value_or(false));
  skip_auto_save_ =
      params.FindBool("skipAutoSave")
          .value_or(params.FindBool("skip_auto_save").value_or(false));
  focus_areas_.clear();
  if (const base::Value::List* focus_areas = params.FindList("focusAreas")) {
    for (const base::Value& area_value : *focus_areas) {
      const base::Value::Dict* area = area_value.GetIfDict();
      const base::Value::List* locators = area ? area->FindList("locators")
                                               : nullptr;
      if (!locators || locators->empty()) {
        continue;
      }
      base::Value::List sanitized_locators =
          SanitizeLocatorsForAgentExecution(*locators);
      if (sanitized_locators.empty()) {
        continue;
      }
      base::Value::Dict normalized_area;
      if (const std::string* label = area->FindString("label");
          label && !label->empty()) {
        normalized_area.Set("label", *label);
      }
      normalized_area.Set("locators", std::move(sanitized_locators));
      for (const char* key : {"frame_id", "shadow_chain", "resource_path",
                              "ax_role", "ax_name",
                              "interactive_ancestor_role"}) {
        if (const std::string* value = area->FindString(key);
            value && !value->empty()) {
          normalized_area.Set(key, *value);
        }
      }
      focus_areas_.Append(std::move(normalized_area));
    }
  } else if (const base::Value::List* locators = params.FindList("locators")) {
    base::Value::List sanitized_locators =
        locators ? SanitizeLocatorsForAgentExecution(*locators)
                 : base::Value::List();
    if (!sanitized_locators.empty()) {
      base::Value::Dict area;
      area.Set("label", "Primary focus");
      area.Set("locators", std::move(sanitized_locators));
      focus_areas_.Append(std::move(area));
    }
  }
  SetSerializedAction("glic_prompt", params.Clone());
  return true;
}

void GlicPromptAction::ResetRunState() {
  step_index_ = 0;
  failure_streak_ = 0;
  plain_text_retry_count_ = 0;
  pending_context_refresh_ = false;
  controls_menu_active_ = false;
  save_script_menu_active_ = false;
  script_review_active_ = false;
  auto_saved_script_id_.clear();
  auto_saved_script_name_.clear();
  pending_completion_summary_.clear();
  context_batch_index_ = 0;
  current_page_title_.clear();
  current_page_url_.clear();
  previous_page_title_.clear();
  previous_page_url_.clear();
  last_sent_context_url_.clear();
  last_context_digest_.clear();
  last_context_candidate_ids_.clear();
  provider_.clear();
  provider_api_key_.clear();
  selected_model_.clear();
  loop_profile_.clear();
  agent_endpoint_.clear();
  last_model_response_.clear();
  use_choice_response_format_ = false;
  current_page_map_.clear();
  current_choice_menu_.clear();
  previous_choice_menu_.clear();
  task_actions_.clear();
  ollama_context_.clear();
  trace_.clear();
  initial_context_readiness_checker_.reset();
  active_web_contents_ = nullptr;
  pending_callback_.reset();
  loader_.reset();
}

void GlicPromptAction::Execute(content::WebContents* web_contents,
                               Callback callback) {
  TRACE_EVENT0("ui", "GlicPromptAction::Execute");
  ResetRunState();
  if (!web_contents) {
    std::move(callback).Run(false, "No active page available for glic prompt");
    return;
  }

  active_web_contents_ = web_contents;
  pending_callback_ = std::move(callback);
  trace_.Set("trace_id", base::Uuid::GenerateRandomV4().AsLowercaseString());
  trace_.Set("prompt", prompt_);
  trace_.Set("focus_only_context", focus_only_context_);
  if (!focus_areas_.empty()) {
    trace_.Set("focus_areas", focus_areas_.Clone());
    trace_.Set("focus_locators", FlattenFocusAreas(focus_areas_));
  }
  trace_.Set(
      "started_at",
      static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  trace_.Set("steps", base::Value::List());
  trace_.Set("initial_context_gates", base::Value::List());

  const base::FilePath scripts_dir =
      profile_ ? dashboard::ScriptStore::ScriptsDir(profile_) : base::FilePath();
  const base::FilePath profile_path =
      profile_ ? profile_->GetPath() : base::FilePath();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&ReadConfiguredDashboardAgentConfigFromScriptsDir,
                     scripts_dir, profile_path),
      base::BindOnce(&GlicPromptAction::OnConfiguredModelLoaded,
                     weak_ptr_factory_.GetWeakPtr()));
}

void GlicPromptAction::OnConfiguredModelLoaded(base::Value::Dict configured_agent) {
  if (!pending_callback_.has_value() || !active_web_contents_) {
    return;
  }

  const std::string* configured_provider = configured_agent.FindString("provider");
  const std::string* configured_model = configured_agent.FindString("model");
  const std::string* configured_endpoint = configured_agent.FindString("endpoint");
  const std::string* configured_api_key = configured_agent.FindString("apiKey");
  const std::string* configured_api_key_name =
      configured_agent.FindString("apiKeyName");
  provider_ = NormalizeAgentProvider(configured_provider ? *configured_provider
                                                         : std::string());
  provider_api_key_ = configured_api_key ? *configured_api_key : std::string();
  use_choice_response_format_ = UsesChoiceResponseFormat(configured_agent);
  const std::string required_api_key_name =
      configured_api_key_name ? *configured_api_key_name
                              : RequiredApiKeyNameForProvider(provider_);
  if (!IsOllamaProvider(provider_) && provider_api_key_.empty()) {
    Finalize(false,
             base::StrCat(
                 {base::ToUpperASCII(provider_),
                  " provider selected but API key is missing. Set ",
                  required_api_key_name, " in Data > Secrets."}));
    return;
  }
  selected_model_ =
      (configured_model && !configured_model->empty()) ? *configured_model
                                                       : DefaultModelForProvider(provider_);
  const std::string* configured_loop_profile =
      configured_agent.FindString("loopProfile");
  loop_profile_ = AgentModelContract::NormalizeLoopProfile(
      configured_loop_profile ? *configured_loop_profile
                              : AgentModelContract::kLoopProfileGlicTask);
  if (IsOllamaProvider(provider_)) {
    agent_endpoint_ = NormalizeOllamaEndpoint(configured_endpoint
                                                  ? *configured_endpoint
                                                  : std::string());
  } else if (IsGeminiProvider(provider_)) {
    agent_endpoint_ = NormalizeHostedEndpoint(
        configured_endpoint ? *configured_endpoint : std::string(),
        kDefaultGeminiEndpoint);
  } else {
    agent_endpoint_ = NormalizeHostedEndpoint(
        configured_endpoint ? *configured_endpoint : std::string(),
        DefaultEndpointForProvider(provider_));
  }
  trace_.Set("provider", provider_);
  trace_.Set("model", selected_model_);
  trace_.Set("loop_profile", loop_profile_);
  trace_.Set("agent_endpoint", agent_endpoint_);
  if (!required_api_key_name.empty()) {
    trace_.Set("api_key_name", required_api_key_name);
  }
  trace_.Set("response_mode",
             use_choice_response_format_ ? "choice" : "json");
  TRACE_EVENT2("ui", "GlicPromptAction::OnConfiguredModelLoaded",
               "provider", provider_, "model", selected_model_);
  FetchInitialPageContext();
}

void GlicPromptAction::FetchInitialPageContext() {
  if (!pending_callback_.has_value() || !active_web_contents_) {
    return;
  }
  TRACE_EVENT1("ui", "GlicPromptAction::FetchInitialPageContext", "url",
               active_web_contents_->GetLastCommittedURL().spec());
  FetchUnifiedPageMapSnapshot(
      active_web_contents_,
      base::BindOnce(&GlicPromptAction::OnPageContextFetched,
                     weak_ptr_factory_.GetWeakPtr(),
                     base::UTF16ToUTF8(active_web_contents_->GetTitle()),
                     active_web_contents_->GetLastCommittedURL().spec()));
}

void GlicPromptAction::OnPageContextFetched(std::string page_title,
                                            std::string page_url,
                                            dashboard::UnifiedPageMapResult result) {
  TRACE_EVENT2("ui", "GlicPromptAction::OnPageContextFetched", "ok",
               result.ok, "url", page_url);
  current_page_title_ = std::move(page_title);
  current_page_url_ = std::move(page_url);
  current_page_map_.clear();
  if (result.ok) {
    current_page_map_ = result.page_map.Clone();
  }

  if (trace_.FindList("steps")->empty()) {
    StartPromptTurn("Begin working on the primary user goal above now.",
                    /*include_page_context=*/true,
                    /*include_system_prompt=*/true,
                    /*attach_screenshot=*/false);
  } else {
    OnContextRefreshComplete("refresh_pagemap", base::Value::Dict(),
                             std::move(result));
  }
}

void GlicPromptAction::StartPromptTurn(const std::string& user_prompt,
                                       bool include_page_context,
                                       bool include_system_prompt,
                                       bool attach_screenshot,
                                       std::optional<gfx::RectF> screenshot_clip,
                                       base::Value::Dict screenshot_request_meta) {
  // Text-only / local Ollama sessions must never capture or attach images.
  if (!IsVisionCapableModel(selected_model_)) {
    attach_screenshot = false;
  }
  if (attach_screenshot && IsVisionCapableModel(selected_model_)) {
    if (RecorderController* rc = RecorderController::GetActive()) {
      rc->CaptureScreenshot(base::BindOnce(
          &GlicPromptAction::OnScreenshotCaptured,
          weak_ptr_factory_.GetWeakPtr(), user_prompt, include_page_context,
          include_system_prompt, std::move(screenshot_request_meta)),
          screenshot_clip);
      return;
    }
  }
  ContinueStartPromptTurn(user_prompt, include_page_context,
                          include_system_prompt, std::string(),
                          std::move(screenshot_request_meta));
}

void GlicPromptAction::OnScreenshotCaptured(
    const std::string& user_prompt,
    bool include_page_context,
    bool include_system_prompt,
    base::Value::Dict screenshot_request_meta,
    std::string screenshot_base64) {
  if (!IsVisionCapableModel(selected_model_)) {
    screenshot_base64.clear();
  }
  ContinueStartPromptTurn(user_prompt, include_page_context,
                          include_system_prompt, std::move(screenshot_base64),
                          std::move(screenshot_request_meta));
}

std::string GlicPromptAction::ComposePageContextForModel(
    bool include_page_context,
    bool force_full_context,
    bool screenshot_attached) {
  std::string digest;
  std::set<std::string> candidate_ids;
  std::string body = BuildAgentTurnContextCompact(
      prompt_, selected_model_, screenshot_attached, current_page_title_,
      current_page_url_, current_page_map_, focus_areas_, focus_only_context_,
      context_batch_index_, include_page_context, force_full_context,
      last_sent_context_url_, last_context_digest_, last_context_candidate_ids_,
      &digest, &candidate_ids);
  if (include_page_context && !digest.empty()) {
    last_sent_context_url_ = current_page_url_;
    last_context_digest_ = std::move(digest);
    last_context_candidate_ids_ = std::move(candidate_ids);
  }
  return body;
}

void GlicPromptAction::ContinueStartPromptTurn(
    const std::string& user_prompt,
    bool include_page_context,
    bool include_system_prompt,
    std::string screenshot_base64,
    base::Value::Dict screenshot_request_meta) {
  if (!pending_callback_.has_value()) {
    return;
  }
  // Hard block: text-only models never receive encoded images in the request.
  if (!IsVisionCapableModel(selected_model_)) {
    screenshot_base64.clear();
  }
  base::Value::List* steps = trace_.FindList("steps");
  if (!steps) {
    trace_.Set("steps", base::Value::List());
    steps = trace_.FindList("steps");
  }

  std::string page_context;
  const bool screenshot_attached = !screenshot_base64.empty();
  if (use_choice_response_format_) {
    if (save_script_menu_active_) {
      current_choice_menu_.clear();
      page_context = BuildSaveScriptPrompt(prompt_, task_actions_);
    } else {
      const base::Value::List focus_matches =
          ResolveFocusAreaMatches(focus_areas_, current_page_map_);
      const std::string prior_page_title = previous_page_title_;
      const std::string prior_page_url = previous_page_url_;
      const base::Value::List prior_choice_menu = previous_choice_menu_.Clone();
      current_choice_menu_ = BuildChoiceMenuItems(
          current_page_url_, current_page_map_, focus_areas_, context_batch_index_);
      const bool allow_screenshot = ShouldOfferScreenshotOption(
          selected_model_, prompt_, current_page_map_, focus_areas_,
          current_choice_menu_, screenshot_attached);
      page_context = BuildChoiceTurnPrompt(
          prompt_, user_prompt, selected_model_, screenshot_attached,
          current_page_title_, current_page_url_, focus_areas_, focus_matches,
          focus_only_context_, context_batch_index_, current_choice_menu_,
          controls_menu_active_, allow_screenshot,
          BuildChoiceDeltaNote(prior_page_title, prior_page_url,
                               prior_choice_menu, current_page_title_,
                               current_page_url_, current_choice_menu_));
      previous_page_title_ = current_page_title_;
      previous_page_url_ = current_page_url_;
      previous_choice_menu_ = current_choice_menu_.Clone();
    }
  } else {
    current_choice_menu_.clear();
    // First turn (include_page_context) is always FULL so the model has
    // anchors; later turns use Compact via followup builders.
    page_context = ComposePageContextForModel(
        include_page_context,
        /*force_full_context=*/include_page_context &&
            last_context_digest_.empty(),
        screenshot_attached);
  }
  const std::string full_prompt =
      use_choice_response_format_ ? page_context : page_context + user_prompt;
  const bool send_system_prompt =
      use_choice_response_format_ || !IsOllamaProvider(provider_)
          ? true
          : (include_system_prompt && ollama_context_.empty());
  const std::string system_prompt =
      use_choice_response_format_ ? std::string(kChoiceAgentSystemPrompt)
                                  : AgentModelContract::BuildSystemPromptForLoopProfile(
                                        loop_profile_.empty()
                                            ? AgentModelContract::kLoopProfileGlicTask
                                            : loop_profile_);

  std::string structured_transport = "prompt_only";
  if (IsOllamaProvider(provider_)) {
    structured_transport = "ollama_format";
  } else if (IsOpenAICompatibleProvider(provider_)) {
    structured_transport = "openai_response_format";
  } else if (IsGeminiProvider(provider_)) {
    structured_transport = "gemini_response_json_schema";
  } else if (IsAnthropicProvider(provider_)) {
    structured_transport = "anthropic_output_config";
  }
  const base::Value::Dict canonical_turn_format =
      save_script_menu_active_
          ? AgentModelContract::BuildScriptReviewResponseFormat()
          : AgentModelContract::BuildAgentTurnResponseFormat();
  const base::Value::Dict structured_plan =
      use_choice_response_format_
          ? base::Value::Dict()
          : AgentModelContract::CompileStructuredOutput(
                provider_, structured_transport, &canonical_turn_format);

  base::Value::Dict step;
  step.Set("index", step_index_);
  step.Set("provider", provider_);
  step.Set("model", selected_model_);
  step.Set("loop_profile", loop_profile_);
  step.Set("response_mode", use_choice_response_format_ ? "choice" : "json");
  if (const base::Value::Dict* diagnostics =
          structured_plan.FindDict("diagnostics")) {
    step.Set("structured_output", diagnostics->Clone());
  }
  step.Set("screenshot_attached", screenshot_attached);
  if (!screenshot_request_meta.empty()) {
    step.Set("screenshot_request", std::move(screenshot_request_meta));
  }
  step.Set("focus_only_context", focus_only_context_);
  step.Set("context_batch_index", context_batch_index_);
  step.Set("user_prompt", full_prompt);
  if (!current_choice_menu_.empty()) {
    step.Set("choice_menu", current_choice_menu_.Clone());
  }
  if (send_system_prompt) {
    step.Set("system_prompt", system_prompt);
  }
  steps->Append(std::move(step));

  base::Value::Dict body;
  if (IsOllamaProvider(provider_)) {
    body.Set("model", selected_model_);
    body.Set("stream", false);

    base::Value::List messages;
    if (send_system_prompt) {
      base::Value::Dict system;
      system.Set("role", "system");
      system.Set("content", system_prompt);
      messages.Append(std::move(system));
    }
    base::Value::Dict user;
    user.Set("role", "user");
    user.Set("content", full_prompt);
    if (screenshot_attached) {
      base::Value::List images;
      images.Append(std::move(screenshot_base64));
      user.Set("images", std::move(images));
    }
    messages.Append(std::move(user));
    body.Set("messages", std::move(messages));
    if (!ollama_context_.empty()) {
      body.Set("context", ollama_context_.Clone());
    }
    if (use_choice_response_format_) {
      body.Set("format", save_script_menu_active_
                             ? BuildSaveScriptTransportSchema()
                             : BuildChoiceTransportSchema(true));
      base::Value::Dict options;
      options.Set("temperature", 0);
      options.Set("num_predict", 1024);
      body.Set("options", std::move(options));
    } else if (const base::Value::Dict* format =
                   structured_plan.FindDict("format")) {
      body.Set("format", format->Clone());
      base::Value::Dict options;
      options.Set("num_predict", 1024);
      body.Set("options", std::move(options));
    }
  } else if (IsOpenAICompatibleProvider(provider_)) {
    body.Set("model", selected_model_);
    body.Set("stream", false);
    base::Value::List messages;
    if (send_system_prompt) {
      base::Value::Dict system;
      system.Set("role", "system");
      system.Set("content", system_prompt);
      messages.Append(std::move(system));
    }
    base::Value::Dict user;
    user.Set("role", "user");
    if (screenshot_attached) {
      base::Value::List content_parts;
      base::Value::Dict text_part;
      text_part.Set("type", "text");
      text_part.Set("text", full_prompt);
      content_parts.Append(std::move(text_part));
      base::Value::Dict image_part;
      image_part.Set("type", "image_url");
      base::Value::Dict image_url;
      image_url.Set("url",
                    base::StrCat({"data:image/png;base64,", screenshot_base64}));
      image_part.Set("image_url", std::move(image_url));
      content_parts.Append(std::move(image_part));
      user.Set("content", std::move(content_parts));
    } else {
      user.Set("content", full_prompt);
    }
    messages.Append(std::move(user));
    body.Set("messages", std::move(messages));
    body.Set("max_tokens", 1024);
    if (use_choice_response_format_) {
      base::Value::Dict response_format;
      response_format.Set("type", "json_schema");
      base::Value::Dict json_schema;
      json_schema.Set("name",
                      save_script_menu_active_ ? "glic_save_script_choice"
                                               : "glic_choice_answer");
      json_schema.Set("strict", true);
      json_schema.Set("schema", save_script_menu_active_
                                    ? BuildSaveScriptTransportSchema()
                                    : BuildChoiceTransportSchema(false));
      response_format.Set("json_schema", std::move(json_schema));
      body.Set("response_format", std::move(response_format));
      body.Set("temperature", 0);
    } else if (const base::Value::Dict* response_format =
                   structured_plan.FindDict("response_format")) {
      body.Set("response_format", response_format->Clone());
      body.Set("temperature", 0);
    }
  } else if (IsGeminiProvider(provider_)) {
    base::Value::List contents;
    base::Value::Dict user_content;
    user_content.Set("role", "user");
    base::Value::List user_parts;
    base::Value::Dict text_part;
    text_part.Set("text", full_prompt);
    user_parts.Append(std::move(text_part));
    if (screenshot_attached) {
      base::Value::Dict image_part;
      base::Value::Dict inline_data;
      inline_data.Set("mime_type", "image/png");
      inline_data.Set("data", screenshot_base64);
      image_part.Set("inline_data", std::move(inline_data));
      user_parts.Append(std::move(image_part));
    }
    user_content.Set("parts", std::move(user_parts));
    contents.Append(std::move(user_content));
    body.Set("contents", std::move(contents));
    if (send_system_prompt) {
      base::Value::Dict system_instruction;
      base::Value::List system_parts;
      base::Value::Dict system_text;
      system_text.Set("text", system_prompt);
      system_parts.Append(std::move(system_text));
      system_instruction.Set("parts", std::move(system_parts));
      body.Set("systemInstruction", std::move(system_instruction));
    }
    base::Value::Dict generation_config;
    generation_config.Set("temperature", 0);
    if (use_choice_response_format_) {
      generation_config.Set("responseMimeType", "application/json");
    } else if (const base::Value::Dict* generation =
                   structured_plan.FindDict("generation_config")) {
      if (const std::string* mime = generation->FindString("responseMimeType")) {
        generation_config.Set("responseMimeType", *mime);
      }
      if (const base::Value::Dict* schema =
              generation->FindDict("responseJsonSchema")) {
        generation_config.Set("responseJsonSchema", schema->Clone());
      }
    }
    body.Set("generationConfig", std::move(generation_config));
  } else if (IsAnthropicProvider(provider_)) {
    body.Set("model", selected_model_);
    body.Set("max_tokens", 4096);
    body.Set("temperature", 0);
    if (send_system_prompt) {
      body.Set("system", system_prompt);
    }
    base::Value::List messages;
    base::Value::Dict user;
    user.Set("role", "user");
    base::Value::List content_parts;
    base::Value::Dict text_part;
    text_part.Set("type", "text");
    text_part.Set("text", full_prompt);
    content_parts.Append(std::move(text_part));
    if (screenshot_attached) {
      base::Value::Dict image_part;
      image_part.Set("type", "image");
      base::Value::Dict source;
      source.Set("type", "base64");
      source.Set("media_type", "image/png");
      source.Set("data", screenshot_base64);
      image_part.Set("source", std::move(source));
      content_parts.Append(std::move(image_part));
    }
    user.Set("content", std::move(content_parts));
    messages.Append(std::move(user));
    body.Set("messages", std::move(messages));
    if (const base::Value::Dict* output_config =
            structured_plan.FindDict("output_config")) {
      body.Set("output_config", output_config->Clone());
    }
  } else {
    Finalize(false, "Unsupported dashboard provider: " + provider_);
    return;
  }

  std::string body_json;
  if (!base::JSONWriter::Write(body, &body_json) || body_json.empty()) {
    Finalize(false, "Failed to build glic prompt request");
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->method = "POST";
  std::string request_url;
  if (IsOllamaProvider(provider_)) {
    request_url = base::StrCat({agent_endpoint_, "/api/chat"});
  } else if (IsGeminiProvider(provider_)) {
    request_url = BuildGeminiGenerateContentEndpoint(agent_endpoint_,
                                                     selected_model_);
  } else {
    request_url = agent_endpoint_;
  }
  request->url = GURL(request_url);
  if (!request->url.is_valid()) {
    Finalize(false, "Configured provider endpoint is invalid");
    return;
  }
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader("Content-Type", "application/json");
  request->headers.SetHeader("Accept", "application/json");
  if (IsOpenAICompatibleProvider(provider_)) {
    request->headers.SetHeader(
        "Authorization", base::StrCat({"Bearer ", provider_api_key_}));
    request->headers.SetHeader("User-Agent", kHostedClientUserAgent);
  } else if (IsGeminiProvider(provider_)) {
    request->headers.SetHeader("x-goog-api-key", provider_api_key_);
    request->headers.SetHeader("User-Agent", kHostedClientUserAgent);
  } else if (IsAnthropicProvider(provider_)) {
    request->headers.SetHeader("x-api-key", provider_api_key_);
    request->headers.SetHeader("anthropic-version", "2023-06-01");
    if (structured_plan.FindDict("output_config")) {
      request->headers.SetHeader("anthropic-beta",
                                 "structured-outputs-2025-11-13");
    }
    request->headers.SetHeader("User-Agent", kHostedClientUserAgent);
  }

  loader_ = network::SimpleURLLoader::Create(std::move(request),
                                             TRAFFIC_ANNOTATION_FOR_TESTS);
  loader_->AttachStringForUpload(body_json, "application/json");
  loader_->SetAllowHttpErrorResults(true);
  // Local models (Ollama) often need many minutes per step.
  loader_->SetTimeoutDuration(ModelStepTimeout());

  auto* partition = profile_->GetDefaultStoragePartition();
  auto factory = partition->GetURLLoaderFactoryForBrowserProcess();
  loader_->DownloadToString(
      factory.get(),
      base::BindOnce(&GlicPromptAction::OnModelResponse,
                     weak_ptr_factory_.GetWeakPtr()),
      kMaxResponseBytes);
}

void GlicPromptAction::OnModelResponse(
    std::unique_ptr<std::string> response_body) {
  if (!pending_callback_.has_value()) {
    return;
  }
  base::Value::List* steps = trace_.FindList("steps");
  base::Value::Dict* step =
      (steps && !steps->empty()) ? &steps->back().GetDict() : nullptr;
  const network::mojom::URLResponseHead* response_info =
      loader_ ? loader_->ResponseInfo() : nullptr;
  const scoped_refptr<net::HttpResponseHeaders> response_headers =
      response_info ? response_info->headers : nullptr;
  const int http_status = response_headers ? response_headers->response_code() : 0;
  const std::string http_status_text =
      response_headers ? response_headers->GetStatusText() : std::string();

  if (step && http_status > 0) {
    step->Set("http_status", http_status);
    if (!http_status_text.empty()) {
      step->Set("http_status_text", http_status_text);
    }
  }

  if (!response_body) {
    const int net_error = loader_ ? loader_->NetError() : net::ERR_FAILED;
    loader_.reset();
    if (step) {
      step->Set("response_error",
                "Dashboard model request failed: net_error=" +
                    std::to_string(net_error));
    }
    Finalize(false, "Dashboard model request failed: net_error=" +
                        std::to_string(net_error));
    return;
  }

  if (http_status >= 300) {
    const std::string failure_message =
        BuildHttpFailureMessage(http_status, http_status_text, *response_body);
    if (step) {
      step->Set("response_error", failure_message);
      if (!response_body->empty()) {
        step->Set("raw_response", TruncateForPrompt(*response_body, 4000u));
      }
    }
    loader_.reset();
    Finalize(false, failure_message);
    return;
  }

  std::optional<base::Value> parsed = base::JSONReader::Read(
      *response_body, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    loader_.reset();
    if (step) {
      step->Set("response_error", "Dashboard model returned invalid JSON");
      step->Set("raw_response", *response_body);
    }
    Finalize(false, "Dashboard model returned invalid JSON");
    return;
  }

  const base::Value::Dict& response = parsed->GetDict();
  if (IsOllamaProvider(provider_)) {
    if (const base::Value::List* context = response.FindList("context")) {
      ollama_context_ = context->Clone();
    }
  }
  std::string response_text;
  if (IsOpenAICompatibleProvider(provider_)) {
    response_text = ExtractOpenAICompatibleResponseText(response);
  } else if (IsGeminiProvider(provider_)) {
    response_text = ExtractGeminiResponseText(response);
  } else if (IsAnthropicProvider(provider_)) {
    response_text = ExtractAnthropicResponseText(response);
  } else {
    response_text = ExtractModelResponseText(response);
  }
  loader_.reset();
  if (step) {
    step->Set("assistant_response", response_text);
  }
  last_model_response_ = response_text;
  if (response_text.empty()) {
    Finalize(false, "Dashboard model returned an empty response");
    return;
  }

  std::optional<base::Value::Dict> command =
      use_choice_response_format_
          ? ExtractChoiceCommandFromText(response_text, current_choice_menu_,
                                         controls_menu_active_,
                                         save_script_menu_active_,
                                         ShouldOfferScreenshotOption(
                                             selected_model_, prompt_,
                                             current_page_map_, focus_areas_,
                                             current_choice_menu_,
                                             /*screenshot_attached=*/false),
                                         task_actions_.size())
          : ExtractJsonCommandFromText(response_text);
  if (!command.has_value()) {
    if (step) {
      step->Set("terminal_response_without_command", true);
    }
    if (plain_text_retry_count_ < kMaxPlainTextRetries &&
        step_index_ < (MaxAutonomousAgentSteps() - 1)) {
      ++plain_text_retry_count_;
      const std::string followup =
          use_choice_response_format_
              ? BuildChoiceOnlyRetryPrompt(response_text)
              : BuildJsonOnlyRetryPrompt(prompt_, response_text) +
                    "Continue working toward the primary user goal.\n";
      ++step_index_;
      StartPromptTurn(followup, /*include_page_context=*/true,
                      /*include_system_prompt=*/false,
                      /*attach_screenshot=*/false);
      return;
    }
    Finalize(false,
             base::StrCat({"Agent returned no valid command. Model response: ",
                           ClipForFinalMessage(response_text)}));
    return;
  }
  plain_text_retry_count_ = 0;

  *command = AgentModelContract::UnwrapStructuredAction(*command);
  base::Value::Dict normalized_command =
      AgentModelContract::NormalizeAgentCommandForExecution(*command);
  normalized_command =
      ResolveFocusAreaLabelTargetsInCommand(normalized_command, focus_areas_);
  normalized_command =
      ResolveElementIdTargetsInCommand(normalized_command, current_page_map_);

  if (std::optional<std::string> invalid_typed_text =
          FindInstructionEchoTypedText(normalized_command)) {
    if (step) {
      step->Set("rejected_command_reason",
                "type_text_looked_like_task_instructions");
      step->Set("rejected_typed_text",
                TruncateForPrompt(*invalid_typed_text, 240u));
    }
    if (plain_text_retry_count_ < kMaxPlainTextRetries &&
        step_index_ < (MaxAutonomousAgentSteps() - 1)) {
      ++plain_text_retry_count_;
      ++step_index_;
      StartPromptTurn(
          base::StrCat(
              {use_choice_response_format_
                   ? BuildChoiceOnlyRetryPrompt(response_text)
                   : BuildJsonOnlyRetryPrompt(prompt_, response_text),
               "The previous choice used C<number> with instructions instead "
               "of literal text to insert. If you choose C, return only the "
               "final user-visible text.\n"}),
          /*include_page_context=*/true,
          /*include_system_prompt=*/false,
          /*attach_screenshot=*/false);
      return;
    }
    Finalize(false,
             "Agent tried to type task instructions instead of final text.");
    return;
  }

  if (step) {
    step->Set("raw_command", command->Clone());
    step->Set("command", normalized_command.Clone());
  }
  DispatchCommand(std::move(normalized_command));
}

void GlicPromptAction::DispatchCommand(base::Value::Dict command) {
  DispatchCommandWhenPageReady(std::move(command), /*wait_attempt=*/0);
}

void GlicPromptAction::DispatchCommandWhenPageReady(base::Value::Dict command,
                                                    int wait_attempt) {
  const std::string* command_name = command.FindString("command");
  const base::Value::Dict* payload = command.FindDict("payload");
  if (!command_name || command_name->empty()) {
    Finalize(false, "Model returned a command block without command name");
    return;
  }

  if ((*command_name == "call_action" || *command_name == "run_timeline") &&
      ShouldWaitForAgentPageReady(active_web_contents_, wait_attempt)) {
    base::Value::List* steps = trace_.FindList("steps");
    base::Value::Dict* step =
        (steps && !steps->empty()) ? &steps->back().GetDict() : nullptr;
    if (step && active_web_contents_) {
      base::Value::Dict gate;
      gate.Set("stage", "before_execution");
      gate.Set("attempt", wait_attempt);
      gate.Set("load_progress", active_web_contents_->GetLoadProgress());
      gate.Set("is_loading", active_web_contents_->IsLoading());
      step->Set("page_load_gate", std::move(gate));
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&GlicPromptAction::DispatchCommandWhenPageReady,
                       weak_ptr_factory_.GetWeakPtr(), std::move(command),
                       wait_attempt + 1),
        kAgentPageReadyPollInterval);
    return;
  }

  if (*command_name == "agent_finish") {
    std::string summary;
    std::string status = "solved";
    if (payload) {
      if (const std::string* status_value = payload->FindString("status")) {
        status = *status_value;
      }
      if (const std::string* summary_value = payload->FindString("summary")) {
        summary = *summary_value;
      }
    }
    const bool success =
        !base::EqualsCaseInsensitiveASCII(status, "blocked");

    // During post-task review, "done" means accept the draft and exit.
    if (script_review_active_) {
      script_review_active_ = false;
      save_script_menu_active_ = false;
      const std::string message = pending_completion_summary_.empty()
                                      ? (summary.empty() ? "Agent finished."
                                                         : summary)
                                      : pending_completion_summary_;
      Finalize(true, message.empty() ? "Agent finished." : message);
      return;
    }

    if (success && !skip_auto_save_ &&
        AgentBoolSetting("autoSaveScriptOnComplete", true) &&
        !task_actions_.empty()) {
      std::string script_name;
      std::string save_error;
      if (!AutoSaveSucceededTaskScript(&script_name, &save_error)) {
        Finalize(false, save_error.empty()
                            ? "Task finished but script auto-save failed."
                            : save_error);
        return;
      }
      if (AgentBoolSetting("reviewScriptAfterComplete", true)) {
        BeginPostTaskScriptReview(
            script_name, summary.empty() ? "Agent finished." : summary);
        return;
      }
      Finalize(true, base::StrCat(
                         {summary.empty() ? "Agent finished." : summary,
                          " Saved script: ", script_name}));
      return;
    }

    Finalize(success, summary.empty() ? "Agent finished." : summary);
    return;
  }

  if (*command_name == "request_more_controls") {
    controls_menu_active_ = true;
    save_script_menu_active_ = false;
    if (step_index_ >= (MaxAutonomousAgentSteps() - 1)) {
      Finalize(false, "Agent step budget exceeded");
      return;
    }
    ++step_index_;
    StartPromptTurn("Secondary controls opened. Choose the best uncommon control or go back to the primary options.",
                    /*include_page_context=*/false,
                    /*include_system_prompt=*/false,
                    /*attach_screenshot=*/false);
    return;
  }

  if (*command_name == "request_primary_options") {
    controls_menu_active_ = false;
    save_script_menu_active_ = false;
    if (step_index_ >= (MaxAutonomousAgentSteps() - 1)) {
      Finalize(false, "Agent step budget exceeded");
      return;
    }
    ++step_index_;
    StartPromptTurn("Primary options restored. Choose the best next action.",
                    /*include_page_context=*/false,
                    /*include_system_prompt=*/false,
                    /*attach_screenshot=*/false);
    return;
  }

  if (*command_name == "refresh_pagemap") {
    if (!active_web_contents_) {
      Finalize(false, "No active page available for context refresh");
      return;
    }
    RefreshContextCommandWhenPageReady(
        *command_name, payload ? payload->Clone() : base::Value::Dict(),
        /*wait_attempt=*/0);
    return;
  }

  if (*command_name == "request_more_context") {
    if (!active_web_contents_) {
      Finalize(false, "No active page available for more-context refresh");
      return;
    }
    ++context_batch_index_;
    RefreshContextCommandWhenPageReady(
        *command_name, payload ? payload->Clone() : base::Value::Dict(),
        /*wait_attempt=*/0);
    return;
  }

  if (*command_name == "reload_page") {
    if (!active_web_contents_) {
      Finalize(false, "No active page available for reload");
      return;
    }
    controls_menu_active_ = false;
    save_script_menu_active_ = false;
    active_web_contents_->GetController().Reload(content::ReloadType::NORMAL,
                                                 /*check_for_repost=*/true);
    RefreshContextCommandWhenPageReady(
        *command_name, payload ? payload->Clone() : base::Value::Dict(),
        /*wait_attempt=*/0);
    return;
  }

  if (*command_name == "inspect_element") {
    controls_menu_active_ = false;
    save_script_menu_active_ = false;
    if (step_index_ >= (MaxAutonomousAgentSteps() - 1)) {
      Finalize(false, "Agent step budget exceeded");
      return;
    }
    std::optional<base::Value::Dict> element =
        payload ? ResolveElementFromCommandPayload(*payload, focus_areas_,
                                                   current_page_map_)
                : std::nullopt;
    const std::string note =
        element.has_value()
            ? BuildElementInspectionNote(*element)
            : std::string("Inspect element could not resolve the requested target.\n\n");
    ++step_index_;
    StartPromptTurn(base::StrCat({note, "Choose the next best action now."}),
                    /*include_page_context=*/false,
                    /*include_system_prompt=*/false,
                    /*attach_screenshot=*/false);
    return;
  }

  if (*command_name == "request_screenshot") {
    if (!active_web_contents_) {
      Finalize(false, "No active page available for screenshot request");
      return;
    }
    if (!IsVisionCapableModel(selected_model_)) {
      Finalize(false,
               "request_screenshot is unavailable for a text-only model");
      return;
    }
    RefreshScreenshotContextWhenPageReady(
        *command_name, payload ? payload->Clone() : base::Value::Dict(),
        /*wait_attempt=*/0);
    return;
  }

  if (*command_name == "save_task_script") {
    if (payload && payload->FindList("indexes")) {
      const base::Value::List* indexes = payload->FindList("indexes");
      const std::string* requested_name = payload->FindString("name");
      const std::string script_name = SanitizeSavedScriptName(
          requested_name ? *requested_name
                         : (auto_saved_script_name_.empty()
                                ? "Saved Agent Script"
                                : auto_saved_script_name_));
      const base::Value::List timeline =
          indexes ? BuildSavedTimelineFromIndexes(task_actions_, *indexes)
                  : base::Value::List();
      if (timeline.empty()) {
        Finalize(false, "No reusable actions were selected for script save");
        return;
      }
      const std::string script_id =
          auto_saved_script_id_.empty()
              ? base::Uuid::GenerateRandomV4().AsLowercaseString()
              : auto_saved_script_id_;
      base::Value::Dict script;
      script.Set("name", script_name);
      script.Set("actions", timeline.Clone());
      if (!current_page_url_.empty()) {
        script.Set("startUrl", current_page_url_);
      }
      script.Set("source", "glic_prompt_action");
      script.Set("sourceGoal", prompt_);
      const base::Value::Dict canonical = dashboard::ScriptSchema::CanonicalizeForSave(
          std::move(script), script_id, base::Time::Now());
      if (!dashboard::ScriptStore::Write(profile_, script_id, canonical)) {
        Finalize(false, "Failed to save generated script");
        return;
      }
      DashboardUIMessageHandler::UpdateRuntimeScriptCache(
          profile_, script_id, canonical.Clone());
      save_script_menu_active_ = false;
      controls_menu_active_ = false;
      auto_saved_script_id_ = script_id;
      auto_saved_script_name_ = script_name;
      trace_.Set("saved_script", canonical.Clone());

      if (script_review_active_) {
        script_review_active_ = false;
        Finalize(true,
                 base::StrCat({pending_completion_summary_.empty()
                                   ? "Agent finished."
                                   : pending_completion_summary_,
                               " Reviewed script: ", script_name}));
        return;
      }

      if (step_index_ >= (MaxAutonomousAgentSteps() - 1)) {
        Finalize(true, "Saved script: " + script_name);
        return;
      }
      ++step_index_;
      StartPromptTurn(
          base::StrCat({"Saved reusable script '", script_name,
                        "'. Continue the primary user goal from the current page."}),
          /*include_page_context=*/false,
          /*include_system_prompt=*/false,
          /*attach_screenshot=*/false);
      return;
    }

    if (task_actions_.empty()) {
      if (step_index_ >= (MaxAutonomousAgentSteps() - 1)) {
        Finalize(false, "No task actions are available to save yet");
        return;
      }
      ++step_index_;
      StartPromptTurn("No executable task actions are available to save yet. Continue the task instead.",
                      /*include_page_context=*/false,
                      /*include_system_prompt=*/false,
                      /*attach_screenshot=*/false);
      return;
    }
    controls_menu_active_ = false;
    save_script_menu_active_ = true;
    if (step_index_ >= (MaxAutonomousAgentSteps() - 1)) {
      Finalize(false, "Agent step budget exceeded");
      return;
    }
    ++step_index_;
    StartPromptTurn("Select the useful recent task actions to save as a reusable script now.",
                    /*include_page_context=*/false,
                    /*include_system_prompt=*/false,
                    /*attach_screenshot=*/false);
    return;
  }

  base::Value::List timeline;
  if (*command_name == "call_action") {
    controls_menu_active_ = false;
    save_script_menu_active_ = false;
    if (!payload) {
      Finalize(false, "call_action requires payload");
      return;
    }
    const std::string* action_name = payload->FindString("action");
    if (!action_name || action_name->empty()) {
      Finalize(false, "call_action requires payload.action");
      return;
    }
    base::Value::Dict action;
    action.Set("action", *action_name);
    if (const base::Value::Dict* params = payload->FindDict("params")) {
      action.Set("params", params->Clone());
    } else {
      action.Set("params", base::Value::Dict());
    }
    timeline.Append(std::move(action));
  } else if (*command_name == "run_timeline") {
    controls_menu_active_ = false;
    save_script_menu_active_ = false;
    if (const base::Value::List* top_level = command.FindList("payload")) {
      timeline = top_level->Clone();
    }
    if (timeline.empty() && payload) {
      if (const base::Value::List* requested = payload->FindList("timeline")) {
        timeline = requested->Clone();
      }
    }
    if (timeline.empty()) {
      Finalize(false, "run_timeline requires payload.timeline");
      return;
    }
  } else {
    Finalize(false, "Unsupported glic_prompt command: " + *command_name);
    return;
  }

  if (!active_web_contents_) {
    Finalize(false, "No active page available for glic prompt execution");
    return;
  }
  dashboard::ScriptExecutionService* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (!exec_service) {
    Finalize(false, "execution service unavailable");
    return;
  }

  std::string command_name_copy = *command_name;
  base::Value::Dict payload_copy = payload ? payload->Clone() : base::Value::Dict();
  if (stay_on_current_tab_ &&
      TimelineWouldLeaveOrSpawnTab(timeline)) {
    StartPromptTurn(
        "Stay on this tab. Do not navigate, open a tab, or call another "
        "script. Complete the captcha here using screenshot coordinates.",
        /*include_page_context=*/true,
        /*include_system_prompt=*/false,
        /*attach_screenshot=*/true);
    return;
  }
  dashboard::ScriptExecutionService::RunOptions run_options;
  run_options.is_agent_mode = true;
  const std::string execution_id = exec_service->StartAdHocRunWithOptions(
      timeline, active_web_contents_, run_options,
      base::BindOnce(&GlicPromptAction::OnTimelineCommandComplete,
                     weak_ptr_factory_.GetWeakPtr(), std::move(command_name_copy),
                     std::move(payload_copy)));
  if (execution_id.empty()) {
    Finalize(false, "Failed to queue agent command execution");
    return;
  }
}

void GlicPromptAction::ContinueAfterActionWhenPageReady(
    base::Value::Dict prior_command,
    base::Value::Dict prior_result,
    std::string previous_page_title,
    std::string previous_page_url,
    int wait_attempt) {
  if (!pending_callback_.has_value() || !active_web_contents_) {
    return;
  }
  if (ShouldWaitForAgentPageReady(active_web_contents_, wait_attempt)) {
    base::Value::List* steps = trace_.FindList("steps");
    base::Value::Dict* step =
        (steps && !steps->empty()) ? &steps->back().GetDict() : nullptr;
    if (step) {
      base::Value::Dict gate;
      gate.Set("stage", "after_action");
      gate.Set("attempt", wait_attempt);
      gate.Set("load_progress", active_web_contents_->GetLoadProgress());
      gate.Set("is_loading", active_web_contents_->IsLoading());
      step->Set("page_load_gate", std::move(gate));
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&GlicPromptAction::ContinueAfterActionWhenPageReady,
                       weak_ptr_factory_.GetWeakPtr(),
                       std::move(prior_command), std::move(prior_result),
                       std::move(previous_page_title),
                       std::move(previous_page_url), wait_attempt + 1),
        kAgentPageReadyPollInterval);
    return;
  }
  FetchUnifiedPageMapSnapshot(
      active_web_contents_,
      base::BindOnce(&GlicPromptAction::OnPostActionContextRefreshComplete,
                     weak_ptr_factory_.GetWeakPtr(), std::move(prior_command),
                     std::move(prior_result), std::move(previous_page_title),
                     std::move(previous_page_url)));
}

void GlicPromptAction::RefreshContextCommandWhenPageReady(
    std::string command_name,
    base::Value::Dict payload,
    int wait_attempt) {
  if (!pending_callback_.has_value() || !active_web_contents_) {
    return;
  }
  if (ShouldWaitForAgentPageReady(active_web_contents_, wait_attempt)) {
    base::Value::List* steps = trace_.FindList("steps");
    base::Value::Dict* step =
        (steps && !steps->empty()) ? &steps->back().GetDict() : nullptr;
    if (step) {
      base::Value::Dict gate;
      gate.Set("stage", "before_context_refresh");
      gate.Set("attempt", wait_attempt);
      gate.Set("load_progress", active_web_contents_->GetLoadProgress());
      gate.Set("is_loading", active_web_contents_->IsLoading());
      step->Set("page_load_gate", std::move(gate));
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&GlicPromptAction::RefreshContextCommandWhenPageReady,
                       weak_ptr_factory_.GetWeakPtr(),
                       std::move(command_name), std::move(payload),
                       wait_attempt + 1),
        kAgentPageReadyPollInterval);
    return;
  }
  FetchUnifiedPageMapSnapshot(
      active_web_contents_,
      base::BindOnce(&GlicPromptAction::OnContextRefreshComplete,
                     weak_ptr_factory_.GetWeakPtr(), std::move(command_name),
                     std::move(payload)));
}

void GlicPromptAction::RefreshScreenshotContextWhenPageReady(
    std::string command_name,
    base::Value::Dict payload,
    int wait_attempt) {
  if (!pending_callback_.has_value() || !active_web_contents_) {
    return;
  }
  if (ShouldWaitForAgentPageReady(active_web_contents_, wait_attempt)) {
    base::Value::List* steps = trace_.FindList("steps");
    base::Value::Dict* step =
        (steps && !steps->empty()) ? &steps->back().GetDict() : nullptr;
    if (step) {
      base::Value::Dict gate;
      gate.Set("stage", "before_screenshot_refresh");
      gate.Set("attempt", wait_attempt);
      gate.Set("load_progress", active_web_contents_->GetLoadProgress());
      gate.Set("is_loading", active_web_contents_->IsLoading());
      step->Set("page_load_gate", std::move(gate));
    }
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&GlicPromptAction::RefreshScreenshotContextWhenPageReady,
                       weak_ptr_factory_.GetWeakPtr(),
                       std::move(command_name), std::move(payload),
                       wait_attempt + 1),
        kAgentPageReadyPollInterval);
    return;
  }
  FetchUnifiedPageMapSnapshot(
      active_web_contents_,
      base::BindOnce(&GlicPromptAction::OnScreenshotContextRefreshComplete,
                     weak_ptr_factory_.GetWeakPtr(), std::move(command_name),
                     std::move(payload)));
}

void GlicPromptAction::OnTimelineCommandComplete(std::string command_name,
                                                 base::Value::Dict payload,
                                                 bool success,
                                                 int failed_step,
                                                 std::string message) {
  if (!pending_callback_.has_value()) {
    return;
  }
  base::Value::List* steps = trace_.FindList("steps");
  base::Value::Dict* step =
      (steps && !steps->empty()) ? &steps->back().GetDict() : nullptr;

  base::Value::Dict result;
  result.Set("ok", success);
  if (failed_step >= 0) {
    result.Set("failed_step", failed_step);
  }
  if (!message.empty()) {
    result.Set("message", message);
  }
  if (step) {
    step->Set("command_result", result.Clone());
  }

  if (command_name == "call_action") {
    const std::string* action_name = payload.FindString("action");
    if (action_name && !action_name->empty()) {
      base::Value::Dict recorded;
      recorded.Set("action", *action_name);
      if (const base::Value::Dict* params = payload.FindDict("params")) {
        recorded.Set("params", params->Clone());
      } else {
        recorded.Set("params", base::Value::Dict());
      }
      task_actions_.Append(BuildRecordedTaskAction(
          recorded, static_cast<int>(task_actions_.size()), step_index_,
          success ? "succeeded" : "failed", /*from_batch=*/false));
    }
  } else if (command_name == "run_timeline") {
    const base::Value::List* executed_timeline = payload.FindList("timeline");
    if (executed_timeline) {
      for (size_t i = 0; i < executed_timeline->size(); ++i) {
        const base::Value::Dict* action = (*executed_timeline)[i].GetIfDict();
        const std::string* action_name = action ? action->FindString("action") : nullptr;
        if (!action || !action_name || action_name->empty()) {
          continue;
        }
        std::string execution = "succeeded";
        if (!success) {
          if (failed_step >= 0 && static_cast<int>(i) > failed_step) {
            execution = "skipped";
          } else if (failed_step >= 0 && static_cast<int>(i) == failed_step) {
            execution = "failed";
          } else if (failed_step < 0) {
            execution = "failed";
          }
        }
        task_actions_.Append(BuildRecordedTaskAction(
            action->Clone(), static_cast<int>(task_actions_.size()), step_index_,
            execution, /*from_batch=*/true));
      }
    }
  }

  if (!success) {
    failure_streak_++;
    if (failure_streak_ > kMaxAutonomousFailureStreak) {
      Finalize(false, base::StrCat(
                          {"Agent command failed repeatedly after command ",
                           command_name, message.empty() ? std::string()
                                                         : base::StrCat({": ", message})}));
      return;
    }
  } else {
    failure_streak_ = 0;
  }

  if (step_index_ >= (MaxAutonomousAgentSteps() - 1)) {
    Finalize(false, "Agent step budget exceeded");
    return;
  }

  const std::string* completed_action_name = payload.FindString("action");
  const bool should_refresh_post_action_context =
      success && active_web_contents_ &&
      ((command_name == "call_action" && completed_action_name &&
        ActionLikelyChangesPageFocusOrInteractiveState(
            *completed_action_name)) ||
       (command_name == "run_timeline" &&
        TimelineLikelyChangesPageFocusOrInteractiveState(payload)));
  if (should_refresh_post_action_context) {
    const std::string previous_page_title = current_page_title_;
    const std::string previous_page_url = current_page_url_;
    base::Value::Dict prior_command;
    if (step) {
      if (const base::Value::Dict* command = step->FindDict("command")) {
        prior_command = command->Clone();
      }
    }
    ContinueAfterActionWhenPageReady(std::move(prior_command), std::move(result),
                                     previous_page_title, previous_page_url,
                                     /*wait_attempt=*/0);
    return;
  }

  const bool include_page_context = pending_context_refresh_;
  pending_context_refresh_ = false;
  base::Value::Dict prior_command;
  if (step) {
    if (const base::Value::Dict* command = step->FindDict("command")) {
      prior_command = command->Clone();
    }
  }
  const std::string followup =
      use_choice_response_format_
          ? BuildChoiceFollowupTurnNote(prior_command, result, step_index_,
                                        MaxAutonomousAgentSteps())
          : BuildAutonomousFollowupPrompt(
                prior_command, result, step_index_, MaxAutonomousAgentSteps(),
                include_page_context,
                include_page_context
                    ? ComposePageContextForModel(
                          /*include_page_context=*/true,
                          /*force_full_context=*/false,
                          /*screenshot_attached=*/false)
                    : std::string());
  ++step_index_;
  StartPromptTurn(followup, /*include_page_context=*/false,
                  /*include_system_prompt=*/false,
                  /*attach_screenshot=*/false);
}

void GlicPromptAction::OnPostActionContextRefreshComplete(
    base::Value::Dict prior_command,
    base::Value::Dict prior_result,
    std::string previous_page_title,
    std::string previous_page_url,
    dashboard::UnifiedPageMapResult result) {
  if (!pending_callback_.has_value()) {
    return;
  }
  if (active_web_contents_) {
    current_page_title_ = base::UTF16ToUTF8(active_web_contents_->GetTitle());
    current_page_url_ = active_web_contents_->GetLastCommittedURL().spec();
  }
  if (result.ok) {
    current_page_map_ = result.page_map.Clone();
  }
  const bool page_changed = !previous_page_url.empty() &&
                            previous_page_url != current_page_url_;
  const bool title_changed = !previous_page_title.empty() &&
                             previous_page_title != current_page_title_;
  const std::string overlay_summary =
      current_page_map_.empty() ? std::string()
                                : FindProminentOverlaySummary(current_page_map_);
  if (page_changed) {
    prior_result.Set("page_changed", true);
    prior_result.Set("previous_page_url", previous_page_url);
    prior_result.Set("current_page_url", current_page_url_);
  }
  if (title_changed) {
    prior_result.Set("title_changed", true);
    prior_result.Set("previous_page_title", previous_page_title);
    prior_result.Set("current_page_title", current_page_title_);
  }
  if (!overlay_summary.empty()) {
    prior_result.Set("overlay_detected", true);
    prior_result.Set("overlay_summary", overlay_summary);
  }
  if (page_changed || title_changed || !overlay_summary.empty()) {
    prior_result.Set("significant_mutation", true);
  }

  base::Value::List* steps = trace_.FindList("steps");
  base::Value::Dict* step =
      (steps && !steps->empty()) ? &steps->back().GetDict() : nullptr;
  if (step) {
    step->Set("command_result", prior_result.Clone());
  }

  const std::string followup =
      use_choice_response_format_
          ? BuildChoiceFollowupTurnNote(prior_command, prior_result, step_index_,
                                        MaxAutonomousAgentSteps())
          : BuildAutonomousFollowupPrompt(
                prior_command, prior_result, step_index_,
                MaxAutonomousAgentSteps(),
                /*include_page_context=*/true,
                ComposePageContextForModel(
                    /*include_page_context=*/true,
                    /*force_full_context=*/page_changed,
                    /*screenshot_attached=*/false));
  ++step_index_;
  StartPromptTurn(followup, /*include_page_context=*/false,
                  /*include_system_prompt=*/false,
                  /*attach_screenshot=*/false);
}

void GlicPromptAction::OnContextRefreshComplete(
    std::string command_name,
    base::Value::Dict payload,
    dashboard::UnifiedPageMapResult result) {
  if (!pending_callback_.has_value()) {
    return;
  }
  if (active_web_contents_) {
    current_page_title_ = base::UTF16ToUTF8(active_web_contents_->GetTitle());
    current_page_url_ = active_web_contents_->GetLastCommittedURL().spec();
  }
  if (result.ok) {
    current_page_map_ = result.page_map.Clone();
  }

  base::Value::List* steps = trace_.FindList("steps");
  base::Value::Dict* step =
      (steps && !steps->empty()) ? &steps->back().GetDict() : nullptr;
  base::Value::Dict command_result;
  command_result.Set("ok", result.ok);
  command_result.Set("message",
                     result.ok ? "Context refreshed."
                               : "Context refresh failed.");
  if (step) {
    step->Set("command_result", command_result.Clone());
  }

  if (!result.ok) {
    failure_streak_++;
    if (failure_streak_ > kMaxAutonomousFailureStreak) {
      Finalize(false, "Context refresh failed repeatedly");
      return;
    }
  } else {
    failure_streak_ = 0;
  }

  if (step_index_ >= (MaxAutonomousAgentSteps() - 1)) {
    Finalize(false, "Agent step budget exceeded");
    return;
  }

  pending_context_refresh_ = true;
  base::Value::Dict prior_command;
  if (step) {
    if (const base::Value::Dict* command = step->FindDict("command")) {
      prior_command = command->Clone();
    }
  }
  const std::string followup =
      use_choice_response_format_
          ? BuildChoiceFollowupTurnNote(prior_command, command_result, step_index_,
                                        MaxAutonomousAgentSteps())
          : BuildAutonomousFollowupPrompt(
                prior_command, command_result, step_index_,
                MaxAutonomousAgentSteps(),
                /*include_page_context=*/true,
                ComposePageContextForModel(
                    /*include_page_context=*/true,
                    /*force_full_context=*/false,
                    /*screenshot_attached=*/false));
  ++step_index_;
  StartPromptTurn(followup, /*include_page_context=*/false,
                  /*include_system_prompt=*/false,
                  /*attach_screenshot=*/false);
}

void GlicPromptAction::OnScreenshotContextRefreshComplete(
    std::string command_name,
    base::Value::Dict payload,
    dashboard::UnifiedPageMapResult result) {
  (void)command_name;
  (void)payload;
  if (!pending_callback_.has_value()) {
    return;
  }
  if (active_web_contents_) {
    current_page_title_ = base::UTF16ToUTF8(active_web_contents_->GetTitle());
    current_page_url_ = active_web_contents_->GetLastCommittedURL().spec();
  }
  if (result.ok) {
    current_page_map_ = result.page_map.Clone();
  }

  base::Value::List* steps = trace_.FindList("steps");
  base::Value::Dict* step =
      (steps && !steps->empty()) ? &steps->back().GetDict() : nullptr;
  base::Value::Dict command_result;
  auto [screenshot_clip, screenshot_request_meta] =
      ResolveScreenshotRequest(payload, focus_areas_, current_page_map_);
  command_result.Set("ok", result.ok);
  command_result.Set(
      "message",
      result.ok ? "Screenshot requested. Current context refreshed."
                : "Screenshot requested, but context refresh failed.");
  command_result.Set("screenshot_requested", true);
  if (!screenshot_request_meta.empty()) {
    command_result.Set("screenshot_request", screenshot_request_meta.Clone());
  }
  if (step) {
    step->Set("command_result", command_result.Clone());
  }

  if (!result.ok) {
    failure_streak_++;
    if (failure_streak_ > kMaxAutonomousFailureStreak) {
      Finalize(false, "Screenshot context refresh failed repeatedly");
      return;
    }
  } else {
    failure_streak_ = 0;
  }

  if (step_index_ >= (MaxAutonomousAgentSteps() - 1)) {
    Finalize(false, "Agent step budget exceeded");
    return;
  }

  base::Value::Dict prior_command;
  if (step) {
    if (const base::Value::Dict* command = step->FindDict("command")) {
      prior_command = command->Clone();
    }
  }
  const std::string followup =
      use_choice_response_format_
          ? BuildChoiceFollowupTurnNote(prior_command, command_result, step_index_,
                                        MaxAutonomousAgentSteps())
          : BuildAutonomousFollowupPrompt(
                prior_command, command_result, step_index_,
                MaxAutonomousAgentSteps(),
                /*include_page_context=*/true,
                ComposePageContextForModel(
                    /*include_page_context=*/true,
                    /*force_full_context=*/false,
                    /*screenshot_attached=*/true));
  ++step_index_;
  StartPromptTurn(followup, /*include_page_context=*/false,
                  /*include_system_prompt=*/false,
                  /*attach_screenshot=*/true, screenshot_clip,
                  std::move(screenshot_request_meta));
}

void GlicPromptAction::RecordTraceCompletion(bool success,
                                             std::string_view message) {
  trace_.Set("ok", success);
  trace_.Set(
      "finished_at",
      static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  trace_.Set("final_message", std::string(message));
  if (!last_model_response_.empty()) {
    trace_.Set("last_model_response", last_model_response_);
  }
  if (!task_actions_.empty()) {
    trace_.Set("task_actions", task_actions_.Clone());
  }
  if (!auto_saved_script_id_.empty()) {
    trace_.Set("auto_saved_script_id", auto_saved_script_id_);
  }
  if (!auto_saved_script_name_.empty()) {
    trace_.Set("auto_saved_script_name", auto_saved_script_name_);
  }
  trace_.Set("current_page_url", current_page_url_);
  trace_.Set("current_page_title", current_page_title_);
}

bool GlicPromptAction::AutoSaveSucceededTaskScript(std::string* out_script_name,
                                                   std::string* out_error) {
  base::Value::List timeline = BuildSucceededTimeline(task_actions_);
  if (timeline.empty()) {
    if (out_error) {
      *out_error = "No successful actions were recorded to save.";
    }
    return false;
  }

  std::string script_name = SanitizeSavedScriptName(
      base::StrCat({"Agent: ", TruncateForPrompt(prompt_, 48u)}));
  const std::string script_id =
      auto_saved_script_id_.empty()
          ? base::Uuid::GenerateRandomV4().AsLowercaseString()
          : auto_saved_script_id_;

  base::Value::Dict script;
  script.Set("name", script_name);
  script.Set("kind", "main");
  script.Set("actions", timeline.Clone());
  if (!current_page_url_.empty()) {
    script.Set("startUrl", current_page_url_);
  }
  script.Set("source", "glic_prompt_action_autosave");
  script.Set("sourceGoal", prompt_);
  const base::Value::Dict canonical = dashboard::ScriptSchema::CanonicalizeForSave(
      std::move(script), script_id, base::Time::Now());
  if (!dashboard::ScriptStore::Write(profile_, script_id, canonical)) {
    if (out_error) {
      *out_error = "Failed to auto-save generated script.";
    }
    return false;
  }
  DashboardUIMessageHandler::UpdateRuntimeScriptCache(profile_, script_id,
                                                      canonical.Clone());

  auto_saved_script_id_ = script_id;
  auto_saved_script_name_ = script_name;
  trace_.Set("saved_script", canonical.Clone());
  if (out_script_name) {
    *out_script_name = script_name;
  }
  LOG(INFO) << "[GlicPromptAction] Auto-saved script id=" << script_id
            << " name=" << script_name
            << " actions=" << timeline.size();
  return true;
}

void GlicPromptAction::BeginPostTaskScriptReview(
    const std::string& draft_script_name,
    const std::string& completion_summary) {
  script_review_active_ = true;
  save_script_menu_active_ = true;
  controls_menu_active_ = false;
  pending_completion_summary_ = completion_summary;
  ++step_index_;
  StartPromptTurn(
      BuildPostTaskScriptReviewPrompt(prompt_, task_actions_, draft_script_name),
      /*include_page_context=*/false,
      /*include_system_prompt=*/true,
      /*attach_screenshot=*/false);
}

void GlicPromptAction::Finalize(bool success, std::string message) {
  if (!pending_callback_.has_value()) {
    return;
  }

  RecordTraceCompletion(success, message);
  const auto [trace_path, latest_trace_path] =
      BuildAgentTracePaths(profile_, trace_);
  ScheduleTraceWrite(trace_path, latest_trace_path, trace_.Clone(),
                     base::TaskShutdownBehavior::BLOCK_SHUTDOWN);

  Callback callback = std::move(*pending_callback_);
  pending_callback_.reset();
  ResetRunState();
  std::move(callback).Run(success,
                          BuildFinalMessage(success ? "Agent run completed"
                                                   : "Agent run failed",
                                            trace_path, message));
}

}  // namespace actions
}  // namespace dashboard
