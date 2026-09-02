// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_emoji.h"

#include <windows.h>

#include <map>

#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_icons.h"

namespace phi_client {

namespace {

struct Catalog {
  std::vector<PhiEmojiGroup> groups;
  std::map<std::string, std::string> text_by_id;
};

// Reads a whole file with plain Win32 I/O. The catalog loads LAZILY, on the
// browser UI thread, from whichever surface first needs an emoji (the icon
// picker, agentSpace.emoji.*) — and that thread disallows blocking calls once
// the main loop runs, so base::ReadFileToString there is a DCHECK crash in
// the dev build (thread_restrictions.cc:62, seen on the box). The client's
// other stores read this way for the same reason; the catalog is ~200 KB.
bool ReadWholeFile(const std::wstring& path, std::string* out) {
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buf[16384];
  DWORD read = 0;
  while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
    out->append(buf, read);
  }
  ::CloseHandle(f);
  return true;
}

// Reads the catalog the Mac bundles, shipped beside the exe by the build.
Catalog LoadCatalog() {
  Catalog catalog;
  base::FilePath dir;
  if (!base::PathService::Get(base::DIR_EXE, &dir)) {
    return catalog;
  }
  const base::FilePath path =
      dir.AppendASCII("resources").AppendASCII("emoji-catalog.json");
  std::string json;
  if (!ReadWholeFile(path.value(), &json)) {
    LOG(ERROR) << "[PhiClient] emoji catalog missing: " << path;
    return catalog;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    LOG(ERROR) << "[PhiClient] emoji catalog is not valid JSON";
    return catalog;
  }
  const base::ListValue* groups = root->FindList("groups");
  if (!groups) {
    return catalog;
  }
  for (const base::Value& group_value : *groups) {
    if (!group_value.is_dict()) {
      continue;
    }
    const base::DictValue& group_dict = group_value.GetDict();
    PhiEmojiGroup group;
    if (const std::string* name = group_dict.FindString("name")) {
      group.name = *name;
    }
    const base::ListValue* items = group_dict.FindList("items");
    if (!items) {
      continue;
    }
    for (const base::Value& item_value : *items) {
      if (!item_value.is_dict()) {
        continue;
      }
      const base::DictValue& item = item_value.GetDict();
      const std::string* id = item.FindString("id");
      const std::string* text = item.FindString("text");
      if (!id || !text) {
        continue;
      }
      PhiEmoji emoji;
      emoji.id = *id;
      emoji.text = *text;
      if (const std::string* name = item.FindString("name")) {
        emoji.name = *name;
      }
      catalog.text_by_id[emoji.id] = emoji.text;
      // Skin-tone variants are addressable by their own ids, exactly as on Mac
      // (EmojiCatalog.text(for:) searches variants too).
      if (const base::ListValue* variants = item.FindList("skinVariants")) {
        for (const base::Value& variant_value : *variants) {
          if (!variant_value.is_dict()) {
            continue;
          }
          const base::DictValue& variant = variant_value.GetDict();
          const std::string* variant_id = variant.FindString("id");
          const std::string* variant_text = variant.FindString("text");
          if (variant_id && variant_text) {
            catalog.text_by_id[*variant_id] = *variant_text;
          }
        }
      }
      group.items.push_back(std::move(emoji));
    }
    catalog.groups.push_back(std::move(group));
  }
  LOG(INFO) << "[PhiClient] emoji catalog: " << catalog.groups.size()
            << " groups, " << catalog.text_by_id.size() << " ids";
  return catalog;
}

const Catalog& GetCatalog() {
  static const base::NoDestructor<Catalog> catalog(LoadCatalog());
  return *catalog;
}

bool StartsWith(const std::string& value, const char* prefix) {
  const std::string p(prefix);
  return value.size() >= p.size() && value.compare(0, p.size(), p) == 0;
}

}  // namespace

const std::vector<PhiEmojiGroup>& PhiEmojiGroups() {
  return GetCatalog().groups;
}

std::string PhiEmojiTextForId(const std::string& id) {
  const auto& by_id = GetCatalog().text_by_id;
  auto it = by_id.find(id);
  return it == by_id.end() ? std::string() : it->second;
}

std::string PhiIconDisplayText(const std::string& storage_value) {
  if (StartsWith(storage_value, kPhiEmojiPrefix)) {
    return PhiEmojiTextForId(
        storage_value.substr(std::string(kPhiEmojiPrefix).size()));
  }
  if (StartsWith(storage_value, kPhiIconPrefix)) {
    // The Mac's Phi icon catalog, stood in by MDL2 glyphs / characters.
    return PhiIconCatalogGlyph(
        storage_value.substr(std::string(kPhiIconPrefix).size()));
  }
  return std::string();
}

bool PhiIconIsValid(const std::string& storage_value) {
  if (StartsWith(storage_value, kPhiIconPrefix)) {
    // In the catalog, or unknown-but-Mac-shaped (an id a NEWER Mac catalog
    // may know); either way it round-trips unharmed.
    return storage_value.size() > std::string(kPhiIconPrefix).size();
  }
  return !PhiIconDisplayText(storage_value).empty();
}

}  // namespace phi_client
