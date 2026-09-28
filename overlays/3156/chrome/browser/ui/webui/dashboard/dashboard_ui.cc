// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/dashboard_ui.h"

#include "base/containers/span.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/dashboard/dashboard.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_handler.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_data_source.h"
#include "chrome/browser/ui/webui/dashboard/dashboard_ui_message_handler.h"
#include "chrome/common/url_constants.h"
#include "chrome/browser/resources/dashboard/grit/dashboard_resources.h"
#include "chrome/browser/resources/dashboard/grit/dashboard_resources_map.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/bindings_policy.h"
#include "content/public/common/url_constants.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/webui/webui_util.h"

DashboardUI::DashboardUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui),
      dashboard_(std::make_unique<dashboard::Dashboard>()) {
  // Enable chrome.send() for message handler communication
  web_ui->SetBindings(content::kWebUIBindingsPolicySet);

  content::BrowserContext* browser_context =
      web_ui->GetWebContents()->GetBrowserContext();

  // Add the dashboard data source
  content::URLDataSource::Add(
      browser_context, std::make_unique<DashboardDataSource>());

  // Attach our message handler for scripts CRUD + playback.
  profile_ = Profile::FromBrowserContext(browser_context);
  auto message_handler =
      std::make_unique<DashboardUIMessageHandler>(profile_, dashboard_.get());
  message_handler_ = message_handler.get();
  web_ui->AddMessageHandler(std::move(message_handler));
}

void DashboardUI::BindInterface(
    mojo::PendingReceiver<dashboard::mojom::DashboardHandler> receiver) {
  handler_ = std::make_unique<DashboardHandler>(
      std::move(receiver), profile_, dashboard_.get());
}

// WebUIImpl destroys the controller before its message handlers, so the
// handler must drop its Dashboard pointer before `dashboard_` goes away.
DashboardUI::~DashboardUI() {
  if (message_handler_) {
    message_handler_->DetachDashboard();
  }
  handler_.reset();
  dashboard_.reset();
}
