// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/glic/host/glic_ui.h"

#include <optional>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/version_info/version_info.h"
#include "build/build_config.h"
#include "chrome/browser/glic/fre/fre_util.h"
#include "chrome/browser/glic/fre/glic_fre_page_handler.h"
#include "chrome/browser/glic/glic_net_log.h"
#include "chrome/browser/glic/host/auth_controller.h"
#include "chrome/browser/glic/host/glic_page_handler.h"
#include "chrome/browser/glic/host/guest_util.h"
#include "chrome/browser/glic/public/glic_enabling.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "chrome/browser/glic/resources/glic_resources.h"
#include "chrome/browser/glic/resources/grit/glic_browser_resources.h"
#include "chrome/browser/glic/shared/webui_shared.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "content/public/common/bindings_policy.h"
#include "chrome/browser/signin/identity_manager_factory.h"
#include "chrome/common/channel_info.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/grit/branded_strings.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/grit/glic_fre_resources.h"
#include "chrome/grit/glic_fre_resources_map.h"
#include "chrome/grit/glic_resources.h"
#include "chrome/grit/glic_resources_map.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/webui/webui_allowlist.h"
#include "ui/webui/webui_util.h"

namespace glic {
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
  return exe.DirName().AppendASCII("Resources").AppendASCII(
      "simulachrome_agent");
#else
  return exe.AppendASCII("simulachrome_agent");
#endif
}

bool ShouldHandleAgentRequest(const std::string& path) {
  return base::StartsWith(path, "agent/");
}

// Packaged ollama_client.js imports serve.py paths. Rewrite them to same-origin
// stubs served from chrome://glic/agent/ so the module actually evaluates.
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
  // Blob guests inherit chrome://glic Trusted Types; innerHTML throws and
  // aborts initialize() before Send / model <select> are wired.
  base::ReplaceSubstringsAfterOffset(data, 0, "this.modelSelectEl.innerHTML = '';",
                                     "this.modelSelectEl.replaceChildren();");
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

// Blob guests inherit chrome://glic script-src, which forbids inline scripts.
// Forward chrome.send to the parent WebUI from an external same-origin file.
constexpr char kAgentParentBridge[] = R"js(
(function() {
  try {
    if (!window.chrome) {
      window.chrome = {};
    }
    var originalSend = window.chrome.send;
    window.chrome.send = function(method, args) {
      try {
        if (window.parent && window.parent !== window && window.parent.chrome &&
            typeof window.parent.chrome.send === 'function') {
          var forwarded = Array.prototype.slice.call(args || []);
          var callbackName = forwarded[0];
          if (typeof callbackName === 'string') {
            var parentCallback = '__sc_bridge_' + callbackName;
            window.parent[parentCallback] = function() {
              var fn = window[callbackName];
              if (typeof fn === 'function') {
                fn.apply(window, arguments);
              }
              try {
                delete window.parent[parentCallback];
              } catch (e) {
              }
            };
            forwarded[0] = parentCallback;
          }
          return window.parent.chrome.send(method, forwarded);
        }
      } catch (e) {
      }
      if (typeof originalSend === 'function') {
        return originalSend.apply(this, arguments);
      }
    };
  } catch (e) {
  }
})();
)js";

void RewriteOllamaHtmlForBlobEmbed(std::string* data) {
  base::ReplaceSubstringsAfterOffset(
      data, 0, "src=\"./ollama_client.js\"",
      "src=\"chrome://glic/agent/ollama_client.js\"");
  base::ReplaceSubstringsAfterOffset(
      data, 0, "src='./ollama_client.js'",
      "src=\"chrome://glic/agent/ollama_client.js\"");
  if (data->find("chrome://glic/agent/parent_bridge.js") == std::string::npos) {
    data->insert(0, "<script src=\"chrome://glic/agent/parent_bridge.js\">"
                    "</script>\n");
  }
}

// Reads a packaged agent file off the UI thread. Virtual stubs never touch disk.
std::optional<std::string> LoadAgentFileContent(const std::string& relative) {
  if (relative == "api_boot.js") {
    return std::string(kAgentApiBootStub);
  }
  if (relative == "glic_api_stub.js") {
    return std::string(kAgentGlicApiStub);
  }
  if (relative == "parent_bridge.js") {
    return std::string(kAgentParentBridge);
  }
  const base::FilePath root = ResolveAgentClientRoot();
  if (root.empty() || relative.empty() ||
      relative.find("..") != std::string::npos) {
    return std::nullopt;
  }
  std::string data;
  if (!base::ReadFileToString(root.AppendASCII(relative), &data)) {
    return std::nullopt;
  }
  if (relative == "ollama_client.js") {
    RewriteOllamaClientImports(&data);
  }
  if (relative == "ollama.html") {
    RewriteOllamaHtmlForBlobEmbed(&data);
  }
  return data;
}

void HandleAgentRequest(const std::string& path,
                        content::WebUIDataSource::GotDataCallback callback) {
  std::string relative = path;
  if (base::StartsWith(relative, "agent/")) {
    relative = relative.substr(6);
  }
  const size_t q = relative.find_first_of("?#");
  if (q != std::string::npos) {
    relative = relative.substr(0, q);
  }
  if (relative.empty() ||
      base::EndsWith(relative, "/", base::CompareCase::SENSITIVE)) {
    relative = "ollama.html";
  }
  if (relative.find("..") != std::string::npos) {
    std::move(callback).Run(nullptr);
    return;
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&LoadAgentFileContent, relative),
      base::BindOnce(
          [](content::WebUIDataSource::GotDataCallback callback,
             std::optional<std::string> data) {
            if (!data) {
              std::move(callback).Run(nullptr);
              return;
            }
            std::move(callback).Run(
                base::MakeRefCounted<base::RefCountedString>(
                    std::move(*data)));
          },
          std::move(callback)));
}

// WebUIImpl destroys the controller before handlers; handlers hold a raw_ptr to
// Dashboard, so the handler must own it.
class OwningDashboardUIMessageHandler : public DashboardUIMessageHandler {
 public:
  OwningDashboardUIMessageHandler(
      Profile* profile,
      std::unique_ptr<dashboard::Dashboard> dashboard)
      : DashboardUIMessageHandler(profile, dashboard.get()),
        owned_dashboard_(std::move(dashboard)) {}
  ~OwningDashboardUIMessageHandler() override {
    // `owned_dashboard_` is destroyed before ~DashboardUIMessageHandler().
    // Clear the base raw_ptr first or Release() CHECKs a dangling pointer
    // and kills the browser when the Glic panel/WebUI is torn down.
    DetachDashboard();
    owned_dashboard_.reset();
  }

  void RegisterMessages() override {
    DashboardUIMessageHandler::RegisterMessages();
    web_ui()->RegisterMessageCallback(
        "dashboardGetAgentHtml",
        base::BindRepeating(
            &OwningDashboardUIMessageHandler::HandleGetAgentHtml,
            base::Unretained(this)));
  }

 private:
  void HandleGetAgentHtml(const base::Value::List& args) {
    AllowJavascript();
    const std::string cb =
        (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                                 : std::string();
    const std::string relative =
        (args.size() > 1 && args[1].is_string() && !args[1].GetString().empty())
            ? args[1].GetString()
            : "ollama.html";
    if (relative.find("..") != std::string::npos) {
      base::Value::Dict err;
      err.Set("ok", false);
      err.Set("error", "invalid path");
      ResolveCallback(cb, err);
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_BLOCKING,
         base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
        base::BindOnce(&LoadAgentFileContent, relative),
        base::BindOnce(
            [](base::WeakPtr<OwningDashboardUIMessageHandler> self,
               std::string cb, std::optional<std::string> data) {
              if (!self) {
                return;
              }
              base::Value::Dict result;
              if (!data) {
                result.Set("ok", false);
                result.Set("error", "agent html missing");
                self->ResolveCallback(cb, result);
                return;
              }
              result.Set("ok", true);
              result.Set("html", std::move(*data));
              self->ResolveCallback(cb, result);
            },
            weak_factory_.GetWeakPtr(), cb));
  }

  std::unique_ptr<dashboard::Dashboard> owned_dashboard_;
  base::WeakPtrFactory<OwningDashboardUIMessageHandler> weak_factory_{this};
};

}  // namespace

// static
bool GlicUI::simulate_no_connection_ = false;

GlicUIConfig::GlicUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, chrome::kChromeUIGlicHost) {}

bool GlicUIConfig::IsWebUIEnabled(content::BrowserContext* browser_context) {
  return GlicEnabling::IsProfileEligible(
      Profile::FromBrowserContext(browser_context));
}

GlicUI::GlicUI(content::WebUI* web_ui) : ui::MojoWebUIController(web_ui) {
  // Enable chrome.send so the glic proxy can invoke dashboardAgentOllamaChat on
  // this already-open host (no chrome://dashboard tab required).
  web_ui->SetBindings(content::kWebUIBindingsPolicySet);
  Profile* profile = Profile::FromWebUI(web_ui);
  auto dashboard = std::make_unique<dashboard::Dashboard>();
  web_ui->AddMessageHandler(std::make_unique<OwningDashboardUIMessageHandler>(
      profile, std::move(dashboard)));

  // chrome://glic/agent/* is a same-origin subframe. It needs bindings + the
  // dashboard handler, but must not recreate the WebUI data source (that
  // would clobber the primary chrome://glic host).
  if (content::RenderFrameHost* rfh = web_ui->GetRenderFrameHost()) {
    if (!rfh->IsInPrimaryMainFrame()) {
      return;
    }
  }

  static constexpr webui::LocalizedString kStrings[] = {
      {"closeButtonLabel", IDS_GLIC_NOTICE_CLOSE_BUTTON_LABEL},
      {"errorNotice", IDS_GLIC_ERROR_NOTICE},
      {"errorNoticeActionButton", IDS_GLIC_ERROR_NOTICE_ACTION_BUTTON},
      {"errorNoticeHeader", IDS_GLIC_ERROR_NOTICE_HEADER},
      {"ineligibleProfileNotice", IDS_GLIC_INELIGIBLE_PROFILE_NOTICE},
      {"ineligibleProfileNoticeActionButton",
       IDS_GLIC_INELIGIBLE_PROFILE_NOTICE_ACTION_BUTTON},
      {"ineligibleProfileNoticeHeader",
       IDS_GLIC_INELIGIBLE_PROFILE_NOTICE_HEADER},
      {"disabledByAdminNotice", IDS_GLIC_DISABLED_BY_ADMIN_NOTICE},
      {"disabledByAdminNoticeCloseButton",
       IDS_GLIC_DISABLED_BY_ADMIN_NOTICE_CLOSE_BUTTON},
      {"disabledByAdminNoticeHeader", IDS_GLIC_DISABLED_BY_ADMIN_NOTICE_HEADER},
      {"offlineNoticeAction", IDS_GLIC_OFFLINE_NOTICE_ACTION},
      {"offlineNoticeActionButton", IDS_GLIC_OFFLINE_NOTICE_ACTION_BUTTON},
      {"offlineNoticeHeader", IDS_GLIC_OFFLINE_NOTICE_HEADER},
      {"signInNotice", IDS_GLIC_SIGN_IN_NOTICE},
      {"signInNoticeActionButton", IDS_GLIC_SIGN_IN_NOTICE_ACTION_BUTTON},
      {"signInNoticeHeader", IDS_GLIC_SIGN_IN_NOTICE_HEADER},
      {"unresponsiveMessage", IDS_GLIC_UNRESPONSIVE_MESSAGE},
  };

  content::BrowserContext* browser_context =
      web_ui->GetWebContents()->GetBrowserContext();

  // Set up the chrome://glic source.
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      browser_context, chrome::kChromeUIGlicHost);

  // Add required resources.
  webui::SetupWebUIDataSource(source, kGlicResources, IDR_GLIC_GLIC_HTML);
  ConfigureSharedWebUISource(*source);

  // Serve packaged agent assets under chrome://glic/agent/* (same-origin as
  // the host so the panel can iframe them without a nested WebUI data source).
  source->SetRequestFilter(base::BindRepeating(&ShouldHandleAgentRequest),
                           base::BindRepeating(&HandleAgentRequest));

  for (const auto& resource : kGlicFreResources) {
    source->AddResourcePath(base::StrCat({"fre/", resource.path}), resource.id);
  }

  // Add localized strings.
  source->AddLocalizedStrings(kStrings);

  // Add parameterized admin notice string.
  source->AddString("disabledByAdminNoticeWithLink",
                    l10n_util::GetStringFUTF16(
                        IDS_GLIC_DISABLED_BY_ADMIN_NOTICE_WITH_LINK,
                        base::UTF8ToUTF16(features::kGlicCaaLinkUrl.Get()),
                        base::UTF8ToUTF16(features::kGlicCaaLinkText.Get())));

  // Register auto-granted permissions.
  auto* allowlist = WebUIAllowlist::GetOrCreate(browser_context);
  allowlist->RegisterAutoGrantedPermission(source->GetOrigin(),
                                           ContentSettingsType::GEOLOCATION);

  auto* command_line = base::CommandLine::ForCurrentProcess();

  source->AddBoolean("loggingEnabled",
                     command_line->HasSwitch(::switches::kGlicHostLogging));

  // Set up guest URL via cli flag or default to finch param value.
  const GURL guest_url = GetGuestURL();
  source->AddString("glicGuestURL", guest_url.spec());
  net_log::LogDummyNetworkRequestForTrafficAnnotation(guest_url,
                                                      net_log::GlicPage::kGlic);
  source->AddBoolean("simulateNoConnection", simulate_no_connection_);

  // Packaged Simulachrome agent guest — skip Gemini web-client handshake /
  // cookie sync expectations in the host WebUI.
  const bool simulachrome_agent_guest =
      command_line->HasSwitch("simulachrome-agent") ||
      guest_url.host() == "simulachrome-agent" ||
      (guest_url.host() == chrome::kChromeUIGlicHost &&
       base::StartsWith(guest_url.path(), "/agent/",
                        base::CompareCase::SENSITIVE));
  source->AddBoolean("simulachromeAgentGuest", simulachrome_agent_guest);
  if (simulachrome_agent_guest) {
    // Packaged agent lives at chrome://simulachrome-agent (separate WebUI with
    // chrome.send). Also allow blob:/self for optional same-origin embeds.
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::FrameSrc,
        "frame-src chrome://simulachrome-agent 'self' blob:;");
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::ChildSrc,
        "child-src chrome://simulachrome-agent 'self' blob:;");
  }

  source->AddResourcePath("glic_logo.svg", GetResourceID(IDR_GLIC_LOGO));

  // Set up guest api source.
  // This comes from 'glic_api_injection' in
  // chrome/browser/resources/glic/BUILD.gn.
  source->AddString(
      "glicGuestAPISource",
      ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
          IDR_GLIC_GLIC_API_IMPL_GLIC_API_INJECTED_CLIENT_ROLLUP_JS));

  std::string allowed_origins =
      command_line->GetSwitchValueASCII(::switches::kGlicAllowedOrigins);
  if (allowed_origins.empty()) {
    allowed_origins = features::kGlicAllowedOriginsOverride.Get();
  }

  // Allow corp origins for @google accounts.
  signin::IdentityManager* identity_manager =
      IdentityManagerFactory::GetForProfile(
          Profile::FromBrowserContext(browser_context));
  if (identity_manager && IsPrimaryAccountGoogleInternal(*identity_manager)) {
    allowed_origins += " https://*.corp.google.com";
  }

  source->AddString("glicAllowedOrigins", allowed_origins);

  bool reload_after_navigation =
      !command_line->HasSwitch(::switches::kGlicSkipReloadAfterNavigation);
  source->AddBoolean("reloadAfterNavigation", reload_after_navigation);

  source->AddBoolean(
      "ignoreOfflineState",
      base::FeatureList::IsEnabled(features::kGlicIgnoreOfflineState));

  source->AddBoolean("enableDebug",
                     base::FeatureList::IsEnabled(features::kGlicDebugWebview));
  const bool enable_automation_tools =
      command_line->HasSwitch(::switches::kGlicDev) ||
      command_line->HasSwitch("simulachrome-agent") ||
      simulachrome_agent_guest;
  source->AddBoolean("enableAutomationTools", enable_automation_tools);
  if (enable_automation_tools) {
    // Dev-only: Lucide icon CDN. Packaged agent guests also need blob: so the
    // blob: iframe can load its module graph (inline scripts stay forbidden).
    std::string script_src =
        "script-src chrome://resources chrome://webui-test 'self' "
        "https://unpkg.com";
    if (simulachrome_agent_guest) {
      script_src += " blob:";
    }
    script_src += ";";
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::ScriptSrc, script_src);
  }

  // Set up for periodic web client responsiveness check and its interval,
  // timeout, and max unresponsive ui time.
  source->AddBoolean(
      "isClientResponsivenessCheckEnabled",
      !simulachrome_agent_guest &&
          base::FeatureList::IsEnabled(features::kGlicClientResponsivenessCheck));
  source->AddInteger("clientResponsivenessCheckIntervalMs",
                     features::kGlicClientResponsivenessCheckIntervalMs.Get());
  source->AddInteger("clientResponsivenessCheckTimeoutMs",
                     features::kGlicClientResponsivenessCheckTimeoutMs.Get());
  source->AddInteger("clientUnresponsiveUiMaxTimeMs",
                     features::kGlicClientUnresponsiveUiMaxTimeMs.Get());
  source->AddBoolean(
      "clientResponsivenessCheckIgnoreWhenDebuggerAttached",
      features::kGlicClientResponsivenessCheckIgnoreWhenDebuggerAttached.Get());
  source->AddBoolean("enableWebClientUnresponsiveMetrics",
                     base::FeatureList::IsEnabled(
                         features::kGlicWebClientUnresponsiveMetrics));
  std::string admin_blocked_redirect_patterns;
  if (base::FeatureList::IsEnabled(features::kGlicCaaGuestError)) {
    admin_blocked_redirect_patterns = command_line->GetSwitchValueASCII(
        ::switches::kGlicAdminRedirectPatterns);
    if (admin_blocked_redirect_patterns.empty()) {
      admin_blocked_redirect_patterns =
          features::kGlicCaaGuestRedirectPatterns.Get();
    }
  }
  source->AddString("adminBlockedRedirectPatterns",
                    admin_blocked_redirect_patterns);

  source->AddString(
      "glicFreURL",
      GetFreURL(Profile::FromBrowserContext(browser_context)).spec());
  source->AddBoolean("isUnifiedFre",
                     GlicEnabling::IsUnifiedFreEnabled(
                         Profile::FromBrowserContext(browser_context)));
  source->AddBoolean("shouldShowFre",
                     !GlicEnabling::HasConsentedForProfile(
                         Profile::FromBrowserContext(browser_context)));
  source->AddInteger("reloadMaxLoadingTimeMs",
                     features::kGlicReloadMaxLoadingTimeMs.Get());
  source->AddBoolean("caaGuestError", base::FeatureList::IsEnabled(
                                          features::kGlicCaaGuestError));
  source->AddBoolean(
      "glicPopupWindowsEnabled",
      base::FeatureList::IsEnabled(features::kGlicPopupWindowsEnabled));
}

WEB_UI_CONTROLLER_TYPE_IMPL(GlicUI)

GlicUI::~GlicUI() = default;

void GlicUI::BindInterface(
    mojo::PendingReceiver<glic::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void GlicUI::BindInterface(
    mojo::PendingReceiver<glic::mojom::FrePageHandlerFactory> receiver) {
  fre_page_factory_receiver_.reset();
  fre_page_factory_receiver_.Bind(std::move(receiver));
}

void GlicUI::CreatePageHandler(
    mojo::PendingReceiver<glic::mojom::PageHandler> receiver,
    mojo::PendingRemote<glic::mojom::Page> page) {
  page_handler_ = std::make_unique<GlicPageHandler>(
      web_ui()->GetWebContents(), std::move(receiver), std::move(page));
}

void GlicUI::CreatePageHandler(
    mojo::PendingReceiver<glic::mojom::FrePageHandler> fre_receiver) {
  fre_page_handler_ = std::make_unique<GlicFrePageHandler>(
      /*is_unified_fre=*/true, web_ui()->GetWebContents(),
      std::move(fre_receiver));
}

}  // namespace glic
