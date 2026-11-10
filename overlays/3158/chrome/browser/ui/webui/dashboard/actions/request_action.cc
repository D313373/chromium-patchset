// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/actions/request_action.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/webui/dashboard/scrape_data_store.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace {

bool ParseAssertStatus(const base::Value::Dict& params, std::set<int>* out) {
  out->clear();
  if (const base::Value::List* list = params.FindList("assertStatus")) {
    for (const auto& item : *list) {
      if (item.is_int()) {
        out->insert(item.GetInt());
      } else if (item.is_string()) {
        int code = 0;
        if (base::StringToInt(item.GetString(), &code) && code > 0) {
          out->insert(code);
        }
      }
    }
  } else if (const std::string* raw = params.FindString("assertStatus")) {
    for (const auto& part :
         base::SplitString(*raw, ",", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      int code = 0;
      if (base::StringToInt(part, &code) && code > 0) {
        out->insert(code);
      }
    }
  } else if (std::optional<int> single = params.FindInt("assertStatus")) {
    if (*single > 0) {
      out->insert(*single);
    }
  }
  if (out->empty()) {
    out->insert(200);
  }
  return true;
}

base::Value::Dict ParseHeaders(const base::Value::Dict& params) {
  if (const base::Value::Dict* headers = params.FindDict("headers")) {
    return headers->Clone();
  }
  const std::string* json = params.FindString("headersJson");
  if (!json || base::TrimWhitespaceASCII(*json, base::TRIM_ALL).empty()) {
    return base::Value::Dict();
  }
  auto parsed = base::JSONReader::Read(
      *json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    return base::Value::Dict();
  }
  return std::move(parsed->GetDict());
}

std::string EscapeForJsStringLiteral(const std::string& input) {
  std::string json;
  if (!base::JSONWriter::Write(base::Value(input), &json)) {
    return "\"\"";
  }
  return json;
}

}  // namespace

RequestAction::RequestAction(Profile* profile)
    : dashboard::actions::Action(profile) {}
RequestAction::~RequestAction() = default;

bool RequestAction::SetParams(const base::Value::Dict& params) {
  const std::string* url = params.FindString("url");
  if (!url || base::TrimWhitespaceASCII(*url, base::TRIM_ALL).empty()) {
    return false;
  }
  url_ = *url;

  if (const std::string* method = params.FindString("method")) {
    method_ = base::ToUpperASCII(base::TrimWhitespaceASCII(*method, base::TRIM_ALL));
  } else {
    method_ = "GET";
  }
  if (method_.empty()) {
    method_ = "GET";
  }

  headers_ = ParseHeaders(params);

  if (const std::string* body = params.FindString("body")) {
    body_ = *body;
  } else {
    body_.clear();
  }

  if (const std::string* capture = params.FindString("captureAs")) {
    capture_as_ = *capture;
  } else {
    capture_as_.clear();
  }
  persist_to_data_ = params.FindBool("persistToData").value_or(true);
  ParseAssertStatus(params, &assert_status_);
  timeout_ms_ = params.FindInt("timeoutMs").value_or(30000);
  if (timeout_ms_ <= 0) {
    timeout_ms_ = 30000;
  }

  SetSerializedAction("request", params.Clone());
  return true;
}

void RequestAction::Fail(Callback callback, const std::string& error) {
  LOG(ERROR) << "[RequestAction] " << error;
  std::move(callback).Run(false, error);
}

void RequestAction::Execute(content::WebContents* web_contents,
                            Callback callback) {
  if (!web_contents) {
    Fail(std::move(callback), "No web contents for request");
    return;
  }
  content::RenderFrameHost* frame = web_contents->GetPrimaryMainFrame();
  if (!frame || !frame->IsRenderFrameLive()) {
    Fail(std::move(callback), "Primary frame unavailable for request");
    return;
  }

  GURL gurl(url_);
  if (!gurl.is_valid()) {
    Fail(std::move(callback), "Invalid request URL: " + url_);
    return;
  }

  std::string headers_json;
  if (!base::JSONWriter::Write(base::Value(headers_.Clone()), &headers_json)) {
    headers_json = "{}";
  }

  const bool has_body =
      !body_.empty() && method_ != "GET" && method_ != "HEAD";
  const std::string body_js =
      has_body ? EscapeForJsStringLiteral(body_) : std::string("undefined");

  // Page-context fetch so cookies / TLS / HTTP2 match the live tab.
  // Tokens in url/headers/body are already expanded by ScriptExecutionService.
  const std::string js = base::StringPrintf(
      R"JS((async () => {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), %d);
  try {
    const init = {
      method: %s,
      headers: %s,
      credentials: 'include',
      signal: ctrl.signal,
    };
    const body = %s;
    if (body !== undefined && body !== null && String(body).length) {
      init.body = body;
    }
    const res = await fetch(%s, init);
    const text = await res.text();
    clearTimeout(timer);
    return JSON.stringify({
      ok: true,
      status: res.status,
      body: text,
    });
  } catch (e) {
    clearTimeout(timer);
    return JSON.stringify({
      ok: false,
      error: String(e && e.message ? e.message : e),
    });
  }
})())JS",
      timeout_ms_, EscapeForJsStringLiteral(method_).c_str(),
      headers_json.c_str(), body_js.c_str(), EscapeForJsStringLiteral(url_).c_str());

  frame->ExecuteJavaScript(
      base::UTF8ToUTF16(js),
      base::BindOnce(&RequestAction::OnFetchResult,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void RequestAction::OnFetchResult(Callback callback, base::Value result) {
  std::string raw;
  if (result.is_string()) {
    raw = result.GetString();
  } else {
    if (!base::JSONWriter::Write(result, &raw)) {
      Fail(std::move(callback), "Request returned unreadable result");
      return;
    }
  }

  auto parsed =
      base::JSONReader::Read(raw, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    Fail(std::move(callback), "Request returned invalid JSON payload");
    return;
  }
  const base::Value::Dict& dict = parsed->GetDict();
  if (!dict.FindBool("ok").value_or(false)) {
    const std::string* error = dict.FindString("error");
    Fail(std::move(callback),
         error && !error->empty() ? *error : "Request failed");
    return;
  }

  const int status = dict.FindInt("status").value_or(0);
  if (!assert_status_.count(status)) {
    Fail(std::move(callback),
         "Unexpected HTTP status " + base::NumberToString(status));
    return;
  }

  const std::string* body = dict.FindString("body");
  const std::string body_text = body ? *body : std::string();

  if (!capture_as_.empty() && persist_to_data_) {
    dashboard::PersistDataVariable(profile_, capture_as_, body_text);
  }

  std::move(callback).Run(true, body_text);
}
