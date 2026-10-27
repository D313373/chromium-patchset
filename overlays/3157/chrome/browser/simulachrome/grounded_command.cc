// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/simulachrome/grounded_command.h"

#include <string_view>

#include "base/strings/string_util.h"

namespace simulachrome {

GroundedCommand::GroundedCommand() = default;
GroundedCommand::~GroundedCommand() = default;
GroundedCommand::GroundedCommand(const GroundedCommand&) = default;
GroundedCommand& GroundedCommand::operator=(const GroundedCommand&) = default;
GroundedCommand::GroundedCommand(GroundedCommand&&) = default;
GroundedCommand& GroundedCommand::operator=(GroundedCommand&&) = default;

namespace {

constexpr float kScrollFraction = 0.8f;

bool StartsWithWord(std::string_view text, std::string_view word) {
  if (!base::StartsWith(text, word, base::CompareCase::INSENSITIVE_ASCII)) {
    return false;
  }
  if (text.size() == word.size()) {
    return true;
  }
  const char next = text[word.size()];
  return next == ' ' || next == '\t' || next == ':' || next == ',';
}

std::string RestAfter(const std::string& text, size_t word_len) {
  return std::string(base::TrimWhitespaceASCII(text.substr(word_len),
                                               base::TRIM_ALL));
}

std::string CollapseSpace(std::string text) {
  std::string out;
  out.reserve(text.size());
  bool pending_space = false;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n') {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(c);
  }
  return std::string(base::TrimWhitespaceASCII(out, base::TRIM_ALL));
}

bool IsTokenChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '#' || c == '.';
}

// Prefer a {Token}. Otherwise a single-quoted or double-quoted literal.
bool TakePayload(std::string* text, std::string* payload) {
  const size_t brace = text->find('{');
  if (brace != std::string::npos) {
    const size_t end = text->find('}', brace + 1);
    if (end != std::string::npos && end > brace + 1) {
      bool ok = true;
      for (size_t i = brace + 1; i < end; ++i) {
        if (!IsTokenChar((*text)[i])) {
          ok = false;
          break;
        }
      }
      if (ok) {
        *payload = text->substr(brace, end - brace + 1);
        text->erase(brace, end - brace + 1);
        *text = CollapseSpace(*text);
        return true;
      }
    }
  }
  for (char quote : {'\'', '"'}) {
    const size_t open = text->find(quote);
    if (open == std::string::npos) {
      continue;
    }
    const size_t close = text->find(quote, open + 1);
    if (close == std::string::npos || close == open + 1) {
      continue;
    }
    *payload = text->substr(open + 1, close - open - 1);
    text->erase(open, close - open + 1);
    *text = CollapseSpace(*text);
    return true;
  }

  // "Type user@example.com into Your email address" has no quotes and no
  // {token}. The words before the lead-in are the text.
  const std::string lower = base::ToLowerASCII(*text);
  static constexpr const char* kLeadIns[] = {
      " into the ", " onto the ", " into ", " in the ", " on the ",
      " at the ",   " in ",       " on ",   " at ",
  };
  size_t best = std::string::npos;
  size_t best_len = 0;
  for (const char* lead : kLeadIns) {
    const size_t pos = lower.find(lead);
    if (pos == std::string::npos || pos == 0 || pos >= best) {
      continue;
    }
    best = pos;
    best_len = std::char_traits<char>::length(lead);
  }
  if (best == std::string::npos) {
    return false;
  }
  *payload = std::string(
      base::TrimWhitespaceASCII(text->substr(0, best), base::TRIM_ALL));
  if (payload->empty()) {
    return false;
  }
  *text = std::string(base::TrimWhitespaceASCII(text->substr(best + best_len),
                                                base::TRIM_ALL));
  return true;
}

void StripLeadIn(std::string* target) {
  static constexpr const char* kPrefixes[] = {
      "into the ", "onto the ", "into ", "in the ", "on the ",
      "at the ",   "in ",       "on ",   "at ",
  };
  bool stripped = true;
  while (stripped) {
    stripped = false;
    for (const char* prefix : kPrefixes) {
      if (base::StartsWith(*target, prefix,
                           base::CompareCase::INSENSITIVE_ASCII)) {
        *target = std::string(base::TrimWhitespaceASCII(
            target->substr(std::char_traits<char>::length(prefix)),
            base::TRIM_ALL));
        stripped = true;
        break;
      }
    }
  }
}

std::string StripPurpose(std::string target) {
  const size_t to = target.find(" to ");
  if (to != std::string::npos && to > 0) {
    target.resize(to);
  }
  return std::string(base::TrimWhitespaceASCII(target, base::TRIM_ALL));
}

bool IsNamedKey(std::string_view word) {
  static constexpr const char* kKeys[] = {
      "enter", "return", "tab",    "escape", "esc",  "backspace",
      "delete", "space", "up",     "down",   "left", "right",
  };
  for (const char* key : kKeys) {
    if (base::EqualsCaseInsensitiveASCII(word, key)) {
      return true;
    }
  }
  return false;
}

std::string CanonicalKey(std::string_view word) {
  if (base::EqualsCaseInsensitiveASCII(word, "return")) {
    return "enter";
  }
  if (base::EqualsCaseInsensitiveASCII(word, "esc")) {
    return "escape";
  }
  return base::ToLowerASCII(word);
}

std::string FirstWord(const std::string& text) {
  const size_t end = text.find_first_of(" \t");
  if (end == std::string::npos) {
    return text;
  }
  return text.substr(0, end);
}

}  // namespace

GroundedCommand ParseGroundedCommand(const std::string& instruction) {
  GroundedCommand cmd;
  const std::string text(
      base::TrimWhitespaceASCII(instruction, base::TRIM_ALL));
  if (text.empty()) {
    cmd.target = "the page";
    return cmd;
  }

  if (StartsWithWord(text, "scroll")) {
    cmd.kind = GroundedCommand::Kind::kScroll;
    cmd.needs_target = false;
    const std::string rest = base::ToLowerASCII(RestAfter(text, 6));
    cmd.scroll_dy = rest.find("up") != std::string::npos ? -kScrollFraction
                                                        : kScrollFraction;
    return cmd;
  }

  if (StartsWithWord(text, "wait")) {
    cmd.kind = GroundedCommand::Kind::kWait;
    cmd.needs_target = false;
    return cmd;
  }

  if (StartsWithWord(text, "press") || StartsWithWord(text, "hit") ||
      StartsWithWord(text, "key")) {
    const size_t verb_len = StartsWithWord(text, "press") ? 5
                            : StartsWithWord(text, "hit") ? 3
                                                          : 3;
    const std::string rest = RestAfter(text, verb_len);
    const std::string word = FirstWord(rest);
    if (IsNamedKey(word)) {
      cmd.kind = GroundedCommand::Kind::kKey;
      cmd.key = CanonicalKey(word);
      cmd.needs_target = false;
      return cmd;
    }
    cmd.kind = GroundedCommand::Kind::kClick;
    cmd.target = StripPurpose(rest);
    if (cmd.target.empty()) {
      cmd.target = text;
    }
    return cmd;
  }

  static constexpr const char* kTypeVerbs[] = {
      "type", "enter", "fill", "input", "paste", "write",
  };
  for (const char* verb : kTypeVerbs) {
    if (!StartsWithWord(text, verb)) {
      continue;
    }
    std::string rest = RestAfter(text, std::char_traits<char>::length(verb));
    std::string payload;
    const bool has_payload = TakePayload(&rest, &payload);
    if (!has_payload && base::EqualsCaseInsensitiveASCII(verb, "enter")) {
      cmd.kind = GroundedCommand::Kind::kKey;
      cmd.key = "enter";
      cmd.needs_target = false;
      if (!rest.empty() && !IsNamedKey(FirstWord(rest))) {
        cmd.kind = GroundedCommand::Kind::kClick;
        cmd.target = StripPurpose(rest);
        cmd.needs_target = true;
      }
      return cmd;
    }
    cmd.kind = GroundedCommand::Kind::kType;
    cmd.text = payload;
    StripLeadIn(&rest);
    cmd.target = StripPurpose(rest);
    if (cmd.target.empty()) {
      cmd.target = "the text field";
    }
    return cmd;
  }

  static constexpr const char* kClickVerbs[] = {
      "click", "tap", "select", "choose", "open",
      "dismiss", "close", "check", "uncheck",
  };
  for (const char* verb : kClickVerbs) {
    if (!StartsWithWord(text, verb)) {
      continue;
    }
    cmd.kind = GroundedCommand::Kind::kClick;
    cmd.target = StripPurpose(
        RestAfter(text, std::char_traits<char>::length(verb)));
    if (cmd.target.empty()) {
      cmd.target = text;
    }
    return cmd;
  }

  std::string rest = text;
  std::string payload;
  if (TakePayload(&rest, &payload)) {
    cmd.kind = GroundedCommand::Kind::kType;
    cmd.text = payload;
    StripLeadIn(&rest);
    cmd.target = StripPurpose(rest);
    if (cmd.target.empty()) {
      cmd.target = "the text field";
    }
    return cmd;
  }

  cmd.kind = GroundedCommand::Kind::kClick;
  cmd.target = StripPurpose(text);
  if (cmd.target.empty()) {
    cmd.target = text;
  }
  return cmd;
}

std::string LocatePromptFor(const GroundedCommand& command) {
  return "Locate this control and nothing else: " + command.target;
}

}  // namespace simulachrome
