// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_TRAFFIC_CAPTURE_STORE_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_TRAFFIC_CAPTURE_STORE_H_

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>

#include "base/containers/flat_map.h"
#include "base/values.h"

namespace dashboard {

// Bounded ring buffer of CDP Network.* requests observed while recording.
class TrafficCaptureStore {
 public:
  static constexpr size_t kDefaultCapacity = 500;

  explicit TrafficCaptureStore(size_t capacity = kDefaultCapacity);
  ~TrafficCaptureStore();

  TrafficCaptureStore(const TrafficCaptureStore&) = delete;
  TrafficCaptureStore& operator=(const TrafficCaptureStore&) = delete;

  void Clear();
  size_t size() const { return entries_.size(); }

  // Ingest CDP Network domain events. |method| is e.g. "Network.requestWillBeSent".
  void OnNetworkEvent(const std::string& method, const base::Value::Dict& params);

  // Snapshot newest-first for the dashboard traffic panel.
  base::Value::List ListEntries(size_t limit = 100) const;

  // Full entry including request/response headers and optional body.
  std::optional<base::Value::Dict> GetEntry(const std::string& request_id) const;

  // Attach a lazily-fetched response body.
  bool SetResponseBody(const std::string& request_id,
                       const std::string& body,
                       bool base64_encoded);

  // Convert a stored entry into a canonical request action dict:
  // {action:"request", params:{...}}.
  // When |secret_tokens| maps secret values -> token names, matching header
  // values are rewritten to "{TokenName}".
  static base::Value::Dict EntryToRequestAction(
      const base::Value::Dict& entry,
      const base::flat_map<std::string, std::string>& secret_value_to_token = {});

  // True when resourceType / MIME should be skipped (images, fonts, css, media).
  static bool ShouldSkipResource(const std::string& resource_type,
                                 const std::string& mime_type);

 private:
  struct Entry {
    Entry();
    ~Entry();
    Entry(Entry&&);
    Entry& operator=(Entry&&);
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;

    std::string request_id;
    std::string method = "GET";
    std::string url;
    std::string resource_type;
    std::string mime_type;
    int status = 0;
    base::Value::Dict request_headers;
    base::Value::Dict response_headers;
    std::string request_body;
    std::string response_body;
    bool response_body_base64 = false;
    bool has_response = false;
    bool finished = false;
    double timestamp = 0;

    base::Value::Dict ToDict(bool include_bodies) const;
  };

  Entry* FindMutable(const std::string& request_id);
  const Entry* Find(const std::string& request_id) const;
  void TrimToCapacity();

  size_t capacity_;
  std::deque<Entry> entries_;
  // request_id -> index into entries_ (rebuilt on trim; kept as map of ids).
  std::map<std::string, Entry*> by_id_;
};

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_TRAFFIC_CAPTURE_STORE_H_
