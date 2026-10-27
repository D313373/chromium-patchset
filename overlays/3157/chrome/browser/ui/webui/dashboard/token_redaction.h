// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_TOKEN_REDACTION_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_TOKEN_REDACTION_H_

#include <string>
#include <utility>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/values.h"

namespace dashboard {

// {Token} expansion happens for the whole timeline before the executor runs
// it, so everything the executor later serializes (step history, promoted
// params written back into saved scripts, training rows) would otherwise
// contain the cleartext values. A TokenReverseMap turns those values back into
// their `{Token}` placeholders at every persistence boundary.
//
// Entries are (cleartext value, "{token}") sorted by value length, longest
// first, so a secret that contains a shorter variable value is replaced whole.
using TokenReverseMap = std::vector<std::pair<std::string, std::string>>;

// Values shorter than this are never reversed: replacing "1" or "a" would
// mangle unrelated text far more often than it would protect anything.
inline constexpr size_t kMinRedactableTokenValueLength = 4;

TokenReverseMap BuildTokenReverseMap(
    const base::flat_map<std::string, std::string>& scalar_tokens);

std::string RedactTokensInString(const std::string& input,
                                 const TokenReverseMap& reverse_map);

// Recursively redacts every string in `value` (dict keys are left alone).
void RedactTokensInValue(base::Value* value,
                         const TokenReverseMap& reverse_map);

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_TOKEN_REDACTION_H_
