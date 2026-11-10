// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_REQUEST_ACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_REQUEST_ACTION_H_

#include <set>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/dashboard/actions/action_base.h"

class RequestAction : public dashboard::actions::Action {
 public:
  explicit RequestAction(Profile* profile);
  ~RequestAction() override;

  void Execute(content::WebContents* web_contents, Callback callback) override;
  bool SetParams(const base::Value::Dict& params) override;

 private:
  void OnFetchResult(Callback callback, base::Value result);
  void Fail(Callback callback, const std::string& error);

  std::string method_ = "GET";
  std::string url_;
  base::Value::Dict headers_;
  std::string body_;
  std::string capture_as_;
  bool persist_to_data_ = true;
  std::set<int> assert_status_{200};
  int timeout_ms_ = 30000;

  base::WeakPtrFactory<RequestAction> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_ACTIONS_REQUEST_ACTION_H_
