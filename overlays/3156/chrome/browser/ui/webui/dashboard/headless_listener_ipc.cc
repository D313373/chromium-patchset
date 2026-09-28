// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/dashboard/headless_listener_ipc.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_POSIX)
#include <unistd.h>

#include "base/posix/eintr_wrapper.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace dashboard {
namespace {

constexpr size_t kMaxIpcMessageBytes = 16 * 1024 * 1024;

bool WriteAll(HeadlessIpcHandle handle,
              base::span<const uint8_t> data,
              std::string* error) {
#if BUILDFLAG(IS_POSIX)
  int fd = handle;
  size_t offset = 0;
  while (offset < data.size()) {
    base::span<const uint8_t> remaining = data.subspan(offset);
    const ssize_t n =
        HANDLE_EINTR(write(fd, remaining.data(), remaining.size()));
    if (n <= 0) {
      if (error) {
        *error = "Failed writing control-channel frame.";
      }
      return false;
    }
    offset += static_cast<size_t>(n);
  }
  return true;
#elif BUILDFLAG(IS_WIN)
  HANDLE win_handle = static_cast<HANDLE>(handle);
  if (!win_handle || win_handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "Invalid named-pipe handle.";
    }
    return false;
  }
  size_t offset = 0;
  while (offset < data.size()) {
    DWORD written = 0;
    const DWORD chunk = static_cast<DWORD>(
        std::min(data.size() - offset, static_cast<size_t>(0x10000)));
    if (!WriteFile(win_handle, data.data() + offset, chunk, &written,
                   nullptr) ||
        written == 0) {
      if (error) {
        *error = "Failed writing named-pipe frame.";
      }
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  return true;
#else
  if (error) {
    *error = "Headless control channel is unsupported on this platform.";
  }
  return false;
#endif
}

bool ReadExact(HeadlessIpcHandle handle,
               base::span<uint8_t> data,
               std::string* error) {
#if BUILDFLAG(IS_POSIX)
  int fd = handle;
  size_t offset = 0;
  while (offset < data.size()) {
    base::span<uint8_t> remaining = data.subspan(offset);
    const ssize_t n =
        HANDLE_EINTR(read(fd, remaining.data(), remaining.size()));
    if (n == 0) {
      if (error) {
        *error = "Control channel closed.";
      }
      return false;
    }
    if (n < 0) {
      if (error) {
        *error = "Failed reading control-channel frame.";
      }
      return false;
    }
    offset += static_cast<size_t>(n);
  }
  return true;
#elif BUILDFLAG(IS_WIN)
  HANDLE win_handle = static_cast<HANDLE>(handle);
  if (!win_handle || win_handle == INVALID_HANDLE_VALUE) {
    if (error) {
      *error = "Invalid named-pipe handle.";
    }
    return false;
  }
  size_t offset = 0;
  while (offset < data.size()) {
    DWORD got = 0;
    const DWORD chunk = static_cast<DWORD>(
        std::min(data.size() - offset, static_cast<size_t>(0x10000)));
    if (!ReadFile(win_handle, data.data() + offset, chunk, &got, nullptr)) {
      if (error) {
        *error = "Failed reading named-pipe frame.";
      }
      return false;
    }
    if (got == 0) {
      if (error) {
        *error = "Named pipe closed.";
      }
      return false;
    }
    offset += static_cast<size_t>(got);
  }
  return true;
#else
  if (error) {
    *error = "Headless control channel is unsupported on this platform.";
  }
  return false;
#endif
}

}  // namespace

base::FilePath DefaultHeadlessControlSocketPath() {
  if (const char* env = std::getenv(kHeadlessControlSocketEnv); env && *env) {
    return base::FilePath::FromUTF8Unsafe(env);
  }
#if BUILDFLAG(IS_WIN)
  return base::FilePath::FromUTF8Unsafe(
      "\\\\.\\pipe\\Simulachrome.Headless.control");
#else
  base::FilePath app_data;
  if (base::PathService::Get(base::DIR_APP_DATA, &app_data) &&
      !app_data.empty()) {
    return app_data.AppendASCII("Simulachrome")
        .AppendASCII("Headless")
        .AppendASCII("control.sock");
  }
  return base::FilePath();
#endif
}

bool WriteFramedJsonMessage(HeadlessIpcHandle handle,
                            const base::Value::Dict& message,
                            std::string* error) {
  std::string json;
  if (!base::JSONWriter::Write(base::Value(message.Clone()), &json)) {
    if (error) {
      *error = "Failed to serialize control-channel JSON.";
    }
    return false;
  }
  if (json.size() > kMaxIpcMessageBytes ||
      json.size() > std::numeric_limits<uint32_t>::max()) {
    if (error) {
      *error = "Control-channel message too large.";
    }
    return false;
  }
  const uint32_t len = static_cast<uint32_t>(json.size());
  if (!WriteAll(handle, base::byte_span_from_ref(len), error)) {
    return false;
  }
  return WriteAll(handle, base::as_byte_span(json), error);
}

std::optional<base::Value::Dict> ReadFramedJsonMessage(
    HeadlessIpcHandle handle,
    std::string* error) {
  uint32_t len = 0;
  if (!ReadExact(handle, base::byte_span_from_ref(len), error)) {
    return std::nullopt;
  }
  if (len == 0 || len > kMaxIpcMessageBytes) {
    if (error) {
      *error = "Invalid control-channel frame length.";
    }
    return std::nullopt;
  }
  std::string json;
  json.resize(len);
  if (!ReadExact(handle, base::as_writable_byte_span(json), error)) {
    return std::nullopt;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict()) {
    if (error) {
      *error = "Control-channel payload is not a JSON object.";
    }
    return std::nullopt;
  }
  return std::move(parsed->GetDict());
}

base::Value::Dict MakeIpcRequest(std::string_view type,
                                 std::string_view request_id) {
  base::Value::Dict msg;
  msg.Set("type", type);
  if (!request_id.empty()) {
    msg.Set("request_id", request_id);
  }
  return msg;
}

base::Value::Dict MakeIpcOkResponse(std::string_view request_id,
                                    base::Value::Dict payload) {
  base::Value::Dict msg = std::move(payload);
  msg.Set("ok", true);
  if (!request_id.empty()) {
    msg.Set("request_id", request_id);
  }
  return msg;
}

base::Value::Dict MakeIpcErrorResponse(std::string_view request_id,
                                       std::string_view error) {
  base::Value::Dict msg;
  msg.Set("ok", false);
  msg.Set("error", error);
  if (!request_id.empty()) {
    msg.Set("request_id", request_id);
  }
  return msg;
}

}  // namespace dashboard
