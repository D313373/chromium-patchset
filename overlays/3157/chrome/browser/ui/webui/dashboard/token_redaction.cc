// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/token_redaction.h"

#include <algorithm>

#include "base/strings/string_util.h"

namespace dashboard {

TokenReverseMap BuildTokenReverseMap(
    const base::flat_map<std::string, std::string>& scalar_tokens) {
  TokenReverseMap out;
  out.reserve(scalar_tokens.size());
  for (const auto& [token, value] : scalar_tokens) {
    if (value.size() < kMinRedactableTokenValueLength) {
      continue;
    }
    out.emplace_back(value, "{" + token + "}");
  }
  // Longest first; ties broken by value for determinism.
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    if (a.first.size() != b.first.size()) {
      return a.first.size() > b.first.size();
    }
    return a.first < b.first;
  });
  // Two tokens with the same value map to whichever sorted first; drop dupes.
  out.erase(std::unique(out.begin(), out.end(),
                        [](const auto& a, const auto& b) {
                          return a.first == b.first;
                        }),
            out.end());
  return out;
}

std::string RedactTokensInString(const std::string& input,
                                 const TokenReverseMap& reverse_map) {
  if (input.empty() || reverse_map.empty()) {
    return input;
  }
  std::string out = input;
  for (const auto& [value, placeholder] : reverse_map) {
    if (out.find(value) == std::string::npos) {
      continue;
    }
    base::ReplaceSubstringsAfterOffset(&out, 0, value, placeholder);
  }
  return out;
}

void RedactTokensInValue(base::Value* value,
                         const TokenReverseMap& reverse_map) {
  if (!value || reverse_map.empty()) {
    return;
  }
  if (value->is_string()) {
    *value = base::Value(RedactTokensInString(value->GetString(), reverse_map));
  } else if (value->is_list()) {
    for (base::Value& item : value->GetList()) {
      RedactTokensInValue(&item, reverse_map);
    }
  } else if (value->is_dict()) {
    for (auto it : value->GetDict()) {
      RedactTokensInValue(&it.second, reverse_map);
    }
  }
}

}  // namespace dashboard
