// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/dashboard_devtools_client.h"

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/strings/utf_string_conversions.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace dashboard {

DashboardDevToolsClient::DashboardDevToolsClient(
    content::WebContents* web_contents)
    : web_contents_(web_contents) {
  AttachToDevTools();
}

DashboardDevToolsClient::~DashboardDevToolsClient() {
  Detach();
}

void DashboardDevToolsClient::Detach() {
  if (agent_host_) {
    agent_host_->DetachClient(this);
    agent_host_ = nullptr;
  }
  FailAllPendingCommands("DevTools client detached");
}

int DashboardDevToolsClient::SendCommand(const std::string& method,
                                         base::Value::Dict params) {
  return SendCommand(method, std::move(params), CommandCallback());
}

int DashboardDevToolsClient::SendCommand(const std::string& method,
                                         base::Value::Dict params,
                                         CommandCallback callback) {
  if (!EnsureAttached()) {
    LOG(WARNING)
        << "[DashboardDevToolsClient] Cannot send command, no agent host";
    if (callback) {
      std::move(callback).Run(false, "DevTools client not attached");
    }
    return 0;
  }

  base::Value::Dict cmd;
  const int id = next_command_id_++;
  cmd.Set("id", id);
  cmd.Set("method", method);
  cmd.Set("params", std::move(params));
  if (callback) {
    pending_commands_.emplace(id, std::move(callback));
  }

  std::string json;
  if (!base::JSONWriter::Write(cmd, &json)) {
    LOG(ERROR) << "[DashboardDevToolsClient] Failed to serialize command";
    CompletePendingCommand(id, false, "Failed to serialize command");
    return 0;
  }

  agent_host_->DispatchProtocolMessage(this, base::as_bytes(base::span(json)));

  return id;
}

int DashboardDevToolsClient::SendCommandForResult(
    const std::string& method,
    base::Value::Dict params,
    CommandResultCallback callback) {
  if (!EnsureAttached()) {
    LOG(WARNING)
        << "[DashboardDevToolsClient] Cannot send command, no agent host";
    if (callback) {
      std::move(callback).Run(false, "DevTools client not attached",
                              base::Value::Dict());
    }
    return 0;
  }

  base::Value::Dict cmd;
  const int id = next_command_id_++;
  cmd.Set("id", id);
  cmd.Set("method", method);
  cmd.Set("params", std::move(params));
  if (callback) {
    pending_result_commands_.emplace(id, std::move(callback));
  }

  std::string json;
  if (!base::JSONWriter::Write(cmd, &json)) {
    LOG(ERROR) << "[DashboardDevToolsClient] Failed to serialize command";
    CompletePendingCommand(id, false, "Failed to serialize command");
    return 0;
  }

  agent_host_->DispatchProtocolMessage(this, base::as_bytes(base::span(json)));
  return id;
}

int DashboardDevToolsClient::AddEventObserver(EventCallback callback) {
  if (!callback) {
    return 0;
  }
  const int observer_id = next_event_observer_id_++;
  event_observers_.emplace(observer_id, std::move(callback));
  return observer_id;
}

void DashboardDevToolsClient::RemoveEventObserver(int observer_id) {
  event_observers_.erase(observer_id);
}

void DashboardDevToolsClient::DispatchProtocolMessage(
    content::DevToolsAgentHost* agent_host,
    base::span<const uint8_t> message) {
  std::string msg_str(reinterpret_cast<const char*>(message.data()),
                      message.size());

  std::optional<base::Value> parsed =
      base::JSONReader::Read(msg_str, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    return;
  }

  const base::Value::Dict& dict = parsed->GetDict();

  // Handle command responses (matched by id)
  if (const base::Value* idv = dict.Find("id")) {
    if (idv->is_int()) {
      int id = idv->GetInt();
      const base::Value::Dict* error = dict.FindDict("error");
      if (error && !error->empty()) {
        const std::string* msg = error->FindString("message");
        CompletePendingCommand(id, false, msg ? *msg : "CDP command failed");
      } else {
        const base::Value::Dict* result = dict.FindDict("result");
        CompletePendingCommand(id, true, "",
                               result ? result->Clone() : base::Value::Dict());
      }

      // Log responses for debugging
      if (error && !error->empty()) {
        const std::string* msg = error->FindString("message");
        VLOG(2) << "[DashboardDevToolsClient] Command " << id
                << " failed: " << (msg ? *msg : "unknown error");
      } else {
        VLOG(2) << "[DashboardDevToolsClient] Command " << id << " succeeded";
      }
    }
  }

  // Handle events (have "method" but no "id")
  if (const std::string* method = dict.FindString("method")) {
    VLOG(2) << "[DashboardDevToolsClient] Event: " << *method;
    const base::Value::Dict* params = dict.FindDict("params");
    const base::Value::Dict empty_params;
    for (const auto& [observer_id, callback] : event_observers_) {
      if (callback) {
        callback.Run(*method, params ? *params : empty_params);
      }
    }
  }
}

void DashboardDevToolsClient::AgentHostClosed(
    content::DevToolsAgentHost* agent_host) {
  if (agent_host == agent_host_.get()) {
    agent_host_ = nullptr;
    FailAllPendingCommands("DevTools agent host closed");
  }
}

bool DashboardDevToolsClient::IsTrusted() {
  // This client is used for dashboard automation which is trusted
  return true;
}

bool DashboardDevToolsClient::UsesBinaryProtocol() {
  return false;
}

std::string DashboardDevToolsClient::GetTypeForMetrics() {
  // ChromeDevToolsSession only accepts a small allowlist of client types.
  // Use the generic bucket so routed CDP fallbacks cannot trip a DCHECK.
  return "Other";
}

void DashboardDevToolsClient::AttachToDevTools() {
  EnsureAttached();
}

bool DashboardDevToolsClient::EnsureAttached() {
  if (agent_host_) {
    return true;
  }

  if (!web_contents_) {
    return false;
  }
  if (web_contents_->IsBeingDestroyed()) {
    return false;
  }

  agent_host_ = content::DevToolsAgentHost::GetOrCreateFor(web_contents_);
  if (!agent_host_) {
    return false;
  }
  if (!agent_host_->AttachClient(this)) {
    LOG(ERROR) << "[DashboardDevToolsClient] Failed to attach DevTools client";
    agent_host_ = nullptr;
    return false;
  }
  VLOG(1) << "[DashboardDevToolsClient] Attached to DevTools for WebContents";
  return true;
}

void DashboardDevToolsClient::CompletePendingCommand(int id,
                                                     bool success,
                                                     const std::string& error,
                                                     base::Value::Dict result) {
  auto it = pending_commands_.find(id);
  if (it != pending_commands_.end()) {
    CommandCallback callback = std::move(it->second);
    pending_commands_.erase(it);
    if (callback) {
      std::move(callback).Run(success, error);
    }
  }
  auto result_it = pending_result_commands_.find(id);
  if (result_it != pending_result_commands_.end()) {
    CommandResultCallback callback = std::move(result_it->second);
    pending_result_commands_.erase(result_it);
    if (callback) {
      std::move(callback).Run(success, error, std::move(result));
    }
  }
}

void DashboardDevToolsClient::FailAllPendingCommands(const std::string& error) {
  auto pending = std::move(pending_commands_);
  pending_commands_.clear();
  for (auto& [id, callback] : pending) {
    if (callback) {
      std::move(callback).Run(false, error);
    }
  }

  auto pending_results = std::move(pending_result_commands_);
  pending_result_commands_.clear();
  for (auto& [id, callback] : pending_results) {
    if (callback) {
      std::move(callback).Run(false, error, base::Value::Dict());
    }
  }
}

namespace {

void CaptureViaCopyFromSurface(
    content::WebContents* web_contents,
    DashboardDevToolsClient::ScreenshotCallback callback) {
  if (!web_contents) {
    std::move(callback).Run(false, "No web contents", std::string());
    return;
  }
  content::RenderWidgetHostView* view = web_contents->GetRenderWidgetHostView();
  if (!view) {
    std::move(callback).Run(false, "Listener surface unavailable for screenshot.",
                            std::string());
    return;
  }
  if (view->GetViewBounds().size().IsEmpty()) {
    view->SetSize(gfx::Size(1280, 720));
  }
  if (!view->IsSurfaceAvailableForCopy()) {
    std::move(callback).Run(false, "Listener surface unavailable for screenshot.",
                            std::string());
    return;
  }
  view->CopyFromSurface(
      gfx::Rect(), gfx::Size(),
      base::BindOnce(
          [](DashboardDevToolsClient::ScreenshotCallback callback,
             const viz::CopyOutputBitmapWithMetadata& result) {
            if (result.bitmap.drawsNothing()) {
              std::move(callback).Run(false, "Captured empty screenshot.",
                                      std::string());
              return;
            }
            std::optional<std::vector<uint8_t>> png =
                gfx::PNGCodec::EncodeBGRASkBitmap(
                    result.bitmap, /*discard_transparency=*/false);
            if (!png || png->empty()) {
              std::move(callback).Run(false, "Failed to encode screenshot.",
                                      std::string());
              return;
            }
            std::move(callback).Run(
                true, std::string(),
                base::Base64Encode(base::span<const uint8_t>(*png)));
          },
          std::move(callback)));
}

}  // namespace

void DashboardDevToolsClient::CapturePngBase64(
    content::WebContents* web_contents,
    ScreenshotCallback callback) {
  if (!web_contents) {
    std::move(callback).Run(false, "No web contents", std::string());
    return;
  }
  auto client = std::make_unique<DashboardDevToolsClient>(web_contents);
  if (!client->agent_host_) {
    CaptureViaCopyFromSurface(web_contents, std::move(callback));
    return;
  }
  DashboardDevToolsClient* raw = client.release();
  base::WeakPtr<content::WebContents> weak_wc = web_contents->GetWeakPtr();

  auto finish_capture = base::BindOnce(
      [](DashboardDevToolsClient* raw,
         base::WeakPtr<content::WebContents> weak_wc,
         ScreenshotCallback callback, bool success, const std::string& error,
         base::Value::Dict result) {
        std::unique_ptr<DashboardDevToolsClient> owned(raw);
        const std::string* data =
            success ? result.FindString("data") : nullptr;
        if (data && !data->empty()) {
          std::move(callback).Run(true, std::string(), *data);
          return;
        }
        CaptureViaCopyFromSurface(
            weak_wc.get(),
            base::BindOnce(
                [](std::string cdp_error, ScreenshotCallback callback,
                   bool copy_ok, std::string copy_error, std::string png) {
                  if (copy_ok) {
                    std::move(callback).Run(true, std::string(),
                                            std::move(png));
                    return;
                  }
                  std::string err =
                      copy_error.empty()
                          ? (cdp_error.empty() ? "Captured empty screenshot."
                                               : cdp_error)
                          : copy_error;
                  std::move(callback).Run(false, std::move(err),
                                          std::string());
                },
                error, std::move(callback)));
      },
      raw, weak_wc, std::move(callback));

  auto capture_now = base::BindOnce(
      [](DashboardDevToolsClient* raw,
         DashboardDevToolsClient::CommandResultCallback finish, bool /*ok*/,
         const std::string& /*error*/, base::Value::Dict /*result*/) {
        base::Value::Dict params;
        params.Set("format", "png");
        params.Set("fromSurface", true);
        params.Set("captureBeyondViewport", true);
        raw->SendCommandForResult("Page.captureScreenshot", std::move(params),
                                  std::move(finish));
      },
      raw, std::move(finish_capture));

  auto set_metrics = base::BindOnce(
      [](DashboardDevToolsClient* raw,
         DashboardDevToolsClient::CommandResultCallback capture_now,
         bool /*ok*/, const std::string& /*error*/,
         base::Value::Dict /*result*/) {
        base::Value::Dict metrics;
        metrics.Set("width", 1280);
        metrics.Set("height", 720);
        metrics.Set("deviceScaleFactor", 1.0);
        metrics.Set("mobile", false);
        raw->SendCommandForResult("Emulation.setDeviceMetricsOverride",
                                  std::move(metrics), std::move(capture_now));
      },
      raw, std::move(capture_now));

  raw->SendCommandForResult("Page.enable", base::Value::Dict(),
                            std::move(set_metrics));
}

}  // namespace dashboard
