// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_ACTOR_KEY_PRESS_TOOL_H_
#define CHROME_RENDERER_ACTOR_KEY_PRESS_TOOL_H_

#include <string>

#include "chrome/common/actor.mojom.h"
#include "chrome/common/actor/task_id.h"
#include "chrome/renderer/actor/tool_base.h"

namespace content {
class RenderFrame;
}  // namespace content

namespace actor {

// Dispatches a trusted key press (RawKeyDown, optional Char, KeyUp) to the
// frame's focused element. Dispatch is fully synchronous (no posted tasks) so
// it cannot stall when invoked from within a tool call. The target is ignored
// apart from validation; callers pass a dummy coordinate.
class KeyPressTool : public ToolBase {
 public:
  KeyPressTool(content::RenderFrame& frame,
               TaskId task_id,
               Journal& journal,
               mojom::KeyPressActionPtr action,
               mojom::ToolTargetPtr target,
               mojom::ObservedToolTargetPtr observed_target);
  ~KeyPressTool() override;

  // actor::ToolBase
  void Execute(ToolFinishedCallback callback) override;
  std::string DebugString() const override;
  bool EnsureTargetInView() override;

 private:
  mojom::KeyPressActionPtr action_;
};

}  // namespace actor

#endif  // CHROME_RENDERER_ACTOR_KEY_PRESS_TOOL_H_
