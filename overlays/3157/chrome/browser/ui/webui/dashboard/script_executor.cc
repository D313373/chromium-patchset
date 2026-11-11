// script_executor.cc — Updated with step tracking and last_run recording

#include "chrome/browser/ui/webui/dashboard/script_executor.h"

#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/time/time.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/uuid.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/hash/hash.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_factory.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_metadata_promotion.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_types.h"
#include "chrome/browser/ui/webui/dashboard/click_handler.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"
#include "chrome/browser/ui/webui/dashboard/page_map_extractor.h"
#include "chrome/browser/ui/webui/dashboard/script_context.h"
#include "chrome/browser/ui/webui/dashboard/script_schema.h"
#include "chrome/browser/ui/webui/dashboard/unified_page_context.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/execution_run_context.h"
#include "chrome/browser/ui/webui/dashboard/native_run_activation.h"
#include "chrome/browser/ui/webui/dashboard/failure_replay_recorder.h"
#include "chrome/browser/ui/webui/dashboard/run_history_store.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/common/chrome_switches.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/common/referrer.h"
#include "ui/base/page_transition_types.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"

namespace {
constexpr int kFailureReplayFps = 2;
const base::TimeDelta kFailureReplayMaxDuration = base::Seconds(15);
constexpr int kHumanizedInterActionDelayMinMs = 1000;
constexpr int kHumanizedInterActionDelayMaxMs = 5000;
constexpr int kStepTrainingScreenshotMaxWidthPx = 1280;
constexpr int kStepTrainingScreenshotMaxHeightPx = 720;
constexpr char kDatasetVersion[] = "0.1.0-prealpha";
constexpr char kDatasetSchemaVersion[] = "simulachrome_dataset_v1";
constexpr char kMmStepSchema[] = "simulachrome_mm_step_v2";
constexpr char kDatasetVersionManifestFile[] = "DATASET_VERSION.json";
constexpr char kTraceBucketSchemaVersion[] = "simulachrome_trace_bucket_v1";
constexpr int kTraceBucketFullDetailRetentionRuns = 3;
constexpr int kTraceBucketFullTrainingRetentionRuns = 2;
constexpr int kAuthRecoveryRequiredConsecutiveFailures = 2;
constexpr char kNestedActionPathPrefix[] = "[nested_action_path=";
constexpr char kNestedActionPathSuffix = ']';

bool IsHumanCaptchaPauseAction(const base::Value::Dict* action_dict) {
  if (!action_dict) {
    return false;
  }
  // Legacy wait markers only. New scripts use glic_prompt on the script tab.
  const std::string* kind = action_dict->FindString("action");
  if (!kind || *kind != "wait") {
    return false;
  }
  const base::Value::Dict* params = action_dict->FindDict("params");
  if (!params) {
    return false;
  }
  return params->FindBool("__glic_human_captcha").value_or(false) ||
         params->FindBool("humanCaptcha").value_or(false);
}

bool ShouldRefreshApcBeforeAction(std::string_view kind) {
  // Do not force APC cache invalidation before ordinary click/type. That path
  // was causing repeated heavy content extraction during form fills and page
  // load. Listen/scrape still benefit from a fresh map.
  if (!dashboard::ExecutionRunContext::Get().ShouldUseUnifiedPageMap()) {
    return false;
  }
  return kind == "listen" || kind == "scrape_table" ||
         kind == "scrape_element";
}

bool IsFailureReplayCaptureEnabled() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          "enable-dashboard-failure-replay")) {
    return true;
  }
  if (const base::Value::Dict* performance =
          dashboard::GlobalSettingsStore().FindDict("performance")) {
    return performance->FindBool("failureReplayEnabled").value_or(false);
  }
  return false;
}

struct ActionValidationFailure {
  size_t top_level_index = 0;
  std::string action_index_label;
  std::string kind;
  std::string error;
};

struct AuthStateSignals {
  bool auth_required = false;
  bool url_suggests_login = false;
  bool has_password_field = false;
  bool has_identity_field = false;
  bool has_login_button = false;
  // Visible log-out / avatar control. A logged-in page often still contains
  // a "Sign in" entry in a menu; this is what keeps that from looking like a wall.
  bool has_session_indicator = false;
  int score = 0;
  std::vector<std::string> reasons;
};

std::string DescribeLoopActionFactoryFailure(
    dashboard::actions::ActionFactory* action_factory,
    const base::Value::Dict& action,
    const std::string& action_index_label);

void RehydrateKeyPressActionIfNeeded(
    dashboard::actions::Action* action,
    const base::Value::Dict* source_action_dict) {
  if (!action || !source_action_dict) {
    return;
  }

  const std::string* source_kind = source_action_dict->FindString("action");
  if (!source_kind || *source_kind != "key_press") {
    return;
  }

  const base::Value::Dict* source_params = source_action_dict->FindDict("params");
  if (!source_params) {
    return;
  }

  base::Value::Dict serialized;
  action->SerializeInto(&serialized);
  const std::string* serialized_kind = serialized.FindString("action");
  const base::Value::Dict* serialized_params = serialized.FindDict("params");
  const bool has_key =
      serialized_params &&
      (serialized_params->FindString("key") || serialized_params->FindString("value"));
  if (serialized_kind && *serialized_kind == "key_press" && has_key) {
    return;
  }

  if (action->SetParams(*source_params)) {
    LOG(WARNING) << "[Executor] Rehydrated key_press params from source action";
  } else {
    LOG(WARNING) << "[Executor] Failed to rehydrate key_press params from source action";
  }
}

std::string NormalizeHeuristicText(std::string text) {
  base::TrimWhitespaceASCII(text, base::TRIM_ALL, &text);
  text = base::ToLowerASCII(text);
  return text;
}

bool ContainsAuthNeedle(std::string_view haystack,
                        std::initializer_list<std::string_view> needles) {
  for (std::string_view needle : needles) {
    if (haystack.find(needle) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

bool UrlSuggestsLoggedOut(const GURL& url) {
  if (!url.is_valid()) {
    return false;
  }
  std::string haystack = std::string(url.path());
  haystack.append("?");
  haystack.append(std::string(url.query()));
  haystack.append("#");
  haystack.append(std::string(url.ref()));
  haystack = NormalizeHeuristicText(std::move(haystack));
  return ContainsAuthNeedle(
      haystack,
      {"login", "log-in", "signin", "sign-in", "logon", "auth",
       "authenticate", "reauth", "session-expired", "session_expired",
       "logged-out", "logged_out"});
}

// Page-map elements always carry bounds. A zero-area node is a dormant
// modal or an off-screen control, and scoring it starts a login on a page
// the user is already in.
bool ElementHasVisibleBounds(const base::Value::Dict& element) {
  const base::Value::Dict* bounds = element.FindDict("bounds");
  if (!bounds) {
    return false;
  }
  double width = bounds->FindDouble("w").value_or(0);
  double height = bounds->FindDouble("h").value_or(0);
  if (width < 1.0) {
    width = bounds->FindDouble("width").value_or(0);
  }
  if (height < 1.0) {
    height = bounds->FindDouble("height").value_or(0);
  }
  return width >= 1.0 && height >= 1.0;
}

std::string JoinSignalText(const base::Value::Dict& element) {
  std::vector<std::string> parts;
  auto append = [&](const char* key) {
    if (const std::string* value = element.FindString(key); value &&
        !value->empty()) {
      parts.push_back(*value);
    }
  };
  append("text");
  append("accessibilityName");
  append("idAttr");
  append("classAttr");
  append("nameAttr");
  append("role");
  append("type");
  append("tag");
  return NormalizeHeuristicText(base::JoinString(parts, " "));
}

void InspectAuthElementSignals(const base::Value::Dict& element,
                               AuthStateSignals* signals) {
  if (!signals) {
    return;
  }
  const std::string signal_text = JoinSignalText(element);
  if (signal_text.empty()) {
    return;
  }

  const bool clickable = element.FindBool("clickable").value_or(false);
  const bool focusable = element.FindBool("focusable").value_or(false);
  const bool editable = element.FindBool("editable").value_or(false);
  const bool disabled = element.FindBool("disabled").value_or(false);
  if (disabled || !ElementHasVisibleBounds(element)) {
    return;
  }

  const std::string tag =
      NormalizeHeuristicText(element.FindString("tag")
                                 ? *element.FindString("tag")
                                 : std::string());
  const bool is_input_like =
      editable || tag == "input" || tag == "textarea";

  if (!signals->has_password_field && is_input_like &&
      ContainsAuthNeedle(signal_text, {"password", "passcode", "passwd"})) {
    signals->has_password_field = true;
    signals->score += 4;
    signals->reasons.push_back("password_field");
  }

  if (!signals->has_identity_field && is_input_like &&
      ContainsAuthNeedle(signal_text,
                         {"username", "user name", "userid", "user id",
                          "email", "e-mail", "account"})) {
    signals->has_identity_field = true;
    signals->score += 2;
    signals->reasons.push_back("identity_field");
  }

  if (!signals->has_login_button && (clickable || focusable) &&
      ContainsAuthNeedle(signal_text,
                         {"login", "log in", "signin", "sign in",
                          "sign-in", "logon", "log on"})) {
    signals->has_login_button = true;
    signals->score += 3;
    signals->reasons.push_back("login_button");
  }

  // Counted once. "log out" does not contain the login needles above, so a
  // real login button is not mistaken for a session.
  if (!signals->has_session_indicator && (clickable || focusable) &&
      ContainsAuthNeedle(signal_text,
                         {"log out", "logout", "sign out", "signout",
                          "avatar"})) {
    signals->has_session_indicator = true;
    signals->score -= 6;
    signals->reasons.push_back("session_indicator");
  }
}

void InspectAuthSignalsFromPageMap(const base::Value::Dict& page_map,
                                   AuthStateSignals* signals) {
  if (!signals) {
    return;
  }
  const base::Value::List* frames = page_map.FindList("frames");
  if (!frames) {
    return;
  }

  auto inspect_element_list = [&](const base::Value::List* elements) {
    if (!elements) {
      return;
    }
    for (const auto& item : *elements) {
      const base::Value::Dict* element = item.GetIfDict();
      if (!element) {
        continue;
      }
      InspectAuthElementSignals(*element, signals);
    }
  };

  for (const auto& frame_value : *frames) {
    const base::Value::Dict* frame = frame_value.GetIfDict();
    if (!frame) {
      continue;
    }
    inspect_element_list(frame->FindList("elements"));
    if (const base::Value::List* shadow_roots = frame->FindList("shadowRoots")) {
      for (const auto& shadow_value : *shadow_roots) {
        const base::Value::Dict* shadow = shadow_value.GetIfDict();
        if (!shadow) {
          continue;
        }
        inspect_element_list(shadow->FindList("elements"));
      }
    }
  }
}

AuthStateSignals DetectAuthStateSignals(const base::Value::Dict& page_map,
                                        const GURL& current_url) {
  AuthStateSignals signals;
  if (UrlSuggestsLoggedOut(current_url)) {
    signals.url_suggests_login = true;
    signals.score += 3;
    signals.reasons.push_back("url_login_hint");
  }
  InspectAuthSignalsFromPageMap(page_map, &signals);
  // A visible session control vetoes the wall. Change-password and account
  // pages have password fields too, and a header "Sign in" is not a login.
  signals.auth_required =
      !signals.has_session_indicator && signals.has_password_field &&
      (signals.has_identity_field || signals.has_login_button ||
       signals.url_suggests_login);
  if (!signals.auth_required && !signals.has_session_indicator &&
      signals.score >= 6 &&
      (signals.has_login_button || signals.url_suggests_login)) {
    signals.auth_required = true;
  }
  return signals;
}

std::string DescribeAuthStateSignals(const AuthStateSignals& signals) {
  if (signals.reasons.empty()) {
    return "no_auth_signals";
  }
  return base::JoinString(signals.reasons, ",");
}

std::string EncodeBytesToBase64(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) {
    return std::string();
  }
  return base::Base64Encode(base::span<const uint8_t>(bytes));
}

bool DecodeBase64ToBytes(std::string_view encoded,
                         std::vector<uint8_t>* out_bytes) {
  if (!out_bytes) {
    return false;
  }
  std::optional<std::vector<uint8_t>> decoded = base::Base64Decode(encoded);
  if (!decoded.has_value()) {
    return false;
  }
  *out_bytes = std::move(*decoded);
  return true;
}

std::optional<gfx::Size> ComputeTrainingCaptureSize(
    content::RenderWidgetHostView* view) {
  if (!view) {
    return std::nullopt;
  }
  const gfx::Size src = view->GetViewBounds().size();
  if (src.IsEmpty()) {
    return std::nullopt;
  }
  const double scale_w = static_cast<double>(kStepTrainingScreenshotMaxWidthPx) /
                         std::max(1, src.width());
  const double scale_h =
      static_cast<double>(kStepTrainingScreenshotMaxHeightPx) /
      std::max(1, src.height());
  const double scale = std::min({scale_w, scale_h, 1.0});
  const int out_w =
      std::max(1, static_cast<int>(std::floor(src.width() * scale)));
  const int out_h =
      std::max(1, static_cast<int>(std::floor(src.height() * scale)));
  return gfx::Size(out_w, out_h);
}

base::FilePath SimulachromeTrainingRoot(Profile* profile) {
  if (!profile) {
    return base::FilePath();
  }
  return profile->GetPath().AppendASCII("SimulachromeTrainingData");
}

base::Value::Dict LoadPersistedGlobalSettingsBlocking(Profile* profile) {
  if (!profile) {
    return dashboard::DefaultGlobalSettings();
  }
  const base::FilePath settings_path =
      dashboard::ScriptStore::ScriptsDir(profile).AppendASCII(
          "global_settings.json");
  std::string json;
  if (!base::ReadFileToString(settings_path, &json)) {
    return dashboard::DefaultGlobalSettings();
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    return dashboard::DefaultGlobalSettings();
  }
  return parsed->GetDict().Clone();
}

void ApplyInputBehaviorSettings(const base::Value::Dict& input_behavior) {
  const double jitter =
      input_behavior.FindDouble("clickPathJitterPx").value_or(1.5);
  const double hover_radius =
      input_behavior.FindDouble("clickHoverRadiusPx").value_or(2.0);
  const int hover_moves = input_behavior.FindInt("clickHoverMoves").value_or(3);
  const bool overlay_enabled =
      input_behavior.FindBool("showPlaybackOverlay").value_or(true);
  const bool loop_preview_enabled =
      input_behavior.FindBool("loopPreviewEnabled").value_or(false);

  dashboard::SetClickPathJitterPx(std::clamp(jitter, 0.0, 12.0));
  dashboard::SetClickMicroHoverRadiusPx(std::clamp(hover_radius, 0.0, 24.0));
  dashboard::SetClickMicroHoverMoves(std::clamp(hover_moves, 0, 10));
  dashboard::SetPlaybackOverlayEnabled(overlay_enabled);
  dashboard::SetLoopPreviewEnabled(loop_preview_enabled);
}

base::Value::Dict BuildDatasetMetadata() {
  base::Value::Dict meta;
  meta.Set("dataset_version", kDatasetVersion);
  meta.Set("dataset_schema_version", kDatasetSchemaVersion);
  meta.Set("step_schema", kMmStepSchema);
  meta.Set("writer", "script_executor");
  return meta;
}

void WriteDatasetVersionManifest(const base::FilePath& training_root) {
  if (training_root.empty()) {
    return;
  }
  if (!base::CreateDirectory(training_root)) {
    return;
  }
  base::Value::Dict manifest = BuildDatasetMetadata();
  manifest.Set(
      "generated_at_unix_ms",
      base::NumberToString(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  const base::FilePath manifest_path =
      training_root.AppendASCII(kDatasetVersionManifestFile);
  std::string json;
  if (!base::JSONWriter::Write(manifest, &json)) {
    return;
  }
  base::WriteFile(manifest_path, base::as_byte_span(json));
}

base::Value::Dict ClassifyFailureTaxonomy(const std::string& raw_message) {
  std::string message = raw_message;
  base::TrimWhitespaceASCII(message, base::TRIM_ALL, &message);

  base::Value::Dict taxonomy;
  taxonomy.Set("category", "unknown");
  taxonomy.Set("subcategory", "unknown");
  taxonomy.Set("retryable", false);
  taxonomy.Set("raw_message", message);
  if (message.empty()) {
    return taxonomy;
  }

  const std::string lower = base::ToLowerASCII(message);
  if (lower.find("timeout") != std::string::npos) {
    taxonomy.Set("category", "timeout");
    taxonomy.Set("subcategory", "wait_or_navigation_timeout");
    taxonomy.Set("retryable", true);
    return taxonomy;
  }
  if (lower.find("target webcontents was destroyed") != std::string::npos ||
      lower.find("no render frame") != std::string::npos ||
      lower.find("no frame") != std::string::npos) {
    taxonomy.Set("category", "target_lifecycle");
    taxonomy.Set("subcategory", "tab_or_frame_destroyed");
    taxonomy.Set("retryable", true);
    return taxonomy;
  }
  if (lower.find("not found") != std::string::npos ||
      lower.find("no_match") != std::string::npos ||
      lower.find("locator") != std::string::npos ||
      lower.find("element") != std::string::npos) {
    taxonomy.Set("category", "locator_resolution");
    taxonomy.Set("subcategory", "element_not_found_or_drift");
    taxonomy.Set("retryable", true);
    return taxonomy;
  }
  if (lower.find("permission") != std::string::npos ||
      lower.find("denied") != std::string::npos ||
      lower.find("blocked") != std::string::npos) {
    taxonomy.Set("category", "permission_or_policy");
    taxonomy.Set("subcategory", "blocked_or_denied");
    taxonomy.Set("retryable", false);
    return taxonomy;
  }
  if (lower.find("invalid") != std::string::npos ||
      lower.find("schema") != std::string::npos ||
      lower.find("parse") != std::string::npos) {
    taxonomy.Set("category", "input_validation");
    taxonomy.Set("subcategory", "invalid_action_or_data");
    taxonomy.Set("retryable", false);
    return taxonomy;
  }

  return taxonomy;
}

std::optional<std::string> ConsumeNestedActionPathToken(std::string* message) {
  if (!message) {
    return std::nullopt;
  }
  std::string text = *message;
  base::TrimWhitespaceASCII(text, base::TRIM_ALL, &text);
  if (!base::StartsWith(text, kNestedActionPathPrefix,
                        base::CompareCase::SENSITIVE)) {
    return std::nullopt;
  }
  const size_t prefix_len = strlen(kNestedActionPathPrefix);
  const size_t suffix_pos = text.find(kNestedActionPathSuffix, prefix_len);
  if (suffix_pos == std::string::npos || suffix_pos <= prefix_len) {
    return std::nullopt;
  }
  std::string path = text.substr(prefix_len, suffix_pos - prefix_len);
  base::TrimWhitespaceASCII(path, base::TRIM_ALL, &path);
  if (path.empty()) {
    return std::nullopt;
  }
  std::string remainder = text.substr(suffix_pos + 1);
  base::TrimWhitespaceASCII(remainder, base::TRIM_ALL, &remainder);
  *message = remainder;
  return path;
}

std::string BuildActionIndexLabel(size_t top_level_index,
                                  const std::string& nested_path = std::string()) {
  std::string label =
      base::NumberToString(static_cast<int>(top_level_index) + 1);
  if (!nested_path.empty()) {
    label.append(".");
    label.append(nested_path);
  }
  return label;
}

std::string BuildChildActionIndexLabel(const std::string& parent_label,
                                       size_t child_index,
                                       std::optional<char> branch_label =
                                           std::nullopt) {
  std::string token;
  if (branch_label.has_value()) {
    token.push_back(*branch_label);
  }
  token.append(base::NumberToString(static_cast<int>(child_index) + 1));
  if (parent_label.empty()) {
    return token;
  }
  return parent_label + "." + token;
}

const base::Value::List* FindActionChildList(const base::Value::Dict& action,
                                             const char* key) {
  if (const base::Value::List* list = action.FindList(key)) {
    return list;
  }
  const base::Value::Dict* params = action.FindDict("params");
  return params ? params->FindList(key) : nullptr;
}

bool ValidateActionTreeRecursive(
    dashboard::actions::ActionFactory* action_factory,
    const base::Value::Dict& raw_action,
    size_t top_level_index,
    const std::string& action_index_label,
    ActionValidationFailure* failure) {
  if (!action_factory || !failure) {
    return false;
  }

  base::Value::Dict action = raw_action.Clone();
  dashboard::ScriptSchema::CanonicalizeAction(action);
  const std::string kind =
      action.FindString("action") ? *action.FindString("action")
                                  : std::string();

  std::string schema_error;
  if (!dashboard::ScriptSchema::ValidateAction(action, &schema_error)) {
    failure->top_level_index = top_level_index;
    failure->action_index_label = action_index_label;
    failure->kind = kind;
    failure->error = schema_error;
    return false;
  }

  if (!action_factory->CreateAction(action)) {
    failure->top_level_index = top_level_index;
    failure->action_index_label = action_index_label;
    failure->kind = kind;
    if (base::EqualsCaseInsensitiveASCII(kind, "loop")) {
      failure->error =
          DescribeLoopActionFactoryFailure(action_factory, action,
                                           action_index_label);
    } else {
      failure->error =
          "ActionFactory rejected the action after schema validation.";
    }
    return false;
  }

  if (base::EqualsCaseInsensitiveASCII(kind, "loop")) {
    if (const base::Value::List* steps = FindActionChildList(action, "steps")) {
      for (size_t i = 0; i < steps->size(); ++i) {
        const base::Value::Dict* child = (*steps)[i].GetIfDict();
        if (!child) {
          failure->top_level_index = top_level_index;
          failure->action_index_label =
              BuildChildActionIndexLabel(action_index_label, i);
          failure->kind.clear();
          failure->error = "Nested loop step is not an action object.";
          return false;
        }
        if (!ValidateActionTreeRecursive(
                action_factory, *child, top_level_index,
                BuildChildActionIndexLabel(action_index_label, i), failure)) {
          return false;
        }
      }
    }
  } else if (base::EqualsCaseInsensitiveASCII(kind, "if_condition")) {
    const struct BranchSpec {
      const char* key;
      char label;
    } branches[] = {{"then", 'a'}, {"else", 'b'}};
    for (const auto& branch : branches) {
      const base::Value::List* actions = FindActionChildList(action, branch.key);
      if (!actions) {
        continue;
      }
      for (size_t i = 0; i < actions->size(); ++i) {
        const base::Value::Dict* child = (*actions)[i].GetIfDict();
        if (!child) {
          failure->top_level_index = top_level_index;
          failure->action_index_label =
              BuildChildActionIndexLabel(action_index_label, i, branch.label);
          failure->kind.clear();
          failure->error = base::StringPrintf(
              "Conditional branch '%s' contains a non-action entry.",
              branch.key);
          return false;
        }
        if (!ValidateActionTreeRecursive(
                action_factory, *child, top_level_index,
                BuildChildActionIndexLabel(action_index_label, i, branch.label),
                failure)) {
          return false;
        }
      }
    }
  } else if (const base::Value::List* nested = FindActionChildList(action, "nested")) {
    for (size_t i = 0; i < nested->size(); ++i) {
      const base::Value::Dict* child = (*nested)[i].GetIfDict();
      if (!child) {
        failure->top_level_index = top_level_index;
        failure->action_index_label =
            BuildChildActionIndexLabel(action_index_label, i);
        failure->kind.clear();
        failure->error = "Nested action entry is not an action object.";
        return false;
      }
      if (!ValidateActionTreeRecursive(
              action_factory, *child, top_level_index,
              BuildChildActionIndexLabel(action_index_label, i), failure)) {
        return false;
      }
    }
  }

  return true;
}

std::optional<ActionValidationFailure> ValidateTimelineForExecution(
    dashboard::actions::ActionFactory* action_factory,
    const base::Value::List& timeline) {
  if (!action_factory) {
    return std::nullopt;
  }
  for (size_t i = 0; i < timeline.size(); ++i) {
    const base::Value::Dict* action = timeline[i].GetIfDict();
    if (!action) {
      ActionValidationFailure failure;
      failure.top_level_index = i;
      failure.action_index_label = BuildActionIndexLabel(i);
      failure.error = "Action entry is not an object.";
      return failure;
    }
    ActionValidationFailure failure;
    if (!ValidateActionTreeRecursive(action_factory, *action, i,
                                     BuildActionIndexLabel(i), &failure)) {
      return failure;
    }
  }
  return std::nullopt;
}

std::string BuildPreflightTroubleshootingHint(
    const ActionValidationFailure& failure) {
  const std::string kind = base::ToLowerASCII(failure.kind);
  if (kind == "click" || kind == "hover") {
    return "Click/hover actions need locators or coordinates. For GLIC "
           "prompts this usually means the page-context element had no "
           "bounds/locators when the action was built — request a fresh "
           "context and retry, or target a different element id.";
  }
  if (kind == "unknown" || failure.error.find("Unknown action type") !=
                               std::string::npos) {
    return "Inspect the failing action type; it may be deprecated or "
           "corrupted.";
  }
  if (kind == "drag" || kind == "cdp_drag") {
    return "Check the drag step. Older scripts may still use deprecated CDP "
           "drag action names that need migration.";
  }
  if (kind == "type" || kind == "key_press" || kind == "key_down") {
    return "Confirm the locator and required fields were preserved on the "
           "action params.";
  }
  return "Review the failing action params (locators/coordinates/required "
         "fields) shown in the run error.";
}

std::string BuildPreflightFailureMessage(
    const ActionValidationFailure& failure,
    bool is_key_script) {
  const std::string action_clause =
      failure.action_index_label.empty()
          ? std::string()
          : (" action=" + failure.action_index_label);
  const std::string kind_clause =
      failure.kind.empty() ? std::string()
                           : (" kind='" + failure.kind + "'");
  const std::string error_clause =
      failure.error.empty() ? std::string() : (": " + failure.error);
  const std::string lead = base::StringPrintf(
      "%s at index=%zu%s%s%s; refusing to run",
      is_key_script ? "Invalid key-script action" : "Invalid action",
      failure.top_level_index, action_clause.c_str(), kind_clause.c_str(),
      error_clause.c_str());
  return lead + " Troubleshooting: " +
         BuildPreflightTroubleshootingHint(failure);
}

std::string DescribeLoopActionFactoryFailure(
    dashboard::actions::ActionFactory* action_factory,
    const base::Value::Dict& action,
    const std::string& action_index_label) {
  const base::Value::Dict* params = action.FindDict("params");
  if (!params) {
    return "Loop action is missing params.";
  }

  const base::Value::List* steps = FindActionChildList(action, "steps");
  if (!steps || steps->empty()) {
    return "Loop has no nested steps.";
  }
  for (size_t i = 0; i < steps->size(); ++i) {
    const base::Value::Dict* child = (*steps)[i].GetIfDict();
    if (!child) {
      return base::StringPrintf("Nested loop step %s is not an action object.",
                                BuildChildActionIndexLabel(action_index_label, i)
                                    .c_str());
    }
    if (!action_factory->CreateAction(child->Clone())) {
      const std::string child_kind =
          child->FindString("action") ? *child->FindString("action")
                                      : std::string();
      const std::string kind_clause =
          child_kind.empty()
              ? std::string()
              : base::StringPrintf(" kind='%s'", child_kind.c_str());
      return base::StringPrintf(
          "Nested loop step %s%s could not be created.",
          BuildChildActionIndexLabel(action_index_label, i).c_str(),
          kind_clause.c_str());
    }
  }

  const std::string mode_raw =
      params->FindString("loopMode") ? *params->FindString("loopMode")
                                     : std::string("count");
  const std::string mode = base::ToLowerASCII(mode_raw);
  if (mode == "for_each" || mode == "while_element_exists") {
    const base::Value::List* locators = params->FindList("locators");
    if (!locators || locators->empty()) {
      return mode == "for_each"
                 ? "for_each loop has no locators."
                 : "while_element_exists loop has no locators.";
    }
    const std::vector<dashboard::Locator> converted =
        dashboard::ConvertLocators(*locators);
    if (converted.empty()) {
      return mode == "for_each"
                 ? "for_each loop locators are present but none contain valid {type,value} pairs."
                 : "while_element_exists loop locators are present but none contain valid {type,value} pairs.";
    }
    const int max_iterations =
        params->FindInt("maxIterations").value_or(mode == "for_each" ? 200 : 50);
    if (max_iterations < 1) {
      return mode == "for_each"
                 ? "for_each loop maxIterations must be >= 1."
                 : "while_element_exists loop maxIterations must be >= 1.";
    }
  } else {
    const int iterations = params->FindInt("iterations").value_or(
        params->FindInt("maxIterations").value_or(0));
    if (iterations < 1) {
      return "count loop iterations must be >= 1.";
    }
  }

  return "Loop action payload passed schema validation but failed runtime creation.";
}

std::string ReadActionContextUrl(const base::Value::Dict* action_dict) {
  if (!action_dict) {
    return std::string();
  }
  const std::string* context = action_dict->FindString("contextUrl");
  if (!context || context->empty()) {
    context = action_dict->FindString("context_url");
  }
  if (!context) {
    return std::string();
  }
  std::string out = *context;
  base::TrimWhitespaceASCII(out, base::TRIM_ALL, &out);
  return out;
}

void SetActionContextUrl(base::Value::Dict* action_dict,
                         const std::string& raw_url) {
  if (!action_dict) {
    return;
  }
  std::string value = raw_url;
  base::TrimWhitespaceASCII(value, base::TRIM_ALL, &value);
  if (value.empty()) {
    action_dict->Remove("contextUrl");
    action_dict->Remove("context_url");
    return;
  }
  const GURL url(value);
  if (url.is_valid()) {
    value = url.GetWithoutRef().spec();
  }
  action_dict->Set("contextUrl", value);
  action_dict->Remove("context_url");
}

bool ContextUrlsEquivalent(const std::string& left, const std::string& right) {
  std::string l = left;
  std::string r = right;
  base::TrimWhitespaceASCII(l, base::TRIM_ALL, &l);
  base::TrimWhitespaceASCII(r, base::TRIM_ALL, &r);
  if (l.empty() || r.empty()) {
    return l == r;
  }
  const GURL left_url(l);
  const GURL right_url(r);
  if (left_url.is_valid() && right_url.is_valid()) {
    return left_url.GetWithoutRef() == right_url.GetWithoutRef();
  }
  return l == r;
}

std::optional<double> ExtractModelConfidence(const base::Value::Dict* action) {
  if (!action) {
    return std::nullopt;
  }
  if (const std::optional<double> v = action->FindDouble("model_confidence")) {
    return v;
  }
  if (const std::optional<double> v = action->FindDouble("confidence")) {
    return v;
  }
  if (const std::optional<int> v = action->FindInt("model_confidence")) {
    return static_cast<double>(*v);
  }
  if (const std::optional<int> v = action->FindInt("confidence")) {
    return static_cast<double>(*v);
  }
  if (const base::Value::Dict* params = action->FindDict("params")) {
    if (const std::optional<double> v = params->FindDouble("model_confidence")) {
      return v;
    }
    if (const std::optional<double> v = params->FindDouble("confidence")) {
      return v;
    }
    if (const std::optional<int> v = params->FindInt("model_confidence")) {
      return static_cast<double>(*v);
    }
    if (const std::optional<int> v = params->FindInt("confidence")) {
      return static_cast<double>(*v);
    }
  }
  return std::nullopt;
}

std::string NormalizeSignatureToken(std::string token) {
  base::TrimWhitespaceASCII(token, base::TRIM_ALL, &token);
  if (token.empty()) {
    return std::string();
  }
  token = base::ToLowerASCII(token);
  for (char& ch : token) {
    if (!base::IsAsciiAlpha(ch) && !base::IsAsciiDigit(ch) &&
        ch != '_' && ch != '-') {
      ch = '_';
    }
  }
  return token;
}

std::string BuildTraceSignature(const std::string& script_id,
                                const base::Value::List& steps_light,
                                bool success,
                                int failed_step,
                                const std::string& failure_message) {
  std::vector<std::string> parts;
  parts.reserve(steps_light.size() + 6);
  parts.push_back("v1");
  parts.push_back("script:" + NormalizeSignatureToken(script_id));
  parts.push_back(success ? "status:success" : "status:error");
  parts.push_back("failed_step:" + base::NumberToString(std::max(failed_step, -1)));

  for (const auto& step_v : steps_light) {
    const base::Value::Dict* step = step_v.GetIfDict();
    if (!step) {
      continue;
    }
    const base::Value::Dict* action = step->FindDict("action");
    const std::string action_type = action && action->FindString("action")
                                        ? *action->FindString("action")
                                        : std::string("unknown");
    const std::string status = step->FindString("status")
                                   ? *step->FindString("status")
                                   : std::string();
    parts.push_back("a:" + NormalizeSignatureToken(action_type) +
                    "|s:" + NormalizeSignatureToken(status));
  }

  if (!success && !failure_message.empty()) {
    const base::Value::Dict taxonomy = ClassifyFailureTaxonomy(failure_message);
    const std::string category = taxonomy.FindString("category")
                                     ? *taxonomy.FindString("category")
                                     : std::string();
    const std::string subcategory = taxonomy.FindString("subcategory")
                                        ? *taxonomy.FindString("subcategory")
                                        : std::string();
    parts.push_back("failure:" + NormalizeSignatureToken(category) + ":" +
                    NormalizeSignatureToken(subcategory));
  }

  const std::string canonical = base::JoinString(parts, "||");
  const uint32_t hash = base::PersistentHash(canonical);
  return base::StringPrintf("tsb1_%08x", hash);
}

std::string BuildTraceBucketKey(const std::string& script_id,
                                const std::string& trace_signature) {
  const std::string sid = NormalizeSignatureToken(script_id);
  const std::string sig = NormalizeSignatureToken(trace_signature);
  if (sid.empty()) {
    return sig.empty() ? std::string("unknown_bucket") : sig;
  }
  if (sig.empty()) {
    return sid;
  }
  return sid + "__" + sig;
}

void AppendCandidatesFromElementList(const base::Value::List* elements,
                                     const std::string& frame_id,
                                     bool in_shadow_dom,
                                     const std::string& shadow_host_selector,
                                     int* ordinal,
                                     base::Value::List* out) {
  if (!elements || !ordinal || !out) {
    return;
  }
  for (const auto& item : *elements) {
    const base::Value::Dict* element = item.GetIfDict();
    if (!element) {
      continue;
    }
    base::Value::Dict candidate;
    candidate.Set("index", (*ordinal)++);
    if (!frame_id.empty()) {
      candidate.Set("frame_id", frame_id);
    }
    candidate.Set("in_shadow_dom", in_shadow_dom);
    if (!shadow_host_selector.empty()) {
      candidate.Set("shadow_host_selector", shadow_host_selector);
    }
    if (const std::string* s = element->FindString("id")) {
      candidate.Set("element_id", *s);
    }
    if (const std::string* s = element->FindString("tag")) {
      candidate.Set("tag", *s);
    }
    if (const std::string* s = element->FindString("role")) {
      candidate.Set("role", *s);
    }
    if (const std::string* s = element->FindString("text")) {
      candidate.Set("text", *s);
    }
    if (const std::string* s = element->FindString("accessibilityName")) {
      candidate.Set("name", *s);
    }
    if (const std::optional<bool> v = element->FindBool("clickable")) {
      candidate.Set("clickable", *v);
    }
    if (const std::optional<bool> v = element->FindBool("focusable")) {
      candidate.Set("focusable", *v);
    }
    if (const std::optional<bool> v = element->FindBool("disabled")) {
      candidate.Set("disabled", *v);
    }
    if (const base::Value::Dict* bounds = element->FindDict("bounds")) {
      candidate.Set("bounds", bounds->Clone());
    }
    if (const base::Value::Dict* selectors = element->FindDict("selectors")) {
      base::Value::List locators;
      if (const std::string* css = selectors->FindString("css")) {
        base::Value::Dict loc;
        loc.Set("type", "CSS_SELECTOR");
        loc.Set("value", *css);
        locators.Append(std::move(loc));
      }
      if (const std::string* xpath = selectors->FindString("xpath")) {
        base::Value::Dict loc;
        loc.Set("type", "XPATH");
        loc.Set("value", *xpath);
        locators.Append(std::move(loc));
      }
      if (const base::Value::List* extra = selectors->FindList("locators")) {
        for (const auto& v : *extra) {
          if (const base::Value::Dict* d = v.GetIfDict()) {
            locators.Append(d->Clone());
          }
        }
      }
      if (!locators.empty()) {
        candidate.Set("locators", std::move(locators));
      }
    }
    out->Append(std::move(candidate));
  }
}

base::Value::List BuildCandidatesFromPageMap(const base::Value::Dict* page_map,
                                             int max_candidates) {
  base::Value::List out;
  if (!page_map) {
    return out;
  }
  const base::Value::List* frames = page_map->FindList("frames");
  if (!frames) {
    return out;
  }
  int ordinal = 0;
  for (const auto& frame_v : *frames) {
    if (max_candidates > 0 &&
        static_cast<int>(out.size()) >= max_candidates) {
      break;
    }
    const base::Value::Dict* frame = frame_v.GetIfDict();
    if (!frame) {
      continue;
    }
    const std::string frame_id =
        frame->FindString("frameId")
            ? *frame->FindString("frameId")
            : std::string();
    AppendCandidatesFromElementList(frame->FindList("elements"), frame_id,
                                    /*in_shadow_dom=*/false,
                                    /*shadow_host_selector=*/std::string(),
                                    &ordinal, &out);
    if (max_candidates > 0 &&
        static_cast<int>(out.size()) >= max_candidates) {
      break;
    }
    if (const base::Value::List* shadow_roots = frame->FindList("shadowRoots")) {
      for (const auto& shadow_v : *shadow_roots) {
        if (max_candidates > 0 &&
            static_cast<int>(out.size()) >= max_candidates) {
          break;
        }
        const base::Value::Dict* shadow = shadow_v.GetIfDict();
        if (!shadow) {
          continue;
        }
        const std::string host =
            shadow->FindString("hostSelector")
                ? *shadow->FindString("hostSelector")
                : std::string();
        AppendCandidatesFromElementList(shadow->FindList("elements"), frame_id,
                                        /*in_shadow_dom=*/true, host, &ordinal,
                                        &out);
      }
    }
  }
  return out;
}

base::Value::Dict BuildTargetHintFromAction(const base::Value::Dict* action) {
  base::Value::Dict out;
  if (!action) {
    return out;
  }
  if (const std::string* action_name = action->FindString("action")) {
    out.Set("action", *action_name);
  }
  const base::Value::Dict* params = action->FindDict("params");
  if (!params) {
    return out;
  }
  if (const base::Value::List* locators = params->FindList("locators");
      locators && !locators->empty()) {
    if (const base::Value::Dict* first = (*locators)[0].GetIfDict()) {
      out.Set("primary_locator", first->Clone());
    }
    out.Set("locator_count", static_cast<int>(locators->size()));
  }
  if (const base::Value::Dict* coords = params->FindDict("coordinates")) {
    out.Set("coordinates", coords->Clone());
  }
  return out;
}

std::string NormalizeLocatorToken(std::string value) {
  base::TrimWhitespaceASCII(value, base::TRIM_ALL, &value);
  value = base::ToLowerASCII(value);
  return value;
}

double ScoreLocatorValueMatch(const base::Value* action_value,
                              const base::Value* candidate_value,
                              std::string* reason_out) {
  if (!action_value || !candidate_value) {
    return 0.0;
  }
  const std::string* action_s = action_value->GetIfString();
  const std::string* candidate_s = candidate_value->GetIfString();
  if (!action_s || !candidate_s) {
    return 0.0;
  }
  const std::string lhs = NormalizeLocatorToken(*action_s);
  const std::string rhs = NormalizeLocatorToken(*candidate_s);
  if (lhs.empty() || rhs.empty()) {
    return 0.0;
  }
  if (lhs == rhs) {
    if (reason_out) {
      *reason_out = "locator_exact_value";
    }
    return 1.0;
  }
  if (lhs.size() >= 4 &&
      (rhs.find(lhs) != std::string::npos ||
       lhs.find(rhs) != std::string::npos)) {
    if (reason_out) {
      *reason_out = "locator_partial_value";
    }
    return 0.82;
  }
  return 0.0;
}

double ScoreLocatorPair(const base::Value::Dict* action_locator,
                        const base::Value::Dict* candidate_locator,
                        std::string* reason_out) {
  if (!action_locator || !candidate_locator) {
    return 0.0;
  }
  const std::string action_type =
      action_locator->FindString("type") ? *action_locator->FindString("type")
                                          : std::string();
  const std::string candidate_type =
      candidate_locator->FindString("type")
          ? *candidate_locator->FindString("type")
          : std::string();
  const bool type_matches =
      action_type.empty() || candidate_type.empty() ||
      base::EqualsCaseInsensitiveASCII(action_type, candidate_type);

  std::string value_reason;
  const double value_score = ScoreLocatorValueMatch(
      action_locator->Find("value"), candidate_locator->Find("value"),
      &value_reason);
  if (value_score <= 0.0) {
    return 0.0;
  }

  const double score = type_matches ? value_score : value_score * 0.65;
  if (reason_out) {
    *reason_out = type_matches ? value_reason : (value_reason + "_type_mismatch");
  }
  return score;
}

bool TryReadNumber(const base::Value::Dict& dict,
                   const char* key,
                   double* out_value) {
  if (!out_value) {
    return false;
  }
  if (const std::optional<double> v = dict.FindDouble(key)) {
    *out_value = *v;
    return true;
  }
  if (const std::optional<int> v = dict.FindInt(key)) {
    *out_value = static_cast<double>(*v);
    return true;
  }
  if (const std::string* s = dict.FindString(key)) {
    double parsed = 0.0;
    if (base::StringToDouble(*s, &parsed)) {
      *out_value = parsed;
      return true;
    }
  }
  return false;
}

bool TryExtractActionPoint(const base::Value::Dict* action,
                           double* out_x,
                           double* out_y) {
  if (!action || !out_x || !out_y) {
    return false;
  }
  const base::Value::Dict* params = action->FindDict("params");
  if (!params) {
    return false;
  }
  if (const base::Value::Dict* coords = params->FindDict("coordinates")) {
    return TryReadNumber(*coords, "x", out_x) &&
           TryReadNumber(*coords, "y", out_y);
  }
  const base::Value::List* locators = params->FindList("locators");
  if (!locators) {
    return false;
  }
  for (const auto& locator_v : *locators) {
    const base::Value::Dict* locator = locator_v.GetIfDict();
    if (!locator) {
      continue;
    }
    const std::string type =
        locator->FindString("type") ? *locator->FindString("type")
                                     : std::string();
    if (!base::EqualsCaseInsensitiveASCII(type, "coordinates") &&
        !base::EqualsCaseInsensitiveASCII(type, "visual_coords")) {
      continue;
    }
    const base::Value* value = locator->Find("value");
    const base::Value::Dict* value_dict = value ? value->GetIfDict() : nullptr;
    if (!value_dict) {
      continue;
    }
    if (TryReadNumber(*value_dict, "x", out_x) &&
        TryReadNumber(*value_dict, "y", out_y)) {
      return true;
    }
  }
  return false;
}

double ScorePointAgainstCandidate(double x,
                                  double y,
                                  const base::Value::Dict* candidate,
                                  std::string* reason_out) {
  if (!candidate) {
    return 0.0;
  }
  const base::Value::Dict* bounds = candidate->FindDict("bounds");
  if (!bounds) {
    return 0.0;
  }
  double bx = 0.0;
  double by = 0.0;
  double bw = 0.0;
  double bh = 0.0;
  if (!TryReadNumber(*bounds, "x", &bx) || !TryReadNumber(*bounds, "y", &by) ||
      !TryReadNumber(*bounds, "w", &bw) || !TryReadNumber(*bounds, "h", &bh) ||
      bw <= 0.0 || bh <= 0.0) {
    return 0.0;
  }
  const bool inside =
      x >= bx && x <= (bx + bw) && y >= by && y <= (by + bh);
  if (inside) {
    if (reason_out) {
      *reason_out = "coordinates_in_bounds";
    }
    return 0.92;
  }
  const double cx = bx + bw * 0.5;
  const double cy = by + bh * 0.5;
  const double dx = x - cx;
  const double dy = y - cy;
  const double distance = std::sqrt(dx * dx + dy * dy);
  if (distance <= 24.0) {
    if (reason_out) {
      *reason_out = "coordinates_near_bounds";
    }
    return 0.58;
  }
  return 0.0;
}

std::optional<base::Value::Dict> MatchActionToCandidates(
    const base::Value::Dict* action,
    const base::Value::List& candidates) {
  if (!action || candidates.empty()) {
    return std::nullopt;
  }

  const base::Value::Dict* params = action->FindDict("params");
  const base::Value::List* action_locators =
      params ? params->FindList("locators") : nullptr;

  int best_index = -1;
  double best_score = 0.0;
  std::string best_reason;
  base::Value::Dict best_action_locator;
  base::Value::Dict best_candidate_locator;
  base::Value::Dict best_candidate_payload;

  for (size_t i = 0; i < candidates.size(); ++i) {
    const base::Value::Dict* candidate = candidates[i].GetIfDict();
    if (!candidate) {
      continue;
    }
    const int candidate_index =
        candidate->FindInt("index").value_or(static_cast<int>(i));

    if (action_locators) {
      const base::Value::List* candidate_locators =
          candidate->FindList("locators");
      if (candidate_locators) {
        for (const auto& action_locator_v : *action_locators) {
          const base::Value::Dict* action_locator = action_locator_v.GetIfDict();
          if (!action_locator) {
            continue;
          }
          for (const auto& candidate_locator_v : *candidate_locators) {
            const base::Value::Dict* candidate_locator =
                candidate_locator_v.GetIfDict();
            if (!candidate_locator) {
              continue;
            }
            std::string reason;
            const double score =
                ScoreLocatorPair(action_locator, candidate_locator, &reason);
            if (score > best_score) {
              best_score = score;
              best_index = candidate_index;
              best_reason = reason;
              best_action_locator = action_locator->Clone();
              best_candidate_locator = candidate_locator->Clone();
              best_candidate_payload = candidate->Clone();
            }
          }
        }
      }
    }

    double px = 0.0;
    double py = 0.0;
    if (TryExtractActionPoint(action, &px, &py)) {
      std::string coord_reason;
      const double score =
          ScorePointAgainstCandidate(px, py, candidate, &coord_reason);
      if (score > best_score) {
        best_score = score;
        best_index = candidate_index;
        best_reason = coord_reason;
        best_action_locator = base::Value::Dict();
        best_candidate_locator = base::Value::Dict();
        best_candidate_payload = candidate->Clone();
      }
    }
  }

  if (best_index < 0 || best_score < 0.45) {
    return std::nullopt;
  }

  base::Value::Dict out;
  out.Set("index", best_index);
  out.Set("confidence", best_score);
  out.Set("reason", best_reason);
  if (!best_action_locator.empty()) {
    out.Set("matched_action_locator", std::move(best_action_locator));
  }
  if (!best_candidate_locator.empty()) {
    out.Set("matched_candidate_locator", std::move(best_candidate_locator));
  }
  if (!best_candidate_payload.empty()) {
    out.Set("candidate", std::move(best_candidate_payload));
  }
  return out;
}

std::string SerializeActionAsTimelineJson(const base::Value::Dict* action) {
  base::Value::Dict payload;
  base::Value::List timeline;
  if (action) {
    timeline.Append(action->Clone());
  }
  payload.Set("timeline", std::move(timeline));
  payload.Set("source", "simulachrome_step_capture");
  std::string out;
  base::JSONWriter::Write(payload, &out);
  return out;
}

// Native input goes to the focused widget, so a background target tab would
// silently drop the first step's keystrokes.
bool RunMayUseNativeInput(bool force_native) {
  if (force_native) {
    return true;
  }
  const dashboard::ActionExecutionRouting routing =
      dashboard::GetActionExecutionRouting();
  return routing.primary == dashboard::ActionExecutionPath::kNative ||
         routing.fallback == dashboard::ActionExecutionPath::kNative;
}
}  // namespace


ScriptExecutor::ScriptExecutor(Profile* profile)
    : content::WebContentsObserver(nullptr),
      profile_(profile),
      action_factory_(std::make_unique<dashboard::actions::ActionFactory>(profile)),
      task_runner_(base::SequencedTaskRunner::GetCurrentDefault()),
      step_index_(0) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&LoadPersistedGlobalSettingsBlocking, profile_),
      base::BindOnce(&ScriptExecutor::OnPersistedInputBehaviorSettingsLoaded,
                     weak_ptr_factory_.GetWeakPtr()));
}

ScriptExecutor::~ScriptExecutor() = default;
void ScriptExecutor::OnPersistedInputBehaviorSettingsLoaded(
    base::Value::Dict settings) {
  dashboard::GlobalSettingsStore() = settings.Clone();
  const base::Value::Dict* input_behavior = settings.FindDict("inputBehavior");
  const base::Value::Dict empty_input_behavior;
  humanized_inter_action_delay_enabled_ =
      input_behavior ? input_behavior->FindBool("randomDelayEnabled").value_or(true)
                     : true;
  ApplyInputBehaviorSettings(input_behavior ? *input_behavior
                                            : empty_input_behavior);
}

void ScriptExecutor::AppendAction(base::Value::Dict action_dict) {
  auto action = action_factory_->CreateAction(action_dict);
  if (!action) {
    LOG(ERROR) << "[Executor] AppendAction: invalid action object";
    return;
  }
  action_queue_.push_back(std::move(action));
  non_critical_.push_back(false);  // or detect this from the dict
  timeline_.Append(std::move(action_dict));  // also update the timeline copy
  LOG(INFO) << "[Executor] Action injected at index=" << (action_queue_.size() - 1);
}
void ScriptExecutor::SetPageSnapshot(base::Value::Dict snapshot) {
  current_snapshot_ = std::move(snapshot);
}

bool ScriptExecutor::ShouldCapturePerStepTrainingArtifacts() const {
  const base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd->HasSwitch("disable-dashboard-step-training-capture")) {
    return false;
  }
  if (cmd->HasSwitch("enable-dashboard-step-training-capture")) {
    return true;
  }
  // Default to disabled so action execution is never gated on capture.
  // Opt-in explicitly via --enable-dashboard-step-training-capture.
  return false;
}

void ScriptExecutor::CapturePageMapSnapshot(PageMapCaptureCallback cb) {
  if (!target_web_contents_) {
    std::move(cb).Run(base::Value::Dict());
    return;
  }
  dashboard::FetchUnifiedPageMapSnapshot(
      target_web_contents_,
      base::BindOnce([](PageMapCaptureCallback inner,
                        dashboard::UnifiedPageMapResult result) {
        std::move(inner).Run(result.ok ? std::move(result.page_map)
                                       : base::Value::Dict());
      },
                     std::move(cb)));
}

void ScriptExecutor::CaptureScreenshotPng(ScreenshotCaptureCallback cb) {
  if (!target_web_contents_) {
    std::move(cb).Run({});
    return;
  }
  content::RenderWidgetHostView* view =
      target_web_contents_->GetRenderWidgetHostView();
  if (!view || !view->IsSurfaceAvailableForCopy()) {
    std::move(cb).Run({});
    return;
  }
  const std::optional<gfx::Size> output_size = ComputeTrainingCaptureSize(view);
  if (!output_size.has_value() || output_size->IsEmpty()) {
    std::move(cb).Run({});
    return;
  }
  view->CopyFromSurface(
      gfx::Rect(), *output_size,
      base::BindOnce(
          [](ScreenshotCaptureCallback inner,
             const viz::CopyOutputBitmapWithMetadata& result) {
            if (result.bitmap.drawsNothing()) {
              std::move(inner).Run({});
              return;
            }
            std::optional<std::vector<uint8_t>> png =
                gfx::PNGCodec::EncodeBGRASkBitmap(
                    result.bitmap, /*discard_transparency=*/false);
            std::move(inner).Run(png.value_or(std::vector<uint8_t>()));
          },
          std::move(cb)));
}

void ScriptExecutor::CaptureStepContext(StepContextCaptureCallback cb) {
  CapturePageMapSnapshot(base::BindOnce(
      [](base::WeakPtr<ScriptExecutor> weak, StepContextCaptureCallback inner,
         base::Value::Dict page_map) {
        if (!weak) {
          std::move(inner).Run(std::move(page_map), {});
          return;
        }
        weak->CaptureScreenshotPng(base::BindOnce(
            [](StepContextCaptureCallback done, base::Value::Dict map,
               std::vector<uint8_t> png) {
              std::move(done).Run(std::move(map), std::move(png));
            },
            std::move(inner), std::move(page_map)));
      },
      weak_ptr_factory_.GetWeakPtr(), std::move(cb)));
}

void ScriptExecutor::OnActionExecutedWithPreCapture(bool is_key_script,
                                                    size_t index,
                                                    int attempt,
                                                    StepCaptureData capture,
                                                    bool success,
                                                    const std::string& message) {
  if (!status_.is_running) {
    return;
  }
  CaptureStepContext(base::BindOnce(
      &ScriptExecutor::OnActionCompleteWithPostCapture,
      weak_ptr_factory_.GetWeakPtr(), is_key_script, index, attempt, success,
      std::string(message), std::move(capture)));
}

void ScriptExecutor::OnActionCompleteWithPostCapture(
    bool is_key_script,
    size_t index,
    int attempt,
    bool success,
    std::string message,
    StepCaptureData capture,
    base::Value::Dict post_page_map,
    std::vector<uint8_t> post_png) {
  capture.post_page_map = std::move(post_page_map);
  capture.post_png_b64 = EncodeBytesToBase64(post_png);
  pending_step_capture_ = std::move(capture);
  OnActionCompleteInternal(is_key_script, index, attempt, success, message);
}

void ScriptExecutor::ExecuteScript(const base::Value::List& timeline,
                                   content::WebContents* web_contents,
                                   dashboard::ScriptContext* script_context,
                                   const base::Value::List* key_timeline) {
  if (status_.is_running) {
    LOG(WARNING) << "[Executor] ExecuteScript ignored: already running";
    return;
  }

  LOG(INFO) << "[Executor] ExecuteScript: timeline size=" << timeline.size();

  const bool preserve_force_cdp = force_cdp_execution_for_run_;
  const bool preserve_force_native = force_native_execution_for_run_;
  const bool preserve_agent_mode = is_agent_mode_for_run_;
  CleanupAfterRun();
  force_cdp_execution_for_run_ = preserve_force_cdp;
  force_native_execution_for_run_ = preserve_force_native;
  is_agent_mode_for_run_ = preserve_agent_mode;
  action_factory_->SetForceCdpExecution(force_cdp_execution_for_run_);
  action_factory_->SetForceNativeExecution(force_native_execution_for_run_);
  action_factory_->SetAgentMode(is_agent_mode_for_run_);
  dashboard::ExecutionRunContext::Get().Set(is_agent_mode_for_run_,
                                            force_native_execution_for_run_);
  run_completion_in_progress_ = false;
  action_queue_.clear();
  non_critical_.clear();
  key_action_queue_.clear();
  key_non_critical_.clear();
  current_step_ = 0;
  key_step_ = 0;
  timeline_.clear();
  key_timeline_.clear();
  in_key_script_ = false;
  resume_main_step_after_key_ = 0;
  key_script_runs_ = 0;
  consecutive_critical_failures_ = 0;
  has_any_critical_failure_ = false;
  first_critical_failed_step_ = -1;
  last_critical_error_.clear();
  for (const auto& item : timeline)
    timeline_.Append(item.Clone());
  if (key_timeline) {
    for (const auto& item : *key_timeline)
      key_timeline_.Append(item.Clone());
  }
  script_context_ = script_context;
  step_index_ = 0;
  run_id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
  run_started_at_ = base::TimeTicks::Now();
  run_started_wall_time_ = base::Time::Now();
  pending_url_before_.reset();
  pending_action_started_at_ = base::TimeTicks();
  pending_step_capture_.reset();
  recording_suppression_ = std::make_unique<dashboard::ScopedRecordingSuppression>();
  current_context_.history.clear();
  current_context_.goal = "(unknown)";
  preflight_failed_action_index_.clear();
  preflight_failed_action_index_.clear();

  if (script_context_) {
    target_web_contents_ = script_context_->current_target();
    if (!target_web_contents_ && web_contents) {
      script_context_->SwitchToTarget(web_contents);
      target_web_contents_ = web_contents;
    }
  } else {
    target_web_contents_ = web_contents;
  }

  Observe(target_web_contents_);

  if (target_web_contents_ &&
      RunMayUseNativeInput(force_native_execution_for_run_)) {
    dashboard::ActivateWebContentsForNativeScriptRun(target_web_contents_);
  }

  // Failure-replay CopyFromSurface is opt-in (Settings → Failure replay
  // capture, or --enable-dashboard-failure-replay). Always-on 2fps captures
  // starve headed SPA paint.
  failure_replay_recorder_.reset();
  if (target_web_contents_ && IsFailureReplayCaptureEnabled()) {
    failure_replay_recorder_ =
        std::make_unique<dashboard::FailureReplayRecorder>(
            target_web_contents_);
    failure_replay_recorder_->Start(kFailureReplayFps,
                                    kFailureReplayMaxDuration);
  }

  if (const std::optional<ActionValidationFailure> validation_failure =
          ValidateTimelineForExecution(action_factory_.get(), timeline)) {
    preflight_failed_action_index_ = validation_failure->action_index_label;
    FinishRun(/*success=*/false,
              /*failed_step=*/static_cast<int>(
                  validation_failure->top_level_index),
              BuildPreflightFailureMessage(*validation_failure,
                                           /*is_key_script=*/false));
    return;
  }

  size_t created = 0;
  for (size_t i = 0; i < timeline.size(); ++i) {
    const auto& action_value = timeline[i];
    if (const base::Value::Dict* action_dict = action_value.GetIfDict()) {
      auto action = action_factory_->CreateAction(*action_dict);
      if (action) {
        action_queue_.push_back(std::move(action));
        bool nc = action_dict->FindBool("nonCritical").value_or(false);
        non_critical_.push_back(nc);
        ++created;
      } else {
        LOG(ERROR) << "[Executor] Failed to create action from node at index=" << i;
      }
    } else {
      LOG(ERROR) << "[Executor] Invalid action node (not a dict) at index=" << i;
    }
  }
  LOG(INFO) << "[Executor] Actions created=" << created;

  if (!key_timeline_.empty()) {
    if (const std::optional<ActionValidationFailure> validation_failure =
            ValidateTimelineForExecution(action_factory_.get(),
                                         key_timeline_)) {
      preflight_failed_action_index_ = validation_failure->action_index_label;
      FinishRun(
          /*success=*/false,
          /*failed_step=*/static_cast<int>(validation_failure->top_level_index),
          BuildPreflightFailureMessage(*validation_failure,
                                       /*is_key_script=*/true));
      return;
    }

    size_t key_created = 0;
    for (size_t i = 0; i < key_timeline_.size(); ++i) {
      const auto& action_value = key_timeline_[i];
      if (const base::Value::Dict* action_dict = action_value.GetIfDict()) {
        auto action = action_factory_->CreateAction(*action_dict);
        if (action) {
          key_action_queue_.push_back(std::move(action));
          bool nc = action_dict->FindBool("nonCritical").value_or(false);
          key_non_critical_.push_back(nc);
          ++key_created;
        } else {
          LOG(ERROR) << "[Executor] Failed to create key-script action at index=" << i;
        }
      } else {
        LOG(ERROR) << "[Executor] Invalid key-script action node (not a dict) at index=" << i;
      }
    }
    LOG(INFO) << "[Executor] Key-script actions created=" << key_created;
  }

  if (action_queue_.empty()) {
    FinishRun(/*success=*/false, /*failed_step=*/-1,
              "Empty action queue; nothing to run");
    return;
  }

  status_ = {true, 0, static_cast<int>(action_queue_.size()), "", "Starting execution"};
  UpdateStatus(status_);
  is_paused_ = false;
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&ScriptExecutor::WaitForTargetLoadThenExecute,
                     weak_ptr_factory_.GetWeakPtr(),
                     base::TimeTicks::Now() + base::Seconds(15)));
}

void ScriptExecutor::ExecuteScriptFromStep(const base::Value::List& timeline,
                                          content::WebContents* web_contents,
                                          dashboard::ScriptContext* script_context,
                                          int start_step_index,
                                          const std::string& resume_url) {
  if (status_.is_running) {
    LOG(WARNING) << "[Executor] ExecuteScriptFromStep ignored: already running";
    return;
  }

  LOG(INFO) << "[Executor] ExecuteScriptFromStep: timeline size=" << timeline.size()
            << " start_step_index=" << start_step_index;

  const bool preserve_force_cdp = force_cdp_execution_for_run_;
  const bool preserve_force_native = force_native_execution_for_run_;
  const bool preserve_agent_mode = is_agent_mode_for_run_;
  CleanupAfterRun();
  force_cdp_execution_for_run_ = preserve_force_cdp;
  force_native_execution_for_run_ = preserve_force_native;
  is_agent_mode_for_run_ = preserve_agent_mode;
  action_factory_->SetForceCdpExecution(force_cdp_execution_for_run_);
  action_factory_->SetForceNativeExecution(force_native_execution_for_run_);
  action_factory_->SetAgentMode(is_agent_mode_for_run_);
  dashboard::ExecutionRunContext::Get().Set(is_agent_mode_for_run_,
                                            force_native_execution_for_run_);
  run_completion_in_progress_ = false;
  action_queue_.clear();
  non_critical_.clear();
  timeline_.clear();
  for (const auto& item : timeline)
    timeline_.Append(item.Clone());
  script_context_ = script_context;
  step_index_ = 0;
  run_id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
  run_started_at_ = base::TimeTicks::Now();
  run_started_wall_time_ = base::Time::Now();
  pending_url_before_.reset();
  pending_action_started_at_ = base::TimeTicks();
  recording_suppression_ =
      std::make_unique<dashboard::ScopedRecordingSuppression>();
  current_context_.history.clear();
  current_context_.goal = "(unknown)";

  if (script_context_) {
    target_web_contents_ = script_context_->current_target();
    if (!target_web_contents_ && web_contents) {
      script_context_->SwitchToTarget(web_contents);
      target_web_contents_ = web_contents;
    }
  } else {
    target_web_contents_ = web_contents;
  }

  Observe(target_web_contents_);

  if (target_web_contents_ &&
      RunMayUseNativeInput(force_native_execution_for_run_)) {
    dashboard::ActivateWebContentsForNativeScriptRun(target_web_contents_);
  }

  // Failure-replay CopyFromSurface is opt-in (Settings → Failure replay
  // capture, or --enable-dashboard-failure-replay). Always-on 2fps captures
  // starve headed SPA paint.
  failure_replay_recorder_.reset();
  if (target_web_contents_ && IsFailureReplayCaptureEnabled()) {
    failure_replay_recorder_ =
        std::make_unique<dashboard::FailureReplayRecorder>(
            target_web_contents_);
    failure_replay_recorder_->Start(kFailureReplayFps,
                                    kFailureReplayMaxDuration);
  }

  if (const std::optional<ActionValidationFailure> validation_failure =
          ValidateTimelineForExecution(action_factory_.get(), timeline)) {
    preflight_failed_action_index_ = validation_failure->action_index_label;
    FinishRun(/*success=*/false,
              /*failed_step=*/static_cast<int>(
                  validation_failure->top_level_index),
              BuildPreflightFailureMessage(*validation_failure,
                                           /*is_key_script=*/false));
    return;
  }

  size_t created = 0;
  for (const auto& action_value : timeline) {
    if (const base::Value::Dict* action_dict = action_value.GetIfDict()) {
      auto action = action_factory_->CreateAction(*action_dict);
      if (action) {
        action_queue_.push_back(std::move(action));
        bool nc = action_dict->FindBool("nonCritical").value_or(false);
        non_critical_.push_back(nc);
        ++created;
      } else {
        LOG(ERROR) << "[Executor] Failed to create action from node";
      }
    }
  }
  LOG(INFO) << "[Executor] Actions created=" << created;

  if (action_queue_.empty()) {
    FinishRun(/*success=*/false, /*failed_step=*/-1,
              "Empty action queue; nothing to run");
    return;
  }

  if (start_step_index < 0 ||
      start_step_index >= static_cast<int>(action_queue_.size())) {
    FinishRun(/*success=*/false, /*failed_step=*/start_step_index,
              "Invalid start_step_index");
    return;
  }

  current_step_ = start_step_index;
  status_ = {true,
             static_cast<int>(current_step_),
             static_cast<int>(action_queue_.size()),
             "",
             "Resuming execution"};
  UpdateStatus(status_);
  is_paused_ = false;

  std::string effective_resume_url = resume_url;
  if (std::string(base::TrimWhitespaceASCII(effective_resume_url,
                                            base::TRIM_ALL))
          .empty()) {
    effective_resume_url =
        dashboard::ScriptSchema::ResolveResumeNavigationUrl(timeline_,
                                                            start_step_index);
    if (!effective_resume_url.empty()) {
      LOG(INFO) << "[Executor] Resume URL from timeline contextUrl="
                << effective_resume_url;
    }
  }
  NavigateAndStart(start_step_index, effective_resume_url);
}

void ScriptExecutor::SetCompletionCallback(CompletionCallback callback) {
  completion_callback_ = std::move(callback);
}

void ScriptExecutor::SetForceCdpExecutionForRun(bool force) {
  force_cdp_execution_for_run_ = force;
  if (force) {
    force_native_execution_for_run_ = false;
  }
  if (action_factory_) {
    action_factory_->SetForceCdpExecution(force_cdp_execution_for_run_);
    action_factory_->SetForceNativeExecution(force_native_execution_for_run_);
    action_factory_->SetAgentMode(is_agent_mode_for_run_);
  }
  dashboard::ExecutionRunContext::Get().Set(is_agent_mode_for_run_,
                                            force_native_execution_for_run_);
}

void ScriptExecutor::SetForceNativeExecutionForRun(bool force) {
  force_native_execution_for_run_ = force;
  if (force) {
    force_cdp_execution_for_run_ = false;
  }
  if (action_factory_) {
    action_factory_->SetForceCdpExecution(force_cdp_execution_for_run_);
    action_factory_->SetForceNativeExecution(force_native_execution_for_run_);
    action_factory_->SetAgentMode(is_agent_mode_for_run_);
  }
  dashboard::ExecutionRunContext::Get().Set(is_agent_mode_for_run_,
                                            force_native_execution_for_run_);
}

void ScriptExecutor::SetTokenReverseMapForRun(
    dashboard::TokenReverseMap reverse_map) {
  token_reverse_map_ = std::move(reverse_map);
}

void ScriptExecutor::SetAgentModeForRun(bool agent_mode) {
  is_agent_mode_for_run_ = agent_mode;
  if (action_factory_) {
    action_factory_->SetAgentMode(is_agent_mode_for_run_);
  }
  dashboard::ExecutionRunContext::Get().Set(is_agent_mode_for_run_,
                                            force_native_execution_for_run_);
}

void ScriptExecutor::FinishRun(bool success,
                               int failed_step,
                               const std::string& message) {
  if (run_completion_in_progress_) {
    return;
  }
  run_completion_in_progress_ = true;

  // Always persist the run trace so debugging/resume can use it, even on
  // failure.
  SaveExecutionResult(success, failed_step, message);

  if (success) {
    status_ = {false, 0, 0, "", "Completed"};
    UpdateStatus(status_);
  } else {
    status_ = {false,
               failed_step < 0 ? 0 : failed_step,
               static_cast<int>(action_queue_.size()),
               "",
               "Error: " + message};
    UpdateStatus(status_);
  }

  // Run completion callback asynchronously to avoid re-entrancy. The execution
  // service may create and register the run after calling ExecuteScript(), and
  // synchronous completion can lead to missed callbacks or (worse) deleting the
  // executor while still on the stack.
  if (completion_callback_) {
    auto cb = std::move(completion_callback_);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(cb), success, failed_step, message));
  }

  CleanupAfterRun();
}

void ScriptExecutor::NavigateAndStart(int start_step_index,
                                     const std::string& resume_url) {
  if (!target_web_contents_) {
    FinishRun(/*success=*/false, start_step_index, "no target");
    return;
  }

  const std::string url =
      std::string(base::TrimWhitespaceASCII(resume_url, base::TRIM_ALL));
  if (url.empty()) {
    AttemptAction(/*is_key_script=*/false, static_cast<size_t>(start_step_index),
                  /*attempt=*/0);
    return;
  }

  const GURL desired(url);
  if (!desired.is_valid()) {
    LOG(WARNING) << "[Executor] Invalid resume_url=" << url;
    AttemptAction(/*is_key_script=*/false, static_cast<size_t>(start_step_index),
                  /*attempt=*/0);
    return;
  }

  const GURL current = target_web_contents_->GetLastCommittedURL();
  if (current.is_valid() && current.EqualsIgnoringRef(desired) &&
      !target_web_contents_->IsLoading()) {
    AttemptAction(/*is_key_script=*/false, static_cast<size_t>(start_step_index),
                  /*attempt=*/0);
    return;
  }

  if (script_context_) {
    script_context_->set_is_loaded(false);
  }

  LOG(INFO) << "[Executor] Navigating to resume_url=" << desired.spec();
  target_web_contents_->GetController().LoadURL(
      desired, content::Referrer(), ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
      std::string());

  PollUntilLoadedThenStart(start_step_index, desired,
                           base::TimeTicks::Now() + base::Seconds(90));
}

void ScriptExecutor::PollUntilLoadedThenStart(int start_step_index,
                                             const GURL& desired_url,
                                             base::TimeTicks deadline) {
  if (!target_web_contents_) {
    FinishRun(/*success=*/false, start_step_index, "no target");
    return;
  }

  const bool loaded = !target_web_contents_->IsLoading();
  const GURL current = target_web_contents_->GetLastCommittedURL();
  const bool url_ok =
      current.is_valid() && current.EqualsIgnoringRef(desired_url);

  if (loaded && url_ok) {
    AttemptAction(/*is_key_script=*/false, static_cast<size_t>(start_step_index),
                  /*attempt=*/0);
    return;
  }

  if (base::TimeTicks::Now() >= deadline) {
    FinishRun(/*success=*/false, start_step_index,
              "Timed out waiting for resume navigation");
    return;
  }

  task_runner_->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ScriptExecutor::PollUntilLoadedThenStart,
                     weak_ptr_factory_.GetWeakPtr(), start_step_index,
                     desired_url, deadline),
      base::Milliseconds(250));
}

// A run can target a tab that was just created for it; input sent before its
// first document commits and finishes loading is dropped.
void ScriptExecutor::WaitForTargetLoadThenExecute(base::TimeTicks deadline) {
  if (!status_.is_running) {
    return;
  }
  if (!target_web_contents_) {
    ExecuteNextAction();
    return;
  }
  const GURL committed = target_web_contents_->GetLastCommittedURL();
  const bool ready = !target_web_contents_->IsLoading() &&
                     committed.is_valid() && !committed.IsAboutBlank();
  if (ready || base::TimeTicks::Now() >= deadline) {
    if (!ready) {
      LOG(WARNING) << "[Executor] Target not loaded before first step; "
                      "starting anyway url="
                   << committed.spec();
    }
    ExecuteNextAction();
    return;
  }
  task_runner_->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ScriptExecutor::WaitForTargetLoadThenExecute,
                     weak_ptr_factory_.GetWeakPtr(), deadline),
      base::Milliseconds(100));
}

void ScriptExecutor::ScheduleContinuationWithHumanizedDelay(
    base::OnceClosure continuation,
    bool skip_delay) {
  if (!status_.is_running)
    return;
  if (!continuation)
    return;
  if (!humanized_inter_action_delay_enabled_) {
    skip_delay = true;
  }
  if (skip_delay) {
    status_.status_message = "Continuing without humanized pause";
    UpdateStatus(status_);
    task_runner_->PostTask(FROM_HERE, std::move(continuation));
    return;
  }
  const int delay_ms =
      base::RandInt(kHumanizedInterActionDelayMinMs,
                    kHumanizedInterActionDelayMaxMs);
  status_.status_message =
      "Humanized pause " + std::to_string(delay_ms) + " ms";
  UpdateStatus(status_);
  task_runner_->PostDelayedTask(FROM_HERE, std::move(continuation),
                                base::Milliseconds(delay_ms));
}

// AgentContext trivial ctor/dtor
ScriptExecutor::AgentContext::AgentContext() = default;
ScriptExecutor::AgentContext::~AgentContext() = default;
ScriptExecutor::StepCaptureData::StepCaptureData() = default;
ScriptExecutor::StepCaptureData::StepCaptureData(StepCaptureData&&) = default;
ScriptExecutor::StepCaptureData& ScriptExecutor::StepCaptureData::operator=(
    StepCaptureData&&) = default;
ScriptExecutor::StepCaptureData::~StepCaptureData() = default;

void ScriptExecutor::ExecuteNextAction() {
  if (!status_.is_running)
    return;
  if (is_paused_)
    return;

  if (in_key_script_) {
    const size_t index = static_cast<size_t>(key_step_);
    if (index >= key_action_queue_.size()) {
      FinishKeyScriptAndResumeMain(/*success=*/true, /*message=*/std::string());
      return;
    }
    AttemptAction(/*is_key_script=*/true, index, /*attempt=*/0);
    return;
  }

  const size_t index = static_cast<size_t>(current_step_);
  if (index >= action_queue_.size()) {
    if (has_any_critical_failure_) {
      FinishRun(/*success=*/false,
                first_critical_failed_step_ < 0 ? 0 : first_critical_failed_step_,
                last_critical_error_.empty() ? "One or more actions failed"
                                             : last_critical_error_);
    } else {
      FinishRun(/*success=*/true, /*failed_step=*/-1, "");
    }
    return;
  }
  AttemptAction(/*is_key_script=*/false, index, /*attempt=*/0);
}

void ScriptExecutor::AttemptAction(bool is_key_script,
                                   size_t index,
                                   int attempt) {
  if (is_key_script) {
    if (index >= key_action_queue_.size())
      return;
  } else {
    if (index >= action_queue_.size())
      return;
  }

  auto* action = is_key_script ? key_action_queue_[index].get()
                               : action_queue_[index].get();
  const base::Value::List& source_timeline =
      is_key_script ? key_timeline_ : timeline_;
  const base::Value::Dict* source_action_dict =
      index < source_timeline.size() ? source_timeline[index].GetIfDict()
                                     : nullptr;
  RehydrateKeyPressActionIfNeeded(action, source_action_dict);
  if (!action || !target_web_contents_) {
    OnActionCompleteInternal(is_key_script, index, attempt, /*success=*/false,
                             "no target");
    return;
  }
  action->SetProgressCallback(base::BindRepeating(
      &ScriptExecutor::UpdatePlaybackOverlayProgress,
      weak_ptr_factory_.GetWeakPtr()));
  {
    base::Value::Dict tmp;
    action->SerializeInto(&tmp);
    const std::string* kind_ptr = tmp.FindString("action");
    std::string kind = kind_ptr ? *kind_ptr : "unknown";
    if ((kind.empty() || kind == "unknown") && source_action_dict) {
      if (const std::string* source_kind =
              source_action_dict->FindString("action")) {
        kind = *source_kind;
      }
    }
    LOG(INFO) << "[Executor] Starting "
              << (is_key_script ? "keyStepIndex=" : "stepIndex=") << index
              << " attempt=" << attempt << " action=" << kind;
    std::string detail = kind;
    const std::string path_label = action->GetExecutionPathLabel();
    if (!path_label.empty()) {
      detail.append(" [");
      detail.append(path_label);
      detail.append("]");
    }
    if (attempt > 0) {
      detail.append(base::StringPrintf(" (retry %d)", attempt));
    }
    UpdatePlaybackOverlayProgress(detail);
    if (ShouldRefreshApcBeforeAction(kind)) {
      dashboard::InvalidateUnifiedPageMapCache(target_web_contents_);
    }
  }
  if (IsHumanCaptchaPauseAction(source_action_dict)) {
    UpdatePlaybackOverlayProgress(
        "Solve the captcha if shown, then press Resume");
    if (is_key_script) {
      key_step_ = static_cast<int>(index + 1);
    } else {
      current_step_ = static_cast<int>(index + 1);
    }
    PauseExecution();
    return;
  }
  pending_url_before_ = target_web_contents_->GetLastCommittedURL().spec();
  pending_action_started_at_ = base::TimeTicks::Now();
  pending_step_capture_.reset();

  if (!ShouldCapturePerStepTrainingArtifacts()) {
    action->Execute(
        target_web_contents_,
        base::BindOnce(&ScriptExecutor::OnActionCompleteInternal,
                       weak_ptr_factory_.GetWeakPtr(), is_key_script, index,
                       attempt));
    return;
  }

  CaptureStepContext(base::BindOnce(
      [](base::WeakPtr<ScriptExecutor> weak, bool run_key_script,
         size_t run_index, int run_attempt, dashboard::actions::Action* run_action,
         base::Value::Dict pre_page_map, std::vector<uint8_t> pre_png) {
        if (!weak || !weak->status_.is_running || !run_action ||
            !weak->target_web_contents_) {
          return;
        }
        ScriptExecutor::StepCaptureData capture;
        capture.pre_page_map = std::move(pre_page_map);
        capture.pre_png_b64 = EncodeBytesToBase64(pre_png);
        run_action->Execute(
            weak->target_web_contents_,
            base::BindOnce(&ScriptExecutor::OnActionExecutedWithPreCapture,
                           weak, run_key_script, run_index, run_attempt,
                           std::move(capture)));
      },
      weak_ptr_factory_.GetWeakPtr(), is_key_script, index, attempt, action));
}

void ScriptExecutor::UpdateStatus(const ExecutionStatus& status) {
  status_ = status;
  if (status_callback_)
    status_callback_.Run(status_);
}

void ScriptExecutor::UpdatePlaybackOverlayProgress(const std::string& detail) {
  if (!target_web_contents_ || !dashboard::IsPlaybackOverlayEnabled()) {
    return;
  }
  const int total = in_key_script_ ? static_cast<int>(key_action_queue_.size())
                                   : static_cast<int>(action_queue_.size());
  int current = in_key_script_ ? key_step_ : current_step_;
  if (status_.is_running && total > 0) {
    current = std::min(std::max(0, current) + 1, total);
  } else {
    current = std::max(0, current);
  }
  std::string header =
      base::StringPrintf("%s %d/%d", in_key_script_ ? "Key action" : "Action",
                         current, std::max(0, total));
  if (!detail.empty()) {
    header.append(" · ");
    header.append(detail);
  }
  dashboard::SetPlaybackOverlayStatus(target_web_contents_, header,
                                      status_.status_message);
}

void ScriptExecutor::ClearPlaybackOverlayProgress() {
  if (!target_web_contents_) {
    return;
  }
  dashboard::ClearPlaybackOverlayStatus(target_web_contents_);
}

void ScriptExecutor::CleanupAfterRun() {
  ClearPlaybackOverlayProgress();
  Observe(nullptr);
  action_queue_.clear();
  non_critical_.clear();
  current_step_ = 0;
  key_action_queue_.clear();
  key_non_critical_.clear();
  key_step_ = 0;
  step_index_ = 0;
  in_key_script_ = false;
  resume_main_step_after_key_ = 0;
  key_script_runs_ = 0;
  preflight_failed_action_index_.clear();
  consecutive_critical_failures_ = 0;
  has_any_critical_failure_ = false;
  first_critical_failed_step_ = -1;
  last_critical_error_.clear();
  is_paused_ = false;
  pending_url_before_.reset();
  pending_action_started_at_ = base::TimeTicks();
  pending_step_capture_.reset();
  recording_suppression_.reset();
  force_cdp_execution_for_run_ = false;
  force_native_execution_for_run_ = false;
  is_agent_mode_for_run_ = false;
  dashboard::ExecutionRunContext::Get().Clear();
  if (action_factory_) {
    action_factory_->SetForceCdpExecution(false);
    action_factory_->SetForceNativeExecution(false);
    action_factory_->SetAgentMode(false);
  }
  if (failure_replay_recorder_) {
    failure_replay_recorder_->Stop();
  }
  failure_replay_recorder_.reset();
  target_web_contents_ = nullptr;
  run_completion_in_progress_ = false;
}

void ScriptExecutor::WebContentsDestroyed() {
  // Target tab was closed while a run was active. Abort gracefully instead of
  // continuing to post tasks/actions against a dangling pointer.
  //
  // Clear the playback overlay *before* dropping target_web_contents_: the
  // overlay host keeps a raw pointer and a repeating ValidateContext timer.
  // Nulling first left that timer holding a dangling WebContents* (SIGTRAP /
  // DanglingRawPtrReleased on tab close during playback).
  ClearPlaybackOverlayProgress();
  if (!status_.is_running) {
    CleanupAfterRun();
    return;
  }
  // Stop observing immediately; this WebContents is going away.
  weak_ptr_factory_.InvalidateWeakPtrs();
  Observe(nullptr);
  target_web_contents_ = nullptr;
  status_.is_running = false;
  FinishRun(/*success=*/false, current_step_, "Target tab was closed");
}

int ScriptExecutor::MaxRetriesFor(bool is_key_script, size_t index) const {
  (void)is_key_script;
  (void)index;
  return 1;  // minimal retry policy
}

int ScriptExecutor::BaseRetryDelayMsFor(bool is_key_script, size_t index) const {
  (void)is_key_script;
  (void)index;
  return base::RandInt(kHumanizedInterActionDelayMinMs,
                       kHumanizedInterActionDelayMaxMs);
}

void ScriptExecutor::EvaluateAuthRecoveryForKeyScript(
    size_t failed_index,
    bool skip_humanized_delay_for_next) {
  if (!status_.is_running || key_action_queue_.empty()) {
    return;
  }
  status_.status_message = "Checking auth state before key script";
  UpdateStatus(status_);
  CapturePageMapSnapshot(base::BindOnce(
      &ScriptExecutor::OnAuthRecoverySnapshotCaptured,
      weak_ptr_factory_.GetWeakPtr(), failed_index,
      skip_humanized_delay_for_next));
}

void ScriptExecutor::OnAuthRecoverySnapshotCaptured(
    size_t failed_index,
    bool skip_humanized_delay_for_next,
    base::Value::Dict page_map) {
  if (!status_.is_running) {
    return;
  }

  const GURL current_url = target_web_contents_
                               ? target_web_contents_->GetLastCommittedURL()
                               : GURL();
  AuthStateSignals signals = DetectAuthStateSignals(page_map, current_url);
  const std::string signal_summary = DescribeAuthStateSignals(signals);
  LOG(INFO) << "[Executor] Auth recovery signals score=" << signals.score
            << " auth_required=" << signals.auth_required
            << " details=" << signal_summary;

  if (signals.auth_required) {
    status_.status_message = "Auth state detected; running Key Script";
    UpdateStatus(status_);
    // Resume the step that first hit the wall. Its contextUrl is what
    // puts the run back on that page after login; restarting at 0 replays
    // every earlier side effect.
    const int resume_step = first_critical_failed_step_ >= 0
                                ? first_critical_failed_step_
                                : static_cast<int>(failed_index);
    ScheduleContinuationWithHumanizedDelay(
        base::BindOnce(&ScriptExecutor::StartKeyScriptAfterFailures,
                       weak_ptr_factory_.GetWeakPtr(), resume_step),
        skip_humanized_delay_for_next);
    return;
  }

  status_.status_message =
      "Auth state not detected; continuing after failure";
  UpdateStatus(status_);
  current_step_ = static_cast<int>(failed_index + 1);
  ScheduleContinuationWithHumanizedDelay(
      base::BindOnce(&ScriptExecutor::ExecuteNextAction,
                     weak_ptr_factory_.GetWeakPtr()),
      skip_humanized_delay_for_next);
}

bool ScriptExecutor::IsNonCritical(bool is_key_script, size_t index) const {
  const auto& v = is_key_script ? key_non_critical_ : non_critical_;
  if (index >= v.size())
    return false;
  return v[index];
}

void ScriptExecutor::OnActionCompleteInternal(bool is_key_script,
                                              size_t index,
                                              int attempt,
                                              bool success,
                                              const std::string& message) {
  if (!status_.is_running) return;

  // Failure text routinely echoes the typed text / locator value; never let a
  // cleartext token value reach logs, history or heal prompts.
  std::string resolved_message =
      dashboard::RedactTokensInString(message, token_reverse_map_);
  const std::optional<std::string> nested_action_path =
      ConsumeNestedActionPathToken(&resolved_message);
  const std::string action_index_label = BuildActionIndexLabel(
      index, nested_action_path.value_or(std::string()));

  base::Value::Dict* source_action_dict = nullptr;
  base::Value::List& source_timeline = is_key_script ? key_timeline_ : timeline_;
  if (index < source_timeline.size()) {
    source_action_dict = source_timeline[index].GetIfDict();
  }
  const std::string expected_context_url = ReadActionContextUrl(source_action_dict);

  const auto& action =
      is_key_script ? key_action_queue_[index] : action_queue_[index];
  std::string action_kind = "unknown";
  {
    base::Value::Dict tmp;
    action->SerializeInto(&tmp);
    const std::string* kind_ptr = tmp.FindString("action");
    action_kind = kind_ptr ? *kind_ptr : "unknown";
    if ((action_kind.empty() || action_kind == "unknown") &&
        source_action_dict) {
      if (const std::string* source_kind =
              source_action_dict->FindString("action")) {
        action_kind = *source_kind;
      }
    }
    LOG(INFO) << "[Executor] Completed "
              << (is_key_script ? "keyStepIndex=" : "stepIndex=") << index
              << " attempt=" << attempt
              << " action=" << action_kind
              << " success=" << success
              << (resolved_message.empty() ? "" : (" message=" + resolved_message));
  }
  const bool skip_humanized_delay_for_next = action_kind == "wait";
  base::Value::Dict snapshot;
  if (!success || dashboard::IsVerboseLoggingEnabled()) {
    snapshot = current_snapshot_.Clone();
  }

  base::Value::Dict action_copy;
  action->SerializeInto(&action_copy);
  // Single choke point: action_copy feeds the step history, the params that
  // are promoted back into source_action_dict below, and (via history) the
  // saved script JSON and training rows. Restore {Token} placeholders before
  // any of those copies are made.
  {
    base::Value action_copy_value(std::move(action_copy));
    dashboard::RedactTokensInValue(&action_copy_value, token_reverse_map_);
    action_copy = std::move(action_copy_value).TakeDict();
  }
  action_copy.Set("stepIndex", static_cast<int>(index));
  action_copy.Set("actionIndex", action_index_label);
  if (success && source_action_dict) {
    if (const std::string* promoted_action = action_copy.FindString("action")) {
      source_action_dict->Set("action", *promoted_action);
    }
    if (const base::Value::Dict* promoted_params = action_copy.FindDict("params")) {
      source_action_dict->Set("params", promoted_params->Clone());
    }
  }

  const std::string url_before =
      pending_url_before_.value_or(target_web_contents_
                                       ? target_web_contents_->GetLastCommittedURL().spec()
                                       : std::string());
  const std::string url_after =
      target_web_contents_ ? target_web_contents_->GetLastCommittedURL().spec() : std::string();
  const std::string observed_context_url =
      !url_before.empty() ? url_before : url_after;
  const bool context_url_mismatch =
      !expected_context_url.empty() && !observed_context_url.empty() &&
      !ContextUrlsEquivalent(expected_context_url, observed_context_url);
  if (context_url_mismatch && !success) {
    if (!resolved_message.empty()) {
      resolved_message.append(" | ");
    }
    resolved_message.append(base::StringPrintf(
        "Context URL mismatch (expected=%s actual=%s)",
        expected_context_url.c_str(), observed_context_url.c_str()));
  }
  // Success only: learn the real page URL into the action's contextUrl.
  // Failures must not overwrite — wrong-page is often the bug, and auto-heal
  // needs the original expected contextUrl for comparison.
  if (success && !observed_context_url.empty() &&
      (expected_context_url.empty() || context_url_mismatch)) {
    SetActionContextUrl(source_action_dict, observed_context_url);
  }
  const std::string effective_context_url = ReadActionContextUrl(source_action_dict);
  if (!effective_context_url.empty()) {
    action_copy.Set("contextUrl", effective_context_url);
  } else if (!expected_context_url.empty()) {
    action_copy.Set("contextUrl", expected_context_url);
  }
  const std::string context_url = script_context_ ? script_context_->current_url() : std::string();
  const double duration_ms = pending_action_started_at_.is_null()
                                 ? 0.0
                                 : (base::TimeTicks::Now() - pending_action_started_at_).InMillisecondsF();

  base::Value::Dict step_entry;
  step_entry.Set("step", step_index_++);
  step_entry.Set("run_id", run_id_);
  step_entry.Set("script_id", script_context_ ? script_context_->script_id() : std::string());
  step_entry.Set("origin", is_key_script ? "key_script" : "script");
  step_entry.Set("stepIndex", static_cast<int>(index));
  step_entry.Set("action_index", action_index_label);
  step_entry.Set("actionIndex", action_index_label);
  step_entry.Set("attempt", attempt);
  step_entry.Set("url_before", url_before);
  step_entry.Set("url_after", url_after);
  step_entry.Set("context_url", context_url);
  if (!expected_context_url.empty()) {
    step_entry.Set("expected_context_url", expected_context_url);
  }
  if (!observed_context_url.empty()) {
    step_entry.Set("observed_context_url", observed_context_url);
  }
  step_entry.Set("context_url_mismatch", context_url_mismatch);
  step_entry.Set("duration_ms", duration_ms);
  step_entry.Set("action", std::move(action_copy));
  step_entry.Set("status", success ? "success" : "fail");
  step_entry.Set("message", resolved_message);
  if (!snapshot.empty()) {
    step_entry.Set("page_map", std::move(snapshot));
  }
  if (pending_step_capture_) {
    if (!pending_step_capture_->pre_page_map.empty()) {
      step_entry.Set("pre_page_map",
                     std::move(pending_step_capture_->pre_page_map));
    }
    if (!pending_step_capture_->post_page_map.empty()) {
      step_entry.Set("post_page_map",
                     std::move(pending_step_capture_->post_page_map));
    }
    if (!pending_step_capture_->pre_png_b64.empty()) {
      step_entry.Set("pre_screenshot_png_base64",
                     std::move(pending_step_capture_->pre_png_b64));
    }
    if (!pending_step_capture_->post_png_b64.empty()) {
      step_entry.Set("post_screenshot_png_base64",
                     std::move(pending_step_capture_->post_png_b64));
    }
    pending_step_capture_.reset();
  }
  current_context_.history.push_back(std::move(step_entry));

  pending_url_before_.reset();
  pending_action_started_at_ = base::TimeTicks();

  if (!success) {
    int maxr = MaxRetriesFor(is_key_script, index);
    if (attempt + 1 <= maxr) {
      int delay = BaseRetryDelayMsFor(is_key_script, index);
      for (int i = 0; i < attempt; ++i) delay *= 2;
      status_.status_message = "Retry in " + std::to_string(delay) + " ms";
      UpdateStatus(status_);
      task_runner_->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&ScriptExecutor::AttemptAction,
                         weak_ptr_factory_.GetWeakPtr(), is_key_script, index,
                         attempt + 1),
          base::Milliseconds(delay));
      return;
    }
    if (IsNonCritical(is_key_script, index)) {
      LOG(WARNING) << "Non-critical action failed at index=" << index << ": "
                   << resolved_message;
      if (is_key_script) {
        key_step_ = static_cast<int>(index + 1);
      } else {
        current_step_ = static_cast<int>(index + 1);
      }
      ScheduleContinuationWithHumanizedDelay(
          base::BindOnce(&ScriptExecutor::ExecuteNextAction,
                         weak_ptr_factory_.GetWeakPtr()),
          skip_humanized_delay_for_next);
      return;
    }

    // Key-script failures are fatal: the recovery sequence failed.
    if (is_key_script) {
      FinishKeyScriptAndResumeMain(/*success=*/false, resolved_message);
      return;
    }

    // Main-script failure policy:
    // - If a key script is configured, attempt it after two consecutive
    //   critical failures.
    // - Otherwise, continue through failures for testing, and mark the run as
    //   failed at the end.
    has_any_critical_failure_ = true;
    if (first_critical_failed_step_ < 0)
      first_critical_failed_step_ = static_cast<int>(index);
    last_critical_error_ = resolved_message;
    consecutive_critical_failures_++;

    if (!key_action_queue_.empty() &&
        consecutive_critical_failures_ >=
            kAuthRecoveryRequiredConsecutiveFailures &&
        key_script_runs_ < 3) {
      EvaluateAuthRecoveryForKeyScript(index, skip_humanized_delay_for_next);
      return;
    }

    current_step_ = static_cast<int>(index + 1);
    ScheduleContinuationWithHumanizedDelay(
        base::BindOnce(&ScriptExecutor::ExecuteNextAction,
                       weak_ptr_factory_.GetWeakPtr()),
        skip_humanized_delay_for_next);
    return;
  }

  // Success: advance within the active sequence.
  if (is_key_script) {
    key_step_ = static_cast<int>(index + 1);
  } else {
    consecutive_critical_failures_ = 0;
    current_step_ = static_cast<int>(index + 1);
  }
  ScheduleContinuationWithHumanizedDelay(
      base::BindOnce(&ScriptExecutor::ExecuteNextAction,
                     weak_ptr_factory_.GetWeakPtr()),
      skip_humanized_delay_for_next);
}

void ScriptExecutor::StartKeyScriptAfterFailures(int resume_step_index) {
  if (in_key_script_) {
    return;
  }
  if (key_action_queue_.empty()) {
    return;
  }

  in_key_script_ = true;
  key_step_ = 0;
  resume_main_step_after_key_ = std::max(0, resume_step_index);
  key_script_runs_++;
  consecutive_critical_failures_ = 0;

  status_.status_message =
      "Running Key Script (attempt " + std::to_string(key_script_runs_) + ")";
  UpdateStatus(status_);

  ExecuteNextAction();
}

void ScriptExecutor::FinishKeyScriptAndResumeMain(bool success,
                                                 const std::string& message) {
  if (!in_key_script_) {
    return;
  }

  in_key_script_ = false;
  key_step_ = 0;

  if (!success) {
    const std::string err =
        "Key Script failed" + (message.empty() ? "" : (": " + message));
    // If the main script already had a failing step recorded, keep that as the
    // failed_step index; otherwise fall back to the current step.
    const int failed_step =
        first_critical_failed_step_ >= 0 ? first_critical_failed_step_
                                         : std::max(0, current_step_);
    FinishRun(/*success=*/false, failed_step, err);
    return;
  }

  status_.status_message = "Key Script complete; checking login";
  UpdateStatus(status_);
  VerifyLoginThenResumeMain(base::TimeTicks::Now() + base::Seconds(15));
}

void ScriptExecutor::VerifyLoginThenResumeMain(base::TimeTicks deadline) {
  if (!status_.is_running) {
    return;
  }
  // The last key-script click usually starts the post-login navigation.
  // Reading the DOM before that commit still shows the password field and
  // would look like a failed login.
  if (target_web_contents_ && target_web_contents_->IsLoading() &&
      base::TimeTicks::Now() < deadline) {
    task_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&ScriptExecutor::VerifyLoginThenResumeMain,
                       weak_ptr_factory_.GetWeakPtr(), deadline),
        base::Milliseconds(250));
    return;
  }
  CapturePageMapSnapshot(base::BindOnce(&ScriptExecutor::OnKeyScriptVerified,
                                        weak_ptr_factory_.GetWeakPtr()));
}

void ScriptExecutor::OnKeyScriptVerified(base::Value::Dict page_map) {
  if (!status_.is_running) {
    return;
  }

  const GURL current_url = target_web_contents_
                               ? target_web_contents_->GetLastCommittedURL()
                               : GURL();
  const AuthStateSignals signals =
      DetectAuthStateSignals(page_map, current_url);
  LOG(INFO) << "[Executor] Post-key-script signals score=" << signals.score
            << " auth_required=" << signals.auth_required
            << " details=" << DescribeAuthStateSignals(signals);

  const int resume_step = std::max(0, resume_main_step_after_key_);
  if (signals.auth_required) {
    FinishRun(/*success=*/false, resume_step,
              "Key Script finished but the login wall is still up; "
              "not submitting credentials again");
    return;
  }

  current_step_ = resume_step;
  status_.current_step = resume_step;
  status_.status_message = "Login cleared; resuming";
  UpdateStatus(status_);

  const std::string resume_url =
      dashboard::ScriptSchema::ResolveResumeNavigationUrl(timeline_,
                                                          resume_step);
  if (!resume_url.empty()) {
    LOG(INFO) << "[Executor] Key-script resume contextUrl=" << resume_url
              << " step=" << resume_step;
  }
  NavigateAndStart(resume_step, resume_url);
}

void ScriptExecutor::SaveExecutionResult(bool success,
                                        int failed_step,
                                        const std::string& message) {
  if (!script_context_) {
    return;
  }

  using ReplayRecorderPtr =
      std::unique_ptr<dashboard::FailureReplayRecorder, base::OnTaskRunnerDeleter>;

  const std::string script_id = script_context_->script_id();
  const base::FilePath scripts_dir = dashboard::ScriptStore::ScriptsDir(profile_);
  const base::FilePath runs_dir = dashboard::RunHistoryStore::RunsDir(profile_);
  const base::FilePath training_root = SimulachromeTrainingRoot(profile_);

  const base::Time ended = base::Time::Now();
  const double started_ms =
      static_cast<double>(run_started_wall_time_.InMillisecondsSinceUnixEpoch());
  const double ended_ms =
      static_cast<double>(ended.InMillisecondsSinceUnixEpoch());

  base::Value::List steps;
  for (auto& e : current_context_.history) {
    steps.Append(std::move(e));
  }

  const int total_steps = static_cast<int>(action_queue_.size());
  int warning_failures = 0;
  base::Value::List steps_light;
  for (const auto& s : steps) {
    const base::Value::Dict* d = s.GetIfDict();
    if (!d)
      continue;
    const std::string* st = d->FindString("status");
    if (st && *st == "fail")
      ++warning_failures;

    base::Value::Dict light = d->Clone();
    light.Remove("page_map");
    light.Remove("pre_page_map");
    light.Remove("post_page_map");
    light.Remove("pre_screenshot_png_base64");
    light.Remove("post_screenshot_png_base64");
    steps_light.Append(std::move(light));
  }
  std::string failed_action_index;
  if (failed_step >= 0) {
    for (const auto& step_v : steps_light) {
      const base::Value::Dict* step = step_v.GetIfDict();
      if (!step) {
        continue;
      }
      const int step_index = step->FindInt("stepIndex").value_or(-1);
      if (step_index != failed_step) {
        continue;
      }
      const std::string* action_index = step->FindString("action_index");
      if (action_index && !action_index->empty()) {
        failed_action_index = *action_index;
      }
      break;
    }
  }
  if (failed_action_index.empty() && !preflight_failed_action_index_.empty()) {
    failed_action_index = preflight_failed_action_index_;
  }

  const std::string trace_signature =
      BuildTraceSignature(script_id, steps_light, success, failed_step, message);
  const std::string trace_bucket_key =
      BuildTraceBucketKey(script_id, trace_signature);

  base::Value::Dict trace;
  trace.Set("run_id", run_id_);
  trace.Set("script_id",
            script_context_ ? script_context_->script_id() : std::string());
  trace.Set("started_ms", started_ms);
  trace.Set("ended_ms", ended_ms);
  trace.Set("success", success);
  trace.Set("failed_step", failed_step);
  if (!failed_action_index.empty()) {
    trace.Set("failed_action_index", failed_action_index);
  }
  trace.Set("error", message);
  trace.Set("start_url", target_web_contents_
                             ? target_web_contents_->GetLastCommittedURL().spec()
                             : std::string());
  trace.Set("started_ms_since_boot",
            static_cast<double>(
                run_started_at_.since_origin().InMillisecondsF()));
  // Keep persisted last_run lightweight. Heavy per-step snapshots should be
  // stored separately (e.g. per-run detail/replay artifacts) rather than
  // bloating script JSON.
  trace.Set("steps", steps_light.Clone());
  trace.Set("dataset", BuildDatasetMetadata());
  trace.Set("trace_signature", trace_signature);
  trace.Set("trace_bucket_key", trace_bucket_key);
  trace.Set("trace_bucket_schema", kTraceBucketSchemaVersion);
  if (!success && !message.empty()) {
    trace.Set("failure_taxonomy", ClassifyFailureTaxonomy(message));
  }

  // Save into the in-memory ScriptContext.
  script_context_->mutable_script_data()->Set("last_run", trace.Clone());

  // Append a compact run summary for the Debugging tab.
  base::Value::Dict summary;
  summary.Set("run_id", run_id_);
  summary.Set("script_id", script_id);
  summary.Set("script_name", "");
  summary.Set("started_ms", started_ms);
  summary.Set("ended_ms", ended_ms);
  summary.Set("status", success ? "success" : "error");
  summary.Set("failed_step", failed_step);
  if (!failed_action_index.empty()) {
    summary.Set("failed_action_index", failed_action_index);
  }
  summary.Set("error", message);
  summary.Set("total_steps", total_steps);
  summary.Set("warning_failures", warning_failures);
  summary.Set("dataset_version", kDatasetVersion);
  summary.Set("dataset_schema_version", kDatasetSchemaVersion);
  summary.Set("trace_signature", trace_signature);
  summary.Set("trace_bucket_key", trace_bucket_key);
  summary.Set("trace_bucket_schema", kTraceBucketSchemaVersion);
  if (!success && !message.empty()) {
    summary.Set("failure_taxonomy", ClassifyFailureTaxonomy(message));
  }

  // `FailureReplayRecorder` owns a WeakPtrFactory that must be invalidated on
  // the same sequence it was created on (the UI sequence). We can still run the
  // blocking file I/O for `SaveToDisk()` on a background thread, but we must
  // ensure destruction happens back on the UI task runner.
  ReplayRecorderPtr replay_recorder(
      nullptr, base::OnTaskRunnerDeleter(
                   base::SequencedTaskRunner::GetCurrentDefault()));
  if (failure_replay_recorder_) {
    replay_recorder.reset(failure_replay_recorder_.release());
  }
  if (replay_recorder) {
    replay_recorder->Stop();
  }

  base::Value::Dict detail;
  if (!success) {
    detail = summary.Clone();
    detail.Set("steps", steps_light.Clone());
    if (!failed_action_index.empty()) {
      detail.Set("failed_action_index", failed_action_index);
    }
  }

  // Persist all run artifacts on a background thread (blocking file I/O).
  scoped_refptr<base::SequencedTaskRunner> ui_task_runner =
      base::SequencedTaskRunner::GetCurrentDefault();
  base::ThreadPool::PostTask(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath scripts_dir, base::FilePath runs_dir,
             base::FilePath training_root, std::string script_id,
             std::string run_id, base::Value::Dict trace,
             base::Value::Dict summary, base::Value::Dict detail,
             base::Value::List training_steps, std::string trace_signature,
             std::string trace_bucket_key, ReplayRecorderPtr replay_recorder,
             Profile* profile,
             scoped_refptr<base::SequencedTaskRunner> ui_task_runner) {
            // 1) Update script JSON with last_run.
            if (!scripts_dir.empty() && !script_id.empty()) {
              base::FilePath path =
                  dashboard::ScriptStore::PathForScriptId(profile, script_id);
              if (path.empty()) {
                path = scripts_dir.AppendASCII(script_id + ".json");
              }
              std::string json;
              if (base::ReadFileToString(path, &json)) {
                auto v = base::JSONReader::Read(
                    json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
                if (v && v->is_dict()) {
                  base::Value::Dict existing = v->GetDict().Clone();
                  const std::string* name = existing.FindString("name");
                  if (name)
                    summary.Set("script_name", *name);
                  int context_url_updates = 0;
                  int promoted_action_updates = 0;
                  if (base::Value::List* script_actions =
                          existing.FindList("actions")) {
                    if (const base::Value::List* trace_steps =
                            trace.FindList("steps")) {
                      for (const auto& step_v : *trace_steps) {
                        const base::Value::Dict* step = step_v.GetIfDict();
                        if (!step) {
                          continue;
                        }
                        const std::string* status = step->FindString("status");
                        if (!status || *status != "success") {
                          continue;
                        }
                        const int step_index = step->FindInt("stepIndex").value_or(-1);
                        if (step_index < 0 ||
                            step_index >= static_cast<int>(script_actions->size())) {
                          continue;
                        }
                        base::Value::Dict* script_action =
                            (*script_actions)[step_index].GetIfDict();
                        const base::Value::Dict* run_action =
                            step->FindDict("action");
                        if (!script_action || !run_action) {
                          continue;
                        }
                        const std::string* run_kind =
                            run_action->FindString("action");
                        if (!run_kind) {
                          continue;
                        }

                        bool action_updated = false;
                        const std::string* script_kind =
                            script_action->FindString("action");
                        if (!script_kind || *script_kind != *run_kind) {
                          script_action->Set("action", *run_kind);
                          action_updated = true;
                        }

                        const base::Value::Dict* run_params =
                            run_action->FindDict("params");
                        if (run_params) {
                          const base::Value::Dict* script_params =
                              script_action->FindDict("params");
                          std::string persistence_reason;
                          const bool should_persist_params =
                              !script_params ||
                              dashboard::ShouldPersistPromotedActionParams(
                                  *script_params, *run_params,
                                  &persistence_reason);
                          if (should_persist_params &&
                              (!script_params || *script_params != *run_params)) {
                            script_action->Set("params", run_params->Clone());
                            action_updated = true;
                          } else if (script_params &&
                                     *script_params != *run_params) {
                            LOG(INFO)
                                << "[ScriptExecutor] Skipping persisted action "
                                   "param promotion for script="
                                << script_id << " step=" << step_index
                                << " reason=" << persistence_reason;
                          }
                        }
                        if (action_updated) {
                          ++promoted_action_updates;
                        }

                        std::string observed_context_url =
                            ReadActionContextUrl(run_action);
                        if (observed_context_url.empty()) {
                          if (const std::string* observed =
                                  step->FindString("observed_context_url")) {
                            observed_context_url = *observed;
                          }
                        }
                        base::TrimWhitespaceASCII(
                            observed_context_url, base::TRIM_ALL,
                            &observed_context_url);
                        if (observed_context_url.empty()) {
                          continue;
                        }
                        const std::string existing_context_url =
                            ReadActionContextUrl(script_action);
                        if (ContextUrlsEquivalent(existing_context_url,
                                                 observed_context_url)) {
                          continue;
                        }
                        SetActionContextUrl(script_action, observed_context_url);
                        ++context_url_updates;
                      }
                    }
                  }
                  if (context_url_updates > 0) {
                    trace.Set("context_url_updates", context_url_updates);
                    summary.Set("context_url_updates", context_url_updates);
                  }
                  if (promoted_action_updates > 0) {
                    trace.Set("promoted_action_updates", promoted_action_updates);
                    summary.Set("promoted_action_updates",
                                promoted_action_updates);
                  }
                  existing.Set("last_run", trace.Clone());
                  existing = dashboard::ScriptSchema::CanonicalizeForSave(
                      std::move(existing), script_id, base::Time::Now());
                  std::string out_json;
                  if (base::JSONWriter::Write(existing, &out_json)) {
                    base::CreateDirectory(scripts_dir);
                    if (base::WriteFile(path, base::as_byte_span(out_json)) > 0 &&
                        ui_task_runner) {
                      ui_task_runner->PostTask(
                          FROM_HERE,
                          base::BindOnce(
                              &DashboardUIMessageHandler::UpdateRuntimeScriptCache,
                              profile, script_id, existing.Clone()));
                    }
                  }
                }
              }
            }

            int trace_bucket_prior_count = 0;
            bool has_trace_bucket =
                !trace_signature.empty() && !trace_bucket_key.empty();
            if (has_trace_bucket && !runs_dir.empty()) {
              const base::FilePath bucket_meta_path =
                  runs_dir.AppendASCII("trace_buckets")
                      .AppendASCII(trace_bucket_key + ".meta.json");
              std::string existing_meta;
              if (base::ReadFileToString(bucket_meta_path, &existing_meta)) {
                auto parsed = base::JSONReader::Read(
                    existing_meta, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
                if (parsed && parsed->is_dict()) {
                  trace_bucket_prior_count =
                      std::max(0, parsed->GetDict().FindInt("count").value_or(0));
                }
              }
            }

            const bool compress_detail_for_bucket =
                trace_bucket_prior_count >= kTraceBucketFullDetailRetentionRuns;
            const bool compress_training_for_bucket =
                trace_bucket_prior_count >= kTraceBucketFullTrainingRetentionRuns;

            summary.Set("trace_bucket_prior_count", trace_bucket_prior_count);
            summary.Set("trace_bucket_compressed", compress_detail_for_bucket);
            trace.Set("trace_bucket_prior_count", trace_bucket_prior_count);
            trace.Set("trace_bucket_compressed", compress_detail_for_bucket);
            if (!detail.empty()) {
              detail.Set("trace_bucket_prior_count", trace_bucket_prior_count);
              detail.Set("trace_bucket_compressed", compress_detail_for_bucket);
              if (compress_detail_for_bucket) {
                if (const base::Value::List* detail_steps = detail.FindList("steps")) {
                  base::Value::List sampled;
                  if (!detail_steps->empty()) {
                    sampled.Append((*detail_steps)[0].Clone());
                  }
                  const int failed =
                      detail.FindInt("failed_step").value_or(-1);
                  if (failed >= 0 &&
                      failed < static_cast<int>(detail_steps->size()) &&
                      failed != 0) {
                    sampled.Append((*detail_steps)[failed].Clone());
                  }
                  if (detail_steps->size() > 1) {
                    const int tail =
                        static_cast<int>(detail_steps->size()) - 1;
                    if (tail > 0 && tail != failed) {
                      sampled.Append((*detail_steps)[tail].Clone());
                    }
                  }
                  detail.Set("steps_sample", std::move(sampled));
                  detail.Set("steps_sampled_from",
                             static_cast<int>(detail_steps->size()));
                }
                detail.Remove("steps");
                detail.Set("steps_compressed", true);
              }
            }

            // 2) Append summary to run history index.
            if (!runs_dir.empty()) {
              base::CreateDirectory(runs_dir);
              const base::FilePath index_path =
                  runs_dir.AppendASCII("index.jsonl");
              std::string line;
              if (base::JSONWriter::Write(summary, &line)) {
                line.push_back('\n');
                base::AppendToFile(index_path, line);
              }

              if (has_trace_bucket) {
                const base::FilePath buckets_dir =
                    runs_dir.AppendASCII("trace_buckets");
                base::CreateDirectory(buckets_dir);

                base::Value::Dict bucket_event;
                bucket_event.Set("schema", kTraceBucketSchemaVersion);
                bucket_event.Set("run_id", run_id);
                bucket_event.Set("script_id", script_id);
                bucket_event.Set("trace_signature", trace_signature);
                bucket_event.Set("trace_bucket_key", trace_bucket_key);
                bucket_event.Set("prior_count", trace_bucket_prior_count);
                bucket_event.Set("compressed", compress_detail_for_bucket);
                bucket_event.Set("status", summary.FindString("status")
                                               ? *summary.FindString("status")
                                               : std::string());
                bucket_event.Set(
                    "failed_step",
                    summary.FindInt("failed_step").value_or(-1));
                bucket_event.Set("error", summary.FindString("error")
                                              ? *summary.FindString("error")
                                              : std::string());
                if (const std::optional<double> started =
                        summary.FindDouble("started_ms")) {
                  bucket_event.Set("started_ms", *started);
                }
                if (const std::optional<double> ended =
                        summary.FindDouble("ended_ms")) {
                  bucket_event.Set("ended_ms", *ended);
                }

                std::string bucket_line;
                if (base::JSONWriter::Write(bucket_event, &bucket_line)) {
                  bucket_line.push_back('\n');
                  const base::FilePath per_bucket_path =
                      buckets_dir.AppendASCII(trace_bucket_key + ".jsonl");
                  base::AppendToFile(per_bucket_path, bucket_line);
                  base::AppendToFile(
                      buckets_dir.AppendASCII("index.jsonl"), bucket_line);
                }

                const base::FilePath bucket_meta_path =
                    buckets_dir.AppendASCII(trace_bucket_key + ".meta.json");
                const int count = trace_bucket_prior_count + 1;
                base::Value::Dict meta;
                meta.Set("schema", kTraceBucketSchemaVersion);
                meta.Set("trace_signature", trace_signature);
                meta.Set("trace_bucket_key", trace_bucket_key);
                meta.Set("script_id", script_id);
                meta.Set("count", count);
                meta.Set("last_run_id", run_id);
                meta.Set("last_compressed", compress_detail_for_bucket);
                meta.Set(
                    "updated_unix_ms",
                    base::NumberToString(
                        base::Time::Now().InMillisecondsSinceUnixEpoch()));
                std::string meta_json;
                if (base::JSONWriter::Write(meta, &meta_json)) {
                  base::WriteFile(bucket_meta_path, base::as_byte_span(meta_json));
                }
              }
            }

            // 3) Optionally write per-run detail (and replay).
            if (!detail.empty() && !runs_dir.empty() && !run_id.empty()) {
              detail.Set("dataset_version", kDatasetVersion);
              detail.Set("dataset_schema_version", kDatasetSchemaVersion);
              if (const std::string* err = detail.FindString("error")) {
                if (!err->empty()) {
                  detail.Set("failure_taxonomy", ClassifyFailureTaxonomy(*err));
                }
              }
              // Replay is stored under runs_dir/replays/<run_id>/...
              if (replay_recorder && replay_recorder->HasFrames()) {
                const base::FilePath replay_dir =
                    runs_dir.AppendASCII("replays").AppendASCII(run_id);
                std::string replay_error;
                const bool wrote =
                    replay_recorder->SaveToDisk(replay_dir, &replay_error);
                if (wrote) {
                  detail.Set("has_replay", true);
                  detail.Set("replay_manifest",
                             "chrome://dashboard/replay/" + run_id +
                                 "/manifest.json");
                } else {
                  detail.Set("has_replay", false);
                  detail.Set("replay_error", replay_error);
                }
              }

              const base::FilePath details_dir = runs_dir.AppendASCII("runs");
              base::CreateDirectory(details_dir);
              const base::FilePath detail_path =
                  details_dir.AppendASCII(run_id + ".json");
              std::string detail_json;
              if (base::JSONWriter::Write(detail, &detail_json)) {
                base::WriteFile(detail_path, base::as_byte_span(detail_json));
              }
            }

            // 4) Persist per-step multimodal training rows with explicit
            // screenshot <-> candidate alignment.
            if (!training_root.empty() && !run_id.empty() &&
                !training_steps.empty()) {
              WriteDatasetVersionManifest(training_root);
              const base::FilePath step_shots_dir =
                  training_root.AppendASCII("step_screenshots");
              base::CreateDirectory(step_shots_dir);
              const base::FilePath steps_jsonl_path =
                  training_root.AppendASCII("simulachrome_mm_steps.jsonl");
              const int total_training_steps =
                  static_cast<int>(training_steps.size());
              const int failed_step_summary =
                  summary.FindInt("failed_step").value_or(-1);
              base::flat_set<int> retained_step_indices;
              if (compress_training_for_bucket) {
                retained_step_indices.insert(0);
                if (total_training_steps > 1) {
                  retained_step_indices.insert(total_training_steps - 1);
                }
                if (failed_step_summary >= 0 &&
                    failed_step_summary < total_training_steps) {
                  retained_step_indices.insert(failed_step_summary);
                }
              }

              int training_row_index = -1;
              for (const auto& step_v : training_steps) {
                ++training_row_index;
                const base::Value::Dict* step = step_v.GetIfDict();
                if (!step) {
                  continue;
                }
                if (compress_training_for_bucket &&
                    !retained_step_indices.contains(training_row_index)) {
                  continue;
                }

                const int step_ordinal = step->FindInt("step").value_or(-1);
                const int step_index = step->FindInt("stepIndex").value_or(-1);
                const int attempt = step->FindInt("attempt").value_or(0);
                const std::string status =
                    step->FindString("status") ? *step->FindString("status")
                                               : std::string();
                const std::string url_before =
                    step->FindString("url_before")
                        ? *step->FindString("url_before")
                        : std::string();
                const std::string url_after = step->FindString("url_after")
                                                  ? *step->FindString("url_after")
                                                  : std::string();

                std::string pre_path;
                if (!compress_training_for_bucket) {
                  if (const std::string* b64 =
                          step->FindString("pre_screenshot_png_base64");
                      b64 && !b64->empty()) {
                    std::vector<uint8_t> bytes;
                    if (DecodeBase64ToBytes(*b64, &bytes) && !bytes.empty()) {
                      const std::string file_name = base::StringPrintf(
                          "%s_step%04d_attempt%02d_pre.png", run_id.c_str(),
                          std::max(step_ordinal, 0), std::max(attempt, 0));
                      const base::FilePath path =
                          step_shots_dir.AppendASCII(file_name);
                      if (base::WriteFile(path, base::span<const uint8_t>(bytes)) >
                          0) {
                        pre_path = path.AsUTF8Unsafe();
                      }
                    }
                  }
                }

                std::string post_path;
                if (!compress_training_for_bucket) {
                  if (const std::string* b64 =
                          step->FindString("post_screenshot_png_base64");
                      b64 && !b64->empty()) {
                    std::vector<uint8_t> bytes;
                    if (DecodeBase64ToBytes(*b64, &bytes) && !bytes.empty()) {
                      const std::string file_name = base::StringPrintf(
                          "%s_step%04d_attempt%02d_post.png", run_id.c_str(),
                          std::max(step_ordinal, 0), std::max(attempt, 0));
                      const base::FilePath path =
                          step_shots_dir.AppendASCII(file_name);
                      if (base::WriteFile(path, base::span<const uint8_t>(bytes)) >
                          0) {
                        post_path = path.AsUTF8Unsafe();
                      }
                    }
                  }
                }

                base::Value::Dict row;
                row.Set("schema", kMmStepSchema);
                row.Set("dataset_version", kDatasetVersion);
                row.Set("dataset_schema_version", kDatasetSchemaVersion);
                row.Set("run_id", run_id);
                row.Set("script_id", script_id);
                row.Set("trace_signature", trace_signature);
                row.Set("trace_bucket_key", trace_bucket_key);
                row.Set("trace_bucket_prior_count", trace_bucket_prior_count);
                row.Set("trace_bucket_compressed",
                        compress_training_for_bucket);
                row.Set("step", step_ordinal);
                row.Set("step_index", step_index);
                row.Set("attempt", attempt);
                row.Set("status", status);
                row.Set("success", status == "success");
                row.Set("url_before", url_before);
                row.Set("url_after", url_after);
                row.Set(
                    "recorded_at_unix_ms",
                    base::NumberToString(
                        base::Time::Now().InMillisecondsSinceUnixEpoch()));
                const base::Value::Dict* action = step->FindDict("action");
                if (action) {
                  row.Set("action", action->Clone());
                  row.Set("target_hint", BuildTargetHintFromAction(action));
                  if (const std::optional<double> conf =
                          ExtractModelConfidence(action)) {
                    row.Set("model_confidence", *conf);
                  }
                }
                if (const std::string* msg = step->FindString("message")) {
                  row.Set("message", *msg);
                  if (!msg->empty() && status != "success") {
                    row.Set("failure_taxonomy", ClassifyFailureTaxonomy(*msg));
                  }
                }
                if (const std::optional<double> ms = step->FindDouble("duration_ms")) {
                  row.Set("duration_ms", *ms);
                }
                if (!pre_path.empty()) {
                  row.Set("pre_screenshot_path", pre_path);
                }
                if (!post_path.empty()) {
                  row.Set("post_screenshot_path", post_path);
                }

                base::Value::List pre_candidates;
                base::Value::List post_candidates;
                if (!compress_training_for_bucket) {
                  const base::Value::Dict* pre_map = step->FindDict("pre_page_map");
                  if (pre_map) {
                    row.Set("pre_page_map", pre_map->Clone());
                    pre_candidates =
                        BuildCandidatesFromPageMap(pre_map, /*max_candidates=*/200);
                    row.Set("pre_candidates", pre_candidates.Clone());
                  }
                  const base::Value::Dict* post_map = step->FindDict("post_page_map");
                  if (post_map) {
                    row.Set("post_page_map", post_map->Clone());
                    post_candidates =
                        BuildCandidatesFromPageMap(post_map, /*max_candidates=*/200);
                    row.Set("post_candidates", post_candidates.Clone());
                  }
                }

                if (action) {
                  std::optional<base::Value::Dict> chosen =
                      MatchActionToCandidates(action, pre_candidates);
                  if (chosen) {
                    chosen->Set("stage", "pre");
                  } else {
                    chosen = MatchActionToCandidates(action, post_candidates);
                    if (chosen) {
                      chosen->Set("stage", "post");
                    }
                  }
                  if (chosen) {
                    row.Set("chosen_candidate", chosen->Clone());
                    if (const std::optional<int> idx = chosen->FindInt("index")) {
                      row.Set("chosen_candidate_index", *idx);
                    }
                    if (const std::optional<double> conf =
                            chosen->FindDouble("confidence")) {
                      row.Set("chosen_candidate_match_confidence", *conf);
                    }
                    if (const std::string* reason = chosen->FindString("reason")) {
                      row.Set("chosen_candidate_match_reason", *reason);
                    }
                    if (const std::string* stage = chosen->FindString("stage")) {
                      row.Set("chosen_candidate_stage", *stage);
                    }
                    if (const base::Value::Dict* matched_action_locator =
                            chosen->FindDict("matched_action_locator")) {
                      row.Set("chosen_candidate_action_locator",
                              matched_action_locator->Clone());
                    }
                    if (const base::Value::Dict* matched_candidate_locator =
                            chosen->FindDict("matched_candidate_locator")) {
                      row.Set("chosen_candidate_locator",
                              matched_candidate_locator->Clone());
                    }
                  }
                }

                if (!compress_training_for_bucket) {
                  base::Value::List messages;
                  base::Value::Dict system_msg;
                  system_msg.Set("role", "system");
                  system_msg.Set(
                      "content",
                      "You are a web automation model. Choose the next robust "
                      "action based on screenshot + candidate metadata.");
                  messages.Append(std::move(system_msg));

                  base::Value::Dict user_msg;
                  user_msg.Set("role", "user");
                  base::Value::List user_content;
                  if (!pre_path.empty()) {
                    base::Value::Dict image_part;
                    image_part.Set("type", "image");
                    image_part.Set("image", pre_path);
                    user_content.Append(std::move(image_part));
                  }
                  base::Value::Dict text_part;
                  text_part.Set("type", "text");
                  text_part.Set(
                      "text",
                      base::StringPrintf(
                          "URL before: %s\nURL after: %s\nStep index: %d\nAttempt: %d\n"
                          "Return the next action timeline JSON.",
                          url_before.c_str(), url_after.c_str(), step_index, attempt));
                  user_content.Append(std::move(text_part));
                  user_msg.Set("content", std::move(user_content));
                  messages.Append(std::move(user_msg));

                  base::Value::Dict assistant_msg;
                  assistant_msg.Set("role", "assistant");
                  base::Value::List assistant_content;
                  base::Value::Dict assistant_text;
                  assistant_text.Set("type", "text");
                  assistant_text.Set(
                      "text",
                      SerializeActionAsTimelineJson(step->FindDict("action")));
                  assistant_content.Append(std::move(assistant_text));
                  assistant_msg.Set("content", std::move(assistant_content));
                  messages.Append(std::move(assistant_msg));
                  row.Set("messages", std::move(messages));
                }

                std::string line;
                if (base::JSONWriter::Write(row, &line)) {
                  line.push_back('\n');
                  base::AppendToFile(steps_jsonl_path, line);
                }
              }
            }
          },
          std::move(scripts_dir), std::move(runs_dir), std::move(training_root),
          script_id, run_id_, trace.Clone(), summary.Clone(), std::move(detail),
          std::move(steps), trace_signature, trace_bucket_key,
          std::move(replay_recorder), profile_, std::move(ui_task_runner)));
}

void ScriptExecutor::PauseExecution() {
  if (!status_.is_running) {
    return;
  }
  is_paused_ = true;
  status_.status_message = "Paused";
  UpdateStatus(status_);
  UpdatePlaybackOverlayProgress("Paused");
}

void ScriptExecutor::ResumeExecution() {
  if (!status_.is_running) {
    return;
  }
  is_paused_ = false;
  status_.status_message = "Resumed";
  UpdateStatus(status_);
  UpdatePlaybackOverlayProgress("Resumed");
  ScheduleContinuationWithHumanizedDelay(
      base::BindOnce(&ScriptExecutor::ExecuteNextAction,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ScriptExecutor::StopExecution() {
  if (!status_.is_running) {
    return;
  }
  status_.is_running = false;
  UpdatePlaybackOverlayProgress("Stopped");
  FinishRun(/*success=*/false, current_step_, "Stopped by user");
}

ScriptExecutor::ExecutionStatus ScriptExecutor::GetStatus() const {
  return status_;
}

void ScriptExecutor::SetStatusCallback(StatusCallback callback) {
  status_callback_ = std::move(callback);
}
