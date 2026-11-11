// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SIMULACHROME_GROUNDED_COMMAND_H_
#define CHROME_BROWSER_SIMULACHROME_GROUNDED_COMMAND_H_

#include <string>

namespace simulachrome {

// The manager names an operation. The embedded model only locates a control.
// Scaffolding performs the operation at that point.
struct GroundedCommand {
  enum class Kind {
    kClick,
    kType,
    kKey,
    kScroll,
    kWait,
  };

  GroundedCommand();
  ~GroundedCommand();
  GroundedCommand(const GroundedCommand&);
  GroundedCommand& operator=(const GroundedCommand&);
  GroundedCommand(GroundedCommand&&);
  GroundedCommand& operator=(GroundedCommand&&);

  Kind kind = Kind::kClick;
  // What to ask the model to find. Empty when |needs_target| is false.
  std::string target;
  // Text for kType. May be a {Token}. Never sent to the model.
  std::string text;
  // Key name for kKey ("enter", "tab", ...).
  std::string key;
  // Viewport fraction, positive is down. Used for kScroll.
  float scroll_dy = 0.8f;
  bool needs_target = true;
};

GroundedCommand ParseGroundedCommand(const std::string& instruction);

// The only sentence the model sees. No verb, no text to type, no tokens.
std::string LocatePromptFor(const GroundedCommand& command);

}  // namespace simulachrome

#endif  // CHROME_BROWSER_SIMULACHROME_GROUNDED_COMMAND_H_
