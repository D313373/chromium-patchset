// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/login_flow_controller.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

namespace simulachrome {

namespace {

// Reads the login surface of one frame. Deliberately returns raw observations
// rather than a verdict -- the prune/retype decision needs the identity value
// compared against the vault token, which only the browser side knows.
//
// querySelectorAll does not pierce shadow roots, and component libraries
// routinely put login inputs inside one, so the walk recurses into every open
// root it finds. Closed roots stay invisible, which is why the VLM fallback
// cannot be removed entirely.
constexpr char kProbeScript[] = R"js(
(() => {
  const visible = (el) => {
    if (!el) return false;
    const r = el.getBoundingClientRect();
    if (r.width < 1 || r.height < 1) return false;
    const s = window.getComputedStyle(el);
    return s.visibility !== 'hidden' && s.display !== 'none' &&
           s.opacity !== '0';
  };

  const queryDeep = (selector) => {
    const found = [];
    const walk = (root) => {
      try {
        for (const el of root.querySelectorAll(selector)) found.push(el);
        for (const el of root.querySelectorAll('*')) {
          if (el.shadowRoot) walk(el.shadowRoot);
        }
      } catch (e) {}
    };
    walk(document);
    return found;
  };

  // :autofill is the DOM-observable form of the browser's is_autofilled bit.
  // Older engines only expose the -webkit- prefixed alias, and matches()
  // throws on an unknown pseudo-class rather than returning false.
  const isAutofilled = (el) => {
    for (const sel of [':autofill', ':-webkit-autofill']) {
      try {
        if (el.matches(sel)) return true;
      } catch (e) {}
    }
    return false;
  };

  // Shortest stable path we can build. An id or name survives re-render in a
  // way that an nth-child chain does not, so those are strongly preferred and
  // an empty result is reported rather than a fragile positional selector.
  const cssPath = (el) => {
    if (!el) return '';
    try {
      const tag = el.tagName.toLowerCase();
      if (el.id && !/\s/.test(el.id) && !/^[0-9]/.test(el.id)) {
        const sel = `#${CSS.escape(el.id)}`;
        if (document.querySelectorAll(sel).length === 1) return sel;
      }
      const name = el.getAttribute('name');
      if (name) {
        const sel = `${tag}[name="${CSS.escape(name)}"]`;
        if (document.querySelectorAll(sel).length === 1) return sel;
      }
      for (const attr of ['autocomplete', 'type', 'aria-label',
                          'placeholder']) {
        const v = el.getAttribute(attr);
        if (!v) continue;
        const sel = `${tag}[${attr}="${CSS.escape(v)}"]`;
        if (document.querySelectorAll(sel).length === 1) return sel;
      }
    } catch (e) {}
    return '';
  };

  const passwords = queryDeep('input[type="password"]').filter(visible);

  const identitySelectors = [
    'input[autocomplete="username"]',
    'input[autocomplete="email"]',
    'input[type="email"]',
    'input[name*="user" i]',
    'input[name*="email" i]',
    'input[id*="user" i]',
    'input[id*="email" i]',
  ];
  let identity = null;
  for (const sel of identitySelectors) {
    const hit = queryDeep(sel).filter(visible);
    if (hit.length) { identity = hit[0]; break; }
  }
  // Plenty of forms label the identity field nothing in particular. If a
  // password field exists, the nearest preceding visible text input is a far
  // better guess than giving up and asking the model to find it.
  if (!identity && passwords.length) {
    const texts = queryDeep('input[type="text"], input:not([type])')
        .filter(visible);
    if (texts.length) identity = texts[texts.length - 1];
  }

  const captchaPatterns = [
    ['recaptcha', /google\.com\/recaptcha|gstatic\.com\/recaptcha/i],
    ['hcaptcha', /hcaptcha\.com/i],
    ['turnstile', /challenges\.cloudflare\.com/i],
  ];
  let captcha = '';
  for (const frame of queryDeep('iframe')) {
    const src = String(frame.getAttribute('src') || '');
    for (const [name, re] of captchaPatterns) {
      if (re.test(src)) { captcha = name; break; }
    }
    if (captcha) break;
  }

  const otp = queryDeep(
      'input[autocomplete="one-time-code"], input[name*="otp" i], ' +
      'input[id*="otp" i]').filter(visible);

  const submits = queryDeep(
      'button[type="submit"], input[type="submit"]').filter(visible);
  let submit = submits.length ? submits[0] : null;
  if (!submit) {
    for (const b of queryDeep('button, [role="button"], a').filter(visible)) {
      if (/\b(log\s*in|login|sign\s*in|signin|continue|next)\b/i.test(
              b.textContent || '')) {
        submit = b;
        break;
      }
    }
  }

  return {
    has_identity_field: !!identity,
    identity_selector: cssPath(identity),
    identity_value: identity ? String(identity.value || '') : '',
    identity_autofilled: identity ? isAutofilled(identity) : false,
    has_password_field: passwords.length > 0,
    password_selector: passwords.length ? cssPath(passwords[0]) : '',
    password_nonempty: passwords.some((p) => String(p.value || '').length > 0),
    password_autofilled: passwords.some(isAutofilled),
    has_captcha: !!captcha,
    captcha_provider: captcha,
    has_otp_field: otp.length > 0,
    has_submit_candidate: !!submit,
    submit_selector: cssPath(submit),
    frame_url: String(location.href || ''),
  };
})()
)js";

bool GetBool(const base::Value::Dict& dict, const char* key) {
  return dict.FindBool(key).value_or(false);
}

std::string GetString(const base::Value::Dict& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}

}  // namespace

DomLoginState::DomLoginState() = default;
DomLoginState::DomLoginState(const DomLoginState&) = default;
DomLoginState& DomLoginState::operator=(const DomLoginState&) = default;
DomLoginState::~DomLoginState() = default;

LoginFlowController::LoginFlowController(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents) {}

LoginFlowController::~LoginFlowController() = default;

void LoginFlowController::WebContentsDestroyed() {
  // A probe in flight would otherwise resolve against a dead frame tree, and
  // the caller is still holding a callback it expects to be run exactly once.
  if (pending_callback_) {
    std::move(pending_callback_).Run(DomLoginState());
  }
  outstanding_frames_ = 0;
}

void LoginFlowController::EvaluateDomState(StateCallback callback) {
  if (!web_contents()) {
    std::move(callback).Run(DomLoginState());
    return;
  }
  // One probe at a time: the accumulator and the frame counter are single
  // instances, so a concurrent call would interleave into the same result.
  if (pending_callback_) {
    LOG(WARNING) << "[LoginFlowController] probe already in flight";
    std::move(callback).Run(DomLoginState());
    return;
  }

  std::vector<content::RenderFrameHost*> frames;
  web_contents()->ForEachRenderFrameHost(
      [&frames](content::RenderFrameHost* rfh) {
        if (rfh->IsRenderFrameLive()) {
          frames.push_back(rfh);
        }
      });

  if (frames.empty()) {
    std::move(callback).Run(DomLoginState());
    return;
  }

  pending_ = DomLoginState();
  pending_.probed = true;
  pending_callback_ = std::move(callback);
  outstanding_frames_ = static_cast<int>(frames.size());

  const std::u16string script = base::UTF8ToUTF16(std::string(kProbeScript));
  for (content::RenderFrameHost* rfh : frames) {
    // Cross-origin subframes are reachable here but not from a content script,
    // which is what lets this see an Auth0 or Okta login form.
    rfh->ExecuteJavaScriptInIsolatedWorld(
        script,
        base::BindOnce(&LoginFlowController::OnFrameProbed,
                       weak_factory_.GetWeakPtr(), !rfh->GetParent()),
        ISOLATED_WORLD_ID_CHROME_INTERNAL);
  }
}

void LoginFlowController::OnFrameProbed(bool is_main_frame,
                                        base::Value result) {
  if (outstanding_frames_ <= 0) {
    return;
  }
  --outstanding_frames_;

  if (const base::Value::Dict* dict = result.GetIfDict()) {
    pending_.frames_probed++;

    // Field signals are taken from whichever frame actually hosts the form,
    // rather than OR-ed across frames: merging an identity value from one
    // frame with a password field in another would describe a page that does
    // not exist.
    const bool frame_has_password = GetBool(*dict, "has_password_field");
    if (frame_has_password && !pending_.has_password_field) {
      pending_.has_password_field = true;
      pending_.password_selector = GetString(*dict, "password_selector");
      pending_.password_nonempty = GetBool(*dict, "password_nonempty");
      pending_.password_autofilled = GetBool(*dict, "password_autofilled");
      pending_.has_identity_field = GetBool(*dict, "has_identity_field");
      pending_.identity_selector = GetString(*dict, "identity_selector");
      pending_.identity_value = GetString(*dict, "identity_value");
      pending_.identity_autofilled = GetBool(*dict, "identity_autofilled");
      pending_.form_frame_url = GetString(*dict, "frame_url");
      pending_.form_in_main_frame = is_main_frame;
    } else if (!pending_.has_password_field &&
               !pending_.has_identity_field &&
               GetBool(*dict, "has_identity_field")) {
      // Identity-first flows (Google, Microsoft) show the username field on
      // its own page, with no password field anywhere yet.
      pending_.has_identity_field = true;
      pending_.identity_selector = GetString(*dict, "identity_selector");
      pending_.identity_value = GetString(*dict, "identity_value");
      pending_.identity_autofilled = GetBool(*dict, "identity_autofilled");
      pending_.form_frame_url = GetString(*dict, "frame_url");
      pending_.form_in_main_frame = is_main_frame;
    }

    // Challenges and submit buttons genuinely can live in a different frame
    // from the form, so these do accumulate.
    if (GetBool(*dict, "has_captcha") && !pending_.has_captcha) {
      pending_.has_captcha = true;
      pending_.captcha_provider = GetString(*dict, "captcha_provider");
    }
    pending_.has_otp_field |= GetBool(*dict, "has_otp_field");
    if (GetBool(*dict, "has_submit_candidate") &&
        !pending_.has_submit_candidate) {
      pending_.has_submit_candidate = true;
      pending_.submit_selector = GetString(*dict, "submit_selector");
    }
  }

  if (outstanding_frames_ > 0) {
    return;
  }

  if (pending_.has_password_field) {
    saw_password_field_ = true;
  }

  LOG(INFO) << "[LoginFlowController] frames=" << pending_.frames_probed
            << " identity=" << pending_.has_identity_field
            << " identity_autofilled=" << pending_.identity_autofilled
            << " password=" << pending_.has_password_field
            << " password_autofilled=" << pending_.password_autofilled
            << " captcha=" << pending_.captcha_provider
            << " otp=" << pending_.has_otp_field
            << " submit=" << pending_.has_submit_candidate
            << " identity_sel='" << pending_.identity_selector
            << "' password_sel='" << pending_.password_selector
            << "' main_frame=" << pending_.form_in_main_frame;

  if (pending_callback_) {
    std::move(pending_callback_).Run(pending_);
  }
}

}  // namespace simulachrome
