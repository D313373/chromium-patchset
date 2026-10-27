// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/embedded_executor_bridge.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/simulachrome/agent_input_injector.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service.h"
#include "chrome/browser/ui/webui/dashboard/script_execution_service_factory.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "services/simulachrome/public/mojom/simulachrome_agent.mojom.h"

namespace simulachrome {
namespace {

base::Value::Dict MakeCoordinates(int x, int y) {
  base::Value::Dict coordinates;
  coordinates.Set("x", x);
  coordinates.Set("y", y);
  return coordinates;
}

base::Value::Dict MakeClick(int x,
                            int y,
                            int click_count = 1,
                            const char* button = "left") {
  base::Value::Dict params;
  params.Set("coordinates", MakeCoordinates(x, y));
  // A grounded point is already the target. Jitter walks off small controls.
  params.Set("humanOffset", false);
  params.Set("forceActivation", true);
  params.Set("clickCount", click_count);
  params.Set("button", button);
  base::Value::Dict action;
  action.Set("action", "click");
  action.Set("params", std::move(params));
  return action;
}

base::Value::Dict MakeHover(int x, int y) {
  base::Value::Dict params;
  params.Set("coordinates", MakeCoordinates(x, y));
  params.Set("humanOffset", false);
  base::Value::Dict action;
  action.Set("action", "hover");
  action.Set("params", std::move(params));
  return action;
}

// Drag always runs on CDP (ActionFactory forces it): native drag latches to the
// real cursor position on macOS and drops wherever that happens to be.
base::Value::Dict MakeDrag(int from_x, int from_y, int to_x, int to_y) {
  base::Value::Dict params;
  params.Set("fromCoordinates", MakeCoordinates(from_x, from_y));
  params.Set("toCoordinates", MakeCoordinates(to_x, to_y));
  params.Set("humanOffset", false);
  params.Set("steps", 12);
  base::Value::Dict action;
  action.Set("action", "drag");
  action.Set("params", std::move(params));
  return action;
}

base::Value::Dict MakeType(const std::string& text) {
  base::Value::Dict params;
  params.Set("text", text);
  base::Value::Dict action;
  action.Set("action", "type");
  action.Set("params", std::move(params));
  return action;
}

base::Value::Dict MakeKeyPress(const std::string& key, int count) {
  base::Value::Dict params;
  params.Set("key", key);
  params.Set("count", std::max(1, count));
  base::Value::Dict action;
  action.Set("action", "key_press");
  action.Set("params", std::move(params));
  return action;
}

base::Value::Dict MakeScroll(double distance) {
  base::Value::Dict params;
  params.Set("element", "window");
  params.Set("distance", distance);
  base::Value::Dict action;
  action.Set("action", "scroll");
  action.Set("params", std::move(params));
  return action;
}

bool DipForAction(content::WebContents* web_contents,
                  const mojom::AgentAction& action,
                  int* x,
                  int* y) {
  return AgentInputInjector::NormalizedToWidgetDip(web_contents, action.x,
                                                   action.y, x, y);
}

// Trailing newlines are a submit, matching the injector. The characters
// themselves are not typed.
void SplitSubmit(std::string* text, int* submits) {
  *submits = 0;
  while (!text->empty() &&
         (text->back() == '\n' || text->back() == '\r')) {
    const char last = text->back();
    text->pop_back();
    if (last == '\n' && !text->empty() && text->back() == '\r') {
      text->pop_back();
    }
    ++(*submits);
  }
}

double ViewportHeight(content::WebContents* web_contents) {
  content::RenderWidgetHostView* view =
      web_contents ? web_contents->GetRenderWidgetHostView() : nullptr;
  if (!view) {
    return 800.0;
  }
  return static_cast<double>(std::max(1, view->GetViewBounds().height()));
}

bool BuildTimeline(content::WebContents* web_contents,
                   const mojom::AgentAction& action,
                   base::Value::List* timeline,
                   std::string* error) {
  switch (action.type) {
    case mojom::AgentActionType::kClick: {
      int x = 0;
      int y = 0;
      if (!DipForAction(web_contents, action, &x, &y)) {
        *error = "script_executor_no_view";
        return false;
      }
      LOG(INFO) << "[EmbeddedExecutor] click via ScriptExecutor dip=(" << x
                << "," << y << ")";
      timeline->Append(MakeClick(x, y));
      return true;
    }
    case mojom::AgentActionType::kDoubleClick:
    case mojom::AgentActionType::kRightClick: {
      int x = 0;
      int y = 0;
      if (!DipForAction(web_contents, action, &x, &y)) {
        *error = "script_executor_no_view";
        return false;
      }
      const bool dbl = action.type == mojom::AgentActionType::kDoubleClick;
      LOG(INFO) << "[EmbeddedExecutor] " << (dbl ? "double" : "right")
                << " click via ScriptExecutor dip=(" << x << "," << y << ")";
      timeline->Append(MakeClick(x, y, dbl ? 2 : 1, dbl ? "left" : "right"));
      return true;
    }
    case mojom::AgentActionType::kHover: {
      int x = 0;
      int y = 0;
      if (!DipForAction(web_contents, action, &x, &y)) {
        *error = "script_executor_no_view";
        return false;
      }
      timeline->Append(MakeHover(x, y));
      return true;
    }
    case mojom::AgentActionType::kDrag: {
      int fx = 0;
      int fy = 0;
      int tx = 0;
      int ty = 0;
      if (!DipForAction(web_contents, action, &fx, &fy) ||
          !AgentInputInjector::NormalizedToWidgetDip(
              web_contents, action.end_x, action.end_y, &tx, &ty)) {
        *error = "script_executor_no_view";
        return false;
      }
      LOG(INFO) << "[EmbeddedExecutor] drag via ScriptExecutor (" << fx << ","
                << fy << ") -> (" << tx << "," << ty << ")";
      timeline->Append(MakeDrag(fx, fy, tx, ty));
      return true;
    }
    case mojom::AgentActionType::kType: {
      std::string text = action.payload;
      int submits = 0;
      SplitSubmit(&text, &submits);
      if (text.empty() && submits == 0) {
        *error = "script_executor_empty_type";
        return false;
      }
      int x = 0;
      int y = 0;
      if (!DipForAction(web_contents, action, &x, &y)) {
        *error = "script_executor_no_view";
        return false;
      }
      LOG(INFO) << "[EmbeddedExecutor] type via ScriptExecutor len="
                << text.size() << " submit=" << submits;
      timeline->Append(MakeClick(x, y));
      if (!text.empty()) {
        timeline->Append(MakeType(text));
      }
      if (submits > 0) {
        timeline->Append(MakeKeyPress("Enter", submits));
      }
      return true;
    }
    case mojom::AgentActionType::kKeyPress: {
      const std::string key(
          base::TrimWhitespaceASCII(action.payload, base::TRIM_ALL));
      if (key.empty()) {
        *error = "script_executor_empty_key";
        return false;
      }
      LOG(INFO) << "[EmbeddedExecutor] key_press via ScriptExecutor";
      timeline->Append(MakeKeyPress(key, 1));
      return true;
    }
    case mojom::AgentActionType::kScroll: {
      const double height = ViewportHeight(web_contents);
      double distance = static_cast<double>(action.scroll_dy) * height;
      if (std::abs(distance) < 40.0) {
        distance = (action.scroll_dy < 0.f) ? -600.0 : 600.0;
      }
      distance = std::clamp(distance, -4000.0, 4000.0);
      LOG(INFO) << "[EmbeddedExecutor] scroll via ScriptExecutor distance="
                << distance;
      timeline->Append(MakeScroll(distance));
      return true;
    }
    case mojom::AgentActionType::kWait:
    case mojom::AgentActionType::kDone:
    case mojom::AgentActionType::kNone:
    case mojom::AgentActionType::kFail:
    case mojom::AgentActionType::kElementNotFound:
      return true;
  }
  *error = "script_executor_unsupported";
  return false;
}

void RunThroughScriptExecutor(content::WebContents* web_contents,
                              const mojom::AgentAction& action,
                              AgentInputInjector::ApplyCallback done) {
  if (!web_contents) {
    std::move(done).Run(false, "script_executor_no_web_contents");
    return;
  }
  if (action.type == mojom::AgentActionType::kWait ||
      action.type == mojom::AgentActionType::kDone ||
      action.type == mojom::AgentActionType::kNone ||
      action.type == mojom::AgentActionType::kFail ||
      action.type == mojom::AgentActionType::kElementNotFound) {
    std::move(done).Run(true, std::string());
    return;
  }

  base::Value::List timeline;
  std::string build_error;
  if (!BuildTimeline(web_contents, action, &timeline, &build_error)) {
    std::move(done).Run(false, build_error);
    return;
  }

  Profile* profile = Profile::FromBrowserContext(
      web_contents->GetBrowserContext());
  dashboard::ScriptExecutionService* service =
      profile ? dashboard::ScriptExecutionServiceFactory::GetForProfile(profile)
              : nullptr;
  if (!service) {
    std::move(done).Run(false, "script_executor_unavailable");
    return;
  }

  dashboard::ScriptExecutionService::RunOptions options;
  options.is_agent_mode = true;
  // StartAdHocRun drops the completion callback when it refuses the run
  // (no target, or the concurrency cap). Keep a second owner so that
  // refusal still finishes the visual step.
  auto done_holder =
      std::make_shared<AgentInputInjector::ApplyCallback>(std::move(done));
  const std::string execution_id = service->StartAdHocRunWithOptions(
      timeline, web_contents, options,
      base::BindOnce(
          [](std::shared_ptr<AgentInputInjector::ApplyCallback> done,
             bool success, int failed_step, std::string message) {
            if (!done || !*done) {
              return;
            }
            // On success the message stays empty. It used to be the literal
            // "script_executor", which callers read as the element that was
            // clicked and which made every click look like a miss to graders.
            // The element under the pointer comes from the host's pre-dispatch
            // probe instead.
            if (message.empty() && !success) {
              message = "script_executor_failed";
            }
            if (!success && failed_step >= 0) {
              message = base::StringPrintf("step %d: %s", failed_step,
                                           message.c_str());
            }
            std::move(*done).Run(success, std::move(message));
          },
          done_holder));
  if (execution_id.empty() && done_holder && *done_holder) {
    std::move(*done_holder).Run(false, "script_executor_busy");
  }
}

}  // namespace

void InstallEmbeddedExecutorBridge() {
  AgentInputInjector::SetApplyOverride(
      base::BindRepeating(&RunThroughScriptExecutor));
  AgentInputInjector::SetTokenExpander(base::BindRepeating(
      [](content::WebContents* web_contents, const std::string& text) {
        Profile* profile =
            web_contents
                ? Profile::FromBrowserContext(web_contents->GetBrowserContext())
                : nullptr;
        return dashboard::ScriptExecutionService::ExpandExecutionTokens(
            profile, text);
      }));
  LOG(INFO) << "[EmbeddedExecutor] visual steps run through ScriptExecutor";
}

}  // namespace simulachrome
