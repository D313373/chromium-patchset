// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/auto_reset.h"
#include "base/base64.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/glic/public/glic_enabling.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/widget/glic_window_controller_impl.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_global_settings.h"
#include "chrome/browser/ui/webui/dashboard/native_run_activation.h"
#include "components/os_crypt/sync/os_crypt.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/webui/dashboard/recorder_controller.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "chrome/browser/ui/webui/dashboard/script_schema.h"
#include "chrome/browser/ui/webui/dashboard/script_store.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "base/strings/strcat.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"

namespace {

bool IsDashboardOrInternalToolingUrl(const GURL& url) {
  if (!url.is_valid()) {
    return false;
  }
  // Agents must never act on the dashboard (or other chrome:// tooling) tab —
  // that is what was navigating chrome://dashboard to Google.
  if (url.SchemeIs("chrome") || url.SchemeIs("chrome-untrusted") ||
      url.SchemeIs("devtools")) {
    return true;
  }
  return false;
}

std::string NormalizeAgentChatProvider(std::string provider) {
  provider = base::ToLowerASCII(
      base::TrimWhitespaceASCII(provider, base::TRIM_ALL));
  if (provider == "ollama") {
    // Legacy settings name → generic local OpenAI-compatible provider.
    return "local";
  }
  if (provider == "cerebras" || provider == "gemini" || provider == "openai" ||
      provider == "anthropic" || provider == "local") {
    return provider;
  }
  return "anthropic";
}

std::string RequiredApiKeyNameForChatProvider(std::string_view provider) {
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
  if (provider == "local") {
    // Optional. Many local servers accept empty auth.
    return "LOCAL_API_KEY";
  }
  return std::string();
}

std::string DecodeSecretIniValueForChat(const std::string& encoded_or_plain) {
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

// Must run on a MayBlock thread (PathExists / ReadFileToString).
std::string ResolveChatProviderApiKeyFromPath(base::FilePath profile_path,
                                              std::string key_name) {
  if (profile_path.empty() || key_name.empty()) {
    return std::string();
  }
  const base::FilePath secrets_path =
      profile_path.AppendASCII("SimulachromeData").AppendASCII("secrets.ini");
  if (base::PathExists(secrets_path)) {
    std::string contents;
    if (base::ReadFileToString(secrets_path, &contents)) {
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
        std::string key = line.substr(0, eq);
        base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
        if (key.size() >= 2 && key.front() == '{' && key.back() == '}') {
          key = key.substr(1, key.size() - 2);
          base::TrimWhitespaceASCII(key, base::TRIM_ALL, &key);
        }
        if (key != key_name) {
          continue;
        }
        std::string raw_value = line.substr(eq + 1);
        base::TrimWhitespaceASCII(raw_value, base::TRIM_ALL, &raw_value);
        const std::string decoded = DecodeSecretIniValueForChat(raw_value);
        if (!decoded.empty()) {
          return decoded;
        }
      }
    }
  }
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> from_env = env->GetVar(key_name);
  if (from_env.has_value() && !from_env->empty()) {
    return *from_env;
  }
  return std::string();
}

content::WebContents* FindPreferredAgentTargetWebContents(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  Browser* browser = chrome::FindLastActiveWithProfile(profile);
  if (!browser || !browser->tab_strip_model()) {
    return nullptr;
  }
  TabStripModel* tabs = browser->tab_strip_model();
  content::WebContents* active = tabs->GetActiveWebContents();
  if (active &&
      !IsDashboardOrInternalToolingUrl(active->GetLastCommittedURL())) {
    return active;
  }
  for (int i = 0; i < tabs->count(); ++i) {
    content::WebContents* wc = tabs->GetWebContentsAt(i);
    if (wc &&
        !IsDashboardOrInternalToolingUrl(wc->GetLastCommittedURL())) {
      return wc;
    }
  }
  return nullptr;
}

content::WebContents* EnsureAgentTargetWebContents(Profile* profile) {
  if (content::WebContents* existing =
          FindPreferredAgentTargetWebContents(profile)) {
    return existing;
  }
  Browser* browser = chrome::FindLastActiveWithProfile(profile);
  if (!browser) {
    return nullptr;
  }
  NavigateParams np(browser, GURL("about:blank"),
                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  np.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&np);
  return np.navigated_or_inserted_contents;
}

std::optional<int> ParseIntLikeValue(const base::Value* value) {
  if (!value) {
    return std::nullopt;
  }
  if (value->is_int()) {
    return value->GetInt();
  }
  if (value->is_double()) {
    return static_cast<int>(value->GetDouble());
  }
  if (value->is_string()) {
    int parsed = 0;
    if (base::StringToInt(value->GetString(), &parsed)) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::optional<int> ReadRequestedTabId(const base::Value::Dict& payload) {
  if (std::optional<int> tab_id = ParseIntLikeValue(payload.Find("tab_id"));
      tab_id.has_value()) {
    return tab_id;
  }
  if (std::optional<int> tab_id = ParseIntLikeValue(payload.Find("tabId"));
      tab_id.has_value()) {
    return tab_id;
  }
  if (const base::Value::Dict* options = payload.FindDict("options")) {
    if (std::optional<int> tab_id =
            ParseIntLikeValue(options->Find("tab_id"));
        tab_id.has_value()) {
      return tab_id;
    }
    if (std::optional<int> tab_id = ParseIntLikeValue(options->Find("tabId"));
        tab_id.has_value()) {
      return tab_id;
    }
  }
  return std::nullopt;
}

content::WebContents* FindWebContentsForTabId(Profile* profile, int tab_id) {
  if (!profile || tab_id <= 0) {
    return nullptr;
  }
  for (Browser* browser : *BrowserList::GetInstance()) {
    if (!browser || browser->profile() != profile) {
      continue;
    }
    TabStripModel* tab_strip = browser->tab_strip_model();
    if (!tab_strip) {
      continue;
    }
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      if (!wc) {
        continue;
      }
      if (sessions::SessionTabHelper::IdForTab(wc).id() == tab_id) {
        return wc;
      }
    }
  }
  return nullptr;
}

bool LooksLikeOauthLoginUrl(const GURL& url) {
  if (!url.is_valid()) {
    return false;
  }
  const std::string host = base::ToLowerASCII(url.host());
  static constexpr std::string_view kExactHosts[] = {
      "accounts.google.com", "accounts.youtube.com", "appleid.apple.com",
      "idmsa.apple.com",     "login.microsoftonline.com",
      "login.live.com",      "login.yahoo.com",
      "facebook.com",        "www.facebook.com",
      "m.facebook.com",      "twitter.com",
      "api.twitter.com",     "x.com",
  };
  for (std::string_view known : kExactHosts) {
    if (host == known) {
      return true;
    }
  }
  if (base::EndsWith(host, ".google.com") &&
      (host.find("accounts") != std::string::npos ||
       host.find("oauth") != std::string::npos)) {
    return true;
  }
  const std::string path = base::ToLowerASCII(url.path());
  return path.find("/o/oauth2") != std::string::npos ||
         path.find("/oauth2") != std::string::npos ||
         path.find("/signin/oauth") != std::string::npos;
}

void ActivateWebContentsWindow(content::WebContents* wc) {
  if (!wc) {
    return;
  }
  Browser* browser = chrome::FindBrowserWithTab(wc);
  if (!browser) {
    return;
  }
  // OAuth popups can exist before BrowserView/widget is ready. Activating
  // then also makes the popup last-active and used to re-dock Glic onto a
  // window with no side-panel entry (CHECK in SidePanelUIBase::Show).
  if (BrowserView* view = BrowserView::GetBrowserViewForBrowser(browser)) {
    if (!view->GetWidget()) {
      return;
    }
  } else if (browser->is_type_popup() || browser->is_type_app_popup()) {
    return;
  }
  dashboard::ActivateWebContentsForNativeScriptRun(wc);
}

base::Value::List BuildAgentTabList(Profile* profile) {
  base::Value::List tabs;
  if (!profile) {
    return tabs;
  }
  for (Browser* browser : *BrowserList::GetInstance()) {
    if (!browser || browser->profile() != profile) {
      continue;
    }
    TabStripModel* strip = browser->tab_strip_model();
    if (!strip) {
      continue;
    }
    const bool popup =
        browser->is_type_popup() || browser->is_type_app_popup();
    const bool window_active =
        browser->window() && browser->window()->IsActive();
    const int window_id = browser->session_id().id();
    for (int i = 0; i < strip->count(); ++i) {
      content::WebContents* wc = strip->GetWebContentsAt(i);
      if (!wc) {
        continue;
      }
      const int tab_id = sessions::SessionTabHelper::IdForTab(wc).id();
      if (tab_id <= 0) {
        continue;
      }
      const GURL& visible = wc->GetVisibleURL().is_empty()
                                ? wc->GetLastCommittedURL()
                                : wc->GetVisibleURL();
      if (IsDashboardOrInternalToolingUrl(visible)) {
        continue;
      }
      base::Value::Dict tab;
      tab.Set("tabId", tab_id);
      tab.Set("tab_id", tab_id);
      tab.Set("windowId", window_id);
      tab.Set("url", visible.spec());
      tab.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
      tab.Set("isActiveInWindow", i == strip->active_index());
      tab.Set("isWindowActive", window_active);
      tab.Set("isPopup", popup);
      tab.Set("isOauth", LooksLikeOauthLoginUrl(visible));
      tabs.Append(std::move(tab));
    }
  }
  return tabs;
}


// Convert OpenAI-style multimodal content (string or list with image_url) into
// Anthropic Messages API content blocks (text + base64 image).
bool ParseDataImageUrl(const std::string& url,
                       std::string* media_type,
                       std::string* data) {
  if (!media_type || !data) {
    return false;
  }
  static constexpr std::string_view kPrefix = "data:";
  if (!base::StartsWith(url, kPrefix)) {
    return false;
  }
  const size_t comma = url.find(',');
  if (comma == std::string::npos || comma <= kPrefix.size()) {
    return false;
  }
  const std::string meta = url.substr(kPrefix.size(), comma - kPrefix.size());
  *data = url.substr(comma + 1);
  if (data->empty()) {
    return false;
  }
  const size_t semi = meta.find(';');
  *media_type = semi == std::string::npos ? meta : meta.substr(0, semi);
  base::TrimWhitespaceASCII(*media_type, base::TRIM_ALL, media_type);
  return !media_type->empty();
}

base::Value::List BuildAnthropicContentParts(const base::Value& content_value) {
  base::Value::List parts;
  auto append_text = [&parts](const std::string& text) {
    if (text.empty()) {
      return;
    }
    base::Value::Dict text_part;
    text_part.Set("type", "text");
    text_part.Set("text", text);
    parts.Append(std::move(text_part));
  };
  auto append_image = [&parts](const std::string& media_type,
                               const std::string& data) {
    if (media_type.empty() || data.empty()) {
      return;
    }
    base::Value::Dict source;
    source.Set("type", "base64");
    source.Set("media_type", media_type);
    source.Set("data", data);
    base::Value::Dict image_part;
    image_part.Set("type", "image");
    image_part.Set("source", std::move(source));
    parts.Append(std::move(image_part));
  };

  if (content_value.is_string()) {
    append_text(content_value.GetString());
    return parts;
  }
  if (!content_value.is_list()) {
    return parts;
  }
  for (const auto& part_value : content_value.GetList()) {
    if (part_value.is_string()) {
      append_text(part_value.GetString());
      continue;
    }
    if (!part_value.is_dict()) {
      continue;
    }
    const base::Value::Dict& part = part_value.GetDict();
    const std::string* type = part.FindString("type");
    const std::string type_str = type ? *type : std::string();
    if (type_str == "text" || type_str.empty()) {
      if (const std::string* text = part.FindString("text")) {
        append_text(*text);
      }
      continue;
    }
    if (type_str == "image_url") {
      const base::Value::Dict* image_url = part.FindDict("image_url");
      const std::string* url =
          image_url ? image_url->FindString("url") : part.FindString("url");
      if (!url) {
        continue;
      }
      std::string media_type;
      std::string data;
      if (ParseDataImageUrl(*url, &media_type, &data)) {
        append_image(media_type, data);
      }
      continue;
    }
    if (type_str == "image") {
      const base::Value::Dict* source = part.FindDict("source");
      if (!source) {
        continue;
      }
      const std::string* media_type = source->FindString("media_type");
      const std::string* data = source->FindString("data");
      if (media_type && data) {
        append_image(*media_type, *data);
      }
    }
  }
  return parts;
}

}  // namespace

namespace {

// Agent screenshots must stay in the same DIP space as SetPositionInWidget.
// Never downscale the copy: a 1280-wide bitmap of a ~1440–1920 DIP view plus
// the guest's old 2x-retina guess mapped every click into the top-left
// quadrant. Callback is (png_b64, bitmap_w, bitmap_h, css_w, css_h, error).
void CaptureWebContentsPngWithRetry(
    base::WeakPtr<content::WebContents> web_contents,
    int attempts_left,
    base::OnceCallback<void(std::string, int, int, int, int, std::string)>
        done) {
  content::WebContents* wc = web_contents.get();
  if (!wc) {
    std::move(done).Run(std::string(), 0, 0, 0, 0, "tab closed");
    return;
  }
  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  const bool surface_ready = view && view->IsSurfaceAvailableForCopy();
  if (!view || (!surface_ready && attempts_left > 3)) {
    if (attempts_left <= 0) {
      std::move(done).Run(std::string(), 0, 0, 0, 0,
                          view ? "surface unavailable" : "no render view");
      return;
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&CaptureWebContentsPngWithRetry, web_contents,
                       attempts_left - 1, std::move(done)),
        base::Milliseconds(250));
    return;
  }
  const gfx::Size view_dip = view->GetViewBounds().size();
  gfx::Size output = view_dip;
  if (output.IsEmpty()) {
    std::move(done).Run(std::string(), 0, 0, 0, 0, "empty view bounds");
    return;
  }
  view->CopyFromSurface(
      gfx::Rect(), output,
      base::BindOnce(
          [](base::WeakPtr<content::WebContents> retry_wc, int attempts_left,
             gfx::Size view_dip,
             base::OnceCallback<void(std::string, int, int, int, int,
                                     std::string)> inner,
             const viz::CopyOutputBitmapWithMetadata& result) {
            if (result.bitmap.drawsNothing()) {
              if (attempts_left > 0) {
                base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
                    FROM_HERE,
                    base::BindOnce(&CaptureWebContentsPngWithRetry, retry_wc,
                                   attempts_left - 1, std::move(inner)),
                    base::Milliseconds(250));
                return;
              }
              std::move(inner).Run(std::string(), 0, 0, view_dip.width(),
                                   view_dip.height(), "empty bitmap");
              return;
            }
            std::optional<std::vector<uint8_t>> png =
                gfx::PNGCodec::EncodeBGRASkBitmap(
                    result.bitmap, /*discard_transparency=*/false);
            if (!png) {
              std::move(inner).Run(std::string(), 0, 0, view_dip.width(),
                                   view_dip.height(), "png encode failed");
              return;
            }
            std::move(inner).Run(base::Base64Encode(*png), result.bitmap.width(),
                                 result.bitmap.height(), view_dip.width(),
                                 view_dip.height(), std::string());
          },
          web_contents, attempts_left, view_dip, std::move(done)));
}

constexpr char kScrapeAgentPageContextScript[] = R"js(
(() => {
  const vw = window.innerWidth || 1;
  const vh = window.innerHeight || 1;
  const cssEscape = (value) => {
    try {
      if (window.CSS && CSS.escape) {
        return CSS.escape(value);
      }
    } catch (e) {}
    return String(value).replace(/[^a-zA-Z0-9_-]/g, '\\$&');
  };
  const clsOf = (el) =>
      (typeof el.className === 'string' ? el.className : '').toLowerCase();
  const idOf = (el) => ((el.id || '') + '').toLowerCase();
  const roleOf = (el) =>
      ((el.getAttribute && el.getAttribute('role')) || '').toLowerCase();
  const overlayBlob = (el) => roleOf(el) + ' ' + clsOf(el) + ' ' + idOf(el);
  const looksStrongOverlayName = (el) =>
      /(dialog|alertdialog|modal|popup|popover|overlay|drawer|sheet)/.test(
          overlayBlob(el));
  const looksWeakOverlayName = (el) =>
      /(store|banner|toast|cookie|consent)/.test(overlayBlob(el));
  const isLaidOut = (el) => {
    if (!el || !el.getBoundingClientRect) {
      return false;
    }
    try {
      if (window.getComputedStyle(el).display === 'none') {
        return false;
      }
    } catch (e) {
      return false;
    }
    return true;
  };
  const rectOf = (el) => {
    const r = el.getBoundingClientRect();
    return {
      x: r.x, y: r.y, w: r.width, h: r.height,
      area: r.width * r.height
    };
  };
  const hasHitBox = (r) => r.w >= 2 && r.h >= 2;
  const inViewport = (r) =>
      r.x < vw + 8 && r.y < vh + 8 && (r.x + r.w) > -8 && (r.y + r.h) > -8;
  const isOffscreenBait = (r) =>
      (r.x + r.w) < -80 || (r.y + r.h) < -80 || r.x > vw + 80 || r.y > vh + 80;
  const labelFor = (el) => {
    if (!el) {
      return null;
    }
    if (el.labels && el.labels[0]) {
      return el.labels[0];
    }
    if (el.id) {
      try {
        return document.querySelector('label[for="' + cssEscape(el.id) + '"]');
      } catch (e) {
        return document.querySelector('label[for="' + el.id + '"]');
      }
    }
    return el.closest ? el.closest('label') : null;
  };
  const overlayRoots = [];
  const rememberRoot = (n) => {
    if (!n || overlayRoots.indexOf(n) >= 0) {
      return;
    }
    const strong = looksStrongOverlayName(n) || roleOf(n) === 'dialog' ||
                   roleOf(n) === 'alertdialog' ||
                   (n.getAttribute && n.getAttribute('aria-modal') === 'true');
    if (!isLaidOut(n)) {
      if (strong) {
        overlayRoots.push(n);
      }
      return;
    }
    const r = rectOf(n);
    let st = null;
    try {
      st = window.getComputedStyle(n);
    } catch (e) {}
    const z = st ? parseInt(st.zIndex, 10) : 0;
    const pos = st ? st.position : '';
    const layered = (pos === 'fixed' || pos === 'absolute') &&
                    ((z || 0) >= 1 || strong);
    const covering = r.w * r.h > vw * vh * 0.18;
    if (r.w < 8 && r.h < 8) {
      if (strong) {
        overlayRoots.push(n);
      }
      return;
    }
    if (strong || covering || (layered && looksWeakOverlayName(n)) ||
        (looksWeakOverlayName(n) && r.w > 200 && r.h > 200)) {
      overlayRoots.push(n);
    }
  };
  document.querySelectorAll(
      '[role="dialog"],[role="alertdialog"],[aria-modal="true"],' +
      '[class*="modal"],[class*="dialog"],[class*="popup"],[class*="popover"],' +
      '[class*="overlay"],[class*="drawer"],[class*="store"],[class*="sheet"],' +
      '[class*="banner"],[class*="toast"],[class*="cookie"]')
      .forEach(rememberRoot);
  const scanLayered = (root, depth) => {
    if (!root || !root.children || depth > 3) {
      return;
    }
    Array.from(root.children).forEach((n) => {
      const st = window.getComputedStyle(n);
      if (st.display === 'none') {
        scanLayered(n, depth + 1);
        return;
      }
      const z = parseInt(st.zIndex, 10);
      const pos = st.position;
      const r = rectOf(n);
      if ((pos === 'fixed' || pos === 'absolute' || pos === 'sticky') &&
          ((z || 0) >= 2 || looksStrongOverlayName(n) ||
           looksWeakOverlayName(n)) &&
          inViewport(r) && (r.w > 40 || r.h > 40)) {
        rememberRoot(n);
      }
      scanLayered(n, depth + 1);
    });
  };
  if (document.body) {
    scanLayered(document.body, 0);
  }
  const inOverlay = (el) => {
    let n = el;
    while (n && n !== document.body) {
      if (overlayRoots.indexOf(n) >= 0 || looksStrongOverlayName(n) ||
          roleOf(n) === 'tablist') {
        return true;
      }
      if (n.getAttribute && n.getAttribute('aria-modal') === 'true') {
        return true;
      }
      n = n.parentElement;
    }
    return false;
  };
  const siblingHit = (el) => {
    const p = el && el.parentElement;
    if (!p) {
      return null;
    }
    const kids = Array.from(p.children);
    for (let i = 0; i < kids.length; i++) {
      const sib = kids[i];
      if (sib === el || !isLaidOut(sib)) {
        continue;
      }
      const r = rectOf(sib);
      if (!hasHitBox(r) || r.w > vw * 0.7 || r.h > vh * 0.7) {
        continue;
      }
      if (String(sib.innerText || '').trim().length >= 1) {
        return sib;
      }
    }
    return null;
  };
  const hitTarget = (el) => {
    if (!el) {
      return null;
    }
    if (isLaidOut(el) && hasHitBox(rectOf(el))) {
      return el;
    }
    const lab = labelFor(el);
    if (lab && isLaidOut(lab) && hasHitBox(rectOf(lab))) {
      return lab;
    }
    const sib = siblingHit(el);
    if (sib) {
      return sib;
    }
    let n = el.parentElement;
    for (let i = 0; i < 4 && n && n !== document.body; i++) {
      if (isLaidOut(n) && hasHitBox(rectOf(n))) {
        const r = rectOf(n);
        if (r.w < vw * 0.96 && r.h < vh * 0.96) {
          return n;
        }
      }
      n = n.parentElement;
    }
    return null;
  };
  const oauthClassOf = (el) => {
    if (!el) {
      return '';
    }
    const hit = clsOf(el).trim().split(/\s+/).find(
        (c) => /^(google-btn|apple-btn|facebook-btn|gsi-material-button)$/i
                   .test(c) ||
               /(google-btn|apple-btn|facebook-btn)/i.test(c));
    return hit || '';
  };
  const looksOauthHost = (el) => {
    if (!el) {
      return false;
    }
    if (oauthClassOf(el)) {
      return true;
    }
    const tag = ((el.tagName || '') + '').toLowerCase();
    return /google-auth|apple-auth|facebook-auth/.test(tag);
  };
  const oauthHost = (el) => {
    let n = el;
    let found = null;
    for (let i = 0; i < 6 && n && n !== document.body; i++) {
      if (looksOauthHost(n)) {
        if (oauthClassOf(n)) {
          return n;
        }
        found = found || n;
      }
      n = n.parentElement;
    }
    return found;
  };
  const selector = [
    'a[href]', 'button', 'input', 'select', 'textarea', 'label',
    '[role="button"]', '[role="link"]', '[role="textbox"]',
    '[role="checkbox"]', '[role="radio"]', '[role="tab"]',
    '[role="menuitem"]', '[contenteditable="true"]',
    '[class*="store-tab"]', '[class*="tab-option"]', '[id*="store_tab"]',
    '[class*="close"]', '[aria-label*="close" i]', '[aria-label*="dismiss" i]',
    '[class*="google-btn"]', '[class*="apple-btn"]', '[class*="facebook-btn"]',
    'app-google-auth-button', 'app-apple-auth-button', 'app-facebook-auth-button'
  ].join(',');
  const raw = Array.from(document.querySelectorAll(selector));
  const candidates = [];
  const seenTarget = new Set();
  for (const el of raw) {
    if (!isLaidOut(el) && !labelFor(el)) {
      continue;
    }
    const type = ((el.getAttribute && el.getAttribute('type')) || '').toLowerCase();
    if (type === 'hidden') {
      continue;
    }
    let target = hitTarget(el);
    const host = oauthHost(el) || oauthHost(target);
    if (host) {
      const hostBtn = host.querySelector &&
                      host.querySelector('button, [role="button"], a[href]');
      if (hostBtn && isLaidOut(hostBtn) && hasHitBox(rectOf(hostBtn))) {
        target = hostBtn;
      } else if (isLaidOut(host) && hasHitBox(rectOf(host))) {
        target = host;
      }
    }
    if (!target || seenTarget.has(target)) {
      continue;
    }
    const r = rectOf(target);
    if (!hasHitBox(r) || isOffscreenBait(r)) {
      continue;
    }
    if (r.w < 8 && r.h < 8 &&
        String(target.innerText || '').trim().length < 2) {
      continue;
    }
    seenTarget.add(target);
    const text = String(
        target.innerText || el.getAttribute('aria-label') ||
        target.getAttribute('aria-label') ||
        el.getAttribute('placeholder') || el.getAttribute('value') ||
        el.getAttribute('name') || '').trim().replace(/\s+/g, ' ').slice(0, 160);
    let css = '';
    let cssClass = oauthClassOf(host) || oauthClassOf(el) || oauthClassOf(target);
    if (!cssClass) {
      let n = host || target || el;
      for (let i = 0; i < 6 && n && n !== document.body && !cssClass; i++) {
        cssClass = oauthClassOf(n);
        n = n.parentElement;
      }
    }
    if (cssClass) {
      css = '.' + cssEscape(cssClass);
      if (target && target.tagName &&
          /^(BUTTON|A)$/.test(target.tagName)) {
        css += ' ' + target.tagName.toLowerCase();
      }
    } else if (el.id) {
      css = '#' + cssEscape(el.id);
    } else if (target.id) {
      css = '#' + cssEscape(target.id);
    } else if (target.getAttribute && target.getAttribute('for')) {
      css = 'label[for="' + target.getAttribute('for') + '"]';
    } else if (el.getAttribute && el.getAttribute('data-testid')) {
      css = '[data-testid="' + el.getAttribute('data-testid') + '"]';
    } else if (el.getAttribute && el.getAttribute('name')) {
      css = el.tagName.toLowerCase() + '[name="' + el.getAttribute('name') + '"]';
    } else {
      const cls = clsOf(target).trim().split(/\s+/).filter(Boolean).slice(0, 2);
      css = target.tagName.toLowerCase() +
            (cls.length ? '.' + cls.map(cssEscape).join('.') : '');
    }
    const overlay = inOverlay(target) || inOverlay(el);
    const dismiss = /^(×|x|close|dismiss|not now|no thanks|got it|accept|agree|continue|skip)$/i
                        .test(text);
    const tabby = type === 'radio' || type === 'checkbox' ||
                  roleOf(el) === 'tab' || roleOf(target) === 'tab' ||
                  /tab|bonus|play for|gold coin/i.test(text + ' ' + css);
    let score = 0;
    if (overlay) {
      score += 100;
    }
    if (inViewport(r)) {
      score += 40;
    }
    if (text) {
      score += 30;
    }
    if (dismiss) {
      score += 35;
    }
    if (tabby) {
      score += 25;
    }
    if (cssClass) {
      score += 40;
    }
    if (r.area >= 400) {
      score += 10;
    }
    candidates.push({
      el: el,
      target: target,
      overlay: overlay,
      score: score,
      text: text,
      type: type,
      css: css,
      cssClass: cssClass,
      x: Math.round(r.x),
      y: Math.round(r.y),
      w: Math.round(r.w),
      h: Math.round(r.h)
    });
  }
  candidates.sort((a, b) => b.score - a.score);
  const overlayOpen = overlayRoots.length > 0 ||
                      candidates.some((c) => c.overlay);
  const overlayCap = overlayOpen ? 100 : 60;
  const pageCap = overlayOpen ? 20 : 60;
  let overlayUsed = 0;
  let pageUsed = 0;
  const elements = [];
  const seen = new Set();
  for (const c of candidates) {
    if (c.overlay) {
      if (overlayUsed >= overlayCap) {
        continue;
      }
    } else if (pageUsed >= pageCap) {
      continue;
    }
    const key = c.css + '@' + c.x + ',' + c.y + ':' + c.text.toLowerCase();
    if (seen.has(key)) {
      continue;
    }
    seen.add(key);
    const locators = [
      {type: 'CSS_SELECTOR', value: c.css},
      {type: 'CSS', value: c.css}
    ];
    if (c.cssClass) {
      locators.unshift(
          {type: 'CSS_SELECTOR', value: '.' + cssEscape(c.cssClass) + ' button'},
          {type: 'CSS_SELECTOR', value: '.' + cssEscape(c.cssClass)});
    }
    if (c.el !== c.target && c.el.id) {
      locators.push({type: 'CSS_SELECTOR', value: '#' + cssEscape(c.el.id)});
    }
    if (c.target.getAttribute && c.target.getAttribute('for')) {
      locators.push({
        type: 'CSS_SELECTOR',
        value: 'label[for="' + c.target.getAttribute('for') + '"]'
      });
    }
    elements.push({
      id: elements.length + 1,
      label: c.text,
      role: roleOf(c.target) || roleOf(c.el) || c.target.tagName.toLowerCase(),
      formType: c.type || (c.el.getAttribute && c.el.getAttribute('type')) || '',
      fieldName: (c.el.getAttribute && (c.el.getAttribute('name') || c.el.id)) ||
                 (c.target.getAttribute && c.target.getAttribute('for')) || '',
      cssClass: c.cssClass || '',
      clickable: true,
      focusable: true,
      overlay: !!c.overlay,
      editable: !!(c.target.isContentEditable ||
                   /^(INPUT|TEXTAREA|SELECT)$/.test(c.el.tagName)),
      visible: true,
      bounds: {x: c.x, y: c.y, width: c.w, height: c.h},
      locators: locators
    });
    if (c.overlay) {
      overlayUsed++;
    } else {
      pageUsed++;
    }
  }
  let overlayText = '';
  overlayRoots.forEach((root) => {
    overlayText += ((root.innerText || '') + '\n');
  });
  const bodyText = (document.body && document.body.innerText) || '';
  const inner = (overlayText + '\n' + bodyText).trim().slice(0, 12000);
  return {
    innerText: inner,
    elements: elements,
    href: location.href || '',
    title: document.title || '',
    readyState: document.readyState || '',
    hasOverlay: overlayRoots.length > 0 || overlayUsed > 0,
    viewportCss: {width: vw, height: vh}
  };
})()
)js";

base::Value::Dict BuildAgentPageContextDict(int tab_id_value,
                                            std::string page_url,
                                            std::string page_title,
                                            base::Value::List apc_elements,
                                            base::Value result) {
  std::string inner;
  base::Value::List js_elements;
  bool has_overlay = false;
  if (result.is_dict()) {
    const base::Value::Dict& dict = result.GetDict();
    if (const std::string* text = dict.FindString("innerText")) {
      inner = *text;
    }
    if (const base::Value::List* scraped = dict.FindList("elements")) {
      js_elements = scraped->Clone();
    }
    has_overlay = dict.FindBool("hasOverlay").value_or(false);
    if (page_url.empty() || page_url == "about:blank") {
      if (const std::string* href = dict.FindString("href")) {
        page_url = *href;
      }
    }
    if (page_title.empty()) {
      if (const std::string* title = dict.FindString("title")) {
        page_title = *title;
      }
    }
  } else if (result.is_string()) {
    inner = result.GetString();
  }
  if (!has_overlay) {
    for (const base::Value& el : js_elements) {
      if (el.is_dict() && el.GetDict().FindBool("overlay").value_or(false)) {
        has_overlay = true;
        break;
      }
    }
  }
  base::Value::List elements =
      !apc_elements.empty() ? std::move(apc_elements) : std::move(js_elements);
  base::Value::Dict out;
  out.Set("ok", true);
  out.Set("tab_id", tab_id_value);
  out.Set("tabId", tab_id_value);
  out.Set("url", page_url);
  out.Set("title", page_title);
  out.Set("documentId",
          base::StringPrintf("%d:%s", tab_id_value, page_url.c_str()));
  out.Set("innerText", inner);
  out.Set("hasOverlay", has_overlay);
  out.Set("element_count", static_cast<int>(elements.size()));
  out.Set("elements", std::move(elements));
  if (result.is_dict()) {
    const base::Value::Dict* vp = result.GetDict().FindDict("viewportCss");
    int css_w = 0;
    int css_h = 0;
    if (vp) {
      css_w = vp->FindInt("width").value_or(0);
      css_h = vp->FindInt("height").value_or(0);
      if (css_w <= 0) {
        css_w = static_cast<int>(vp->FindDouble("width").value_or(0));
      }
      if (css_h <= 0) {
        css_h = static_cast<int>(vp->FindDouble("height").value_or(0));
      }
    }
    if (css_w > 0 && css_h > 0) {
      base::Value::Dict css;
      css.Set("width", css_w);
      css.Set("height", css_h);
      out.Set("viewportCss", std::move(css));
      out.Set("viewportWidth", css_w);
      out.Set("viewportHeight", css_h);
    }
  }
  return out;
}

glic::GlicWindowControllerImpl* SingleGlicWindow(Profile* profile) {
  glic::GlicKeyedService* glic = glic::GlicKeyedService::Get(profile);
  if (!glic || glic::GlicEnabling::IsMultiInstanceEnabled()) {
    return nullptr;
  }
  return static_cast<glic::GlicWindowControllerImpl*>(
      &glic->GetSingleInstanceWindowController());
}

}  // namespace

void DashboardUIMessageHandler::HandleAgentAttachPanel(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  glic::GlicWindowControllerImpl* window = SingleGlicWindow(profile_);
  if (!window) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("error", "glic unavailable");
    ResolveCallback(cb, out);
    return;
  }
  window->Attach();
  base::Value::Dict out;
  const bool detached = window->IsDetached();
  out.Set("ok", !detached);
  out.Set("detached", detached);
  if (detached) {
    out.Set("error", "attach failed");
  }
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentDetachPanel(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  glic::GlicWindowControllerImpl* window = SingleGlicWindow(profile_);
  if (!window) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("error", "glic unavailable");
    ResolveCallback(cb, out);
    return;
  }
  window->Detach();
  base::Value::Dict out;
  out.Set("ok", true);
  out.Set("detached", window->IsDetached());
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentGetPanelState(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  glic::GlicKeyedService* glic = glic::GlicKeyedService::Get(profile_);
  base::Value::Dict out;
  out.Set("ok", true);
  out.Set("showing", glic && glic->IsWindowShowing());
  out.Set("detached", glic && glic->IsWindowDetached());
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentOpenTab(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;

  std::string url_str;
  if (payload) {
    if (const std::string* url = payload->FindString("url")) {
      url_str = *url;
    }
  }
  base::TrimWhitespaceASCII(url_str, base::TRIM_ALL, &url_str);
  GURL url(url_str.empty() ? "about:blank" : url_str);
  base::Value::Dict out;
  if (!url.is_valid() || IsDashboardOrInternalToolingUrl(url)) {
    out.Set("ok", false);
    out.Set("error", "invalid or internal url");
    ResolveCallback(cb, out);
    return;
  }

  // Same path the --glic-dev guest uses via browser.createTab(): open a
  // foreground tab and do not touch the Glic panel. The panel is one
  // floating instance for the whole browser.
  NavigateParams np(profile_, url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  np.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&np);
  content::WebContents* wc = np.navigated_or_inserted_contents;
  if (!wc) {
    out.Set("ok", false);
    out.Set("error", "failed to open tab");
    ResolveCallback(cb, out);
    return;
  }

  const int tab_id = sessions::SessionTabHelper::IdForTab(wc).id();
  out.Set("ok", true);
  out.Set("url", wc->GetVisibleURL().spec());
  if (tab_id > 0) {
    out.Set("tab_id", tab_id);
    out.Set("tabId", tab_id);
  }
  LOG(INFO) << "[Agent] Opened task tab id=" << tab_id
            << " url=" << wc->GetVisibleURL().spec();
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentGetActivePageInfo(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::Dict out;
  // Prefer the last active browser. This avoids returning the dashboard
  // WebUI itself when it is not the tab the user is actually acting on.
  if (Browser* browser = chrome::FindLastActiveWithProfile(profile_)) {
    content::WebContents* wc = browser->tab_strip_model()->GetActiveWebContents();
    if (wc) {
      out.Set("url", wc->GetLastCommittedURL().spec());
      out.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
      const int tab_id = sessions::SessionTabHelper::IdForTab(wc).id();
      if (tab_id > 0) {
        out.Set("tab_id", tab_id);
        out.Set("tabId", tab_id);
      }
    }
  }
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentListTabs(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::Dict out;
  out.Set("ok", true);
  out.Set("tabs", BuildAgentTabList(profile_));
  ResolveCallback(cb, std::move(out));
}

void DashboardUIMessageHandler::HandleAgentActivateTab(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;
  const std::optional<int> requested_tab_id =
      payload ? ReadRequestedTabId(*payload) : std::nullopt;
  base::Value::Dict out;
  if (!requested_tab_id.has_value()) {
    out.Set("ok", false);
    out.Set("error", "missing tab_id");
    ResolveCallback(cb, std::move(out));
    return;
  }
  content::WebContents* wc =
      FindWebContentsForTabId(profile_, *requested_tab_id);
  if (!wc) {
    out.Set("ok", false);
    out.Set("error", base::StringPrintf("no tab found for tab_id=%d",
                                        *requested_tab_id));
    ResolveCallback(cb, std::move(out));
    return;
  }
  ActivateWebContentsWindow(wc);
  const GURL& visible = wc->GetVisibleURL().is_empty()
                            ? wc->GetLastCommittedURL()
                            : wc->GetVisibleURL();
  out.Set("ok", true);
  out.Set("tabId", *requested_tab_id);
  out.Set("tab_id", *requested_tab_id);
  out.Set("url", visible.spec());
  out.Set("title", base::UTF16ToUTF8(wc->GetTitle()));
  out.Set("isOauth", LooksLikeOauthLoginUrl(visible));
  ResolveCallback(cb, std::move(out));
}

void DashboardUIMessageHandler::HandleAgentGetPageContext(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;
  const std::optional<int> requested_tab_id =
      payload ? ReadRequestedTabId(*payload) : std::nullopt;
  content::WebContents* wc = requested_tab_id.has_value()
                                 ? FindWebContentsForTabId(profile_,
                                                           *requested_tab_id)
                                 : FindPreferredAgentTargetWebContents(profile_);
  if (wc && IsDashboardOrInternalToolingUrl(wc->GetLastCommittedURL())) {
    wc = FindPreferredAgentTargetWebContents(profile_);
  }
  if (!wc) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("error", "no task tab");
    ResolveCallback(cb, out);
    return;
  }
  {
    Browser* browser = chrome::FindBrowserWithTab(wc);
    const bool popup = browser &&
                       (browser->is_type_popup() || browser->is_type_app_popup());
    if (popup || LooksLikeOauthLoginUrl(wc->GetVisibleURL()) ||
        LooksLikeOauthLoginUrl(wc->GetLastCommittedURL())) {
      ActivateWebContentsWindow(wc);
    }
  }

  const int tab_id = sessions::SessionTabHelper::IdForTab(wc).id();
  std::string url = wc->GetLastCommittedURL().spec();
  if (url.empty() || url == "about:blank") {
    const std::string visible = wc->GetVisibleURL().spec();
    if (!visible.empty()) {
      url = visible;
    }
  }
  const std::string title = base::UTF16ToUTF8(wc->GetTitle());
  content::RenderFrameHost* rfh = wc->GetPrimaryMainFrame();
  if (!rfh) {
    ResolveCallback(cb, BuildAgentPageContextDict(tab_id, std::move(url), title,
                                                  base::Value::List(),
                                                  base::Value()));
    return;
  }

  // Scrape the live DOM instead of waiting on APC. Casino/SPA pages often
  // never trip "document loaded" for FetchUnifiedPageMapSnapshot, and that
  // helper is also a no-op unless an agent ExecutionRunContext is active.
  rfh->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(std::string_view(kScrapeAgentPageContextScript)),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb_id,
             int tab_id_value, std::string page_url, std::string page_title,
             base::Value result) {
            if (!self) {
              return;
            }
            self->ResolveCallback(
                cb_id, BuildAgentPageContextDict(
                           tab_id_value, std::move(page_url),
                           std::move(page_title), base::Value::List(),
                           std::move(result)));
          },
          weak_ptr_factory_.GetWeakPtr(), cb, tab_id, std::move(url), title),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void DashboardUIMessageHandler::HandleAgentSaveScript(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;
  base::Value::Dict out;
  if (!payload) {
    out.Set("ok", false);
    out.Set("error", "missing payload");
    ResolveCallback(cb, out);
    return;
  }

  const base::Value::List* actions = payload->FindList("actions");
  if (!actions || actions->empty()) {
    out.Set("ok", false);
    out.Set("error", "No successful actions were recorded to save.");
    ResolveCallback(cb, out);
    return;
  }

  std::string script_id;
  if (const std::string* requested_id = payload->FindString("script_id")) {
    script_id = *requested_id;
  } else if (const std::string* alt_id = payload->FindString("id")) {
    script_id = *alt_id;
  }
  base::TrimWhitespaceASCII(script_id, base::TRIM_ALL, &script_id);
  if (script_id.empty()) {
    script_id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  }
  if (script_id == "welcome_demo" ||
      !dashboard::ScriptStore::IsValidScriptId(script_id)) {
    out.Set("ok", false);
    out.Set("error", "invalid_script_id");
    ResolveCallback(cb, out);
    return;
  }

  std::string name;
  if (const std::string* requested = payload->FindString("name")) {
    name = *requested;
  } else if (const std::string* goal = payload->FindString("goal")) {
    name = *goal;
  }
  base::TrimWhitespaceASCII(name, base::TRIM_ALL, &name);
  if (name.empty()) {
    name = "Saved Agent Script";
  }
  if (name.size() > 80u) {
    name.resize(80u);
  }

  std::string source = "glic_panel_autosave";
  if (const std::string* src = payload->FindString("source")) {
    if (!src->empty()) {
      source = *src;
    }
  }
  std::string goal;
  if (const std::string* requested_goal = payload->FindString("goal")) {
    goal = *requested_goal;
  } else if (const std::string* source_goal = payload->FindString("sourceGoal")) {
    goal = *source_goal;
  }
  std::string start_url;
  if (const std::string* url = payload->FindString("startUrl")) {
    start_url = *url;
  }

  base::Value::Dict script;
  script.Set("name", name);
  script.Set("kind", "main");
  script.Set("actions", actions->Clone());
  if (!start_url.empty()) {
    script.Set("startUrl", start_url);
  }
  script.Set("source", source);
  if (!goal.empty()) {
    script.Set("sourceGoal", goal);
  }
  script = dashboard::ScriptSchema::CanonicalizeForSave(std::move(script),
                                                        script_id,
                                                        base::Time::Now());
  const int action_count = static_cast<int>(actions->size());
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(
          [](Profile* profile, std::string id, base::Value::Dict dict) {
            return dashboard::ScriptStore::Write(profile, id, dict);
          },
          profile_, script_id, script.Clone()),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb_id,
             std::string id, std::string script_name, int count,
             base::Value::Dict dict, bool ok) {
            if (!self) {
              return;
            }
            base::Value::Dict reply;
            if (!ok) {
              reply.Set("ok", false);
              reply.Set("error", "Failed to auto-save generated script.");
              self->ResolveCallback(cb_id, reply);
              return;
            }
            DashboardUIMessageHandler::UpdateRuntimeScriptCache(
                self->profile_, id, dict.Clone());
            reply.Set("ok", true);
            reply.Set("id", id);
            reply.Set("name", script_name);
            reply.Set("action_count", count);
            self->ResolveCallback(cb_id, reply);
          },
          weak_ptr_factory_.GetWeakPtr(), cb, script_id, name, action_count,
          script.Clone()));
}

void DashboardUIMessageHandler::HandlePerformAgentAction(
    const base::Value::List& args) {
  // Enter agent mode for the duration of this execution.
  base::AutoReset<bool> agent_mode_guard(&is_agent_mode_, true);
  std::string cb;
  if (args.size() > 0 && args[0].is_string())
    cb = args[0].GetString();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;

  base::Value::Dict out;
  if (!payload) {
    out.Set("ok", false);
    out.Set("error", "missing payload");
    ResolveCallback(cb, out);
    return;
  }

  base::Value::List timeline;
  bool has_timeline = false;

  if (const base::Value::List* provided = payload->FindList("timeline")) {
    timeline = provided->Clone();
    has_timeline = !timeline.empty();
  } else if (const base::Value::List* actions = payload->FindList("actions")) {
    timeline = actions->Clone();
    has_timeline = !timeline.empty();
  } else if (const base::Value::Dict* action_dict = payload->FindDict("action")) {
    timeline.Append(action_dict->Clone());
    has_timeline = true;
  } else if (const base::Value::Dict* step_dict = payload->FindDict("step")) {
    timeline.Append(step_dict->Clone());
    has_timeline = true;
  } else if (const std::string* action_name = payload->FindString("action")) {
    base::Value::Dict action_entry;
    action_entry.Set("action", *action_name);
    if (const base::Value::Dict* params = payload->FindDict("params"))
      action_entry.Set("params", params->Clone());
    timeline.Append(std::move(action_entry));
    has_timeline = true;
  }

  if (!has_timeline) {
    out.Set("ok", false);
    out.Set("error", "missing action or timeline");
    ResolveCallback(cb, out);
    return;
  }

  auto read_bool_option =
      [&](const char* key, const char* alt_key, bool default_value) -> bool {
    if (std::optional<bool> direct = payload->FindBool(key);
        direct.has_value()) {
      return *direct;
    }
    if (std::optional<bool> direct_alt = payload->FindBool(alt_key);
        direct_alt.has_value()) {
      return *direct_alt;
    }
    if (const base::Value::Dict* options = payload->FindDict("options")) {
      if (std::optional<bool> from_options = options->FindBool(key);
          from_options.has_value()) {
        return *from_options;
      }
      if (std::optional<bool> from_options_alt = options->FindBool(alt_key);
          from_options_alt.has_value()) {
        return *from_options_alt;
      }
    }
    return default_value;
  };
  const std::optional<int> requested_tab_id = ReadRequestedTabId(*payload);
  content::WebContents* target_wc = requested_tab_id.has_value()
                                        ? FindWebContentsForTabId(profile_,
                                                                  *requested_tab_id)
                                        : nullptr;
  if (!target_wc) {
    target_wc = EnsureAgentTargetWebContents(profile_);
  } else if (IsDashboardOrInternalToolingUrl(
                 target_wc->GetLastCommittedURL())) {
    // Explicit tab_id pointed at dashboard/tooling — retarget instead of
    // destroying the WebUI.
    LOG(WARNING) << "[Agent] Refusing tooling tab_id="
                 << (requested_tab_id.has_value() ? *requested_tab_id : -1)
                 << " url=" << target_wc->GetLastCommittedURL().spec()
                 << "; opening/using a non-dashboard task tab";
    target_wc = EnsureAgentTargetWebContents(profile_);
  }
  if (!target_wc) {
    out.Set("ok", false);
    if (requested_tab_id.has_value()) {
      out.Set("error",
              base::StringPrintf("no tab found for tab_id=%d",
                                 *requested_tab_id));
    } else {
      out.Set("error", "no active tab");
    }
    ResolveCallback(cb, out);
    return;
  }
  ActivateWebContentsWindow(target_wc);

  const int resolved_tab_id = sessions::SessionTabHelper::IdForTab(target_wc).id();
  LOG(INFO) << "[Agent] Starting timeline run"
            << " tab_id=" << resolved_tab_id
            << " url=" << target_wc->GetLastCommittedURL().spec()
            << " requested_tab_id="
            << (requested_tab_id.has_value()
                    ? base::NumberToString(*requested_tab_id)
                    : "none")
            << " actions=" << timeline.size()
            << " wait_for_completion="
            << read_bool_option("wait_for_completion", "waitForCompletion", false)
            << " force_native="
            << read_bool_option("force_native_execution", "forceNativeExecution",
                                false);

  dashboard::ScriptExecutionService* exec_service =
      dashboard::ScriptExecutionServiceFactory::GetForProfile(profile_);
  if (!exec_service) {
    out.Set("ok", false);
    out.Set("error", "execution service unavailable");
    ResolveCallback(cb, out);
    return;
  }
  const bool wait_for_completion =
      read_bool_option("wait_for_completion", "waitForCompletion", false);
  const bool force_cdp_execution =
      read_bool_option("force_cdp_execution", "forceCdpExecution", false);
  const bool force_native_execution =
      read_bool_option("force_native_execution", "forceNativeExecution", false);
  dashboard::ScriptExecutionService::RunOptions run_options;
  run_options.force_cdp_execution = force_cdp_execution;
  run_options.force_native_execution = force_native_execution;
  run_options.is_agent_mode =
      read_bool_option("is_agent_mode", "isAgentMode", is_agent_mode_);
  // Optional: include a screenshot of current tab state before queuing.
  bool want_screenshot = false;
  std::optional<bool> b = payload->FindBool("includeScreenshot");
  if (b.has_value())
    want_screenshot = b.value();

  if (want_screenshot && wait_for_completion) {
    out.Set("ok", false);
    out.Set("error",
            "wait_for_completion is not supported when includeScreenshot=true");
    ResolveCallback(cb, out);
    return;
  }

  if (want_screenshot) {
    RecorderController* rc = RecorderController::GetActive();
    if (!rc)
      rc = recorder_.get();
    if (rc) {
      rc->CaptureScreenshot(base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self,
             std::string cb_id,
             std::optional<int> tab_id_hint,
             base::Value::List actions,
             std::string base64_png) {
            if (!self)
              return;
            dashboard::ScriptExecutionService* exec_service =
                dashboard::ScriptExecutionServiceFactory::GetForProfile(
                    self->profile_);
            if (exec_service) {
              content::WebContents* wc =
                  tab_id_hint.has_value()
                      ? FindWebContentsForTabId(self->profile_, *tab_id_hint)
                      : nullptr;
              if (!wc ||
                  IsDashboardOrInternalToolingUrl(wc->GetLastCommittedURL())) {
                wc = EnsureAgentTargetWebContents(self->profile_);
              }
              if (wc) {
                ActivateWebContentsWindow(wc);
                exec_service->StartAdHocRun(actions, wc);
              }
            }
            base::Value::Dict res;
            res.Set("ok", true);
            res.Set("queued", true);
            res.Set("count", static_cast<int>(actions.size()));
            if (!base64_png.empty())
              res.Set("screenshot", base::StrCat(
                                        {"data:image/png;base64,", base64_png}));
            self->ResolveCallback(cb_id, res);
          },
          weak_ptr_factory_.GetWeakPtr(), cb, requested_tab_id, timeline.Clone()));
      return;
    }
  }

  if (wait_for_completion) {
    auto execution_id_ref = std::make_shared<std::string>();
    std::string execution_id = exec_service->StartAdHocRunWithOptions(
        timeline, target_wc, run_options,
        base::BindOnce(
            [](base::WeakPtr<DashboardUIMessageHandler> self,
               std::string cb_id,
               std::shared_ptr<std::string> run_id_ref,
               bool success,
               int failed_step,
               std::string message) {
              if (!self) {
                return;
              }
              base::Value::Dict res;
              res.Set("ok", success);
              res.Set("success", success);
              res.Set("completed", true);
              res.Set("queued", false);
              if (run_id_ref && !run_id_ref->empty()) {
                res.Set("execution_id", *run_id_ref);
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
      out.Set("ok", false);
      out.Set("error", "failed to queue ad-hoc run");
      ResolveCallback(cb, out);
      return;
    }
    *execution_id_ref = execution_id;
    return;
  }

  const std::string execution_id =
      exec_service->StartAdHocRunWithOptions(timeline, target_wc, run_options);
  out.Set("ok", !execution_id.empty());
  out.Set("queued", !execution_id.empty());
  out.Set("count", static_cast<int>(timeline.size()));
  if (!execution_id.empty()) {
    out.Set("execution_id", execution_id);
  } else {
    out.Set("error", "failed to queue ad-hoc run");
  }
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentCallAction(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const std::string action =
      (args.size() > 1 && args[1].is_string()) ? args[1].GetString()
                                               : std::string();
  const base::Value::Dict* params =
      (args.size() > 2 && args[2].is_dict()) ? &args[2].GetDict() : nullptr;
  const base::Value::Dict* options =
      (args.size() > 3 && args[3].is_dict()) ? &args[3].GetDict() : nullptr;
  base::Value::Dict payload;
  if (!action.empty())
    payload.Set("action", action);
  if (params)
    payload.Set("params", params->Clone());
  if (options)
    payload.Set("options", options->Clone());
  base::Value::List forwarded_args;
  forwarded_args.Append(cb);
  forwarded_args.Append(std::move(payload));
  HandlePerformAgentAction(forwarded_args);
}

void DashboardUIMessageHandler::HandleAgentRunTimeline(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::List forwarded_args;
  forwarded_args.Append(cb);
  if (args.size() > 1 && args[1].is_dict()) {
    base::Value::Dict payload = args[1].GetDict().Clone();
    if (!payload.contains("is_agent_mode") &&
        !payload.contains("isAgentMode")) {
      payload.Set("is_agent_mode", false);
    }
    forwarded_args.Append(std::move(payload));
    HandlePerformAgentAction(forwarded_args);
    return;
  }
  const base::Value::List* timeline =
      (args.size() > 1 && args[1].is_list()) ? &args[1].GetList() : nullptr;
  base::Value::Dict payload;
  if (timeline) {
    payload.Set("timeline", timeline->Clone());
  }
  payload.Set("is_agent_mode", false);
  forwarded_args.Append(std::move(payload));
  HandlePerformAgentAction(forwarded_args);
}

void DashboardUIMessageHandler::HandleAgentRefreshPageMap(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::Dict out;
  RecorderController* rc = RecorderController::GetActive();
  if (!rc && recorder_)
    rc = recorder_.get();
  // Allow PageMap refresh even when recording has never been started by
  // lazily creating a RecorderController bound to the active browser.
  if (!rc) {
    Browser* browser = chrome::FindLastActiveWithProfile(profile_);
    BrowserView* bv = browser ? BrowserView::GetBrowserViewForBrowser(browser)
                              : nullptr;
    if (bv) {
      if (!recorder_)
        recorder_ = std::make_unique<RecorderController>();
      rc = recorder_.get();
      RecorderController::SetActive(rc);
      // No need to enter recording/passive mode here; snapshot capture will
      // lazily attach to the active tab as needed.
    }
  }
  if (!rc) {
    out.Set("ok", false);
    out.Set("error", "recorder not active");
    ResolveCallback(cb, out);
    return;
  }
  rc->CapturePageMap(base::BindOnce(
      [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb_id,
         base::Value::Dict pagemap) {
        if (!self)
          return;
        base::Value::Dict result;
        result.Set("ok", true);
        result.Set("pagemap", pagemap.Clone());
        self->ResolveCallback(cb_id, result);
      },
      weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandleAgentReloadPage(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  base::Value::Dict out;
  Browser* browser = chrome::FindLastActiveWithProfile(profile_);
  content::WebContents* wc =
      browser ? browser->tab_strip_model()->GetActiveWebContents() : nullptr;
  if (!wc) {
    out.Set("ok", false);
    out.Set("error", "no active tab");
    ResolveCallback(cb, out);
    return;
  }
  wc->GetController().Reload(content::ReloadType::NORMAL, false);
  out.Set("ok", true);
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentInspectElement(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;

  base::Value::Dict out;
  int index = -1;
  if (payload) {
    if (auto idx = payload->FindInt("index"))
      index = *idx;
  }

  if (index < 0) {
    out.Set("ok", false);
    out.Set("error", "missing or invalid index");
    ResolveCallback(cb, out);
    return;
  }

  RecorderController* rc = RecorderController::GetActive();
  if (!rc && recorder_)
    rc = recorder_.get();
  // Make inspect_element usable even when the recorder has not been put into
  // recording mode yet by lazily creating / attaching a controller.
  if (!rc) {
    Browser* browser = chrome::FindLastActiveWithProfile(profile_);
    BrowserView* bv = browser ? BrowserView::GetBrowserViewForBrowser(browser)
                              : nullptr;
    if (bv) {
      if (!recorder_)
        recorder_ = std::make_unique<RecorderController>();
      rc = recorder_.get();
      RecorderController::SetActive(rc);
    }
  }
  if (!rc) {
    out.Set("ok", false);
    out.Set("error", "recorder not active");
    ResolveCallback(cb, out);
    return;
  }

  auto report = rc->BuildElementReportForIndex(index);
  if (!report) {
    out.Set("ok", false);
    out.Set("error", "no page_map element or element out of range");
    ResolveCallback(cb, out);
    return;
  }

  out.Set("ok", true);
  out.Set("index", index);
  out.Set("report", std::move(*report));
  ResolveCallback(cb, out);
}

void DashboardUIMessageHandler::HandleAgentScreenshot(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;
  const std::optional<int> requested_tab_id =
      payload ? ReadRequestedTabId(*payload) : std::nullopt;
  content::WebContents* wc = requested_tab_id.has_value()
                                 ? FindWebContentsForTabId(profile_,
                                                           *requested_tab_id)
                                 : FindPreferredAgentTargetWebContents(profile_);
  if (wc && IsDashboardOrInternalToolingUrl(wc->GetLastCommittedURL())) {
    wc = FindPreferredAgentTargetWebContents(profile_);
  }
  if (!wc) {
    base::Value::Dict out;
    out.Set("ok", false);
    out.Set("error", "no task tab");
    ResolveCallback(cb, out);
    return;
  }
  CaptureWebContentsPngWithRetry(
      wc->GetWeakPtr(), /*attempts_left=*/16,
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb_id,
             std::string base64_png, int width, int height, int css_width,
             int css_height, std::string error) {
            if (!self) {
              return;
            }
            base::Value::Dict res;
            if (!base64_png.empty()) {
              res.Set("ok", true);
              res.Set("image",
                      base::StrCat({"data:image/png;base64,", base64_png}));
              res.Set("base64", base64_png);
              res.Set("mimeType", "image/png");
              if (width > 0) {
                res.Set("widthPixels", width);
              }
              if (height > 0) {
                res.Set("heightPixels", height);
              }
              if (css_width > 0) {
                res.Set("cssWidth", css_width);
              }
              if (css_height > 0) {
                res.Set("cssHeight", css_height);
              }
            } else {
              res.Set("ok", false);
              res.Set("error", error.empty() ? "screenshot failed" : error);
            }
            self->ResolveCallback(cb_id, res);
          },
          weak_ptr_factory_.GetWeakPtr(), cb));
}

void DashboardUIMessageHandler::HandleAgentOllamaChat(
    const base::Value::List& args) {
  const std::string cb =
      (args.size() > 0 && args[0].is_string()) ? args[0].GetString()
                                               : std::string();
  const base::Value::Dict* payload =
      (args.size() > 1 && args[1].is_dict()) ? &args[1].GetDict() : nullptr;

  base::Value::Dict early;
  if (!payload) {
    early.Set("ok", false);
    early.Set("error", "missing_payload");
    ResolveCallback(cb, early);
    return;
  }

  auto read_string = [](const base::Value::Dict& dict,
                        std::string_view primary,
                        std::string_view alternate) -> std::string {
    if (const std::string* value = dict.FindString(primary)) {
      return *value;
    }
    if (const std::string* value = dict.FindString(alternate)) {
      return *value;
    }
    return std::string();
  };

  std::string raw_provider = read_string(*payload, "provider", "provider");
  // Empty / legacy "ollama" → prefer Settings-configured provider.
  if (raw_provider.empty() ||
      base::EqualsCaseInsensitiveASCII(raw_provider, "ollama")) {
    if (const base::Value::Dict* agent =
            dashboard::GlobalSettingsStore().FindDict("agent")) {
      if (const std::string* configured = agent->FindString("provider")) {
        raw_provider = *configured;
      }
    }
  }
  std::string provider = NormalizeAgentChatProvider(raw_provider);

  std::string endpoint = read_string(*payload, "endpoint", "endpoint");
  while (!endpoint.empty() && endpoint.back() == '/') {
    endpoint.pop_back();
  }

  std::string model = read_string(*payload, "model", "model");
  const std::string system_prompt =
      read_string(*payload, "system_prompt", "systemPrompt");
  int max_tokens = 1024;
  if (const std::optional<int> configured = payload->FindInt("max_tokens")) {
    max_tokens = std::clamp(*configured, 256, 4096);
  } else if (const std::optional<int> configured_alt =
                 payload->FindInt("maxTokens")) {
    max_tokens = std::clamp(*configured_alt, 256, 4096);
  }

  base::Value::List chat_messages;
  if (const base::Value::List* messages = payload->FindList("messages")) {
    for (const auto& item : *messages) {
      if (item.is_dict()) {
        chat_messages.Append(item.Clone());
      }
    }
  }

  const std::string api_key_name = [&]() {
    std::string from_payload =
        read_string(*payload, "api_key_name", "apiKeyName");
    if (!from_payload.empty()) {
      return from_payload;
    }
    return RequiredApiKeyNameForChatProvider(provider);
  }();
  // secrets.ini IO is blocking; never do it on the UI thread. Local servers
  // often need no key, but LOCAL_API_KEY is still resolved when present.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ResolveChatProviderApiKeyFromPath, profile_->GetPath(),
                     api_key_name),
      base::BindOnce(&DashboardUIMessageHandler::StartAgentOllamaChatRequest,
                     weak_ptr_factory_.GetWeakPtr(), cb, provider, endpoint,
                     model, system_prompt, max_tokens,
                     std::move(chat_messages), api_key_name));
}

void DashboardUIMessageHandler::StartAgentOllamaChatRequest(
    std::string cb,
    std::string provider,
    std::string endpoint,
    std::string model,
    std::string system_prompt,
    int max_tokens,
    base::Value::List chat_messages,
    std::string api_key_name,
    std::string api_key) {
  base::Value::Dict early;
  if (provider != "local" && api_key.empty()) {
    early.Set("ok", false);
    early.Set("error",
              base::StrCat({"missing_api_key:", api_key_name,
                            " (set in Data > Secrets)"}));
    ResolveCallback(cb, early);
    return;
  }

  base::Value::Dict body;
  GURL request_url;
  auto request = std::make_unique<network::ResourceRequest>();
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader("Content-Type", "application/json");
  request->headers.SetHeader("Accept", "application/json");

  if (provider == "anthropic") {
    if (endpoint.empty()) {
      endpoint = "https://api.anthropic.com/v1/messages";
    }
    if (model.empty() || model == "claude-haiku-4-5-20251001" ||
        model == "claude-3-5-haiku-latest") {
      model = "claude-haiku-4-5";
    } else if (model == "claude-sonnet-4-20250514" ||
               model == "claude-3-5-sonnet-latest") {
      model = "claude-sonnet-4-5";
    }
    body.Set("model", model);
    body.Set("max_tokens", max_tokens);
    body.Set("temperature", 0);
    if (!system_prompt.empty()) {
      body.Set("system", system_prompt);
    }
    base::Value::List anthropic_messages;
    for (const auto& item : chat_messages) {
      if (!item.is_dict()) {
        continue;
      }
      const base::Value::Dict& src = item.GetDict();
      const std::string* role = src.FindString("role");
      if (!role || *role == "system") {
        continue;
      }
      const base::Value* content_value = src.Find("content");
      if (!content_value) {
        continue;
      }
      base::Value::List parts = BuildAnthropicContentParts(*content_value);
      if (parts.empty()) {
        continue;
      }
      base::Value::Dict msg;
      msg.Set("role", *role);
      msg.Set("content", std::move(parts));
      anthropic_messages.Append(std::move(msg));
    }
    body.Set("messages", std::move(anthropic_messages));
    request_url = GURL(endpoint);
    request->headers.SetHeader("x-api-key", api_key);
    request->headers.SetHeader("anthropic-version", "2023-06-01");
    request->headers.SetHeader("User-Agent", "SimulachromeDashboard/1.0");
  } else if (provider == "openai" || provider == "cerebras" ||
             provider == "local") {
    if (endpoint.empty()) {
      if (provider == "cerebras") {
        endpoint = "https://api.cerebras.ai/v1/chat/completions";
      } else if (provider == "local") {
        endpoint = "http://127.0.0.1:11434/v1/chat/completions";
      } else {
        endpoint = "https://api.openai.com/v1/chat/completions";
      }
    }
    // Accept base URLs like http://host:port/v1 and append chat completions.
    if (provider == "local") {
      if (base::EndsWith(endpoint, "/v1", base::CompareCase::SENSITIVE)) {
        endpoint = base::StrCat({endpoint, "/chat/completions"});
      } else if (!base::EndsWith(endpoint, "/chat/completions",
                                 base::CompareCase::SENSITIVE) &&
                 !base::EndsWith(endpoint, "/completions",
                                 base::CompareCase::SENSITIVE)) {
        if (base::EndsWith(endpoint, "/", base::CompareCase::SENSITIVE)) {
          endpoint = base::StrCat({endpoint, "v1/chat/completions"});
        } else {
          endpoint = base::StrCat({endpoint, "/v1/chat/completions"});
        }
      }
    }
    if (model.empty()) {
      if (provider == "cerebras") {
        model = "qwen-3-235b-a22b-instruct-2507";
      } else if (provider == "local") {
        model = "local-model";
      } else {
        model = "gpt-4.1-mini";
      }
    }
    base::Value::List request_messages;
    if (!system_prompt.empty()) {
      base::Value::Dict system_message;
      system_message.Set("role", "system");
      system_message.Set("content", system_prompt);
      request_messages.Append(std::move(system_message));
    }
    for (const auto& item : chat_messages) {
      request_messages.Append(item.Clone());
    }
    body.Set("model", model);
    body.Set("temperature", 0);
    body.Set("max_tokens", max_tokens);
    body.Set("messages", std::move(request_messages));
    request_url = GURL(endpoint);
    if (!api_key.empty()) {
      request->headers.SetHeader("Authorization",
                                 base::StrCat({"Bearer ", api_key}));
    }
    request->headers.SetHeader("User-Agent", "SimulachromeDashboard/1.0");
  } else if (provider == "gemini") {
    if (endpoint.empty()) {
      endpoint = "https://generativelanguage.googleapis.com/v1beta";
    }
    if (model.empty()) {
      model = "gemini-2.5-flash";
    }
    base::Value::List contents;
    for (const auto& item : chat_messages) {
      if (!item.is_dict()) {
        continue;
      }
      const base::Value::Dict& src = item.GetDict();
      const std::string* role = src.FindString("role");
      const std::string* content = src.FindString("content");
      if (!role || !content || *role == "system") {
        continue;
      }
      base::Value::Dict entry;
      entry.Set("role", *role == "assistant" ? "model" : "user");
      base::Value::List parts;
      base::Value::Dict text_part;
      text_part.Set("text", *content);
      parts.Append(std::move(text_part));
      entry.Set("parts", std::move(parts));
      contents.Append(std::move(entry));
    }
    body.Set("contents", std::move(contents));
    if (!system_prompt.empty()) {
      base::Value::Dict system_instruction;
      base::Value::List parts;
      base::Value::Dict text_part;
      text_part.Set("text", system_prompt);
      parts.Append(std::move(text_part));
      system_instruction.Set("parts", std::move(parts));
      body.Set("systemInstruction", std::move(system_instruction));
    }
    base::Value::Dict generation;
    generation.Set("temperature", 0);
    generation.Set("maxOutputTokens", max_tokens);
    body.Set("generationConfig", std::move(generation));
    request_url = GURL(base::StrCat(
        {endpoint, "/models/", model, ":generateContent"}));
    request->headers.SetHeader("x-goog-api-key", api_key);
    request->headers.SetHeader("User-Agent", "SimulachromeDashboard/1.0");
  } else {
    early.Set("ok", false);
    early.Set("error", base::StrCat({"unsupported_provider:", provider}));
    ResolveCallback(cb, early);
    return;
  }

  std::string body_json;
  if (!base::JSONWriter::Write(body, &body_json) || body_json.empty()) {
    early.Set("ok", false);
    early.Set("error", "failed_to_build_request_body");
    ResolveCallback(cb, early);
    return;
  }

  if (!request_url.is_valid()) {
    early.Set("ok", false);
    early.Set("error", "invalid_provider_endpoint");
    ResolveCallback(cb, early);
    return;
  }
  request->url = request_url;

  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(request),
                                       TRAFFIC_ANNOTATION_FOR_TESTS);
  loader->AttachStringForUpload(body_json, "application/json");
  loader->SetAllowHttpErrorResults(true);
  int timeout_ms = provider == "local" ? 20 * 60 * 1000 : 90 * 1000;
  if (const base::Value::Dict* agent =
          dashboard::GlobalSettingsStore().FindDict("agent")) {
    if (const std::optional<int> configured =
            agent->FindInt("modelStepTimeoutMs")) {
      timeout_ms = std::clamp(*configured, 30 * 1000, 60 * 60 * 1000);
    }
  }
  loader->SetTimeoutDuration(base::Milliseconds(timeout_ms));

  network::SimpleURLLoader* loader_raw = loader.get();
  auto* partition = profile_->GetDefaultStoragePartition();
  loader_raw->DownloadToString(
      partition->GetURLLoaderFactoryForBrowserProcess().get(),
      base::BindOnce(
          [](base::WeakPtr<DashboardUIMessageHandler> self, std::string cb,
             std::unique_ptr<network::SimpleURLLoader> loader,
             std::unique_ptr<std::string> response_body) {
            if (!self) {
              return;
            }
            base::Value::Dict out;
            int http_status = 0;
            if (loader && loader->ResponseInfo() &&
                loader->ResponseInfo()->headers) {
              http_status = loader->ResponseInfo()->headers->response_code();
            }
            if (!response_body) {
              out.Set("ok", false);
              out.Set("http_status", http_status);
              out.Set("error",
                      base::StrCat(
                          {"network_error:",
                           base::NumberToString(loader ? loader->NetError()
                                                       : net::ERR_FAILED)}));
              self->ResolveCallback(cb, out);
              return;
            }
            out.Set("ok", http_status >= 200 && http_status < 300);
            out.Set("http_status", http_status);
            out.Set("body", *response_body);
            if (http_status < 200 || http_status >= 300) {
              out.Set("error", *response_body);
            }
            self->ResolveCallback(cb, out);
          },
          weak_ptr_factory_.GetWeakPtr(), cb, std::move(loader)),
      4 * 1024 * 1024);
}
