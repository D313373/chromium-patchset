// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/simulachrome_agent/simulachrome_agent_ui.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/memory/ref_counted_memory.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/bindings_policy.h"

namespace {

base::FilePath ResolveAgentClientRoot() {
  if (const char* env = std::getenv("SIMULACHROME_AGENT_DIR"); env && *env) {
    return base::FilePath(env);
  }
  base::FilePath exe;
  if (base::PathService::Get(base::DIR_EXE, &exe)) {
#if BUILDFLAG(IS_MAC)
    // Chromium.app/Contents/MacOS -> Resources/simulachrome_agent
    base::FilePath resources =
        exe.DirName().DirName().AppendASCII("Resources").AppendASCII(
            "simulachrome_agent");
#else
    base::FilePath resources = exe.AppendASCII("simulachrome_agent");
#endif
    if (base::DirectoryExists(resources)) {
      return resources;
    }
  }
  // Dev fallback: generated test_client next to the build output.
  if (!exe.empty()) {
    base::FilePath gen = exe.DirName().AppendASCII("gen")
                             .AppendASCII("chrome")
                             .AppendASCII("test")
                             .AppendASCII("data")
                             .AppendASCII("webui")
                             .AppendASCII("glic")
                             .AppendASCII("glic")
                             .AppendASCII("test_client");
    if (base::DirectoryExists(gen)) {
      return gen;
    }
  }
  return base::FilePath();
}

class SimulachromeAgentDataSource : public content::URLDataSource {
 public:
  explicit SimulachromeAgentDataSource(base::FilePath root)
      : root_(std::move(root)) {}
  ~SimulachromeAgentDataSource() override = default;

  std::string GetSource() override { return "simulachrome-agent"; }

  void StartDataRequest(const GURL& url,
                        const content::WebContents::Getter& wc_getter,
                        GotDataCallback callback) override {
    std::string path = content::URLDataSource::URLToRequestPath(url);
    while (base::StartsWith(path, "/")) {
      path = path.substr(1);
    }
    if (path.empty() || base::EndsWith(path, "/", base::CompareCase::SENSITIVE)) {
      path = "ollama.html";
    }
    // Basic path traversal guard.
    if (path.find("..") != std::string::npos) {
      std::move(callback).Run(nullptr);
      return;
    }
    if (root_.empty()) {
      static constexpr char kMissing[] =
          "<!doctype html><meta charset=utf-8><title>Simulachrome Agent</title>"
          "<body style='font:14px system-ui;padding:24px'>"
          "<h1>Agent UI not packaged</h1>"
          "<p>Copy the generated Glic test client into "
          "<code>Chromium.app/Contents/Resources/simulachrome_agent/</code> "
          "(or set <code>SIMULACHROME_AGENT_DIR</code>).</p>"
          "<p>From a build tree:</p>"
          "<pre>autoninja -C out/Release "
          "chrome/test/data/webui/glic:generate_test_files\n"
          "rsync -a out/Release/gen/chrome/test/data/webui/glic/glic/test_client/ "
          "out/Release/Chromium.app/Contents/Resources/simulachrome_agent/"
          "</pre>"
          "<p>Then reopen the Glic panel. Hosted providers need only Settings + "
          "Secrets — no local server auto-start.</p>"
          "</body>";
      std::string page(kMissing);
      std::move(callback).Run(
          base::MakeRefCounted<base::RefCountedString>(std::move(page)));
      return;
    }
    base::FilePath file = root_.AppendASCII(path);
    std::string data;
    if (!base::ReadFileToString(file, &data)) {
      std::move(callback).Run(nullptr);
      return;
    }
    std::move(callback).Run(
        base::MakeRefCounted<base::RefCountedString>(std::move(data)));
  }

  std::string GetMimeType(const GURL& url) override {
    std::string path = content::URLDataSource::URLToRequestPath(url);
    if (base::EndsWith(path, ".html", base::CompareCase::INSENSITIVE_ASCII) ||
        path.empty() || path == "/") {
      return "text/html";
    }
    if (base::EndsWith(path, ".js", base::CompareCase::INSENSITIVE_ASCII) ||
        base::EndsWith(path, ".mjs", base::CompareCase::INSENSITIVE_ASCII)) {
      return "text/javascript";
    }
    if (base::EndsWith(path, ".css", base::CompareCase::INSENSITIVE_ASCII)) {
      return "text/css";
    }
    if (base::EndsWith(path, ".svg", base::CompareCase::INSENSITIVE_ASCII)) {
      return "image/svg+xml";
    }
    if (base::EndsWith(path, ".png", base::CompareCase::INSENSITIVE_ASCII)) {
      return "image/png";
    }
    if (base::EndsWith(path, ".json", base::CompareCase::INSENSITIVE_ASCII)) {
      return "application/json";
    }
    return "text/plain";
  }

 private:
  base::FilePath root_;
};

}  // namespace

SimulachromeAgentUI::SimulachromeAgentUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  web_ui->SetBindings(content::kWebUIBindingsPolicySet);
  Profile* profile = Profile::FromWebUI(web_ui);
  agent_chat_dashboard_ = std::make_unique<dashboard::Dashboard>();
  web_ui->AddMessageHandler(std::make_unique<DashboardUIMessageHandler>(
      profile, agent_chat_dashboard_.get()));
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<SimulachromeAgentDataSource>(ResolveAgentClientRoot()));
}

SimulachromeAgentUI::~SimulachromeAgentUI() = default;
