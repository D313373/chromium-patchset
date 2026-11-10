// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/traffic_capture_store.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

namespace dashboard {
namespace {

base::Value::Dict HeadersFromCdp(const base::Value::Dict* headers) {
  base::Value::Dict out;
  if (!headers) {
    return out;
  }
  for (auto it : *headers) {
    if (it.second.is_string()) {
      out.Set(it.first, it.second.GetString());
    } else if (it.second.is_int()) {
      out.Set(it.first, base::NumberToString(it.second.GetInt()));
    }
  }
  return out;
}

std::string Lower(const std::string& s) {
  return base::ToLowerASCII(s);
}

}  // namespace

TrafficCaptureStore::Entry::Entry() = default;
TrafficCaptureStore::Entry::~Entry() = default;
TrafficCaptureStore::Entry::Entry(Entry&&) = default;
TrafficCaptureStore::Entry& TrafficCaptureStore::Entry::operator=(Entry&&) =
    default;

base::Value::Dict TrafficCaptureStore::Entry::ToDict(
    bool include_bodies) const {
  base::Value::Dict dict;
  dict.Set("requestId", request_id);
  dict.Set("method", method);
  dict.Set("url", url);
  dict.Set("resourceType", resource_type);
  dict.Set("mimeType", mime_type);
  dict.Set("status", status);
  dict.Set("hasResponse", has_response);
  dict.Set("finished", finished);
  dict.Set("timestamp", timestamp);
  dict.Set("requestHeaders", request_headers.Clone());
  dict.Set("responseHeaders", response_headers.Clone());
  if (include_bodies) {
    dict.Set("requestBody", request_body);
    dict.Set("responseBody", response_body);
    dict.Set("responseBodyBase64", response_body_base64);
  } else {
    dict.Set("hasRequestBody", !request_body.empty());
    dict.Set("hasResponseBody", !response_body.empty());
  }
  return dict;
}

TrafficCaptureStore::TrafficCaptureStore(size_t capacity)
    : capacity_(capacity == 0 ? kDefaultCapacity : capacity) {}

TrafficCaptureStore::~TrafficCaptureStore() = default;

void TrafficCaptureStore::Clear() {
  entries_.clear();
  by_id_.clear();
}

bool TrafficCaptureStore::ShouldSkipResource(const std::string& resource_type,
                                             const std::string& mime_type) {
  const std::string type = Lower(resource_type);
  if (type == "image" || type == "font" || type == "media" ||
      type == "stylesheet" || type == "ping" || type == "cspviolationreport" ||
      type == "manifest") {
    return true;
  }
  const std::string mime = Lower(mime_type);
  if (base::StartsWith(mime, "image/", base::CompareCase::SENSITIVE) ||
      base::StartsWith(mime, "font/", base::CompareCase::SENSITIVE) ||
      base::StartsWith(mime, "audio/", base::CompareCase::SENSITIVE) ||
      base::StartsWith(mime, "video/", base::CompareCase::SENSITIVE) ||
      mime == "text/css") {
    return true;
  }
  return false;
}

TrafficCaptureStore::Entry* TrafficCaptureStore::FindMutable(
    const std::string& request_id) {
  auto it = by_id_.find(request_id);
  return it == by_id_.end() ? nullptr : it->second;
}

const TrafficCaptureStore::Entry* TrafficCaptureStore::Find(
    const std::string& request_id) const {
  auto it = by_id_.find(request_id);
  return it == by_id_.end() ? nullptr : it->second;
}

void TrafficCaptureStore::TrimToCapacity() {
  while (entries_.size() > capacity_) {
    by_id_.erase(entries_.front().request_id);
    entries_.pop_front();
  }
}

void TrafficCaptureStore::OnNetworkEvent(const std::string& method,
                                         const base::Value::Dict& params) {
  if (method == "Network.requestWillBeSent") {
    const std::string* request_id = params.FindString("requestId");
    const base::Value::Dict* request = params.FindDict("request");
    if (!request_id || request_id->empty() || !request) {
      return;
    }
    const std::string* url = request->FindString("url");
    if (!url || url->empty()) {
      return;
    }
    std::string resource_type;
    if (const std::string* type = params.FindString("type")) {
      resource_type = *type;
    }
    if (ShouldSkipResource(resource_type, std::string())) {
      return;
    }

    Entry* existing = FindMutable(*request_id);
    if (!existing) {
      Entry entry;
      entry.request_id = *request_id;
      entries_.push_back(std::move(entry));
      TrimToCapacity();
      by_id_[*request_id] = &entries_.back();
      existing = &entries_.back();
    }
    existing->url = *url;
    if (const std::string* req_method = request->FindString("method")) {
      existing->method = base::ToUpperASCII(*req_method);
    }
    existing->resource_type = resource_type;
    existing->request_headers = HeadersFromCdp(request->FindDict("headers"));
    if (const base::Value::Dict* post = request->FindDict("postDataEntries")) {
      // Rare shape; ignore and fall through to postData string.
      (void)post;
    }
    if (const std::string* post_data = request->FindString("postData")) {
      existing->request_body = *post_data;
    }
    if (std::optional<double> ts = params.FindDouble("timestamp")) {
      existing->timestamp = *ts;
    }
    return;
  }

  if (method == "Network.responseReceived") {
    const std::string* request_id = params.FindString("requestId");
    const base::Value::Dict* response = params.FindDict("response");
    if (!request_id || !response) {
      return;
    }
    Entry* entry = FindMutable(*request_id);
    if (!entry) {
      return;
    }
    if (const std::string* mime = response->FindString("mimeType")) {
      entry->mime_type = *mime;
    }
    if (ShouldSkipResource(entry->resource_type, entry->mime_type)) {
      by_id_.erase(*request_id);
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->request_id == *request_id) {
          entries_.erase(it);
          break;
        }
      }
      return;
    }
    entry->status = response->FindInt("status").value_or(0);
    entry->response_headers = HeadersFromCdp(response->FindDict("headers"));
    entry->has_response = true;
    return;
  }

  if (method == "Network.loadingFinished") {
    const std::string* request_id = params.FindString("requestId");
    if (!request_id) {
      return;
    }
    if (Entry* entry = FindMutable(*request_id)) {
      entry->finished = true;
    }
  }
}

base::Value::List TrafficCaptureStore::ListEntries(size_t limit) const {
  base::Value::List out;
  if (limit == 0) {
    limit = 100;
  }
  size_t count = 0;
  for (auto it = entries_.rbegin(); it != entries_.rend() && count < limit;
       ++it, ++count) {
    out.Append(it->ToDict(/*include_bodies=*/false));
  }
  return out;
}

std::optional<base::Value::Dict> TrafficCaptureStore::GetEntry(
    const std::string& request_id) const {
  const Entry* entry = Find(request_id);
  if (!entry) {
    return std::nullopt;
  }
  return entry->ToDict(/*include_bodies=*/true);
}

bool TrafficCaptureStore::SetResponseBody(const std::string& request_id,
                                          const std::string& body,
                                          bool base64_encoded) {
  Entry* entry = FindMutable(request_id);
  if (!entry) {
    return false;
  }
  entry->response_body = body;
  entry->response_body_base64 = base64_encoded;
  return true;
}

base::Value::Dict TrafficCaptureStore::EntryToRequestAction(
    const base::Value::Dict& entry,
    const base::flat_map<std::string, std::string>& secret_value_to_token) {
  base::Value::Dict params;
  params.Set("method", entry.FindString("method")
                           ? *entry.FindString("method")
                           : "GET");
  params.Set("url", entry.FindString("url") ? *entry.FindString("url") : "");

  base::Value::Dict headers;
  if (const base::Value::Dict* req_headers = entry.FindDict("requestHeaders")) {
    for (auto it : *req_headers) {
      if (!it.second.is_string()) {
        continue;
      }
      std::string value = it.second.GetString();
      auto tok = secret_value_to_token.find(value);
      if (tok != secret_value_to_token.end() && !tok->second.empty()) {
        value = "{" + tok->second + "}";
      } else {
        const std::string name = Lower(it.first);
        if ((name == "authorization" || name == "cookie" ||
             name == "x-api-key" || name == "x-auth-token") &&
            !secret_value_to_token.empty()) {
          // Leave literal if no exact secret match; user can edit.
        }
      }
      headers.Set(it.first, value);
    }
  }
  params.Set("headers", headers.Clone());
  std::string headers_json;
  if (!base::JSONWriter::Write(base::Value(headers.Clone()), &headers_json)) {
    headers_json = "{}";
  }
  params.Set("headersJson", headers_json);

  if (const std::string* body = entry.FindString("requestBody")) {
    params.Set("body", *body);
  } else {
    params.Set("body", "");
  }
  params.Set("assertStatus",
             base::NumberToString(entry.FindInt("status").value_or(200)));
  params.Set("timeoutMs", 30000);
  params.Set("persistToData", true);

  base::Value::Dict action;
  action.Set("action", "request");
  action.Set("params", std::move(params));
  action.Set("comment", "");
  return action;
}

}  // namespace dashboard
