// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/simulachrome_startup.h"

#include "chrome/browser/ui/webui/dashboard/embedded_executor_bridge.h"

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/base_paths.h"
#include "base/path_service.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/common/chrome_switches.h"
#include "build/build_config.h"
#include "content/public/common/content_switches.h"

#include <algorithm>
#include <cstdlib>

namespace simulachrome {
namespace {

constexpr char kPackagedGuestURL[] =
    "chrome://simulachrome-agent/ollama.html";
constexpr char kPackagedFreURL[] =
    "chrome://simulachrome-agent/fre.html";

bool ReadBool(const base::Value::Dict* dict,
              std::string_view key,
              bool default_value) {
  if (!dict) {
    return default_value;
  }
  if (const std::optional<bool> v = dict->FindBool(key)) {
    return *v;
  }
  return default_value;
}

base::FilePath DefaultSimulachromeHome() {
  if (const char* env = std::getenv("SIMULACHROME_HOME"); env && *env) {
    return base::FilePath::FromUTF8Unsafe(env);
  }
  base::FilePath app_data;
  if (base::PathService::Get(
#if BUILDFLAG(IS_WIN)
          base::DIR_LOCAL_APP_DATA
#elif BUILDFLAG(IS_APPLE)
          base::DIR_APP_DATA
#else
          base::DIR_HOME
#endif
          ,
          &app_data) &&
      !app_data.empty()) {
    return app_data.AppendASCII("Simulachrome");
  }
  return base::FilePath();
}

base::FilePath ScriptsDirFromEnvOrDefault() {
  if (const char* env = std::getenv("DASHBOARD_SCRIPTS_DIR"); env && *env) {
    return base::FilePath::FromUTF8Unsafe(env);
  }
  base::FilePath home = DefaultSimulachromeHome();
  if (home.empty()) {
    return base::FilePath();
  }
  return home.AppendASCII("Scripts");
}

base::FilePath HeadlessDirFromEnvOrDefault() {
  if (const char* env = std::getenv("HEADLESS_LOG_DIR"); env && *env) {
    return base::FilePath::FromUTF8Unsafe(env);
  }
  base::FilePath home = DefaultSimulachromeHome();
  if (home.empty()) {
    return base::FilePath();
  }
  return home.AppendASCII("Headless");
}

void EnsureSimulachromeDirs() {
  const base::FilePath scripts = ScriptsDirFromEnvOrDefault();
  const base::FilePath headless = HeadlessDirFromEnvOrDefault();
  if (!scripts.empty()) {
    base::CreateDirectory(scripts);
  }
  if (!headless.empty()) {
    base::CreateDirectory(headless);
    base::CreateDirectory(headless.AppendASCII("headless_shared"));
  }
}

void AppendEnableFeatures(base::CommandLine* cl, const std::string& add) {
  if (add.empty()) {
    return;
  }
  if (!cl->HasSwitch("enable-features")) {
    cl->AppendSwitchASCII("enable-features", add);
    return;
  }
  std::string existing = cl->GetSwitchValueASCII("enable-features");
  std::vector<std::string> parts = base::SplitString(
      existing, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (const auto& piece :
       base::SplitString(add, ",", base::TRIM_WHITESPACE,
                         base::SPLIT_WANT_NONEMPTY)) {
    if (std::find(parts.begin(), parts.end(), piece) == parts.end()) {
      parts.push_back(piece);
    }
  }
  cl->RemoveSwitch("enable-features");
  cl->AppendSwitchASCII("enable-features",
                        base::JoinString(parts, ","));
}

void AppendSwitchIfMissing(base::CommandLine* cl, const char* name) {
  if (!cl->HasSwitch(name)) {
    cl->AppendSwitch(name);
  }
}

void AppendSwitchASCIIIfMissing(base::CommandLine* cl,
                                const char* name,
                                const std::string& value) {
  if (!cl->HasSwitch(name)) {
    cl->AppendSwitchASCII(name, value);
  }
}

void AppendDisableFeatures(base::CommandLine* cl, const std::string& add) {
  if (add.empty()) {
    return;
  }
  if (!cl->HasSwitch("disable-features")) {
    cl->AppendSwitchASCII("disable-features", add);
    return;
  }
  std::string existing = cl->GetSwitchValueASCII("disable-features");
  std::vector<std::string> parts = base::SplitString(
      existing, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (const auto& piece : base::SplitString(add, ",", base::TRIM_WHITESPACE,
                                             base::SPLIT_WANT_NONEMPTY)) {
    if (std::find(parts.begin(), parts.end(), piece) == parts.end()) {
      parts.push_back(piece);
    }
  }
  cl->RemoveSwitch("disable-features");
  cl->AppendSwitchASCII("disable-features", base::JoinString(parts, ","));
}

void ApplySettingsDerivedSwitches(base::CommandLine* cl) {
  const base::FilePath scripts_dir = ScriptsDirFromEnvOrDefault();
  if (scripts_dir.empty()) {
    return;
  }
  const base::FilePath settings_path =
      scripts_dir.AppendASCII("global_settings.json");
  std::string json;
  if (!base::PathExists(settings_path) ||
      !base::ReadFileToString(settings_path, &json) || json.empty()) {
    return;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    return;
  }
  const base::Value::Dict& root = parsed->GetDict();
  const base::Value::Dict* performance = root.FindDict("performance");
  const base::Value::Dict* subsystem = root.FindDict("subsystem");

  if (ReadBool(performance, "aggressivePerformance", false)) {
    AppendSwitchIfMissing(cl, "disable-site-isolation-trials");
    AppendSwitchIfMissing(cl, "process-per-site");
    AppendSwitchASCIIIfMissing(cl, "renderer-process-limit", "4");
  }

  std::vector<std::string> disabled;
  if (ReadBool(performance, "deterministicMode", false)) {
    disabled.push_back("BackForwardCache");
    disabled.push_back("PrerenderFallbackToPreconnect");
    disabled.push_back("InstantUsesSpareRenderer");
  }
  if (!ReadBool(subsystem, "extensionsEnabled", true)) {
    AppendSwitchIfMissing(cl, "disable-extensions");
  }
  if (!ReadBool(subsystem, "printPreviewEnabled", true)) {
    AppendSwitchIfMissing(cl, "disable-print-preview");
  }
  if (ReadBool(subsystem, "printStubEnabled", false)) {
    AppendSwitchIfMissing(cl, "simulachrome-print-stub");
  }
  if (!ReadBool(subsystem, "safeBrowsingEnabled", true)) {
    AppendSwitchIfMissing(cl, "safebrowsing-disable-auto-update");
    AppendSwitchIfMissing(cl, "disable-client-side-phishing-detection");
  }
  if (!ReadBool(subsystem, "serviceDiscoveryEnabled", true)) {
    disabled.push_back("MediaRouter");
  }
  if (!disabled.empty()) {
    if (cl->HasSwitch("disable-features")) {
      std::string existing =
          cl->GetSwitchValueASCII("disable-features");
      for (const auto& piece : base::SplitString(
               existing, ",", base::TRIM_WHITESPACE,
               base::SPLIT_WANT_NONEMPTY)) {
        if (std::find(disabled.begin(), disabled.end(), piece) ==
            disabled.end()) {
          disabled.push_back(piece);
        }
      }
      cl->RemoveSwitch("disable-features");
    }
    cl->AppendSwitchASCII("disable-features",
                          base::JoinString(disabled, ","));
  }

  // Remote debugging is opt-in via Settings (default off).
  if (ReadBool(subsystem, "remoteDebuggingEnabled", false)) {
    AppendSwitchASCIIIfMissing(cl, ::switches::kRemoteDebuggingPort, "9223");
    AppendSwitchASCIIIfMissing(cl, "remote-allow-origins", "*");
  }
}

}  // namespace

void ApplyBrowserCommandLineDefaults() {
  base::CommandLine* cl = base::CommandLine::ForCurrentProcess();
  if (!cl || cl->HasSwitch(switches::kProcessType)) {
    return;
  }

  EnsureSimulachromeDirs();
  InstallEmbeddedExecutorBridge();

  // Isolated headless listener process: skip Glic/agent UI wiring. It has no
  // startup window and only needs ListenerManager + the control socket.
  if (cl->HasSwitch(switches::kHeadlessListener)) {
    ApplySettingsDerivedSwitches(cl);
    VLOG(1) << "Simulachrome headless-listener defaults applied (no Glic)";
    return;
  }

  // One Glic conversation for the whole window. Docking (attached side
  // panel) is allowed; GlicMultiInstance stays off so that docked panel
  // is not recreated per tab. GlicDetached is off so Pop out / Dock work.
  AppendEnableFeatures(
      cl,
      "Glic:glic-initial-width/480/glic-initial-height/640,GlicUseNonClient,"
      "GlicDefaultTabContextSetting");
  AppendDisableFeatures(cl, "GlicMultiInstance,GlicDetached");
  AppendSwitchASCIIIfMissing(cl, switches::kGlicOpenOnStartup, "attached");
  AppendSwitchIfMissing(cl, "disable-backgrounding-occluded-windows");
  AppendSwitchIfMissing(cl, "disable-background-timer-throttling");
  AppendSwitchIfMissing(cl, "disable-renderer-backgrounding");
  AppendSwitchIfMissing(cl, "simulachrome-force-visible-widgets");
  // A covered window is occluded. The switch above keeps the page lifecycle
  // visible, but macOS still App-Naps the process until the window is key.
  HoldForegroundActivity();

  // Packaged agent UI (chrome://simulachrome-agent). Explicit --glic-guest-url
  // or --glic-dev localhost workflows still win.
  if (!cl->HasSwitch(switches::kGlicGuestURL) &&
      !cl->HasSwitch(switches::kGlicDev)) {
    cl->AppendSwitchASCII(switches::kGlicGuestURL, kPackagedGuestURL);
    if (!cl->HasSwitch(switches::kGlicFreURL)) {
      cl->AppendSwitchASCII(switches::kGlicFreURL, kPackagedFreURL);
    }
    // Allow non-Google guest without the localhost-only --glic-dev bypass.
    AppendSwitchIfMissing(cl, "simulachrome-agent");
  }

  ApplySettingsDerivedSwitches(cl);
  VLOG(1) << "Simulachrome browser command-line defaults applied";
}

#if !BUILDFLAG(IS_MAC)
void HoldForegroundActivity() {}
#endif

}  // namespace simulachrome
