// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_IPC_H_
#define CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_IPC_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/values.h"

namespace dashboard {

// Env var: absolute path of the Unix domain control socket shared by the
// headed controller and the --headless-listener agent process.
//
// Transport notes:
// - This is NOT Mojo between headed and headless (separate browser processes).
// - This is NOT a Chrome Native Messaging Host (.json host manifest).
// - Framing matches native-messaging style (uint32 LE length + JSON UTF-8) on a
//   local IPC socket (Unix domain today; Windows named pipe planned).
inline constexpr char kHeadlessControlSocketEnv[] =
    "SIMULACHROME_HEADLESS_CONTROL_SOCKET";

// Default socket under ~/Library/Application Support/Simulachrome/Headless/.
base::FilePath DefaultHeadlessControlSocketPath();

// Length-prefixed (uint32 LE) JSON framing. Used on the headed↔headless
// control channel.
bool WriteFramedJsonMessage(int fd, const base::Value::Dict& message,
                            std::string* error);
std::optional<base::Value::Dict> ReadFramedJsonMessage(int fd,
                                                       std::string* error);

// Helpers for building common envelope fields.
base::Value::Dict MakeIpcRequest(std::string_view type,
                                 std::string_view request_id = {});
base::Value::Dict MakeIpcOkResponse(std::string_view request_id,
                                    base::Value::Dict payload = {});
base::Value::Dict MakeIpcErrorResponse(std::string_view request_id,
                                       std::string_view error);

}  // namespace dashboard

#endif  // CHROME_BROWSER_UI_WEBUI_DASHBOARD_HEADLESS_LISTENER_IPC_H_
