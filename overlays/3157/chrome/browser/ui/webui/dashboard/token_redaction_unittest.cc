// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/token_redaction.h"

#include "base/containers/flat_map.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace dashboard {
namespace {

base::flat_map<std::string, std::string> Tokens() {
  return {{"password", "hunter2!Pass"},
          {"username", "alice@example.com"},
          {"short", "ab"},
          // A secret that contains another token's value; longest wins.
          {"combo", "alice@example.com:hunter2!Pass"}};
}

TEST(TokenRedactionTest, RestoresPlaceholders) {
  const TokenReverseMap map = BuildTokenReverseMap(Tokens());
  EXPECT_EQ("{password}", RedactTokensInString("hunter2!Pass", map));
  EXPECT_EQ("type {username} now",
            RedactTokensInString("type alice@example.com now", map));
}

TEST(TokenRedactionTest, LongestValueWins) {
  const TokenReverseMap map = BuildTokenReverseMap(Tokens());
  EXPECT_EQ("{combo}",
            RedactTokensInString("alice@example.com:hunter2!Pass", map));
}

TEST(TokenRedactionTest, ShortValuesAreNeverReversed) {
  const TokenReverseMap map = BuildTokenReverseMap(Tokens());
  EXPECT_EQ("abacus", RedactTokensInString("abacus", map));
}

TEST(TokenRedactionTest, RecursesIntoDictsAndLists) {
  const TokenReverseMap map = BuildTokenReverseMap(Tokens());
  std::optional<base::Value> v = base::JSONReader::Read(
      R"({"action":"type","params":{"text":"hunter2!Pass",
          "locators":[{"type":"css","value":"#alice@example.com"}],
          "delay_ms":50}})",
      base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  ASSERT_TRUE(v);
  RedactTokensInValue(&*v, map);
  const base::Value::Dict& params = *v->GetDict().FindDict("params");
  EXPECT_EQ("{password}", *params.FindString("text"));
  EXPECT_EQ("#{username}",
            *(*params.FindList("locators"))[0].GetDict().FindString("value"));
  EXPECT_EQ(50, *params.FindInt("delay_ms"));
}

TEST(TokenRedactionTest, EmptyMapIsNoOp) {
  EXPECT_EQ("hunter2!Pass", RedactTokensInString("hunter2!Pass", {}));
}

}  // namespace
}  // namespace dashboard
