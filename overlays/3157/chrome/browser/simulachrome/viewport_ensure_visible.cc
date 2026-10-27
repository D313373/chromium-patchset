// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/viewport_ensure_visible.h"

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"

namespace simulachrome {
namespace {

// Instant scroll + partial-visibility check. Returns a dict:
//   { found: bool, scrolled: bool, top, bottom, left, right }
constexpr char kEnsureScript[] = R"((function(sel) {
  try {
    const el = document.querySelector(sel);
    if (!el) return {found:false, scrolled:false};
    const r = el.getBoundingClientRect();
    const vw = window.innerWidth || 0;
    const vh = window.innerHeight || 0;
    // Require a meaningful intersection with the viewport (40pct of element
    // height, or 32 CSS px) so partially clipped targets still get scrolled.
    const visible_h = Math.max(0, Math.min(r.bottom, vh) - Math.max(r.top, 0));
    const visible_w = Math.max(0, Math.min(r.right, vw) - Math.max(r.left, 0));
    const need_h = Math.max(32, Math.min(r.height || 0, vh) * 0.4);
    const need_w = Math.max(16, Math.min(r.width || 0, vw) * 0.2);
    const ok = visible_h >= need_h && visible_w >= need_w;
    if (ok) {
      return {found:true, scrolled:false,
              top:r.top, bottom:r.bottom, left:r.left, right:r.right};
    }
    el.scrollIntoView({block:'center', inline:'nearest', behavior:'instant'});
    return {found:true, scrolled:true,
            top:r.top, bottom:r.bottom, left:r.left, right:r.right};
  } catch (e) {
    return {found:false, scrolled:false, error:String(e)};
  }
})(%s))";

}  // namespace

// static
void ViewportEnsureVisible::Ensure(content::WebContents* web_contents,
                                   const std::string& css_selector,
                                   DoneCallback done) {
  if (css_selector.empty()) {
    std::move(done).Run(Result::kNoSelector);
    return;
  }
  if (!web_contents) {
    std::move(done).Run(Result::kNotFound);
    return;
  }
  content::RenderFrameHost* rfh = web_contents->GetPrimaryMainFrame();
  if (!rfh) {
    std::move(done).Run(Result::kNotFound);
    return;
  }

  std::string quoted;
  {
    base::Value v(css_selector);
    if (!base::JSONWriter::Write(v, &quoted)) {
      std::move(done).Run(Result::kNotFound);
      return;
    }
  }
  // Avoid StringPrintf: the script body must not contain stray '%' chars.
  std::string js = kEnsureScript;
  const std::string placeholder = "%s";
  const size_t pos = js.rfind(placeholder);
  if (pos == std::string::npos) {
    std::move(done).Run(Result::kNotFound);
    return;
  }
  js.replace(pos, placeholder.size(), quoted);

  rfh->ExecuteJavaScriptForTests(
      base::UTF8ToUTF16(js),
      base::BindOnce(
          [](DoneCallback done, base::Value result) {
            if (!result.is_dict()) {
              std::move(done).Run(Result::kNotFound);
              return;
            }
            const base::Value::Dict& d = result.GetDict();
            const bool found = d.FindBool("found").value_or(false);
            const bool scrolled = d.FindBool("scrolled").value_or(false);
            if (!found) {
              LOG(INFO) << "[ViewportEnsureVisible] selector not found";
              std::move(done).Run(Result::kNotFound);
              return;
            }
            if (scrolled) {
              LOG(INFO) << "[ViewportEnsureVisible] scrolled into view";
              std::move(done).Run(Result::kScrolled);
              return;
            }
            LOG(INFO) << "[ViewportEnsureVisible] already visible";
            std::move(done).Run(Result::kAlreadyVisible);
          },
          std::move(done)),
      /*world_id=*/content::ISOLATED_WORLD_ID_CONTENT_END);
}

}  // namespace simulachrome
