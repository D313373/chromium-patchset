//===----------------------------------------------------------------------===//
//  dashboard_ui_message_handler.cc
//===----------------------------------------------------------------------===//

#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "chrome/browser/ui/webui/dashboard/scrape_data_store.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <vector>
#include "build/build_config.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/auto_reset.h"
#include "base/strings/string_number_conversions.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/memory/raw_ptr.h"
#include "base/containers/flat_map.h"
#include "base/containers/flat_set.h"
#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/important_file_writer.h"
#include "base/files/file_util.h"
#include "base/i18n/time_formatting.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/json/string_escape.h"
#include "base/base64.h"
#include "base/files/scoped_file.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#if BUILDFLAG(IS_POSIX)
#include <unistd.h>
#include "base/posix/eintr_wrapper.h"
#endif
#include "chrome/common/pref_names.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/dashboard/debug_config.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"
#include "chrome/browser/ui/webui/dashboard/headless_investigation_service.h"
#include "chrome/browser/ui/webui/dashboard/recorder_controller.h"
#include "chrome/browser/ui/webui/dashboard/anti_detection_controller.h"
#include "chrome/browser/ui/webui/dashboard/chrome_profile_importer.h"
#include "chrome/browser/ui/webui/dashboard/script_context_manager.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/run_history_store.h"
#include "chrome/browser/ui/webui/dashboard/runtime_builtins.h"
#include "chrome/browser/ui/webui/dashboard/script_schema.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "chrome/browser/ui/webui/dashboard/headless_listener_controller.h"
#include "chrome/browser/ui/webui/dashboard/listener_manager.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/common/url_constants.h"
#include "content/public/browser/reload_type.h"
#include "components/prefs/pref_service.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/safe_browsing/buildflags.h"
#if BUILDFLAG(SAFE_BROWSING_AVAILABLE)
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"
#endif
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "components/sessions/content/session_tab_helper.h"
#include "net/http/http_status_code.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_request_headers.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "components/os_crypt/sync/os_crypt.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/codec/png_codec.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace {

// ----------------------------------------------------------------------
// Message names (used by the WebUI JS side)
// ----------------------------------------------------------------------
constexpr char kMsgInitialize[]               = "dashboardInitialize";
constexpr char kMsgGetScripts[]               = "dashboardGetScripts";
constexpr char kMsgCreateNew[]                = "dashboardCreateNewScript";
constexpr char kMsgSaveScript[]               = "dashboardSaveScript";
constexpr char kMsgDeleteScript[]             = "dashboardDeleteScript";
constexpr char kMsgPlayScript[]               = "dashboardPlayScript";
constexpr char kMsgStopAllScripts[]           = "dashboardStopAllScripts";
constexpr char kMsgResumeScriptFromStep[]     = "dashboardResumeScriptFromStep";
constexpr char kMsgSetDebugLogging[]          = "dashboardSetDebugLogging";
constexpr char kMsgGetDebugLogging[]          = "dashboardGetDebugLogging";
constexpr char kMsgGetRunHistory[]            = "dashboardGetRunHistory";
constexpr char kMsgGetRunDetails[]            = "dashboardGetRunDetails";

bool HasNonVisualCaptureLocators(const base::Value::Dict& capture) {
  const base::Value::List* locators = capture.FindList("locators");
  if (!locators) {
    return false;
  }
  for (const auto& entry : *locators) {
    const base::Value::Dict* locator = entry.GetIfDict();
    if (!locator) {
      continue;
    }
    const std::string type =
        locator->FindString("type")
            ? base::ToUpperASCII(*locator->FindString("type"))
            : std::string();
    const std::string value =
        locator->FindString("value") ? *locator->FindString("value")
                                     : std::string();
    const std::string lowered = base::ToLowerASCII(value);
    const bool root_like =
        ((type == "CSS_SELECTOR" || type == "TAG_NAME") &&
         (lowered == "html" || lowered == "body")) ||
        (type == "XPATH" &&
         (lowered == "//html" || lowered == "//body" ||
          lowered == "/html" || lowered == "/html/body")) ||
        (type == "JS_EVAL" &&
         (lowered == "document.queryselector(\"html\")" ||
          lowered == "document.queryselector('html')" ||
          lowered == "document.queryselector(\"body\")" ||
          lowered == "document.queryselector('body')"));
    if (type != "VISUAL_COORDS" && !value.empty() && !root_like) {
      return true;
    }
  }
  return false;
}
constexpr char kMsgGetListenerEvents[]        = "dashboardGetListenerEvents";
constexpr char kMsgGetEffectiveSettings[]     = "dashboardGetEffectiveSettings";
constexpr char kMsgSetGlobalSettings[]        = "dashboardSetGlobalSettings";
constexpr char kMsgImportChromeProfile[]      = "dashboardImportChromeProfile";
constexpr char kMsgStartListener[]            = "dashboardStartListener";
constexpr char kMsgStopListener[]             = "dashboardStopListener";
constexpr char kMsgStopHeadlessTask[]         = "dashboardStopHeadlessTask";
constexpr char kMsgRefreshHeadlessTab[]       = "dashboardRefreshHeadlessTab";
constexpr char kMsgGetActiveListeners[]       = "dashboardGetActiveListeners";
constexpr char kMsgRequestHeadlessScreenshot[] =
    "dashboardRequestHeadlessScreenshot";
constexpr char kMsgGetDataVariables[]         = "dashboardGetDataVariables";
constexpr char kMsgSaveDataVariables[]        = "dashboardSaveDataVariables";
constexpr char kMsgGetDataSecrets[]           = "dashboardGetDataSecrets";
constexpr char kMsgSaveDataSecrets[]          = "dashboardSaveDataSecrets";
constexpr char kMsgGetScrapeParentDir[]       = "dashboardGetScrapeParentDir";
constexpr char kMsgSetScrapeParentDir[]       = "dashboardSetScrapeParentDir";
constexpr char kMsgListScrapes[]              = "dashboardListScrapes";
constexpr char kMsgReadScrapeCsv[]            = "dashboardReadScrapeCsv";
constexpr char kMsgDeleteScrapeCsv[]          = "dashboardDeleteScrapeCsv";
constexpr char kMsgArmLocatorCapture[]        = "dashboardArmLocatorCapture";
constexpr char kMsgCancelLocatorCapture[]     = "dashboardCancelLocatorCapture";
constexpr char kMsgPollLocatorCapture[]       = "dashboardPollLocatorCapture";
constexpr char kMsgGetTrafficEntries[]        = "dashboardGetTrafficEntries";
constexpr char kMsgGetTrafficEntry[]          = "dashboardGetTrafficEntry";
constexpr char kMsgPromoteTrafficEntry[]      = "dashboardPromoteTrafficEntry";

constexpr char kMsgPerformAgentAction[]       = "dashboard.performAgentAction";
constexpr char kMsgAgentGetActivePageInfo[]   = "dashboardAgentGetActivePageInfo";
constexpr char kMsgAgentInvestigatePage[]     = "dashboardAgentInvestigatePage";
constexpr char kMsgAgentExecuteActions[]      = "dashboardAgentExecuteActions";
constexpr char kMsgAgentRunScriptHeadless[]   = "dashboardAgentRunScriptHeadless";
constexpr char kMsgAgentResumeHeadless[]      = "dashboardAgentResumeFromStepHeadless";

void ApplyInputBehaviorSettings(const base::Value::Dict& settings) {
  const base::Value::Dict* input = settings.FindDict("inputBehavior");
  if (!input) {
    dashboard::SetClickPathJitterPx(1.5);
    dashboard::SetClickMicroHoverRadiusPx(2.0);
    dashboard::SetClickMicroHoverMoves(3);
    dashboard::SetPlaybackOverlayEnabled(true);
    dashboard::SetLoopPreviewEnabled(false);
    return;
  }

  const double jitter = input->FindDouble("clickPathJitterPx").value_or(1.5);
  const double hover_radius =
      input->FindDouble("clickHoverRadiusPx").value_or(2.0);
  const int hover_moves = input->FindInt("clickHoverMoves").value_or(3);
  const bool overlay_enabled =
      input->FindBool("showPlaybackOverlay").value_or(true);
  const bool loop_preview_enabled =
      input->FindBool("loopPreviewEnabled").value_or(false);

  dashboard::SetClickPathJitterPx(std::clamp(jitter, 0.0, 12.0));
  dashboard::SetClickMicroHoverRadiusPx(std::clamp(hover_radius, 0.0, 24.0));
  dashboard::SetClickMicroHoverMoves(std::clamp(hover_moves, 0, 10));
  dashboard::SetPlaybackOverlayEnabled(overlay_enabled);
  dashboard::SetLoopPreviewEnabled(loop_preview_enabled);
}

const base::Value::Dict* ReadSubsystemSettings(
    const base::Value::Dict& settings) {
  return settings.FindDict("subsystem");
}

const base::Value::Dict* ReadPerformanceSettings(
    const base::Value::Dict& settings) {
  return settings.FindDict("performance");
}

bool ReadBoolSetting(const base::Value::Dict* settings,
                     std::string_view key,
                     bool default_value) {
  if (!settings)
    return default_value;
  return settings->FindBool(key).value_or(default_value);
}

void ApplySubsystemRuntimeSettings(Profile* profile,
                                   const base::Value::Dict& settings) {
  if (!profile)
    return;
  PrefService* prefs = profile->GetPrefs();
  if (!prefs)
    return;

  const base::Value::Dict* subsystem = ReadSubsystemSettings(settings);
  const bool pdf_viewer_enabled =
      ReadBoolSetting(subsystem, "pdfViewerEnabled", true);
  const bool print_preview_enabled =
      ReadBoolSetting(subsystem, "printPreviewEnabled", true);
  const bool safe_browsing_enabled =
      ReadBoolSetting(subsystem, "safeBrowsingEnabled", true);
  const bool geolocation_allowed_by_default =
      ReadBoolSetting(subsystem, "geolocationAllowedByDefault", true);

  prefs->SetBoolean(prefs::kPluginsAlwaysOpenPdfExternally,
                    !pdf_viewer_enabled);
  prefs->SetBoolean(prefs::kPrintPreviewDisabled, !print_preview_enabled);
  if (HostContentSettingsMap* settings_map =
          HostContentSettingsMapFactory::GetForProfile(profile)) {
    // Site-level default: allow location prompts to auto-accept for automation.
    // This does NOT grant macOS Location Services access — that is separate.
    settings_map->SetDefaultContentSetting(
        ContentSettingsType::GEOLOCATION,
        geolocation_allowed_by_default ? CONTENT_SETTING_ALLOW
                                       : CONTENT_SETTING_ASK);
  }

#if BUILDFLAG(SAFE_BROWSING_AVAILABLE)
  if (!safe_browsing_enabled) {
    safe_browsing::SetSafeBrowsingState(
        prefs, safe_browsing::SafeBrowsingState::NO_SAFE_BROWSING);
  } else if (safe_browsing::GetSafeBrowsingState(*prefs) ==
             safe_browsing::SafeBrowsingState::NO_SAFE_BROWSING) {
    safe_browsing::SetSafeBrowsingState(
        prefs, safe_browsing::SafeBrowsingState::STANDARD_PROTECTION);
  }
#endif
}

std::vector<std::string> BuildRestartRequiredLaunchSwitches(
    const base::Value::Dict& settings) {
  std::vector<std::string> switches;
  std::vector<std::string> disabled_features;

  const base::Value::Dict* performance = ReadPerformanceSettings(settings);
  const base::Value::Dict* subsystem = ReadSubsystemSettings(settings);

  const bool aggressive_performance =
      ReadBoolSetting(performance, "aggressivePerformance", false);
  const bool deterministic_mode =
      ReadBoolSetting(performance, "deterministicMode", false);
  const bool extensions_enabled =
      ReadBoolSetting(subsystem, "extensionsEnabled", true);
  const bool print_preview_enabled =
      ReadBoolSetting(subsystem, "printPreviewEnabled", true);
  const bool print_stub_enabled =
      ReadBoolSetting(subsystem, "printStubEnabled", false);
  const bool safe_browsing_enabled =
      ReadBoolSetting(subsystem, "safeBrowsingEnabled", true);
  const bool service_discovery_enabled =
      ReadBoolSetting(subsystem, "serviceDiscoveryEnabled", true);

  if (aggressive_performance) {
    switches.emplace_back("--disable-site-isolation-trials");
    switches.emplace_back("--process-per-site");
    switches.emplace_back("--renderer-process-limit=4");
  }

  if (deterministic_mode) {
    disabled_features.emplace_back("BackForwardCache");
    disabled_features.emplace_back("PrerenderFallbackToPreconnect");
    disabled_features.emplace_back("InstantUsesSpareRenderer");
  }

  if (!extensions_enabled) {
    switches.emplace_back("--disable-extensions");
  }

  if (!print_preview_enabled) {
    switches.emplace_back("--disable-print-preview");
  }

  if (print_stub_enabled) {
    switches.emplace_back("--simulachrome-print-stub");
  }

  if (!safe_browsing_enabled) {
    switches.emplace_back("--safebrowsing-disable-auto-update");
    switches.emplace_back("--disable-client-side-phishing-detection");
  }

  if (!service_discovery_enabled) {
    disabled_features.emplace_back("MediaRouter");
  }

  if (!disabled_features.empty()) {
    switches.emplace_back("--disable-features=" +
                          base::JoinString(disabled_features, ","));
  }

  return switches;
}

base::Value::List FlattenTimelineForListenerScript(const base::Value::Dict& script) {
  base::Value::List flat;
  bool appended_from_actions = false;
  if (const base::Value::List* acts = script.FindList("actions")) {
    for (const auto& a : *acts) {
      flat.Append(a.Clone());
      appended_from_actions = true;
    }
  }
  if (!appended_from_actions) {
    if (const base::Value::List* ctxs = script.FindList("url_contexts")) {
      for (const auto& c : *ctxs) {
        if (!c.is_dict())
          continue;
        if (const base::Value::List* acts = c.GetDict().FindList("actions")) {
          for (const auto& a : *acts) {
            flat.Append(a.Clone());
          }
        }
      }
    }
  }
  return flat;
}

[[maybe_unused]] std::optional<GURL> ExtractScriptStartUrl(
    const base::Value::Dict& script) {
  if (const std::string* explicit_start = script.FindString("startUrl")) {
    if (!explicit_start->empty()) {
      GURL gurl(*explicit_start);
      if (gurl.is_valid())
        return gurl;
    }
  }

  // Fallback: legacy context URL hints for older scripts. We intentionally do
  // not preload tabs to the first navigate target, because NavigateAction owns
  // that transition and may route through the directory page first.
  if (const base::Value::List* contexts = script.FindList("url_contexts")) {
    for (const auto& ctx : *contexts) {
      const base::Value::Dict* d = ctx.GetIfDict();
      if (!d)
        continue;
      const std::string* url = d->FindString("url");
      if (!url || url->empty())
        continue;
      GURL gurl(*url);
      if (gurl.is_valid())
        return gurl;
    }
  }
  return std::nullopt;
}

// ----------------------------------------------------------------------
// Helper to return the path where scripts are stored.
// ----------------------------------------------------------------------
base::FilePath ScriptsDir(const Profile* profile) {
  // Allow overriding via env var for local testing.
  const char* env = std::getenv("DASHBOARD_SCRIPTS_DIR");
  if (env && *env) {
    base::FilePath custom = base::FilePath::FromUTF8Unsafe(env);
    if (!custom.empty())
      return custom;
  }
  if (const char* home = std::getenv("SIMULACHROME_HOME"); home && *home) {
    return base::FilePath::FromUTF8Unsafe(home).AppendASCII("Scripts");
  }
  base::FilePath app_data_dir;
  if (base::PathService::Get(base::DIR_APP_DATA, &app_data_dir) &&
      !app_data_dir.empty()) {
    return app_data_dir.AppendASCII("Simulachrome").AppendASCII("Scripts");
  }
  return profile->GetPath().AppendASCII("SimulachromeScripts");
}

// ----------------------------------------------------------------------
// Data-tab helpers (variables.ini + secrets.ini).
// ----------------------------------------------------------------------
base::FilePath DataDir(const Profile* profile) {
  return profile->GetPath().AppendASCII("SimulachromeData");
}

base::FilePath VariablesIniPath(const Profile* profile) {
  return DataDir(profile).AppendASCII("variables.ini");
}

base::FilePath SecretsIniPath(const Profile* profile) {
  return DataDir(profile).AppendASCII("secrets.ini");
}

std::string NormalizeDataKey(std::string key) {
  base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  if (key.size() >= 2 && key.front() == '{' && key.back() == '}') {
    key = key.substr(1, key.size() - 2);
    base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
  }
  return key;
}

base::Value::Dict DefaultVariables() {
  base::Value::Dict d;
  d.Set("FirstName", "Ada");
  d.Set("LastName", "Lovelace");
  d.Set("Phone", "555-0100");
  d.Set("Email", "ada@example.com");
  return d;
}

base::Value::Dict ReadKeyValueIniBlocking(const base::FilePath& path) {
  base::Value::Dict out;
  if (!base::PathExists(path))
    return out;
  std::string content;
  if (!base::ReadFileToString(path, &content)) {
    PLOG(ERROR) << "[Dashboard] Failed to read ini: " << path.AsUTF8Unsafe();
    return out;
  }

  for (const auto& raw :
       base::SplitStringPiece(content, "\n", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_ALL)) {
    std::string line(raw);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::string trimmed = line;
    base::TrimWhitespaceASCII(trimmed, base::TRIM_ALL, &trimmed);
    if (trimmed.empty())
      continue;
    if (trimmed[0] == '#' || trimmed[0] == ';')
      continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = NormalizeDataKey(line.substr(0, eq));
    std::string value = line.substr(eq + 1);
    if (key.empty())
      continue;
    out.Set(key, value);
  }
  return out;
}

bool WriteKeyValueIniBlocking(const base::FilePath& path,
                              const base::Value::Dict& dict) {
  if (!base::CreateDirectory(path.DirName())) {
    PLOG(ERROR) << "[Dashboard] Failed to create dir: "
                << path.DirName().AsUTF8Unsafe();
    return false;
  }

  std::vector<std::string> keys;
  keys.reserve(dict.size());
  for (auto it : dict) {
    keys.push_back(it.first);
  }
  std::sort(keys.begin(), keys.end());

  std::string out;
  for (const auto& k : keys) {
    const std::string* v = dict.FindString(k);
    std::string value = v ? *v : std::string();
    base::ReplaceChars(value, "\n", " ", &value);
    base::ReplaceChars(value, "\r", " ", &value);
    out.append(k);
    out.push_back('=');
    out.append(value);
    out.push_back('\n');
  }
  if (!base::ImportantFileWriter::WriteFileAtomically(path, out)) {
    LOG(ERROR) << "[Dashboard] Failed to write ini: " << path.AsUTF8Unsafe();
    return false;
  }
  return true;
}

bool EncryptSecretToIniValue(const std::string& plaintext,
                             std::string* out_ini_value) {
  if (!out_ini_value)
    return false;
  std::string ciphertext;
  if (!OSCrypt::EncryptString(plaintext, &ciphertext))
    return false;
  *out_ini_value = base::Base64Encode(ciphertext);
  return true;
}

// ----------------------------------------------------------------------
// Ensure the script directory exists.
// ----------------------------------------------------------------------
void EnsureScriptsDir(const Profile* profile) {
  const base::FilePath dir = ScriptsDir(profile);
  bool ok = base::CreateDirectory(dir);
  LOG(INFO) << "[Dashboard] EnsureScriptsDir path=" << dir.AsUTF8Unsafe()
            << " created?=" << ok;
}

// ----------------------------------------------------------------------
// Write a script to disk (atomic write, JSON format).
// ----------------------------------------------------------------------
bool WriteScriptToDisk(const Profile* profile,
                       const std::string& id,
                       const base::Value::Dict& script) {
  if (!dashboard::ScriptStore::IsValidScriptId(id)) {
    LOG(ERROR) << "[Dashboard] WriteScriptToDisk rejected invalid id=" << id;
    return false;
  }
  // ScriptStore::Write needs a non-const Profile* but only uses path helpers.
  Profile* mutable_profile = const_cast<Profile*>(profile);
  const base::FilePath path =
      dashboard::ScriptStore::PathForScriptId(mutable_profile, id);
  if (path.empty()) {
    return false;
  }
  if (!base::CreateDirectory(path.DirName())) {
    PLOG(ERROR) << "[Dashboard] WriteScriptToDisk mkdir failed "
                << path.DirName().AsUTF8Unsafe();
    return false;
  }
  std::string json;
  base::JSONWriter::Write(script, &json);
  if (!base::ImportantFileWriter::WriteFileAtomically(path, json)) {
    LOG(ERROR) << "[Dashboard] WriteScriptToDisk FAILED id=" << id
               << " path=" << path.AsUTF8Unsafe();
    return false;
  }
  LOG(INFO) << "[Dashboard] WriteScriptToDisk OK id=" << id
            << " path=" << path.AsUTF8Unsafe();
  return true;
}

// ----------------------------------------------------------------------
// Delete a script from disk.
// ----------------------------------------------------------------------
bool DeleteScriptFromDisk(const Profile* profile, const std::string& id) {
  if (!dashboard::ScriptStore::IsValidScriptId(id)) {
    LOG(ERROR) << "[Dashboard] DeleteScriptFromDisk rejected invalid id=" << id;
    return false;
  }
  Profile* mutable_profile = const_cast<Profile*>(profile);
  const base::FilePath path =
      dashboard::ScriptStore::PathForScriptId(mutable_profile, id);
  LOG(INFO) << "[Dashboard] DeleteScriptFromDisk path=" << path.AsUTF8Unsafe();
  if (path.empty() || !base::PathExists(path)) {
    return true;
  }
  return base::DeleteFile(path);
}

base::Value::Dict LoadGlobalSettingsFromDiskBlocking(base::FilePath scripts_dir) {
  const base::FilePath path = scripts_dir.AppendASCII("global_settings.json");
  std::string json;
  if (!base::PathExists(path)) {
    LOG(INFO) << "[Dashboard] Settings file missing, using defaults.";
    return dashboard::DefaultGlobalSettings();
  }
  if (!base::ReadFileToString(path, &json)) {
    LOG(ERROR) << "[Dashboard] Failed to read settings file.";
    return dashboard::DefaultGlobalSettings();
  }
  std::optional<base::Value> val = base::JSONReader::Read(
      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!val || !val->is_dict()) {
    LOG(ERROR) << "[Dashboard] Invalid settings JSON.";
    return dashboard::DefaultGlobalSettings();
  }
  return val->GetDict().Clone();
}

bool SaveGlobalSettingsToDiskBlocking(base::FilePath scripts_dir,
                                     base::Value::Dict settings) {
  if (!base::CreateDirectory(scripts_dir)) {
    PLOG(ERROR) << "[Dashboard] Failed to create scripts dir: "
                << scripts_dir.AsUTF8Unsafe();
    return false;
  }
  const base::FilePath path = scripts_dir.AppendASCII("global_settings.json");
  std::string json;
  if (!base::JSONWriter::Write(settings, &json) || json.empty() ||
      json == "null") {
    LOG(ERROR) << "[Dashboard] Failed to serialize global settings.";
    return false;
  }
  if (!base::ImportantFileWriter::WriteFileAtomically(path, json)) {
    LOG(ERROR) << "[Dashboard] Write global settings FAILED.";
    return false;
  }
  LOG(INFO) << "[Dashboard] Global settings saved to " << path.AsUTF8Unsafe()
            << " (" << json.size() << " bytes).";
  return true;
}

const base::Value::List* FindActionChildList(const base::Value::Dict& action,
                                             const char* key) {
  if (const base::Value::List* list = action.FindList(key)) {
    return list;
  }
  const base::Value::Dict* params = action.FindDict("params");
  return params ? params->FindList(key) : nullptr;
}

bool ValidateActionTreeRecursive(const base::Value::Dict& raw_action) {
  base::Value::Dict action = raw_action.Clone();
  dashboard::ScriptSchema::CanonicalizeAction(action);
  std::string error;
  if (!dashboard::ScriptSchema::ValidateAction(action, &error)) {
    LOG(WARNING) << "[Dashboard] Invalid welcome_demo action: " << error;
    return false;
  }

  const std::string* kind = action.FindString("action");
  if (!kind) {
    return false;
  }

  if (base::EqualsCaseInsensitiveASCII(*kind, "loop")) {
    if (const base::Value::List* steps = FindActionChildList(action, "steps")) {
      for (const auto& step : *steps) {
        const base::Value::Dict* child = step.GetIfDict();
        if (!child || !ValidateActionTreeRecursive(*child)) {
          return false;
        }
      }
    }
  } else if (base::EqualsCaseInsensitiveASCII(*kind, "if_condition")) {
    for (const char* branch : {"then", "else"}) {
      if (const base::Value::List* actions = FindActionChildList(action, branch)) {
        for (const auto& entry : *actions) {
          const base::Value::Dict* child = entry.GetIfDict();
          if (!child || !ValidateActionTreeRecursive(*child)) {
            return false;
          }
        }
      }
    }
  } else if (const base::Value::List* nested =
                 FindActionChildList(action, "nested")) {
    for (const auto& entry : *nested) {
      const base::Value::Dict* child = entry.GetIfDict();
      if (!child || !ValidateActionTreeRecursive(*child)) {
        return false;
      }
    }
  }

  return true;
}

bool ScriptHasInvalidActions(const base::Value::Dict& script) {
  auto validate_list = [](const base::Value::List* actions) {
    if (!actions) {
      return false;
    }
    for (const auto& action_value : *actions) {
      const base::Value::Dict* action = action_value.GetIfDict();
      if (!action || !ValidateActionTreeRecursive(*action)) {
        return true;
      }
    }
    return false;
  };

  if (validate_list(script.FindList("actions"))) {
    return true;
  }
  if (const base::Value::List* contexts = script.FindList("url_contexts")) {
    for (const auto& context_value : *contexts) {
      const base::Value::Dict* context = context_value.GetIfDict();
      if (!context || validate_list(context->FindList("actions"))) {
        return true;
      }
    }
  }
  return false;
}

// ----------------------------------------------------------------------
// Build the built-in default “welcome demo” script.
// ----------------------------------------------------------------------
base::Value::Dict BuildDefaultScript() {
  base::Value::Dict script;
  script.Set("id", "welcome_demo");
  script.Set("name", "Welcome to Simulachrome");
  script.Set("kind", "main");
  script.Set("description",
             "Canonical schema demo. This script includes examples for the "
             "current executable action types and inline notes.");

  base::Value::Dict settings;
  settings.Set("demoVersion", 6);
  settings.Set("startUrl", "https://the-internet.herokuapp.com/login");
  settings.Set("timeoutMs", 30000);
  settings.Set("headless", false);
  settings.Set("userAgent", "Default");
  script.Set("settings", std::move(settings));

  auto make_locator = [](const char* type, const char* value) {
    base::Value::Dict loc;
    loc.Set("type", type);
    loc.Set("value", value);
    return loc;
  };

  auto make_action = [](const char* action_name, base::Value::Dict params,
                        const char* comment) {
    base::Value::Dict action;
    action.Set("action", action_name);
    action.Set("params", std::move(params));
    action.Set("comment", comment);
    return action;
  };

  base::Value::List context_actions;

  {
    base::Value::Dict p;
    p.Set("url", "https://the-internet.herokuapp.com/login");
    context_actions.Append(make_action(
        "navigate", std::move(p),
        "NAVIGATE: Open a deterministic page before interacting."));
  }
  {
    base::Value::Dict p;
    p.Set("waitType", "time");
    p.Set("time", 1000);
    context_actions.Append(make_action(
        "wait", std::move(p),
        "WAIT(time): Let navigation settle before locators resolve."));
  }
  {
    base::Value::Dict p;
    base::Value::List locs;
    locs.Append(make_locator("ID", "username"));
    p.Set("locators", std::move(locs));
    p.Set("text", "tomsmith");
    context_actions.Append(make_action(
        "type", std::move(p),
        "TYPE: Fill username using a stable ID locator."));
  }
  {
    base::Value::Dict p;
    p.Set("key", "Tab");
    p.Set("count", 1);
    context_actions.Append(make_action(
        "key_press", std::move(p),
        "KEY_PRESS: Move focus to the next field."));
  }
  {
    base::Value::Dict p;
    base::Value::List locs;
    locs.Append(make_locator("ID", "password"));
    p.Set("locators", std::move(locs));
    p.Set("text", "SuperSecretPassword!");
    context_actions.Append(make_action(
        "type", std::move(p),
        "TYPE: Fill password (demo credentials from the-internet site)."));
  }
  {
    base::Value::Dict p;
    p.Set("key", "Enter");
    context_actions.Append(make_action(
        "key_press", std::move(p),
        "KEY_PRESS: Submit current form with Enter."));
  }
  {
    base::Value::Dict p;
    base::Value::List locs;
    locs.Append(make_locator("CSS_SELECTOR", ".flash.success"));
    p.Set("conditionType", "element_exists");
    p.Set("locators", std::move(locs));
    base::Value::List then_actions;
    {
      base::Value::Dict wp;
      wp.Set("waitType", "time");
      wp.Set("time", 250);
      then_actions.Append(make_action("wait", std::move(wp),
                                      "IF then: success branch example."));
    }
    base::Value::List else_actions;
    {
      base::Value::Dict wp;
      wp.Set("waitType", "time");
      wp.Set("time", 250);
      else_actions.Append(make_action("wait", std::move(wp),
                                      "IF else: fallback branch example."));
    }
    p.Set("then", std::move(then_actions));
    p.Set("else", std::move(else_actions));
    context_actions.Append(make_action(
        "if_condition", std::move(p),
        "IF_CONDITION: Branch based on element existence."));
  }
  {
    base::Value::Dict p;
    base::Value::List locs;
    locs.Append(make_locator("CSS_SELECTOR", "h2"));
    p.Set("locators", std::move(locs));
    p.Set("variableName", "auth_heading");
    p.Set("dataType", "text");
    context_actions.Append(make_action(
        "scrape_element", std::move(p),
        "SCRAPE_ELEMENT: Capture the heading text into a variable."));
  }
  {
    base::Value::Dict p;
    p.Set("percentY", 50.0);
    p.Set("percentX", 0.0);
    p.Set("element", "window");
    context_actions.Append(make_action(
        "scroll", std::move(p),
        "SCROLL: Move the document viewport to a percentage."));
  }
  {
    base::Value::Dict p;
    p.Set("loopMode", "count");
    p.Set("iterations", 2);
    p.Set("maxIterations", 2);
    base::Value::List steps;
    {
      base::Value::Dict wp;
      wp.Set("waitType", "time");
      wp.Set("time", 150);
      steps.Append(make_action("wait", std::move(wp),
                               "LOOP step: lightweight nested action."));
    }
    p.Set("steps", std::move(steps));
    context_actions.Append(make_action(
        "loop", std::move(p),
        "LOOP: Repeat nested `steps` with explicit count/condition."));
  }
  {
    base::Value::Dict p;
    context_actions.Append(make_action(
        "back", std::move(p),
        "BACK: Navigate browser history backward one entry."));
  }
  {
    base::Value::Dict p;
    context_actions.Append(make_action(
        "forward", std::move(p),
        "FORWARD: Navigate browser history forward one entry."));
  }
  {
    base::Value::Dict p;
    p.Set("url", "https://the-internet.herokuapp.com/tables");
    context_actions.Append(make_action(
        "navigate", std::move(p),
        "NAVIGATE: Move to the table sample page for scraping examples."));
  }
  {
    base::Value::Dict p;
    p.Set("waitType", "time");
    p.Set("time", 800);
    context_actions.Append(make_action(
        "wait", std::move(p),
        "WAIT(time): brief pause before scrape/click actions."));
  }
  {
    base::Value::Dict p;
    base::Value::List row_locs;
    row_locs.Append(make_locator("CSS_SELECTOR", "#table1 tbody tr"));
    p.Set("rowLocators", std::move(row_locs));
    base::Value::List fields;
    {
      base::Value::Dict f;
      f.Set("name", "last_name");
      base::Value::List locs;
      locs.Append(make_locator("CSS_SELECTOR", "td:nth-child(1)"));
      f.Set("locators", std::move(locs));
      fields.Append(std::move(f));
    }
    {
      base::Value::Dict f;
      f.Set("name", "email");
      base::Value::List locs;
      locs.Append(make_locator("CSS_SELECTOR", "td:nth-child(3)"));
      f.Set("locators", std::move(locs));
      fields.Append(std::move(f));
    }
    p.Set("fields", std::move(fields));
    p.Set("variableName", "table_rows");
    p.Set("rowSelectionMode", "all");
    p.Set("rowIndex", 1);
    p.Set("saveCsv", true);
    p.Set("appendCsv", false);
    p.Set("csvFileName", "table_rows.csv");
    context_actions.Append(make_action(
        "scrape_table", std::move(p),
        "SCRAPE_TABLE: Extract row data using row-relative column locators."));
  }
  {
    // Same-tab click only. Do not click the site footer "Elemental Selenium"
    // link — it uses target=_blank and opens elementalselenium.com in a new
    // tab under trusted/native click injection.
    base::Value::Dict p;
    base::Value::List locs;
    locs.Append(make_locator("LINK_TEXT", "edit"));
    p.Set("locators", std::move(locs));
    p.Set("clickCount", 1);
    context_actions.Append(make_action(
        "click", std::move(p),
        "CLICK: Same-tab link text (tables page edit link uses href=#edit)."));
  }
  {
    base::Value::Dict p;
    base::Value::List locs;
    locs.Append(make_locator("TAG_NAME", "body"));
    p.Set("locators", std::move(locs));
    p.Set("condition", "visible");
    p.Set("timeout_ms", 1000);
    // In a normal Play this is one-shot (wait → optional onMatch once → done).
    // Persistent rearm only happens when kind=listener is started as a listener.
    // Leave onMatch empty in the stock demo; users can set it for one-shot runs.
    p.Set("onMatchScriptId", "");
    p.Set("autoRefresh", false);
    p.Set("executionFrequencyMs", 60000);
    p.Set("sampleIntervalMs", 5000);
    p.Set("triggerThrottleMs", 5000);
    p.Set("refreshJitter", true);
    p.Set("extremeEvasion", false);
    context_actions.Append(make_action(
        "listen", std::move(p),
        "LISTEN: Example listener config shape (onMatch applies only when "
        "this script is started as a listener, not during a normal Play)."));
  }

  base::Value::Dict context;
  context.Set("url", "https://the-internet.herokuapp.com/login");
  context.Set("actions", std::move(context_actions));
  base::Value::List contexts;
  contexts.Append(std::move(context));
  script.Set("url_contexts", std::move(contexts));
  script.Set("actions", base::Value::List());

  script = dashboard::ScriptSchema::CanonicalizeForSave(
      std::move(script), "welcome_demo", base::Time::Now());
  return script;
}

// ----------------------------------------------------------------------
// Ensure the default demo script exists (writes it only if missing).
// ----------------------------------------------------------------------
void EnsureDefaultScript(const Profile* profile) {
  const base::FilePath path = ScriptsDir(profile).AppendASCII("welcome_demo.json");
  if (base::PathExists(path)) {
    std::string contents;
    if (base::ReadFileToString(path, &contents)) {
      std::optional<base::Value> val = base::JSONReader::Read(
          contents, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
      if (val && val->is_dict()) {
        base::Value::Dict existing = val->GetDict().Clone();
        const std::string* name = existing.FindString("name");
        const base::Value::Dict* settings = existing.FindDict("settings");
        const std::optional<int> demo_version =
            settings ? settings->FindInt("demoVersion") : std::nullopt;
        const bool needs_upgrade =
            (name && *name == "Welcome to Automation Studio") ||
            ScriptHasInvalidActions(existing) ||
            // Only force demoVersion upgrades on the stock untitled name so
            // customized copies (e.g. typo-test) keep user edits.
            (name && *name == "Welcome to Simulachrome" &&
             (!demo_version.has_value() || *demo_version < 6));
        if (needs_upgrade) {
          base::Value::Dict def = BuildDefaultScript();
          if (!WriteScriptToDisk(profile, "welcome_demo", def)) {
            LOG(ERROR) << "[Dashboard] Failed to upgrade default demo script "
                       << path.AsUTF8Unsafe();
          } else {
            LOG(INFO) << "[Dashboard] Upgraded default demo script at "
                      << path.AsUTF8Unsafe();
          }
        }
      }
    }
    LOG(INFO) << "[Dashboard] Default script already present at "
              << path.AsUTF8Unsafe();
    return;
  }
  base::Value::Dict def = BuildDefaultScript();
  if (!WriteScriptToDisk(profile, "welcome_demo", def)) {
    LOG(ERROR) << "[Dashboard] Failed to write default demo script.";
  } else {
    LOG(INFO) << "[Dashboard] Default demo script written to "
              << path.AsUTF8Unsafe();
  }
}

// ----------------------------------------------------------------------
// Load **all** scripts from the on-disk directory.
// ----------------------------------------------------------------------
base::Value::Dict LoadAllScriptsFromDisk(const Profile* profile) {
  EnsureScriptsDir(profile);
  EnsureDefaultScript(profile);
  Profile* mutable_profile = const_cast<Profile*>(profile);
  base::Value::Dict out = dashboard::ScriptStore::LoadAll(mutable_profile);
  LOG(INFO) << "[Dashboard] Loaded " << out.size() << " scripts from disk.";
  return out;
}

// ----------------------------------------------------------------------
// Normalise a script for the UI (flatten action dicts, resolve aliases, …)
// ----------------------------------------------------------------------
base::Value::Dict NormalizeScriptForUI(const base::Value::Dict& raw) {
  base::Value::Dict ui;
  if (const std::string* name = raw.FindString("name"))
    ui.Set("name", *name);
  else
    ui.Set("name", "Untitled");

  if (const std::string* kind = raw.FindString("kind"))
    ui.Set("kind", *kind);

  if (const base::Value* settings = raw.Find("settings"))
    ui.Set("settings", settings->Clone());

  if (const base::Value* ov = raw.Find("settingsOverrides")) {
    ui.Set("settingsOverrides", ov->Clone());
  }

  if (const std::string* ks = raw.FindString("keyScriptId")) {
    ui.Set("keyScriptId", *ks);
  }

  if (const std::string* collection = raw.FindString("collection")) {
    ui.Set("collection", *collection);
  }

  if (const base::Value* actions = raw.Find("actions")) {
    if (actions->is_list()) {
      ui.Set("actions", actions->Clone());
    }
  }

  if (const base::Value* ctxs = raw.Find("url_contexts")) {
    ui.Set("url_contexts", ctxs->Clone());
  }

  return ui;
}

// ----------------------------------------------------------------------
// Normalise the **entire** script map for UI consumption.
// ----------------------------------------------------------------------
base::Value::Dict NormalizeScriptsForUI(const base::Value::Dict& raw) {
  base::Value::Dict out;
  for (const auto [id, val] : raw) {
    if (!val.is_dict())
      continue;
    out.Set(id, NormalizeScriptForUI(val.GetDict()));
  }
  return out;
}

void PopulateDashboardScriptCache(dashboard::Dashboard* dashboard,
                                  const base::Value::Dict& raw) {
  if (!dashboard) {
    return;
  }
  dashboard->ReplaceAllScripts(raw.Clone());
}

bool CanExecuteJavascriptOnFrame(content::RenderFrameHost* frame) {
  if (!frame)
    return false;

  const GURL& url = frame->GetLastCommittedURL();
  if (!url.is_valid() || url.SchemeIs(content::kChromeDevToolsScheme) ||
      url.spec() == url::kAboutBlankURL) {
    return true;
  }

  return content::ChildProcessSecurityPolicy::GetInstance()->HasWebUIBindings(
      frame->GetProcess()->GetDeprecatedID());
}

bool IsTrustedWebUIFrame(content::RenderFrameHost* frame) {
  if (!frame)
    return false;

  const GURL& url = frame->GetLastCommittedURL();
  if (!url.is_valid() || url.SchemeIs(content::kChromeDevToolsScheme) ||
      url.spec() == url::kAboutBlankURL) {
    return true;
  }

  return content::ChildProcessSecurityPolicy::GetInstance()->HasWebUIBindings(
      frame->GetProcess()->GetDeprecatedID());
}

void ExecuteJavascriptIfAllowed(content::RenderFrameHost* frame,
                                const std::string& js) {
  if (!CanExecuteJavascriptOnFrame(frame))
    return;
  // The automation JS kill switch is for web-page injection, not for trusted
  // chrome://dashboard callback plumbing. Keep dashboard WebUI responsive even
  // when page JS execution is disabled.
  if (!IsTrustedWebUIFrame(frame) && !dashboard::IsJavaScriptExecutionEnabled())
    return;
  frame->ExecuteJavaScript(base::UTF8ToUTF16(js), base::NullCallback());
}

// ----------------------------------------------------------------------
// Helper to invoke a WebUI callback (JavaScript) with a JSON payload.
// ----------------------------------------------------------------------
void ResolveCallback(content::WebUI* web_ui,
                       const std::string& cb_fn,
                       const base::Value& payload) {
  if (cb_fn.empty() || !web_ui || !web_ui->CanCallJavascript())
    return;
  content::RenderFrameHost* frame =
      web_ui->GetWebContents()
          ? web_ui->GetWebContents()->GetPrimaryMainFrame()
          : nullptr;
  if (!CanExecuteJavascriptOnFrame(frame))
    return;

  // Callback names are dynamically generated (for example "dashMutation_*") and
  // can time out on the JS side before the browser responds. Calling a missing
  // callback via CallJavascriptFunctionUnsafe triggers a ReferenceError.
  // Dispatch through window[cb] with a type guard so late callbacks are safely
  // ignored.
  std::string cb_js;
  base::EscapeJSONString(cb_fn, /*put_in_quotes=*/true, &cb_js);
  std::string payload_json;
  if (!base::JSONWriter::Write(payload, &payload_json))
    payload_json = "null";
  std::string js = base::StrCat(
      {"try{const __cb=", cb_js,
       ";const __fn=(window&&window[__cb]);if(typeof __fn==='function'){__fn(",
       payload_json, ");}}catch(e){};"});
  ExecuteJavascriptIfAllowed(frame, js);
}

}  // namespace

// ----------------------------------------------------------------------
// Static vector that tracks every alive UI-handler instance.
// ----------------------------------------------------------------------
std::vector<DashboardUIMessageHandler*>& GetInstances() {
  static base::NoDestructor<std::vector<DashboardUIMessageHandler*>> instances;
  return *instances;
}

// ----------------------------------------------------------------------
// DashboardUIMessageHandler implementation
// ----------------------------------------------------------------------
DashboardUIMessageHandler::DashboardUIMessageHandler(Profile* profile,
                                                     dashboard::Dashboard* dashboard)
    : profile_(profile), dashboard_(dashboard), weak_ptr_factory_(this) {
  DashboardUIMessageHandler::Instances().push_back(this);
}

DashboardUIMessageHandler::~DashboardUIMessageHandler() {
  // Drop listener manager before removing from Instances so teardown
  // notifications cannot target this dying handler.
  listener_manager_.reset();
  auto& vec = DashboardUIMessageHandler::Instances();
  auto it = std::find(vec.begin(), vec.end(), this);
  if (it != vec.end())
    vec.erase(it);
}

void DashboardUIMessageHandler::DetachDashboard() {
  dashboard_ = nullptr;
  if (listener_manager_) {
    listener_manager_->DetachDashboard();
  }
}

// ----------------------------------------------------------------------
// HandleToggleRecording
// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleToggleRecording(
    const base::Value::List& args) {
  bool enable = false;
  if (args.size() > 0 && args[0].is_bool())
    enable = args[0].GetBool();

  LOG(INFO) << "[Dashboard] ToggleRecording called: enable=" << enable;
  if (enable) {
      LOG(INFO) << "[Dashboard] Entering user recording mode";
      is_agent_mode_ = false;
  }
  if (auto* rc = RecorderController::GetActive()) {
    if (enable)
      rc->StartPassiveMode(nullptr, /*explicit_recording=*/true);
    else
      rc->StopPassiveMode();
  } else {
    // Lazy-create one if needed
    Browser* browser = chrome::FindLastActive();
    if (!browser) return;
    BrowserView* bv = BrowserView::GetBrowserViewForBrowser(browser);
    auto* new_recorder = new RecorderController();
    new_recorder->StartPassiveMode(bv, /*explicit_recording=*/true);
  }
}

void DashboardUIMessageHandler::HandleGetTrafficEntries(
    const base::Value::List& args) {
  AllowJavascript();
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  size_t limit = 100;
  if (args.size() > 1) {
    if (args[1].is_int()) {
      limit = static_cast<size_t>(std::max(0, args[1].GetInt()));
    } else if (args[1].is_dict()) {
      limit = static_cast<size_t>(
          std::max(0, args[1].GetDict().FindInt("limit").value_or(100)));
    }
  }
  base::Value::Dict payload;
  auto* rc = RecorderController::GetActive();
  if (!rc) {
    payload.Set("ok", true);
    payload.Set("entries", base::Value::List());
    ResolveCallback(cb, payload);
    return;
  }
  payload.Set("ok", true);
  payload.Set("entries", rc->GetTrafficEntries(limit));
  ResolveCallback(cb, payload);
}

void DashboardUIMessageHandler::HandleGetTrafficEntry(
    const base::Value::List& args) {
  AllowJavascript();
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  std::string request_id;
  if (args.size() > 1 && args[1].is_string()) {
    request_id = args[1].GetString();
  } else if (args.size() > 1 && args[1].is_dict()) {
    if (const std::string* id = args[1].GetDict().FindString("requestId")) {
      request_id = *id;
    }
  }
  auto* rc = RecorderController::GetActive();
  if (!rc || request_id.empty()) {
    base::Value::Dict payload;
    payload.Set("ok", false);
    payload.Set("error", "No recorder or requestId");
    ResolveCallback(cb, payload);
    return;
  }
  rc->GetTrafficEntry(
      request_id,
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             std::optional<base::Value::Dict> entry) {
            if (!self) {
              return;
            }
            base::Value::Dict payload;
            if (!entry) {
              payload.Set("ok", false);
              payload.Set("error", "Unknown requestId");
              self->ResolveCallback(cb, payload);
              return;
            }
            payload.Set("ok", true);
            payload.Set("entry", std::move(*entry));
            self->ResolveCallback(cb, payload);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandlePromoteTrafficEntry(
    const base::Value::List& args) {
  AllowJavascript();
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  std::string request_id;
  if (args.size() > 1 && args[1].is_string()) {
    request_id = args[1].GetString();
  } else if (args.size() > 1 && args[1].is_dict()) {
    if (const std::string* id = args[1].GetDict().FindString("requestId")) {
      request_id = *id;
    }
  }
  auto* rc = RecorderController::GetActive();
  if (!rc || request_id.empty()) {
    base::Value::Dict payload;
    payload.Set("ok", false);
    payload.Set("error", "No recorder or requestId");
    ResolveCallback(cb, payload);
    return;
  }

  Profile* profile = profile_;
  rc->GetTrafficEntry(
      request_id,
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             Profile* profile, RecorderController* rc,
             std::optional<base::Value::Dict> entry) {
            if (!self) {
              return;
            }
            if (!entry || !rc) {
              base::Value::Dict payload;
              payload.Set("ok", false);
              payload.Set("error", "Unknown requestId");
              self->ResolveCallback(cb, payload);
              return;
            }

            base::Value::Dict entry_copy = entry->Clone();
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE,
                {base::MayBlock(),
                 base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
                base::BindOnce(
                    [](Profile* profile) -> base::flat_map<std::string, std::string> {
                      base::flat_map<std::string, std::string> out;
                      if (!profile) {
                        return out;
                      }
                      base::Value::Dict secrets =
                          ReadKeyValueIniBlocking(SecretsIniPath(profile));
                      for (auto it : secrets) {
                        if (!it.second.is_string()) {
                          continue;
                        }
                        const std::string& value = it.second.GetString();
                        if (!value.empty()) {
                          out[value] = it.first;
                        }
                      }
                      return out;
                    },
                    profile),
                base::BindOnce(
                    [](base::WeakPtr<DashboardUIMessageHandler> self,
                       std::string cb, RecorderController* rc,
                       base::Value::Dict entry,
                       base::flat_map<std::string, std::string>
                           secret_value_to_token) {
                      if (!self) {
                        return;
                      }
                      base::Value::Dict payload;
                      if (!rc) {
                        payload.Set("ok", false);
                        payload.Set("error", "Recorder gone");
                        self->ResolveCallback(cb, payload);
                        return;
                      }
                      base::Value::Dict action =
                          rc->PromoteTrafficEntryToRequestAction(
                              entry, secret_value_to_token);
                      rc->AppendRecordedAction(action.Clone());
                      payload.Set("ok", true);
                      payload.Set("action", std::move(action));
                      self->ResolveCallback(cb, payload);
                    },
                    self, cb, rc, std::move(entry_copy)));
          },
          weak_ptr_factory_.GetWeakPtr(), cb, profile, rc));
}


// ----------------------------------------------------------------------
// Register all WebUI callbacks.
// ----------------------------------------------------------------------
void DashboardUIMessageHandler::RegisterMessages() {
  LOG(INFO) << "[Dashboard] RegisterMessages";

  // Initialize anti-detection controller for this WebContents
  InitializeAntiDetectionController();

  web_ui()->RegisterMessageCallback(
      kMsgInitialize,
      base::BindRepeating(&DashboardUIMessageHandler::HandleInitialize,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgGetScripts,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetScripts,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgCreateNew,
      base::BindRepeating(&DashboardUIMessageHandler::HandleCreateNewScript,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgSaveScript,
      base::BindRepeating(&DashboardUIMessageHandler::HandleSaveScript,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgDeleteScript,
      base::BindRepeating(&DashboardUIMessageHandler::HandleDeleteScript,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgPlayScript,
      base::BindRepeating(&DashboardUIMessageHandler::HandlePlayScript,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgStopAllScripts,
      base::BindRepeating(&DashboardUIMessageHandler::HandleStopAllScripts,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgResumeScriptFromStep,
      base::BindRepeating(
          &DashboardUIMessageHandler::HandleResumeScriptFromStep,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgSetDebugLogging,
      base::BindRepeating(&DashboardUIMessageHandler::HandleSetDebugLogging,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgGetDebugLogging,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetDebugLogging,
                          base::Unretained(this)));
  // Run history
  web_ui()->RegisterMessageCallback(
      kMsgGetRunHistory,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetRunHistory,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgGetRunDetails,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetRunDetails,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgGetListenerEvents,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetListenerEvents,
                          base::Unretained(this)));
  // Listener management
  web_ui()->RegisterMessageCallback(
      kMsgStartListener,
      base::BindRepeating(&DashboardUIMessageHandler::HandleStartListener,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgStopListener,
      base::BindRepeating(&DashboardUIMessageHandler::HandleStopListener,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgStopHeadlessTask,
      base::BindRepeating(&DashboardUIMessageHandler::HandleStopHeadlessTask,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgRefreshHeadlessTab,
      base::BindRepeating(&DashboardUIMessageHandler::HandleRefreshHeadlessTab,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgGetActiveListeners,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetActiveListeners,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgRequestHeadlessScreenshot,
      base::BindRepeating(
          &DashboardUIMessageHandler::HandleRequestHeadlessScreenshot,
          base::Unretained(this)));
  // Data tab
  web_ui()->RegisterMessageCallback(
      kMsgGetDataVariables,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetDataVariables,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgSaveDataVariables,
      base::BindRepeating(&DashboardUIMessageHandler::HandleSaveDataVariables,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgGetDataSecrets,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetDataSecrets,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgSaveDataSecrets,
      base::BindRepeating(&DashboardUIMessageHandler::HandleSaveDataSecrets,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgGetScrapeParentDir,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetScrapeParentDir,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgSetScrapeParentDir,
      base::BindRepeating(&DashboardUIMessageHandler::HandleSetScrapeParentDir,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgListScrapes,
      base::BindRepeating(&DashboardUIMessageHandler::HandleListScrapes,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgReadScrapeCsv,
      base::BindRepeating(&DashboardUIMessageHandler::HandleReadScrapeCsv,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgDeleteScrapeCsv,
      base::BindRepeating(&DashboardUIMessageHandler::HandleDeleteScrapeCsv,
                          base::Unretained(this)));
  // Settings
  web_ui()->RegisterMessageCallback(
      kMsgGetEffectiveSettings,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetEffectiveSettings,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgSetGlobalSettings,
      base::BindRepeating(&DashboardUIMessageHandler::HandleSetGlobalSettings,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgImportChromeProfile,
      base::BindRepeating(&DashboardUIMessageHandler::HandleImportChromeProfile,
                          base::Unretained(this)));
  // Recorder
  web_ui()->RegisterMessageCallback(
      kMsgArmLocatorCapture,
      base::BindRepeating(&DashboardUIMessageHandler::HandleArmLocatorCapture,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgCancelLocatorCapture,
      base::BindRepeating(&DashboardUIMessageHandler::HandleCancelLocatorCapture,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgPollLocatorCapture,
      base::BindRepeating(&DashboardUIMessageHandler::HandlePollLocatorCapture,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardDebugUserClick",
      base::BindRepeating(&DashboardUIMessageHandler::HandleDebugUserClick,
                          base::Unretained(this)));

  web_ui()->RegisterMessageCallback(
    "toggleRecording",
    base::BindRepeating(&DashboardUIMessageHandler::HandleToggleRecording,
                        base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgGetTrafficEntries,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetTrafficEntries,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgGetTrafficEntry,
      base::BindRepeating(&DashboardUIMessageHandler::HandleGetTrafficEntry,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgPromoteTrafficEntry,
      base::BindRepeating(&DashboardUIMessageHandler::HandlePromoteTrafficEntry,
                          base::Unretained(this)));

  web_ui()->RegisterMessageCallback(
    "dashboardSaveSnapshot",
    base::BindRepeating(&DashboardUIMessageHandler::HandleSaveSnapshot,
                        base::Unretained(this)));

  web_ui()->RegisterMessageCallback(
    kMsgPerformAgentAction,
    base::BindRepeating(&DashboardUIMessageHandler::HandlePerformAgentAction,
                        base::Unretained(this)));
  
  // Agent action aliases used by dashboard_app.js
  web_ui()->RegisterMessageCallback(
      "dashboardAgentOpenTab",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentOpenTab,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentAttachPanel",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentAttachPanel,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentDetachPanel",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentDetachPanel,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentGetPanelState",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentGetPanelState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentCallAction",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentCallAction,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentRunTimeline",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentRunTimeline,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentRefreshPageMap",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentRefreshPageMap,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentReloadPage",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentReloadPage,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentInspectElement",
      base::BindRepeating(
          &DashboardUIMessageHandler::HandleAgentInspectElement,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentScreenshot",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentScreenshot,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgAgentInvestigatePage,
      base::BindRepeating(
          &DashboardUIMessageHandler::HandleAgentInvestigatePage,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgAgentExecuteActions,
      base::BindRepeating(
          &DashboardUIMessageHandler::HandleAgentExecuteActions,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgAgentRunScriptHeadless,
      base::BindRepeating(
          &DashboardUIMessageHandler::HandleAgentRunScriptOnHeadless,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgAgentResumeHeadless,
      base::BindRepeating(
          &DashboardUIMessageHandler::HandleAgentResumeFromStepOnHeadless,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      kMsgAgentGetActivePageInfo,
      base::BindRepeating(
          &DashboardUIMessageHandler::HandleAgentGetActivePageInfo,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentListTabs",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentListTabs,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentActivateTab",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentActivateTab,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentGetPageContext",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentGetPageContext,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentSaveScript",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentSaveScript,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentOllamaChat",
      base::BindRepeating(&DashboardUIMessageHandler::HandleAgentOllamaChat,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "dashboardAgentListOllamaModels",
      base::BindRepeating(
          &DashboardUIMessageHandler::HandleAgentListOllamaModels,
          base::Unretained(this)));
}

// ----------------------------------------------------------------------
// ----------------------------------------------------------------------
//   MESSAGE HANDLERS (called from JavaScript)
// ----------------------------------------------------------------------

// ----------------------------------------------------------------------
// FIX: Re-add the ResolveCallback wrappers to fix build errors.
// These are the member functions that should be called by handlers.
// ----------------------------------------------------------------------
void DashboardUIMessageHandler::ResolveCallback(const std::string& cb_fn,
                                                const base::Value& payload) {
  // During Ctrl-R / WebUI teardown, re-enabling JS on a dying frame crashes.
  // Drop the reply if the page is mid-reload instead of AllowJavascript().
  if (!web_ui() || !web_ui()->GetWebContents() ||
      web_ui()->GetWebContents()->IsBeingDestroyed()) {
    return;
  }
  if (!IsJavascriptAllowed()) {
    return;
  }
  ::ResolveCallback(web_ui(), cb_fn, payload);
}

void DashboardUIMessageHandler::ResolveCallback(const std::string& cb_fn,
                                                const base::Value::Dict& payload) {
  if (!web_ui() || !web_ui()->GetWebContents() ||
      web_ui()->GetWebContents()->IsBeingDestroyed()) {
    return;
  }
  if (!IsJavascriptAllowed()) {
    return;
  }
  ::ResolveCallback(web_ui(), cb_fn, base::Value(payload.Clone()));
}

void DashboardUIMessageHandler::ResolveCallback(const std::string& cb_fn,
                                                const base::Value::List& payload) {
  if (!web_ui() || !web_ui()->GetWebContents() ||
      web_ui()->GetWebContents()->IsBeingDestroyed()) {
    return;
  }
  if (!IsJavascriptAllowed()) {
    return;
  }
  ::ResolveCallback(web_ui(), cb_fn, base::Value(payload.Clone()));
}

// ----------------------------------------------------------------------


void DashboardUIMessageHandler::HandleInitialize(const base::Value::List& args) {
  AllowJavascript();
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(),
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&::LoadAllScriptsFromDisk, base::Unretained(profile_)),
      base::BindOnce(&DashboardUIMessageHandler::OnInitializeDone,
                     weak_ptr_factory_.GetWeakPtr(), cb));
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::OnInitializeDone(std::string cb,
                                                 base::Value::Dict scripts) {
  PopulateDashboardScriptCache(dashboard_, scripts);
  base::Value::Dict ok;
  ok.Set("ok", true);
  ResolveCallback(cb, ok); // FIX: Use member function
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleGetScripts(const base::Value::List& args) {
  AllowJavascript();
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(),
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&::LoadAllScriptsFromDisk, base::Unretained(profile_)),
      base::BindOnce(&DashboardUIMessageHandler::OnGetScriptsLoaded,
                     weak_ptr_factory_.GetWeakPtr(), cb));
}

// ----------------------------------------------------------------------
// FIX: Re-add missing function definition
void DashboardUIMessageHandler::OnGetScriptsLoaded(std::string cb,
                                                   base::Value::Dict raw) {
  PopulateDashboardScriptCache(dashboard_, raw);

  base::Value::Dict payload;
  payload.Set("scripts", ::NormalizeScriptsForUI(raw));
  ResolveCallback(cb, payload); // FIX: Use member function
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleCreateNewScript(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();

  const base::Value::Dict* payload = (args.size() > 1 && args[1].is_dict())
                                         ? &args[1].GetDict()
                                         : nullptr;
  std::string name = "New Script";
  std::string collection;
  if (payload) {
    if (const std::string* n = payload->FindString("name"))
      name = *n;
    if (const std::string* c = payload->FindString("collection"))
      collection = *c;
  }

  std::string id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  if (!collection.empty()) {
    if (!dashboard::ScriptStore::IsValidCollectionName(collection)) {
      base::Value::Dict err;
      err.Set("ok", false);
      err.Set("error", "invalid_collection");
      ResolveCallback(cb, err);
      return;
    }
    id = collection + "/" + id;
  }
  base::Value::Dict script;
  script.Set("id", id);
  script.Set("name", name);
  if (!collection.empty()) {
    script.Set("collection", collection);
  }
  script.Set("settings", base::Value::Dict());
  script.Set("actions", base::Value::List());

  const base::FilePath scripts_dir = ScriptsDir(profile_);
  base::ThreadPool::PostTaskAndReply(
      FROM_HERE,
      {base::MayBlock(),
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](base::FilePath dir, Profile* profile, std::string id,
             base::Value::Dict script) {
            // Avoid touching the DashboardUIMessageHandler instance from a
            // background thread; refresh/reload can destroy the WebUI.
            base::CreateDirectory(dir);
            (void)WriteScriptToDisk(profile, id, script);
          },
          scripts_dir, profile_, id, script.Clone()),
      base::BindOnce(&DashboardUIMessageHandler::OnCreateNewDone,
                     weak_ptr_factory_.GetWeakPtr(), cb, id,
                     std::move(script)));
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::OnCreateNewDone(std::string cb,
                                                std::string id,
                                                base::Value::Dict script) {
  dashboard_->AddScript(id, script.Clone());
  base::Value::Dict out;
  out.Set("scriptId", id);
  ResolveCallback(cb, out); // FIX: Use member function
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleSaveScript(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  const std::string id = (args.size() > 1 && args[1].is_string())
                             ? args[1].GetString()
                             : std::string();
  const base::Value* data_val = (args.size() > 2) ? &args[2] : nullptr;

  if (!data_val || !data_val->is_dict()) {
    LOG(ERROR) << "[Dashboard] HandleSaveScript: missing or malformed data.";
    base::Value::Dict res;
    res.Set("ok", false);
    ResolveCallback(cb, res); // FIX: Use member function
    return;
  }

  const base::Value::Dict& data = data_val->GetDict();
  if (!dashboard::ScriptStore::IsValidScriptId(id)) {
    LOG(ERROR) << "[Dashboard] HandleSaveScript: invalid id=" << id;
    base::Value::Dict res;
    res.Set("ok", false);
    res.Set("error", "invalid_script_id");
    ResolveCallback(cb, res);
    return;
  }
  base::Value::Dict script;
  script.Set("id", id);
  if (const std::string* n = data.FindString("name"))
    script.Set("name", *n);
  else
    script.Set("name", "Untitled");

  const std::string collection =
      dashboard::ScriptStore::CollectionOfScriptId(id);
  if (!collection.empty()) {
    script.Set("collection", collection);
  } else if (const std::string* c = data.FindString("collection")) {
    if (!c->empty())
      script.Set("collection", *c);
  }

  if (const std::string* start_url = data.FindString("startUrl")) {
    const std::string v = *start_url;
    if (!v.empty()) {
      script.Set("startUrl", v);
    }
  }

  // kind (optional)
  if (const std::string* k = data.FindString("kind"))
    script.Set("kind", *k);

  // keyScriptId (optional)
  if (const std::string* ks = data.FindString("keyScriptId")) {
    const std::string v = *ks;
    if (!v.empty())
      script.Set("keyScriptId", v);
  }

  // Top-level actions are the primary timeline.
  if (const base::Value::List* l = data.FindList("actions")) {
    script.Set("actions", l->Clone());
  } else {
    script.Set("actions", base::Value::List());
  }

  if (const base::Value::Dict* d = data.FindDict("settings")) {
    script.Set("settings", d->Clone());
  } else {
    script.Set("settings", base::Value::Dict());
  }

  if (const base::Value::Dict* d = data.FindDict("settingsOverrides")) {
    script.Set("settingsOverrides", d->Clone());
  }

  // url_contexts is optional compatibility metadata.
  if (const base::Value::List* ctx = data.FindList("url_contexts")) {
    script.Set("url_contexts", ctx->Clone());
  } else {
    script.Set("url_contexts", base::Value::List());
  }

  // Canonicalise (validation, normalisation) and persist.
  script = dashboard::ScriptSchema::CanonicalizeForSave(std::move(script), id,
                                                        base::Time::Now());

  // Suppress no-op saves: autosave can fire repeatedly while the document is
  // otherwise unchanged, and CanonicalizeForSave always refreshes `updated_at`.
  // Compare semantic content with `updated_at` stripped to avoid unnecessary
  // disk writes, script reload broadcasts, and UI churn.
  {
    base::Value::Dict existing = dashboard_->LoadScript(id);
    if (!existing.empty()) {
      base::Value::Dict existing_cmp = existing.Clone();
      base::Value::Dict incoming_cmp = script.Clone();
      existing_cmp.Remove("updated_at");
      incoming_cmp.Remove("updated_at");
      if (base::Value(existing_cmp.Clone()) == base::Value(incoming_cmp.Clone())) {
        base::Value::Dict ok;
        ok.Set("ok", true);
        ResolveCallback(cb, ok);
        return;
      }
    }
  }

  const base::FilePath scripts_dir = ScriptsDir(profile_);
  base::ThreadPool::PostTaskAndReply(
      FROM_HERE,
      {base::MayBlock(),
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](base::FilePath dir, Profile* profile, std::string id,
             base::Value::Dict script) {
            base::CreateDirectory(dir);
            (void)WriteScriptToDisk(profile, id, script);
          },
          scripts_dir, profile_, id, script.Clone()),
      base::BindOnce(&DashboardUIMessageHandler::OnSaveDone,
                     weak_ptr_factory_.GetWeakPtr(), cb, id,
                     std::move(script)));
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::OnSaveDone(std::string cb,
                                           std::string id,
                                           base::Value::Dict script) {
  dashboard_->SaveScript(id, script.Clone());
  NotifyScriptsChanged();
  base::Value::Dict ok;
  ok.Set("ok", true);
  ResolveCallback(cb, ok); // FIX: Use member function
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleGetRunHistory(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  const std::string script_id_filter =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                              : std::string();
  const std::string from_iso =
      (args.size() > 2 && args[2].is_string()) ? args[2].GetString()
                                              : std::string();
  const std::string to_iso =
      (args.size() > 3 && args[3].is_string()) ? args[3].GetString()
                                              : std::string();
  uint32_t limit = 2000;
  if (args.size() > 4 && args[4].is_int()) {
    limit = static_cast<uint32_t>(std::max(0, args[4].GetInt()));
  } else if (args.size() > 4 && args[4].is_double()) {
    limit = static_cast<uint32_t>(std::max(0.0, args[4].GetDouble()));
  }

  const base::Time now = base::Time::Now();
  base::Time from = now - base::Days(7);
  base::Time to = now + base::Days(1);
  if (!from_iso.empty()) {
    base::Time parsed;
    if (base::Time::FromUTCString(from_iso.c_str(), &parsed) ||
        base::Time::FromString(from_iso.c_str(), &parsed)) {
      from = parsed;
    }
  }
  if (!to_iso.empty()) {
    base::Time parsed;
    if (base::Time::FromUTCString(to_iso.c_str(), &parsed) ||
        base::Time::FromString(to_iso.c_str(), &parsed)) {
      to = parsed;
    }
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, std::string script_filter, base::Time from,
             base::Time to, uint32_t limit) -> base::Value::List {
            base::Value::List summaries =
                dashboard::RunHistoryStore::ReadSummaries(profile, from, to,
                                                          limit);
            if (script_filter.empty())
              return summaries;
            base::Value::List filtered;
            for (const auto& v : summaries) {
              const base::Value::Dict* d = v.GetIfDict();
              if (!d)
                continue;
              const std::string* sid = d->FindString("script_id");
              if (sid && *sid == script_filter)
                filtered.Append(v.Clone());
            }
            return filtered;
          },
          base::Unretained(profile_), script_id_filter, from, to, limit),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             base::Value::List runs) {
            if (!self)
              return;
            base::Value::Dict payload;
            payload.Set("runs", std::move(runs));
            self->ResolveCallback(cb, payload);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleGetRunDetails(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  const std::string run_id =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                              : std::string();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, std::string run_id) -> base::Value::Dict {
            base::Value::Dict detail;
            if (!dashboard::RunHistoryStore::ReadRunDetail(profile, run_id,
                                                           &detail)) {
              base::Value::Dict out;
              out.Set("ok", false);
              out.Set("error", "Run detail not found: " + run_id);
              return out;
            }

            // Inline the replay manifest JSON when present so the Debugging tab
            // doesn't have to fetch chrome://dashboard/replay/... (which can be
            // blocked by fetch() depending on WebUI restrictions).
            const bool has_replay = detail.FindBool("has_replay").value_or(false);
            if (has_replay) {
              const base::FilePath manifest_path =
                  dashboard::RunHistoryStore::RunsDir(profile)
                      .AppendASCII("replays")
                      .AppendASCII(run_id)
                      .AppendASCII("manifest.json");
              std::string json;
              if (base::ReadFileToString(manifest_path, &json)) {
                std::optional<base::Value> v = base::JSONReader::Read(
                    json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
                if (v && v->is_dict()) {
                  detail.Set("replay_manifest_json", v->GetDict().Clone());
                }
              }
            }
            detail.Set("ok", true);
            return detail;
          },
          base::Unretained(profile_), run_id),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             base::Value::Dict run) {
            if (!self)
              return;
            base::Value::Dict payload;
            payload.Set("run", std::move(run));
            self->ResolveCallback(cb, payload);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandleGetDataVariables(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile) -> base::Value::List {
            const base::FilePath path = VariablesIniPath(profile);
            base::Value::Dict dict = ReadKeyValueIniBlocking(path);
            if (dict.empty() && !base::PathExists(path)) {
              dict = DefaultVariables();
              (void)WriteKeyValueIniBlocking(path, dict);
            }
            std::vector<std::string> keys;
            keys.reserve(dict.size());
            for (auto it : dict) {
              if (dashboard::IsRuntimeBuiltinKey(it.first)) {
                continue;
              }
              keys.push_back(it.first);
            }
            std::sort(keys.begin(), keys.end());
            base::Value::List variables;
            for (const auto& k : keys) {
              const std::string* v = dict.FindString(k);
              base::Value::Dict row;
              row.Set("key", k);
              row.Set("value", v ? *v : std::string());
              variables.Append(std::move(row));
            }
            return variables;
          },
          base::Unretained(profile_)),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             base::Value::List variables) {
            if (!self)
              return;
            base::Value::List builtins =
                dashboard::RuntimeBuiltinRowsForDataTab(self->profile_);
            base::Value::List merged;
            for (auto& row : builtins) {
              merged.Append(std::move(row));
            }
            for (auto& row : variables) {
              merged.Append(std::move(row));
            }
            base::Value::Dict payload;
            payload.Set("variables", std::move(merged));
            self->ResolveCallback(cb, payload);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleSaveDataVariables(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  const base::Value* payload = (args.size() > 1) ? &args[1] : nullptr;

  base::Value::Dict dict;
  if (payload && payload->is_dict()) {
    for (auto it : payload->GetDict()) {
      const std::string key = NormalizeDataKey(it.first);
      if (key.empty() || dashboard::IsRuntimeBuiltinKey(key))
        continue;
      std::string value;
      if (it.second.is_string())
        value = it.second.GetString();
      else
        value = it.second.DebugString();
      dict.Set(key, value);
    }
  } else if (payload && payload->is_list()) {
    for (const auto& v : payload->GetList()) {
      const base::Value::Dict* d = v.GetIfDict();
      if (!d)
        continue;
      if (d->FindBool("builtin").value_or(false) ||
          d->FindBool("locked").value_or(false)) {
        continue;
      }
      const std::string* k = d->FindString("key");
      const std::string* val = d->FindString("value");
      if (!k)
        continue;
      const std::string key = NormalizeDataKey(*k);
      if (key.empty() || dashboard::IsRuntimeBuiltinKey(key))
        continue;
      dict.Set(key, val ? *val : std::string());
    }
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, base::Value::Dict dict) -> bool {
            return WriteKeyValueIniBlocking(VariablesIniPath(profile), dict);
          },
          base::Unretained(profile_), dict.Clone()),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             bool ok) {
            if (!self)
              return;
            if (ok) {
              dashboard::ScriptExecutionService::
                  InvalidateTokenCatalogCacheForProfile(self->profile_);
            }
            base::Value::Dict payload;
            payload.Set("ok", ok);
            self->ResolveCallback(cb, payload);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleGetDataSecrets(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile) -> base::Value::List {
            base::Value::Dict dict = ReadKeyValueIniBlocking(SecretsIniPath(profile));
            bool wrote_defaults = false;
            for (const char* key : {"GEMINI_API_KEY", "CEREBRAS_API_KEY",
                                    "OPENAI_API_KEY", "ANTHROPIC_API_KEY"}) {
              if (!dict.FindString(key)) {
                dict.Set(key, "");
                wrote_defaults = true;
              }
            }
            if (wrote_defaults) {
              (void)WriteKeyValueIniBlocking(SecretsIniPath(profile), dict);
            }
            std::vector<std::string> keys;
            keys.reserve(dict.size());
            for (auto it : dict)
              keys.push_back(it.first);
            std::sort(keys.begin(), keys.end());
            base::Value::List secrets;
            for (const auto& k : keys) {
              const std::string* v = dict.FindString(k);
              base::Value::Dict row;
              row.Set("key", k);
              row.Set("hasValue", v && !v->empty());
              secrets.Append(std::move(row));
            }
            return secrets;
          },
          base::Unretained(profile_)),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             base::Value::List secrets) {
            if (!self)
              return;
            base::Value::Dict payload;
            payload.Set("secrets", std::move(secrets));
            self->ResolveCallback(cb, payload);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleSaveDataSecrets(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;

  std::vector<std::string> keep_keys;
  base::Value::List updates;
  if (payload) {
    if (const base::Value::List* keys = payload->FindList("keys")) {
      for (const auto& v : *keys) {
        if (!v.is_string())
          continue;
        const std::string key = NormalizeDataKey(v.GetString());
        if (!key.empty())
          keep_keys.push_back(key);
      }
    }
    if (const base::Value::List* u = payload->FindList("updates")) {
      updates = u->Clone();
    }
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, std::vector<std::string> keep_keys,
             base::Value::List updates) -> bool {
            base::Value::Dict existing = ReadKeyValueIniBlocking(SecretsIniPath(profile));
            base::Value::Dict next;

            base::flat_set<std::string> keep;
            keep.reserve(keep_keys.size());
            for (const auto& k : keep_keys)
              keep.insert(k);

            for (auto it : existing) {
              if (keep.contains(it.first)) {
                const std::string* v = existing.FindString(it.first);
                next.Set(it.first, v ? *v : std::string());
              }
            }

            for (const auto& v : updates) {
              const base::Value::Dict* d = v.GetIfDict();
              if (!d)
                continue;
              const std::string* kraw = d->FindString("key");
              const std::string* vraw = d->FindString("value");
              if (!kraw || !vraw)
                continue;
              const std::string key = NormalizeDataKey(*kraw);
              if (key.empty() || !keep.contains(key))
                continue;
              std::string ini_value;
              if (!EncryptSecretToIniValue(*vraw, &ini_value))
                continue;
              next.Set(key, ini_value);
            }

            // Ensure newly added keys persist even if value wasn't set yet.
            for (const auto& k : keep_keys) {
              if (!next.FindString(k))
                next.Set(k, "");
            }

            return WriteKeyValueIniBlocking(SecretsIniPath(profile), next);
          },
          base::Unretained(profile_), std::move(keep_keys), std::move(updates)),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             bool ok) {
            if (!self)
              return;
            if (ok) {
              dashboard::ScriptExecutionService::
                  InvalidateTokenCatalogCacheForProfile(self->profile_);
            }
            base::Value::Dict payload;
            payload.Set("ok", ok);
            self->ResolveCallback(cb, payload);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

// ----------------------------------------------------------------------

void DashboardUIMessageHandler::HandleGetScrapeParentDir(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  base::Value::Dict out;
  out.Set("path",
          dashboard::ResolveScrapeParentDir(profile_).AsUTF8Unsafe());
  out.Set("defaultPath",
          dashboard::DefaultScrapeParentDir(profile_).AsUTF8Unsafe());
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleSetScrapeParentDir(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  std::string path;
  if (args.size() > 1 && args[1].is_string()) {
    path = args[1].GetString();
  } else if (args.size() > 1 && args[1].is_dict()) {
    if (const std::string* p = args[1].GetDict().FindString("path")) {
      path = *p;
    }
  }
  base::TrimWhitespaceASCII(path, base::TRIM_ALL, &path);
  base::Value::Dict settings = dashboard::GlobalSettingsStore().Clone();
  base::Value::Dict* data = settings.EnsureDict("data");
  if (path.empty()) {
    data->Remove("scrapeParentDir");
  } else {
    data->Set("scrapeParentDir", path);
  }
  dashboard::GlobalSettingsStore() = settings.Clone();
  const base::FilePath scripts_dir = ::ScriptsDir(profile_);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&SaveGlobalSettingsToDiskBlocking, scripts_dir,
                     std::move(settings)),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             bool ok) {
            if (!self) return;
            base::Value::Dict res;
            res.Set("ok", ok);
            res.Set("path",
                    dashboard::ResolveScrapeParentDir(self->profile_)
                        .AsUTF8Unsafe());
            self->ResolveCallback(cb, res);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandleListScrapes(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile) {
            base::Value::List files;
            for (const auto& info :
                 dashboard::ListScrapeCsvFilesBlocking(profile)) {
              base::Value::Dict row;
              row.Set("relativePath", info.relative_path);
              row.Set("project", info.project);
              row.Set("fileName", info.file_name);
              row.Set("sizeBytes", static_cast<double>(info.size_bytes));
              row.Set("modifiedMs", info.modified_ms);
              files.Append(std::move(row));
            }
            base::Value::Dict out;
            out.Set("files", std::move(files));
            out.Set("parentDir",
                    dashboard::ResolveScrapeParentDir(profile).AsUTF8Unsafe());
            return out;
          },
          profile_),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             base::Value::Dict out) {
            if (!self) return;
            self->ResolveCallback(cb, out);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandleReadScrapeCsv(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  std::string relative;
  if (args.size() > 1 && args[1].is_string()) {
    relative = args[1].GetString();
  } else if (args.size() > 1 && args[1].is_dict()) {
    if (const std::string* p = args[1].GetDict().FindString("relativePath")) {
      relative = *p;
    }
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, std::string relative) {
            base::Value::Dict out;
            std::string text;
            std::string error;
            const bool ok = dashboard::ReadScrapeCsvBlocking(
                profile, relative, &text, &error);
            out.Set("ok", ok);
            if (ok) {
              out.Set("text", text);
              out.Set("relativePath", relative);
            } else {
              out.Set("error", error.empty() ? "read_failed" : error);
            }
            return out;
          },
          profile_, relative),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             base::Value::Dict out) {
            if (!self) return;
            self->ResolveCallback(cb, out);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}


void DashboardUIMessageHandler::HandleDeleteScrapeCsv(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  std::string relative;
  if (args.size() > 1 && args[1].is_string()) {
    relative = args[1].GetString();
  } else if (args.size() > 1 && args[1].is_dict()) {
    if (const std::string* p = args[1].GetDict().FindString("relativePath")) {
      relative = *p;
    }
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, std::string relative) {
            base::Value::Dict out;
            std::string error;
            const bool ok =
                dashboard::DeleteScrapeCsvBlocking(profile, relative, &error);
            out.Set("ok", ok);
            if (ok) {
              out.Set("relativePath", relative);
            } else {
              out.Set("error", error.empty() ? "delete_failed" : error);
            }
            return out;
          },
          profile_, relative),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             base::Value::Dict out) {
            if (!self)
              return;
            self->ResolveCallback(cb, out);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandleDeleteScript(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  const std::string id = (args.size() > 1 && args[1].is_string())
                             ? args[1].GetString()
                             : std::string();
  if (id.empty()) {
    LOG(ERROR) << "[Dashboard] HandleDeleteScript: empty id.";
    base::Value::Dict res;
    res.Set("ok", false);
    ResolveCallback(cb, res); // FIX: Use member function
    return;
  }

  const base::FilePath scripts_dir = ScriptsDir(profile_);
  base::ThreadPool::PostTaskAndReply(
      FROM_HERE,
      {base::MayBlock(),
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](base::FilePath dir, Profile* profile, std::string id) {
            base::CreateDirectory(dir);
            (void)DeleteScriptFromDisk(profile, id);
          },
          scripts_dir, profile_, id),
      base::BindOnce(&DashboardUIMessageHandler::OnDeleteDone,
                     weak_ptr_factory_.GetWeakPtr(), cb, id));
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::OnDeleteDone(std::string cb,
                                             std::string id) {
  dashboard_->DeleteScript(id);
  NotifyScriptsChanged();
  base::Value::Dict ok;
  ok.Set("ok", true);
  ResolveCallback(cb, ok); // FIX: Use member function
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandlePlayScript(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  const std::string id = (args.size() > 1 && args[1].is_string())
                             ? args[1].GetString()
                             : std::string();
  const base::Value::Dict* options =
      (args.size() > 2 && args[2].is_dict()) ? &args[2].GetDict() : nullptr;
  const bool wait_for_completion =
      options &&
      options->FindBool("waitForCompletion").value_or(false);

  if (id.empty()) {
    LOG(ERROR) << "[Dashboard] HandlePlayScript: empty id.";
    base::Value::Dict res;
    res.Set("ok", false);
    ResolveCallback(cb, res); // FIX
    return;
  }

  base::Value::Dict script = dashboard_->LoadScript(id);
  if (script.empty()) {
    LOG(ERROR) << "[Dashboard] HandlePlayScript: script not found id=" << id;
    base::Value::Dict res;
    res.Set("ok", false);
    ResolveCallback(cb, res); // FIX
    return;
  }

  // 2) Expand
  base::flat_set<std::string> visiting;
  std::vector<std::string> blocked_listener_targets;
  std::function<base::Value::List(const base::Value::List&)> expand;
  expand = [&](const base::Value::List& in) -> base::Value::List {
    base::Value::List out;
    for (const auto& v : in) {
      if (!v.is_dict())
        continue;
      const base::Value::Dict& d = v.GetDict();
      const std::string* act = d.FindString("action");
      const std::string kind = act ? *act : std::string();

      if (kind == "call_script") {
        const base::Value::Dict* params = d.FindDict("params");
        std::string sid;
        if (params) {
          if (const std::string* s = params->FindString("scriptId")) sid = *s;
        }
        if (!sid.empty() && !visiting.contains(sid)) {
          visiting.insert(sid);
          base::Value::Dict callee = dashboard_->LoadScript(sid);
          const std::string callee_kind =
              callee.FindString("kind") ? *callee.FindString("kind")
                                        : std::string("main");
          if (base::EqualsCaseInsensitiveASCII(callee_kind, "listener")) {
            blocked_listener_targets.push_back(sid);
            visiting.erase(sid);
            continue;
          }
          base::Value::List flat = FlattenTimelineForListenerScript(callee);
          base::Value::List expanded = expand(flat);
          for (auto& e : expanded)
            out.Append(std::move(e));
          visiting.erase(sid);
          continue;
        }
      }
      out.Append(v.Clone());
    }
    return out;
  };

  base::Value::List flat = FlattenTimelineForListenerScript(script);
  LOG(INFO) << "[Dashboard] HandlePlayScript flat size=" << flat.size();
  base::Value::List final_seq = expand(flat);
  LOG(INFO) << "[Dashboard] HandlePlayScript expanded size="
            << final_seq.size();
  if (!blocked_listener_targets.empty()) {
    base::Value::Dict bad;
    bad.Set("ok", false);
    bad.Set("error",
            "call_script cannot target listener scripts. Start listeners from "
            "Debugging > Listeners.");
    bad.Set("blockedCount", static_cast<int>(blocked_listener_targets.size()));
    ResolveCallback(cb, bad);
    return;
  }

  // Optional Key Script: run as a recovery sequence after repeated failures.
  std::string key_script_id;
  if (const std::string* ks = script.FindString("keyScriptId"))
    key_script_id = *ks;
  base::Value::List key_seq;
  if (!key_script_id.empty()) {
    base::Value::Dict key_script = dashboard_->LoadScript(key_script_id);
    if (!key_script.empty()) {
      base::Value::List kflat = FlattenTimelineForListenerScript(key_script);
      key_seq = expand(kflat);
      LOG(INFO) << "[Dashboard] HandlePlayScript key script id="
                << key_script_id << " expanded size=" << key_seq.size();
    }
  }

  // 3) Execute. Always open about:blank — navigate actions own page loads.
  // ExtractScriptStartUrl / url_contexts must not preload the first target.
  content::WebContents* target_wc = nullptr;
  if (Browser* browser = chrome::FindLastActiveWithProfile(profile_)) {
    const GURL start_url("about:blank");
    NavigateParams np(browser, start_url,
                      ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    np.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
    Navigate(&np);
    target_wc = np.navigated_or_inserted_contents;
  } else {
    LOG(ERROR) << "[Dashboard] No active browser to open a new tab.";
  }

  dashboard::ScriptExecutionService* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  std::string execution_id;
  if (exec_service && target_wc) {
    const base::Value::List* key_ptr = key_seq.empty() ? nullptr : &key_seq;
    if (wait_for_completion) {
      auto execution_id_ref = std::make_shared<std::string>();
      execution_id = exec_service->StartScriptRun(
          id, final_seq, target_wc, key_ptr,
          base::BindOnce(
              [](base::WeakPtr<DashboardUIMessageHandler> self,
                 std::string cb_id, std::shared_ptr<std::string> run_id_ref,
                 bool success, int failed_step, std::string message) {
                if (!self) {
                  return;
                }
                base::Value::Dict res;
                res.Set("ok", success);
                res.Set("success", success);
                res.Set("completed", true);
                if (run_id_ref && !run_id_ref->empty()) {
                  res.Set("runId", *run_id_ref);
                }
                if (failed_step >= 0) {
                  res.Set("failed_step", failed_step);
                }
                if (!message.empty()) {
                  res.Set("message", message);
                }
                self->ResolveCallback(cb_id, res);
              },
              weak_ptr_factory_.GetWeakPtr(), cb, execution_id_ref));
      if (execution_id.empty()) {
        base::Value::Dict res;
        res.Set("ok", false);
        res.Set("error", "Failed to queue script run.");
        ResolveCallback(cb, res);
        return;
      }
      *execution_id_ref = execution_id;
      return;
    }
    execution_id = exec_service->StartScriptRun(id, final_seq, target_wc, key_ptr);
  } else {
    LOG(ERROR) << "[Dashboard] No ScriptExecutionService (or no target tab).";
  }

  base::Value::Dict ok;
  ok.Set("ok", !execution_id.empty());
  ok.Set("runId", execution_id);
  if (execution_id.empty()) {
    ok.Set("error", "Failed to queue script run.");
  }
  ResolveCallback(cb, ok); // FIX
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleResumeScriptFromStep(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const std::string id = (args.size() > 1 && args[1].is_string())
                             ? args[1].GetString()
                             : std::string();
  const int step_index = (args.size() > 2 && args[2].is_int())
                             ? args[2].GetInt()
                             : -1;
  const std::string run_id = (args.size() > 3 && args[3].is_string())
                                 ? args[3].GetString()
                                 : std::string();

  base::Value::Dict out;
  if (id.empty()) {
    out.Set("success", false);
    out.Set("error", "Empty script id.");
    ResolveCallback(cb, out);
    return;
  }
  if (step_index < 0) {
    out.Set("success", false);
    out.Set("error", "Invalid step index.");
    ResolveCallback(cb, out);
    return;
  }

  base::Value::Dict script = dashboard_->LoadScript(id);
  if (script.empty()) {
    out.Set("success", false);
    out.Set("error", "Script not found.");
    ResolveCallback(cb, out);
    return;
  }

  // Expand call_script nodes recursively.
  base::flat_set<std::string> visiting;
  std::vector<std::string> blocked_listener_targets;
  std::function<base::Value::List(const base::Value::List&)> expand;
  expand = [&](const base::Value::List& in) -> base::Value::List {
    base::Value::List out_list;
    for (const auto& v : in) {
      if (!v.is_dict())
        continue;
      const base::Value::Dict& d = v.GetDict();
      const std::string* act = d.FindString("action");
      const std::string kind = act ? *act : std::string();

      if (kind == "call_script") {
        const base::Value::Dict* params = d.FindDict("params");
        std::string sid;
        if (params) {
          if (const std::string* s = params->FindString("scriptId"))
            sid = *s;
        }
        if (!sid.empty() && !visiting.contains(sid)) {
          visiting.insert(sid);
          base::Value::Dict callee = dashboard_->LoadScript(sid);
          const std::string callee_kind =
              callee.FindString("kind") ? *callee.FindString("kind")
                                        : std::string("main");
          if (base::EqualsCaseInsensitiveASCII(callee_kind, "listener")) {
            blocked_listener_targets.push_back(sid);
            visiting.erase(sid);
            continue;
          }
          base::Value::List flat = FlattenTimelineForListenerScript(callee);
          base::Value::List expanded = expand(flat);
          for (auto& e : expanded)
            out_list.Append(std::move(e));
          visiting.erase(sid);
          continue;
        }
      }
      out_list.Append(v.Clone());
    }
    return out_list;
  };

  base::Value::List final_seq =
      expand(FlattenTimelineForListenerScript(script));
  if (!blocked_listener_targets.empty()) {
    out.Set("success", false);
    out.Set("error",
            "call_script cannot target listener scripts. Start listeners from "
            "Debugging > Listeners.");
    ResolveCallback(cb, out);
    return;
  }
  if (final_seq.empty()) {
    out.Set("success", false);
    out.Set("error", "Script has no actions to resume.");
    ResolveCallback(cb, out);
    return;
  }

  std::string resume_url;
  auto extract_url_for_step = [&](const base::Value::List* steps) {
    if (resume_url.empty()) {
      resume_url =
          dashboard::ScriptSchema::ExtractRunHistoryStepUrl(steps, step_index);
    }
  };

  // Prefer an already-loaded last_run (in-memory) before optional disk detail.
  if (const base::Value::Dict* last_run = script.FindDict("last_run")) {
    extract_url_for_step(last_run->FindList("steps"));
  }

  const std::string timeline_context_url =
      dashboard::ScriptSchema::ResolveResumeNavigationUrl(final_seq, step_index);

  if (!run_id.empty()) {
    // Disk read — never on the UI thread.
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::BindOnce(
            [](Profile* profile, std::string run_id,
               int step_index) -> std::string {
              base::Value::Dict detail;
              if (!dashboard::RunHistoryStore::ReadRunDetail(profile, run_id,
                                                             &detail)) {
                return std::string();
              }
              return dashboard::ScriptSchema::ExtractRunHistoryStepUrl(
                  detail.FindList("steps"), step_index);
            },
            base::Unretained(profile_), run_id, step_index),
        base::BindOnce(
            [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
               std::string id, int step_index, base::Value::List final_seq,
               std::string in_memory_history_url,
               std::string timeline_context_url,
               std::string disk_history_url) {
              if (!self) {
                return;
              }
              std::string run_history_url = disk_history_url;
              if (run_history_url.empty()) {
                run_history_url = in_memory_history_url;
              }
              self->FinishResumeScriptFromStep(
                  std::move(cb), std::move(id), step_index, std::move(final_seq),
                  std::move(run_history_url), std::move(timeline_context_url));
            },
            weak_ptr_factory_.GetWeakPtr(), cb, id, step_index,
            std::move(final_seq), resume_url, timeline_context_url));
    return;
  }

  FinishResumeScriptFromStep(std::move(cb), std::move(id), step_index,
                             std::move(final_seq), std::move(resume_url),
                             timeline_context_url);
}

void DashboardUIMessageHandler::FinishResumeScriptFromStep(
    std::string cb,
    std::string id,
    int step_index,
    base::Value::List final_seq,
    std::string run_history_url,
    std::string timeline_context_url) {
  base::Value::Dict out;
  // Preference: run-history URL > timeline contextUrl. Never start blank when
  // either source has a real URL. Do NOT rewrite the script's contextUrl here
  // on failure — only successful executions update it.
  std::string resume_url = run_history_url;
  if (resume_url.empty()) {
    resume_url = timeline_context_url;
  }
  const bool context_url_mismatch =
      !run_history_url.empty() && !timeline_context_url.empty() &&
      !dashboard::ScriptSchema::ContextUrlsEquivalent(run_history_url,
                                                      timeline_context_url);
  if (context_url_mismatch) {
    LOG(WARNING) << "[Dashboard] Resume URL mismatch script=" << id
                 << " step=" << step_index << " run_history=" << run_history_url
                 << " contextUrl=" << timeline_context_url
                 << " (navigating to run history; leaving contextUrl)";
    out.Set("context_url_mismatch", true);
    out.Set("run_history_url", run_history_url);
    out.Set("timeline_context_url", timeline_context_url);
  }

  content::WebContents* target_wc = nullptr;
  if (Browser* browser = chrome::FindLastActiveWithProfile(profile_)) {
    NavigateParams np(browser, GURL("about:blank"),
                      ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    np.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
    Navigate(&np);
    target_wc = np.navigated_or_inserted_contents;
  }
  if (!target_wc) {
    out.Set("success", false);
    out.Set("error", "No active browser to open a new tab.");
    ResolveCallback(cb, out);
    return;
  }

  dashboard::ScriptExecutionService* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (!exec_service) {
    out.Set("success", false);
    out.Set("error", "Execution service unavailable.");
    ResolveCallback(cb, out);
    return;
  }

  const std::string execution_id = exec_service->StartScriptRunFromStep(
      id, final_seq, target_wc, step_index, resume_url);

  out.Set("success", !execution_id.empty());
  out.Set("runId", execution_id);
  if (!resume_url.empty()) {
    out.Set("resume_url", resume_url);
  }
  if (execution_id.empty()) {
    out.Set("error", "Failed to start resumed script run.");
  }
  ResolveCallback(cb, out);
}

// ----------------------------------------------------------------------
// Debug-logging toggles
// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleSetDebugLogging(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  bool enabled = (args.size() > 1 && args[1].is_bool()) ? args[1].GetBool()
                                                       : false;
  dashboard::SetVerboseLoggingEnabled(enabled);
  base::Value::Dict out;
  out.Set("ok", true);
  out.Set("enabled", enabled);
  ResolveCallback(cb, out); // FIX
}

void DashboardUIMessageHandler::HandleGetDebugLogging(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  base::Value::Dict out;
  out.Set("enabled", dashboard::IsVerboseLoggingEnabled());
  ResolveCallback(cb, out); // FIX
}

// ----------------------------------------------------------------------
// Global settings (read / write)
// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleGetEffectiveSettings(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();

  const base::FilePath scripts_dir = ::ScriptsDir(profile_);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(),
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&LoadGlobalSettingsFromDiskBlocking, scripts_dir),
      base::BindOnce(&DashboardUIMessageHandler::OnGetEffectiveSettingsDone,
                     weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::OnGetEffectiveSettingsDone(std::string cb,
                                                           base::Value::Dict settings) {
  dashboard::GlobalSettingsStore() = settings.Clone();
  ApplyInputBehaviorSettings(dashboard::GlobalSettingsStore());
  ApplySubsystemRuntimeSettings(profile_, dashboard::GlobalSettingsStore());
  base::Value::Dict out;
  out.Set("settings", std::move(settings));
  ResolveCallback(cb, out); // FIX
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleSetGlobalSettings(const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;
  if (!payload) {
    LOG(ERROR) << "[Dashboard] SetGlobalSettings: missing payload.";
    base::Value::Dict res;
    res.Set("ok", false);
    ResolveCallback(cb, res); // FIX
    return;
  }

  base::Value::Dict new_settings = payload->Clone();
  dashboard::GlobalSettingsStore() = new_settings.Clone();
  const base::FilePath scripts_dir = ::ScriptsDir(profile_);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(),
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&SaveGlobalSettingsToDiskBlocking, scripts_dir,
                     std::move(new_settings)),
      base::BindOnce(&DashboardUIMessageHandler::OnSetGlobalSettingsDone,
                     weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandleImportChromeProfile(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  if (!chrome_profile_importer_) {
    chrome_profile_importer_ =
        std::make_unique<dashboard::ChromeProfileImporter>(profile_);
  }
  content::WebContents* contents =
      web_ui() ? web_ui()->GetWebContents() : nullptr;
  gfx::NativeWindow parent =
      contents ? contents->GetTopLevelNativeWindow() : gfx::NativeWindow();
  chrome_profile_importer_->PickProfileDirectoryAndImport(
      parent, contents,
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self,
             std::string callback, base::Value::Dict result) {
            if (!self) {
              return;
            }
            self->ResolveCallback(callback, result);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::OnSetGlobalSettingsDone(std::string cb,
                                                        bool ok) {
  base::Value::Dict res;
  res.Set("ok", ok);

  // Apply anti-detection settings to all active tabs
  if (ok) {
    const base::Value::Dict* settings = &dashboard::GlobalSettingsStore();
    ApplyInputBehaviorSettings(*settings);
    ApplySubsystemRuntimeSettings(profile_, *settings);
    if (auto* anti = settings->FindDict("antiDetection")) {
      ApplyAntiDetectionSettings(*anti);
    }
    base::Value::List restart_switches_value;
    const std::vector<std::string> restart_switches =
        BuildRestartRequiredLaunchSwitches(*settings);
    for (const std::string& sw : restart_switches) {
      restart_switches_value.Append(sw);
    }
    res.Set("restartRequired", !restart_switches.empty());
    res.Set("restartSwitches", std::move(restart_switches_value));
  } else {
    res.Set("restartRequired", false);
    res.Set("restartSwitches", base::Value::List());
  }

  ResolveCallback(cb, res); // FIX
}

// ----------------------------------------------------------------------
// Anti-detection helpers
// ----------------------------------------------------------------------
void DashboardUIMessageHandler::InitializeAntiDetectionController() {
  content::WebContents* wc = web_ui()->GetWebContents();
  if (!wc) {
    return;
  }

  if (!anti_detection_controller_) {
    anti_detection_controller_ =
        std::make_unique<dashboard::AntiDetectionController>(wc);
  }

  // Load and apply current settings
  const base::Value::Dict* settings = &dashboard::GlobalSettingsStore();
  if (auto* anti = settings->FindDict("antiDetection")) {
    anti_detection_controller_->InjectAntiDetectionScripts(*anti);
  }
}

void DashboardUIMessageHandler::ApplyAntiDetectionSettings(
    const base::Value::Dict& settings) {
  // Apply to the current WebContents if we have one
  content::WebContents* wc = web_ui()->GetWebContents();
  if (wc) {
    if (!anti_detection_controller_) {
      anti_detection_controller_ =
          std::make_unique<dashboard::AntiDetectionController>(wc);
    }
    anti_detection_controller_->InjectAntiDetectionScripts(settings);
  }

  // Also apply to all other active tabs in the browser
  if (Browser* browser = chrome::FindLastActiveWithProfile(profile_)) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* tab_wc = tab_strip->GetWebContentsAt(i);
      if (tab_wc && tab_wc != wc) {
        // The override is stored on the tab, so the controller can go away.
        auto temp_controller =
            std::make_unique<dashboard::AntiDetectionController>(tab_wc);
        temp_controller->InjectAntiDetectionScripts(settings);
      }
    }
  }
}

// ----------------------------------------------------------------------
// Recorder callbacks
// ----------------------------------------------------------------------
void DashboardUIMessageHandler::HandleArmLocatorCapture(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  const std::string source =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                              : std::string();
  const std::string preferred_context_url =
      (args.size() > 2 && args[2].is_string()) ? args[2].GetString()
                                               : std::string();
  base::Value::Dict out;
  content::WebContents* dashboard_contents = web_ui()->GetWebContents();
  BrowserView* preferred_browser_view = nullptr;
  if (dashboard_contents) {
    if (Browser* browser = chrome::FindBrowserWithTab(dashboard_contents)) {
      preferred_browser_view = BrowserView::GetBrowserViewForBrowser(browser);
    }
  }

  RecorderController* rc = RecorderController::GetActive();
  bool started_passive_for_capture = false;

  if (!rc) {
    if (!recorder_)
      recorder_ = std::make_unique<RecorderController>();
    rc = recorder_.get();
    RecorderController::SetActive(rc);
  }

  if (!rc->is_recording()) {
    rc->StartPassiveMode(/*browser_view=*/preferred_browser_view,
                         /*explicit_recording=*/false);
    started_passive_for_capture = true;
  }

  if (!rc->is_recording()) {
    out.Set("ok", false);
    out.Set("error", "Unable to arm locator capture (recorder not attached).");
    LOG(WARNING) << "[Dashboard][CaptureUI] arm failed recorder_not_attached";
    ResolveCallback(cb, out);
    return;
  }

  if (!rc->AttachToBestCapturableTarget(preferred_browser_view,
                                        dashboard_contents,
                                        preferred_context_url)) {
    if (started_passive_for_capture) {
      rc->StopPassiveMode();
    }
    out.Set("ok", false);
    out.Set("error",
            "Unable to arm locator capture (no capturable page tab found).");
    LOG(WARNING) << "[Dashboard][CaptureUI] arm failed no_capturable_tab";
    ResolveCallback(cb, out);
    return;
  }

  // Reverse-order flow support: only reuse a recent capture when it came from
  // the same currently attached page. This avoids injecting stale bundles from
  // unrelated tabs/sites into the editor.
  const bool allow_recent_capture_reuse =
      !base::EqualsCaseInsensitiveASCII(source, "editor_arm");
  if (allow_recent_capture_reuse) {
    uint64_t latest_sequence = 0;
    std::optional<base::Value::Dict> latest_capture =
        RecorderController::GetLatestLocatorCaptureResult(
            /*min_sequence_exclusive=*/0, &latest_sequence);
    if (latest_capture) {
      const double captured_at_ms =
          latest_capture->FindDouble("captured_at_ms").value_or(0.0);
      const std::string captured_page_url =
          latest_capture->FindString("page_url")
              ? *latest_capture->FindString("page_url")
              : std::string();
      const std::string current_page_url = rc->GetCurrentUrl();
      if (captured_at_ms > 0.0 && !captured_page_url.empty() &&
          !current_page_url.empty()) {
        const double now_ms =
            static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch());
        if ((now_ms - captured_at_ms) <= 30000.0 &&
            captured_page_url == current_page_url &&
            HasNonVisualCaptureLocators(*latest_capture)) {
          LOG(WARNING) << "[Dashboard][CaptureUI] arm reused_recent_capture"
                       << " sequence=" << latest_sequence;
          out = std::move(*latest_capture);
          out.Set("ok", true);
          out.Set("sequence", static_cast<double>(latest_sequence));
          ResolveCallback(cb, out);
          return;
        }
      }
    }
  }

  rc->ArmOneShotLocatorCapture(
      base::TimeDelta(),
      /*suppress_recording_side_effects=*/true,
      base::BindOnce(
          [](bool started_passive, base::Value::Dict result) {
            if (started_passive) {
              if (RecorderController* active = RecorderController::GetActive()) {
                active->StopPassiveMode();
              }
            }
          },
          started_passive_for_capture));

  rc->WaitForOneShotCaptureReady(
      base::Seconds(12),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self,
             std::string cb_name,
             bool started_passive,
             bool ready,
             std::string error) {
            if (!self)
              return;
            base::Value::Dict response;
            if (!ready) {
              const std::string normalized_error =
                  error.empty() ? std::string("locator_capture_not_ready")
                                : error;
              LOG(WARNING) << "[Dashboard][CaptureUI] ready=false error="
                           << normalized_error;
              const bool soft_ready_failure =
                  normalized_error == "locator_capture_not_ready" ||
                  normalized_error == "capture_guard_probe_failed";
              if (!soft_ready_failure) {
                if (RecorderController* active = RecorderController::GetActive()) {
                  active->CancelOneShotLocatorCapture(normalized_error);
                  if (started_passive) {
                    active->StopPassiveMode();
                  }
                }
                response.Set("ok", false);
                response.Set("error", normalized_error);
                self->ResolveCallback(cb_name, std::move(response));
                return;
              }
              response.Set("ok", true);
              response.Set("armed", true);
              response.Set("ready", false);
              response.Set("warning", normalized_error);
              response.Set("sequence",
                           static_cast<double>(
                               RecorderController::GetLatestLocatorCaptureSequence()));
              self->ResolveCallback(cb_name, std::move(response));
              return;
            }
            LOG(WARNING) << "[Dashboard][CaptureUI] ready=true";
            response.Set("ok", true);
            response.Set("armed", true);
            response.Set("ready", true);
            response.Set("sequence",
                         static_cast<double>(
                             RecorderController::GetLatestLocatorCaptureSequence()));
            self->ResolveCallback(cb_name, std::move(response));
          },
          weak_ptr_factory_.GetWeakPtr(), cb, started_passive_for_capture));
}

void DashboardUIMessageHandler::HandleCancelLocatorCapture(
    const base::Value::List& args) {
  LOG(WARNING) << "[Dashboard][CaptureUI] cancel";
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  base::Value::Dict out;
  if (RecorderController* rc = RecorderController::GetActive()) {
    rc->CancelOneShotLocatorCapture("locator_capture_canceled");
  }
  out.Set("ok", true);
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandlePollLocatorCapture(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                              : std::string();
  uint64_t last_sequence = 0;
  if (args.size() > 1 && args[1].is_int()) {
    last_sequence = static_cast<uint64_t>(std::max(0, args[1].GetInt()));
  } else if (args.size() > 1 && args[1].is_double()) {
    last_sequence =
        static_cast<uint64_t>(std::max(0.0, args[1].GetDouble()));
  }

  base::Value::Dict out;
  out.Set("ok", true);
  uint64_t sequence = RecorderController::GetLatestLocatorCaptureSequence();
  auto captured = RecorderController::GetLatestLocatorCaptureResult(
      last_sequence, &sequence);
  out.Set("sequence", static_cast<double>(sequence));
  if (!captured) {
    out.Set("hasCapture", false);
    ResolveCallback(cb, out);
    return;
  }
  out.Set("hasCapture", true);
  out.Set("result", std::move(*captured));
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleDebugUserClick(
    const base::Value::List& args) {
  // args[0]: dict { css, xpath, shadow_chain, x, y, frame_id }
  if (args.empty() || !args[0].is_dict())
    return;
  const base::Value::Dict& d = args[0].GetDict();
  base::Value::Dict click = d.Clone();
  if (RecorderController* rc = RecorderController::GetActive()) {
    // Provide a browser-only path to test selector ingestion.
    rc->OnUserClickFromRenderer(std::move(click));
  }
}

void DashboardUIMessageHandler::HandleSaveSnapshot(const base::Value::List& args) {
  // --- Argument Parsing (from your code, which is great) ---
    if (args.size() != 2 || !args[0].is_string() || !args[1].is_dict()) {
        LOG(ERROR) << "[Dashboard] Invalid arguments for dashboardSaveSnapshot";
        return;
      }

      const std::string callback_name = args[0].GetString();
      const base::Value& data_to_save = args[1];

      // 2. Get profile (UI thread operation)
      Profile* profile = Profile::FromWebUI(web_ui());
      if (!profile) {
        LOG(ERROR) << "[Dashboard] Could not get profile.";
        // Call the reply function directly with the error.
        OnSnapshotSaved(callback_name, {false, "Could not get profile"});
        return;
      }
      
      // 3. Get the profile path (cheap, non-blocking)
      base::FilePath profile_path = profile->GetPath();

      // 4. Post ALL blocking work to a background thread
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE,
          // These traits are important:
          // - MayBlock: Tells the scheduler this task will block.
          // - USER_VISIBLE: It's in response to a user action.
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
          // The task to run on a background thread.
          base::BindOnce(&DashboardUIMessageHandler::SaveSnapshotToDisk,
                         profile_path, data_to_save.Clone()),
          // The reply to run back on the UI thread.
          base::BindOnce(&DashboardUIMessageHandler::OnSnapshotSaved,
                         weak_ptr_factory_.GetWeakPtr(), callback_name));
    }



// ----------------------------------------------------------------------
void DashboardUIMessageHandler::NotifyRecordingStopped() {
  for (auto* inst : Instances()) {
    if (!inst)
      continue;
    base::Value::List empty;
    inst->web_ui()->CallJavascriptFunctionUnsafe("DashboardRecordingStopped",
                                                std::move(empty));
  }
}

// static
std::pair<bool, std::string> DashboardUIMessageHandler::SaveSnapshotToDisk(
    base::FilePath profile_path,
    base::Value data_to_save) {
  // This entire function runs on a background thread.
  // We can safely perform blocking I/O here.

  // 1. Get and create the directory
  base::FilePath snapshot_dir =
      profile_path.AppendASCII("Dashboard Snapshots");
  if (!base::CreateDirectory(snapshot_dir)) {
    LOG(ERROR) << "[Dashboard] Failed to create snapshot directory.";
    return {false, "Failed to create snapshot directory"};
  }

  // 2. Generate filename
  std::string timestamp = base::NumberToString(base::Time::Now().ToDeltaSinceWindowsEpoch().InMilliseconds());
  base::FilePath snapshot_file =
      snapshot_dir.AppendASCII("site_map_" + timestamp + ".json");

  // 3. Serialize JSON
  std::string json_string;
  if (!base::JSONWriter::Write(data_to_save, &json_string)) {
    LOG(ERROR) << "[Dashboard] Failed to serialize JSON to string.";
    return {false, "Failed to serialize JSON"};
  }

  // 4. Write the file
  if (base::WriteFile(snapshot_file, json_string) !=
      static_cast<int>(json_string.length())) {
    LOG(ERROR) << "[Dashboard] Failed to write file.";
    return {false, "Failed to write file"};
  }

  // 5. Success!
  return {true, ""};
}

void DashboardUIMessageHandler::OnSnapshotSaved(
    const std::string& callback_name,
    std::pair<bool, std::string> result) {
  // We are back on the UI thread.
  // Check if the handler is still valid.
  if (!web_ui())
    return;

  bool success = result.first;
  std::string error_message = result.second;

  // Execute the JavaScript callback in the dashboard WebUI.
  std::string js_result = success ? "true" : "false";
  // Use GetQuotedJSONString to safely escape the error message.
  std::string js_error =
      success ? "null" : base::GetQuotedJSONString(error_message);

  std::string js =
      base::StringPrintf("if(window['%s']) window['%s'](%s, %s);",
                         callback_name.c_str(), callback_name.c_str(),
                         js_result.c_str(), js_error.c_str());

  ExecuteJavascriptIfAllowed(
      web_ui()->GetWebContents()
          ? web_ui()->GetWebContents()->GetPrimaryMainFrame()
          : nullptr,
      js);
}

// You can now delete the old GetSnapshotDirectory() function.
/*
base::FilePath DashboardUIMessageHandler::GetSnapshotDirectory() {
  // ... This whole function is no longer needed ...
}
*/

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::NotifyScriptsChanged() {
  for (auto* inst : Instances()) {
    if (!inst)
      continue;
    if (!inst->web_ui() || !inst->web_ui()->CanCallJavascript())
      continue;
    content::RenderFrameHost* frame =
        inst->web_ui()->GetWebContents()
            ? inst->web_ui()->GetWebContents()->GetPrimaryMainFrame()
            : nullptr;
    if (!CanExecuteJavascriptOnFrame(frame))
      continue;
    std::string scripts_json = "null";
    if (inst->dashboard_) {
      const base::Value::Dict scripts =
          ::NormalizeScriptsForUI(inst->dashboard_->LoadAllScripts());
      base::JSONWriter::Write(base::Value(scripts.Clone()), &scripts_json);
    }
    // Push the updated script payload directly to avoid an immediate follow-up
    // dashboardGetScripts round trip on every save/delete.
    const std::string js = base::StrCat(
        {"try{if(window&&typeof window.DashboardScriptsChanged==='function'){",
         "window.DashboardScriptsChanged(", scripts_json,
         ");}}catch(e){};"});
    ExecuteJavascriptIfAllowed(frame, js);
  }
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::UpdateRuntimeScriptCache(
    Profile* profile,
    const std::string& id,
    base::Value::Dict script) {
  bool updated_any = false;
  for (auto* inst : Instances()) {
    if (!inst || inst->profile_ != profile || !inst->dashboard_) {
      continue;
    }
    inst->dashboard_->SaveScript(id, script.Clone());
    updated_any = true;
  }
  if (updated_any) {
    NotifyScriptsChanged();
  }
}

// ----------------------------------------------------------------------
void DashboardUIMessageHandler::NotifyListenersChanged() {
  for (auto* inst : Instances()) {
    if (!inst)
      continue;
    if (!inst->web_ui() || !inst->web_ui()->CanCallJavascript())
      continue;
    content::RenderFrameHost* frame =
        inst->web_ui()->GetWebContents()
            ? inst->web_ui()->GetWebContents()->GetPrimaryMainFrame()
            : nullptr;
    if (!CanExecuteJavascriptOnFrame(frame))
      continue;
    const std::string js =
        "try{if(window&&typeof window.DashboardListenersChanged==='function'){"
        "window.DashboardListenersChanged();}}catch(e){};";
    ExecuteJavascriptIfAllowed(frame, js);
  }
}

// ----------------------------------------------------------------------
std::vector<DashboardUIMessageHandler*>& DashboardUIMessageHandler::Instances() {
  return GetInstances();
}

// ----------------------------------------------------------------------
// End of file
// ----------------------------------------------------------------------
