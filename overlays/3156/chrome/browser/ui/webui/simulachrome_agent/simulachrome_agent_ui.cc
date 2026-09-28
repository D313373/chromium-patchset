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
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/bindings_policy.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"

namespace {

base::FilePath ResolveAgentClientRoot() {
  if (const char* env = std::getenv("SIMULACHROME_AGENT_DIR"); env && *env) {
    return base::FilePath(env);
  }
  base::FilePath exe;
  if (!base::PathService::Get(base::DIR_EXE, &exe) || exe.empty()) {
    return base::FilePath();
  }
#if BUILDFLAG(IS_MAC)
  // DIR_EXE is .../Chromium.app/Contents/MacOS → Contents/Resources/...
  // Do not call DirectoryExists here: WebUI construction runs on the UI
  // thread and DCHECK_IS_ON builds abort on blocking disk I/O.
  return exe.DirName().AppendASCII("Resources").AppendASCII(
      "simulachrome_agent");
#else
    return exe.AppendASCII("simulachrome_agent");
#endif
}

// Packaged ollama_client.js imports serve.py paths. Rewrite them to files this
// WebUI actually serves so the module graph evaluates.
void RewriteOllamaClientImports(std::string* data) {
  base::ReplaceSubstringsAfterOffset(data, 0,
                                     "from '/glic/glic_api/glic_api.js'",
                                     "from './glic_api_stub.js'");
  base::ReplaceSubstringsAfterOffset(data, 0,
                                     "from \"/glic/glic_api/glic_api.js\"",
                                     "from './glic_api_stub.js'");
  base::ReplaceSubstringsAfterOffset(data, 0, "from '../api_boot.js'",
                                     "from './api_boot.js'");
  base::ReplaceSubstringsAfterOffset(data, 0, "from \"../api_boot.js\"",
                                     "from './api_boot.js'");
}

constexpr char kAgentApiBootStub[] = R"js(
export function createGlicHostRegistryOnLoad() {
  return new Promise((resolve) => {
    function messageHandler(event) {
      if (event.origin !== 'chrome://glic' || event.source === null) {
        return;
      }
      const bootMessage = event.data;
      if (bootMessage && bootMessage.type === 'glic-bootstrap' &&
          bootMessage.glicApiSource) {
        try {
          const scriptElement = document.createElement('script');
          scriptElement.textContent = bootMessage.glicApiSource;
          document.head.appendChild(scriptElement);
          const bootFunc = window.internalAutoGlicBoot;
          if (typeof bootFunc === 'function') {
            window.removeEventListener('message', messageHandler);
            resolve(bootFunc(event.source));
          }
        } catch (e) {}
      }
    }
    window.addEventListener('message', messageHandler);
  });
}
)js";

constexpr char kAgentGlicApiStub[] = R"js(
export const PanelStateKind = {
  HIDDEN: 0, DETACHED: 1, ATTACHED: 2,
  0: 'HIDDEN', 1: 'DETACHED', 2: 'ATTACHED',
};
export const WebClientMode = {
  TEXT: 0, AUDIO: 1,
  0: 'TEXT', 1: 'AUDIO',
};
export const HostCapability = {
  SCROLL_TO_PDF: 0,
  RESET_SIZE_AND_LOCATION_ON_OPEN: 1,
  GET_MODEL_QUALITY_CLIENT_ID: 2,
  MULTI_INSTANCE: 3,
  UNIFIED_AGENT_ORCHESTRATOR: 4,
  0: 'SCROLL_TO_PDF',
  1: 'RESET_SIZE_AND_LOCATION_ON_OPEN',
  2: 'GET_MODEL_QUALITY_CLIENT_ID',
  3: 'MULTI_INSTANCE',
  4: 'UNIFIED_AGENT_ORCHESTRATOR',
};
)js";

class SimulachromeAgentDataSource : public content::URLDataSource {
 public:
  explicit SimulachromeAgentDataSource(base::FilePath root)
      : root_(std::move(root)) {}
  ~SimulachromeAgentDataSource() override = default;

  std::string GetSource() override { return "simulachrome-agent"; }

  // Allow chrome://glic to embed the agent UI in an iframe. <webview> guests
  // cannot load chrome:// URLs, so the packaged guest uses iframe embedding.
  bool ShouldDenyXFrameOptions() override { return false; }

  std::string GetContentSecurityPolicy(
      network::mojom::CSPDirectiveName directive) override {
    if (directive == network::mojom::CSPDirectiveName::FrameAncestors) {
      return "frame-ancestors chrome://glic chrome://glic-fre 'self';";
    }
    if (directive == network::mojom::CSPDirectiveName::ScriptSrc) {
      return "script-src chrome://resources chrome://webui-test 'self';";
    }
    return content::URLDataSource::GetContentSecurityPolicy(directive);
  }

  void StartDataRequest(const GURL& url,
                        const content::WebContents::Getter& wc_getter,
                        GotDataCallback callback) override {
    std::string path = content::URLDataSource::URLToRequestPath(url);
    // GetLocalizedGuestURL appends ?hl=…; strip query/fragment for file lookup.
    const size_t q = path.find_first_of("?#");
    if (q != std::string::npos) {
      path = path.substr(0, q);
    }
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
    if (path == "api_boot.js") {
      std::string page(kAgentApiBootStub);
      std::move(callback).Run(
          base::MakeRefCounted<base::RefCountedString>(std::move(page)));
      return;
    }
    if (path == "glic_api_stub.js") {
      std::string page(kAgentGlicApiStub);
      std::move(callback).Run(
          base::MakeRefCounted<base::RefCountedString>(std::move(page)));
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

    const base::FilePath file = root_.AppendASCII(path);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
         base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
        base::BindOnce(
            [](base::FilePath file, bool rewrite_imports)
                -> scoped_refptr<base::RefCountedMemory> {
              std::string data;
              if (!base::ReadFileToString(file, &data)) {
                return nullptr;
              }
              if (rewrite_imports) {
                RewriteOllamaClientImports(&data);
              }
              return base::MakeRefCounted<base::RefCountedString>(
                  std::move(data));
            },
            file, path == "ollama_client.js"),
        std::move(callback));
  }

  std::string GetMimeType(const GURL& url) override {
    std::string path = content::URLDataSource::URLToRequestPath(url);
    const size_t q = path.find_first_of("?#");
    if (q != std::string::npos) {
      path = path.substr(0, q);
    }
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

namespace {

// WebUIImpl destroys the controller *before* message handlers. Handlers hold a
// raw_ptr to Dashboard, so the controller must not own the Dashboard. Own it
// from the handler instead so lifetimes stay aligned.
class OwningDashboardUIMessageHandler : public DashboardUIMessageHandler {
 public:
  OwningDashboardUIMessageHandler(
      Profile* profile,
      std::unique_ptr<dashboard::Dashboard> dashboard)
      : DashboardUIMessageHandler(profile, dashboard.get()),
        owned_dashboard_(std::move(dashboard)) {}
  ~OwningDashboardUIMessageHandler() override {
    // See GlicUI: derived unique_ptr dies before the base raw_ptr.
    DetachDashboard();
    owned_dashboard_.reset();
  }

 private:
  std::unique_ptr<dashboard::Dashboard> owned_dashboard_;
};

}  // namespace

SimulachromeAgentUI::SimulachromeAgentUI(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  web_ui->SetBindings(content::kWebUIBindingsPolicySet);
  Profile* profile = Profile::FromWebUI(web_ui);
  auto dashboard = std::make_unique<dashboard::Dashboard>();
  web_ui->AddMessageHandler(std::make_unique<OwningDashboardUIMessageHandler>(
      profile, std::move(dashboard)));
  content::URLDataSource::Add(
      web_ui->GetWebContents()->GetBrowserContext(),
      std::make_unique<SimulachromeAgentDataSource>(ResolveAgentClientRoot()));
}

SimulachromeAgentUI::~SimulachromeAgentUI() = default;
