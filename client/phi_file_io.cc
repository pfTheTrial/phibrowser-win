// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_file_io.h"

#include <windows.h>

#include <chrono>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"

namespace phi_client {

bool PhiReadFileBytes(const std::wstring& path, std::string* out) {
  out->clear();
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buffer[16384];
  DWORD read = 0;
  bool ok = true;
  for (;;) {
    if (!::ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
      ok = false;
      break;
    }
    if (read == 0) {
      break;
    }
    out->append(buffer, read);
  }
  ::CloseHandle(file);
  if (!ok) {
    LOG(ERROR) << "[PhiFile] read failed " << base::WideToUTF8(path)
               << " err=" << ::GetLastError();
    out->clear();
  }
  return ok;
}

bool PhiWriteFileAtomic(const std::wstring& path, std::string_view bytes) {
  const std::wstring temp = path + L".tmp";
  HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    LOG(ERROR) << "[PhiFile] create failed " << base::WideToUTF8(temp)
               << " err=" << ::GetLastError();
    return false;
  }
  DWORD wrote = 0;
  const BOOL written = ::WriteFile(
      file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
  const DWORD write_err = ::GetLastError();
  const BOOL flushed = ::FlushFileBuffers(file);
  ::CloseHandle(file);
  if (!written || wrote != bytes.size() || !flushed) {
    LOG(ERROR) << "[PhiFile] write failed " << base::WideToUTF8(temp)
               << " err=" << write_err << " wrote=" << wrote << "/"
               << bytes.size();
    ::DeleteFileW(temp.c_str());
    return false;
  }
  if (::ReplaceFileW(path.c_str(), temp.c_str(), nullptr,
                     REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
    return true;
  }
  // First save: there is nothing to replace yet.
  if (::MoveFileExW(temp.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  LOG(ERROR) << "[PhiFile] replace failed " << base::WideToUTF8(path)
             << " err=" << ::GetLastError();
  ::DeleteFileW(temp.c_str());
  return false;
}

void PhiQuarantineFile(const std::wstring& path, const char* reason) {
  if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return;
  }
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const std::wstring aside = path + L".corrupt-" + std::to_wstring(now);
  if (::MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    LOG(ERROR) << "[PhiFile] quarantined " << base::WideToUTF8(path)
               << " -> " << base::WideToUTF8(aside) << " (" << reason << ")";
  } else {
    LOG(ERROR) << "[PhiFile] could not quarantine " << base::WideToUTF8(path)
               << " err=" << ::GetLastError() << " (" << reason << ")";
  }
}

PhiJsonFileStatus PhiReadJsonFile(const std::wstring& path,
                                  base::DictValue* dict,
                                  int* schema_out) {
  if (schema_out) {
    *schema_out = 0;
  }
  std::string json;
  if (!PhiReadFileBytes(path, &json)) {
    return PhiJsonFileStatus::kMissing;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    PhiQuarantineFile(path, "not a JSON object");
    return PhiJsonFileStatus::kCorrupt;
  }
  if (schema_out) {
    *schema_out = root->FindInt("schema").value_or(0);
  }
  *dict = std::move(*root);
  return PhiJsonFileStatus::kOk;
}

bool PhiWriteJsonFile(const std::wstring& path,
                      base::DictValue dict,
                      int schema) {
  if (schema > 0) {
    dict.Set("schema", schema);
  }
  std::string json;
  if (!base::JSONWriter::Write(dict, &json)) {
    LOG(ERROR) << "[PhiFile] serialise failed " << base::WideToUTF8(path);
    return false;
  }
  return PhiWriteFileAtomic(path, json);
}

}  // namespace phi_client
