// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SIMULACHROME_LOGIN_FLOW_CONTROLLER_H_
#define CHROME_BROWSER_SIMULACHROME_LOGIN_FLOW_CONTROLLER_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class WebContents;
}

namespace simulachrome {

// What the scaffolding could see in the page, gathered without asking the VLM.
// Every field is a browser-side fact, so a step decision based on this costs
// a few milliseconds instead of a full inference pass.
struct DomLoginState {
  DomLoginState();
  DomLoginState(const DomLoginState&);
  DomLoginState& operator=(const DomLoginState&);
  ~DomLoginState();

  // True once the probe has actually run; distinguishes "no password field"
  // from "never looked", which is what makes the success latch safe.
  bool probed = false;

  bool has_identity_field = false;
  // CSS selector for the identity field, suitable as a CSS_SELECTOR locator on
  // a cdp_type action. Empty when no stable path could be built, which is the
  // signal to fall back to the VLM rather than guessing.
  std::string identity_selector;
  // Current contents of the identity field. Compared against the resolved
  // vault token to decide prune vs. clear-and-retype -- a non-empty field is
  // not on its own a reason to skip typing, because it may hold a stale
  // autofill for a different account.
  std::string identity_value;
  // Matches :autofill, i.e. a password manager put this here rather than a
  // human or a previous step of ours.
  bool identity_autofilled = false;

  bool has_password_field = false;
  std::string password_selector;
  bool password_nonempty = false;
  bool password_autofilled = false;

  // Turnstile / reCAPTCHA / hCaptcha frame, or a one-time-code input. Both are
  // detected structurally so the escalation tier is chosen without spending an
  // inference pass discovering it.
  bool has_captcha = false;
  std::string captcha_provider;
  bool has_otp_field = false;

  bool has_submit_candidate = false;
  std::string submit_selector;

  // The form's frame. A CSS selector is only meaningful against the document
  // that produced it, so a caller driving CDP has to target this frame -- and
  // a selector from a cross-origin subframe is unreachable from the main
  // document, which is where the VLM fallback earns its place.
  bool form_in_main_frame = false;
  std::string form_frame_url;

  // Frames that answered the probe. Cross-origin login forms (Auth0, Okta)
  // live in a subframe, so a count above one means the signals below may come
  // from a frame other than the main one.
  int frames_probed = 0;
};

// Reads login-relevant page state for the scaffolding that sequences a login.
// Intentionally does not drive the flow or talk to the model: this is the
// "evaluate browser state" box, kept separate so the state machine on top of
// it stays testable against synthetic DomLoginState values.
class LoginFlowController : public content::WebContentsObserver {
 public:
  using StateCallback = base::OnceCallback<void(DomLoginState)>;

  explicit LoginFlowController(content::WebContents* web_contents);
  ~LoginFlowController() override;

  LoginFlowController(const LoginFlowController&) = delete;
  LoginFlowController& operator=(const LoginFlowController&) = delete;

  // Probes every frame and reports the merged result. Runs in an isolated
  // world: ExecuteJavaScript is restricted to chrome:// and devtools:// URLs,
  // so it cannot be used against a real site.
  void EvaluateDomState(StateCallback callback);

  // True if any probe so far reported a password field. The disappearance of
  // a password field only means a completed login if one was there to begin
  // with; without this latch a page that has not finished loading its form
  // reads as an authenticated session.
  bool saw_password_field() const { return saw_password_field_; }

  // content::WebContentsObserver:
  void WebContentsDestroyed() override;

 private:
  // Accumulates one frame's answer into |pending_|, then fires the callback
  // once every frame has reported. |is_main_frame| is bound per frame because
  // the result carries no identity of its own.
  void OnFrameProbed(bool is_main_frame, base::Value result);

  bool saw_password_field_ = false;

  DomLoginState pending_;
  int outstanding_frames_ = 0;
  StateCallback pending_callback_;

  base::WeakPtrFactory<LoginFlowController> weak_factory_{this};
};

}  // namespace simulachrome

#endif  // CHROME_BROWSER_SIMULACHROME_LOGIN_FLOW_CONTROLLER_H_
