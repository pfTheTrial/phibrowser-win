// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_coordinator.h"

#include <windows.h>

#include <algorithm>
#include <optional>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "base/rand_util.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_menu_bar.h"

namespace phi_client {

namespace {
// The one coordinator, reachable from the static keyboard hook. The client
// creates exactly one for the process lifetime.
PhiCoordinator* g_coordinator = nullptr;
}  // namespace

PhiCoordinator::PhiCoordinator() {
  g_coordinator = this;
}

PhiCoordinator::~PhiCoordinator() {
  if (keyboard_hook_) {
    ::UnhookWindowsHookEx(keyboard_hook_);
  }
  g_coordinator = nullptr;
}

namespace {
constexpr wchar_t kMsgClassName[] = L"PhiClientControlMsg";
constexpr UINT WM_PHI_CONTROL = WM_APP + 1;

// The command marshaled from the I/O thread to the UI thread by SendMessage.
// The pointers reference stack strings that outlive the blocking SendMessage.
struct ControlCall {
  RAW_PTR_EXCLUSION const std::string* type;
  RAW_PTR_EXCLUSION const std::string* payload;
  std::string result;
};
}  // namespace

void PhiCoordinator::OnFrameworkReady(phi_framework::PhiFramework* framework,
                                     const char* user_data_dir_utf8) {
  framework_ = framework;

  // Message-only window on the UI thread (this callback runs on it), so the
  // control server's I/O thread can marshal framework calls here via
  // SendMessage.
  static bool class_registered = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PhiCoordinator::MsgWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kMsgClassName;
    return ::RegisterClassExW(&wc) != 0;
  }();
  (void)class_registered;
  msg_hwnd_ = ::CreateWindowExW(0, kMsgClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr,
                                ::GetModuleHandleW(nullptr), this);

  // Menu-bar accelerators. Keys land in the focused window — the browser
  // child while browsing — so a plain WM_KEYDOWN handler on the shell never
  // sees them; a thread-scoped hook on the UI thread does, before dispatch.
  keyboard_hook_ = ::SetWindowsHookExW(WH_KEYBOARD, &KeyboardHookProc,
                                       nullptr, ::GetCurrentThreadId());

  const std::wstring user_data_dir =
      base::UTF8ToWide(user_data_dir_utf8 ? user_data_dir_utf8 : "");
  spaces_path_ = user_data_dir + L"\\PhiSpaces.json";
  layout_path_ = user_data_dir + L"\\PhiLayout.json";
  theme_path_ = user_data_dir + L"\\PhiTheme.json";
  prefs_path_ = user_data_dir + L"\\PhiPrefs.json";
  LoadSpaces();
  LoadLayoutMode();
  LoadTheme();
  LoadPrefs();
  tab_store_.Load(user_data_dir + L"\\PhiTabData.json");

  control_server_.Start(user_data_dir, this);
}

// static
LRESULT CALLBACK PhiCoordinator::MsgWndProc(HWND hwnd,
                                            UINT msg,
                                            WPARAM wparam,
                                            LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_TIMER) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->TryFinishProfileSpace()) {
      ::KillTimer(hwnd, 1);
    }
    return 0;
  }
  if (msg == WM_PHI_CONTROL) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    auto* call = reinterpret_cast<ControlCall*>(lparam);
    if (self && call) {
      call->result = self->ExecuteControlOnUi(*call->type, *call->payload);
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

std::string PhiCoordinator::HandleControlCommand(
    const std::string& type,
    const std::string& payload_json) {
  // Called on the control server's I/O thread. Marshal to the UI thread; the
  // blocking SendMessage runs ExecuteControlOnUi there and returns its result.
  if (!msg_hwnd_) {
    return "{\"error\":\"not ready\"}";
  }
  ControlCall call{&type, &payload_json, {}};
  ::SendMessageW(msg_hwnd_, WM_PHI_CONTROL, 0,
                 reinterpret_cast<LPARAM>(&call));
  return call.result;
}

void PhiCoordinator::LoadSpaces() {
  HANDLE f = ::CreateFileW(spaces_path_.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;  // First run: keep the built-in default Space.
  }
  std::string json;
  char buf[4096];
  DWORD read = 0;
  while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
    json.append(buf, read);
  }
  ::CloseHandle(f);

  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  const base::ListValue* list = root->FindList("spaces");
  if (!list || list->empty()) {
    return;
  }
  std::vector<Space> loaded;
  int max_numeric_id = 1;
  for (const base::Value& v : *list) {
    if (!v.is_dict()) {
      continue;
    }
    const base::DictValue& d = v.GetDict();
    const std::string* id = d.FindString("id");
    const std::string* name = d.FindString("name");
    const std::string* profile = d.FindString("profileId");
    const std::string* icon = d.FindString("icon");
    const std::string* theme = d.FindString("themeId");
    if (!id || id->empty()) {
      continue;
    }
    loaded.push_back({*id, name ? *name : ("Space " + *id),
                      profile && !profile->empty() ? *profile : "Default",
                      icon && PhiIconIsValid(*icon) ? *icon
                                                    : kPhiDefaultSpaceIcon,
                      theme ? *theme : std::string(),
                      d.FindInt("saturation").value_or(-1)});
    max_numeric_id = std::max(max_numeric_id, atoi(id->c_str()));
  }
  if (!loaded.empty()) {
    spaces_ = std::move(loaded);
    next_space_id_ = max_numeric_id + 1;
    LOG(INFO) << "[PhiClient] restored " << spaces_.size() << " spaces";
  }
}

void PhiCoordinator::SaveSpaces() const {
  base::ListValue list;
  for (const Space& space : spaces_) {
    base::DictValue d;
    d.Set("id", space.id);
    d.Set("name", space.name);
    d.Set("profileId", space.profile_id);
    d.Set("icon", space.icon);
    d.Set("themeId", space.theme_id);
    d.Set("saturation", space.saturation);
    list.Append(std::move(d));
  }
  base::DictValue root;
  root.Set("spaces", std::move(list));
  std::string json;
  base::JSONWriter::Write(root, &json);

  HANDLE f = ::CreateFileW(spaces_path_.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD wrote = 0;
  ::WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &wrote, nullptr);
  ::CloseHandle(f);
}

void PhiCoordinator::LoadLayoutMode() {
  HANDLE f = ::CreateFileW(layout_path_.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;  // First run: the Mac default, Performance.
  }
  std::string json;
  char buf[512];
  DWORD read = 0;
  while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
    json.append(buf, read);
  }
  ::CloseHandle(f);
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  if (const std::string* name = root->FindString("layoutMode")) {
    PhiLayoutMode parsed = PhiLayoutMode::kPerformance;
    if (PhiLayoutModeFromName(*name, &parsed)) {
      layout_mode_ = parsed;
      LOG(INFO) << "[PhiClient] layout mode: " << *name;
    }
  }
  if (std::optional<int> width = root->FindInt("sidebarWidth")) {
    sidebar_width_ =
        std::max(kSidebarWidthMin, std::min(*width, kSidebarWidthMax));
  }
}

void PhiCoordinator::SaveLayoutMode() const {
  base::DictValue root;
  root.Set("layoutMode", PhiLayoutModeName(layout_mode_));
  root.Set("sidebarWidth", sidebar_width_);
  std::string json;
  base::JSONWriter::Write(root, &json);
  HANDLE f = ::CreateFileW(layout_path_.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD wrote = 0;
  ::WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &wrote, nullptr);
  ::CloseHandle(f);
}

bool PhiCoordinator::OwnerForSlot(int64_t slot, std::string* profile_id,
                                  std::string* space_id) const {
  auto it = windows_.find(slot);
  if (it == windows_.end()) {
    return false;
  }
  *space_id = it->second->active_space();
  *profile_id = "Default";
  for (const Space& space : spaces_) {
    if (space.id == *space_id) {
      *profile_id = space.profile_id;
      break;
    }
  }
  return true;
}

std::string PhiCoordinator::LiveUrl(int64_t slot) {
  if (!framework_) {
    return std::string();
  }
  std::optional<base::DictValue> state = base::JSONReader::ReadDict(
      framework_->NavStateJson(ResolveBrowser(slot)), base::JSON_PARSE_RFC);
  if (!state) {
    return std::string();
  }
  const std::string* url = state->FindString("url");
  return url ? *url : std::string();
}

std::string PhiCoordinator::CaptureFavicon(int64_t slot, int* width,
                                           int* height) {
  *width = 0;
  *height = 0;
  if (!framework_) {
    return std::string();
  }
  const int64_t browser = ResolveBrowser(slot);
  std::optional<base::Value> tabs = base::JSONReader::Read(
      framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
  int active_index = -1;
  if (tabs && tabs->is_list()) {
    for (const base::Value& v : tabs->GetList()) {
      if (v.is_dict() && v.GetDict().FindBool("active").value_or(false)) {
        active_index = v.GetDict().FindInt("index").value_or(-1);
        break;
      }
    }
  }
  if (active_index < 0) {
    return std::string();
  }
  const unsigned char* pixels =
      framework_->TabFaviconBgra(browser, active_index, width, height);
  if (!pixels || *width <= 0 || *height <= 0) {
    return std::string();
  }
  const size_t bytes =
      static_cast<size_t>(*width) * static_cast<size_t>(*height) * 4;
  // SAFETY: the framework reports the dimensions of the buffer it returned.
  const auto view = UNSAFE_BUFFERS(base::span<const uint8_t>(pixels, bytes));
  return base::Base64Encode(view);
}

namespace {

// Store row → chrome view, decoding the favicon ONCE here instead of at
// every consumer (the old JSON interface base64-round-tripped per rebuild).
PhiStoredRowView StoredRowView(const PhiRow& row) {
  PhiStoredRowView view;
  view.guid = row.guid;
  view.title = row.title;
  view.url = row.url;
  view.folder = row.type == PhiRowType::kBookmarkFolder;
  if (!row.favicon_base64.empty()) {
    std::string decoded;
    if (base::Base64Decode(row.favicon_base64, &decoded)) {
      view.favicon_bgra.assign(decoded.begin(), decoded.end());
      view.favicon_width = row.favicon_width;
      view.favicon_height = row.favicon_height;
    }
  }
  return view;
}

}  // namespace

std::vector<PhiStoredRowView> PhiCoordinator::PinnedRows(int64_t slot) const {
  std::vector<PhiStoredRowView> out;
  std::string profile_id, space_id;
  if (OwnerForSlot(slot, &profile_id, &space_id)) {
    for (const PhiRow& row : tab_store_.PinnedFor(profile_id, space_id)) {
      out.push_back(StoredRowView(row));
      UpgradeFavicon(slot, row.url, &out.back());
    }
  }
  return out;
}

std::vector<PhiStoredRowView> PhiCoordinator::BookmarkRows(
    int64_t slot,
    const std::string& parent_guid) const {
  std::vector<PhiStoredRowView> out;
  std::string profile_id, space_id;
  if (OwnerForSlot(slot, &profile_id, &space_id)) {
    for (const PhiRow& row :
         tab_store_.BookmarksFor(profile_id, space_id, parent_guid)) {
      out.push_back(StoredRowView(row));
      UpgradeFavicon(slot, row.url, &out.back());
    }
  }
  return out;
}

void PhiCoordinator::UpgradeFavicon(int64_t slot,
                                    const std::string& url,
                                    PhiStoredRowView* view) const {
  if (!framework_ || url.empty()) {
    return;
  }
  // The history-backed icon (async; misses schedule a fetch and repaint on
  // arrival) beats the 16px live-tab capture stored with the row.
  int width = 0;
  int height = 0;
  const unsigned char* pixels = framework_->FaviconForUrlBgra(
      ResolveBrowser(slot), url.c_str(), &width, &height);
  if (!pixels || width <= view->favicon_width) {
    return;
  }
  const size_t bytes = static_cast<size_t>(width) * height * 4;
  // SAFETY: FaviconForUrlBgra reports the dimensions of the buffer it just
  // returned.
  const auto span =
      UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
  view->favicon_bgra.assign(span.begin(), span.end());
  view->favicon_width = width;
  view->favicon_height = height;
}

void PhiCoordinator::RequestOpen(int64_t slot, const std::string& url) {
  if (framework_ && !url.empty()) {
    framework_->NewTab(ResolveBrowser(slot), url.c_str());
  }
}

void PhiCoordinator::RequestStoreTab(int64_t slot, int index,
                                     bool as_bookmark) {
  std::string profile_id, space_id;
  if (!framework_ || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return;
  }
  const int64_t browser = ResolveBrowser(slot);
  std::string title, url;
  std::optional<base::Value> tabs = base::JSONReader::Read(
      framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
  if (tabs && tabs->is_list()) {
    for (const base::Value& v : tabs->GetList()) {
      if (v.is_dict() && v.GetDict().FindInt("index").value_or(-1) == index) {
        if (const std::string* t = v.GetDict().FindString("title")) {
          title = *t;
        }
        if (const std::string* u = v.GetDict().FindString("url")) {
          url = *u;
        }
        break;
      }
    }
  }
  if (url.empty()) {
    return;
  }
  int favicon_width = 0, favicon_height = 0;
  std::string favicon;
  const unsigned char* pixels = framework_->TabFaviconBgra(
      browser, index, &favicon_width, &favicon_height);
  if (pixels && favicon_width > 0 && favicon_height > 0) {
    const size_t bytes = static_cast<size_t>(favicon_width) *
                         static_cast<size_t>(favicon_height) * 4;
    // SAFETY: TabFaviconBgra reports the buffer's own dimensions.
    const auto view = UNSAFE_BUFFERS(base::span<const uint8_t>(pixels, bytes));
    favicon = base::Base64Encode(view);
  }
  const std::string guid =
      as_bookmark
          ? tab_store_.AddBookmark(profile_id, space_id, std::string(), title,
                                   url, /*folder=*/false, favicon,
                                   favicon_width, favicon_height)
          : tab_store_.AddPinned(profile_id, space_id, title, url, favicon,
                                 favicon_width, favicon_height);
  if (!guid.empty()) {
    for (const auto& [s2, controller] : windows_) {
      controller->RefreshTabs();
    }
  }
}

void PhiCoordinator::RemoveStoredRow(const std::string& guid) {
  if (tab_store_.Remove(guid)) {
    // Clear the identity tag from any tab bound to the removed row — it
    // returns to the tab list as a normal tab, and a stale tag would keep
    // it excluded from tab groups (the Phi-managed rule).
    if (framework_) {
      for (const auto& [browser_id, slot] : browser_to_slot_) {
        std::optional<base::Value> parsed = base::JSONReader::Read(
            framework_->ListTabsJson(browser_id), base::JSON_PARSE_RFC);
        if (!parsed || !parsed->is_list()) {
          continue;
        }
        for (const base::Value& v : parsed->GetList()) {
          const base::DictValue* d = v.GetIfDict();
          const std::string* tag = d ? d->FindString("tag") : nullptr;
          if (tag && *tag == guid) {
            framework_->SetTabTag(browser_id,
                                  d->FindInt("index").value_or(-1), "");
          }
        }
      }
    }
    for (const auto& [s2, controller] : windows_) {
      controller->RefreshTabs();
    }
    PhiSettingsWindow::Refresh();
  }
}

void PhiCoordinator::ReorderStoredRow(const std::string& guid,
                                      int new_index) {
  if (tab_store_.Reorder(guid, new_index)) {
    for (const auto& [slot, controller] : windows_) {
      controller->RefreshTabs();
    }
    PhiSettingsWindow::Refresh();
  }
}

int PhiCoordinator::SidebarWidth() const {
  return sidebar_width_;
}

void PhiCoordinator::RequestSidebarWidth(int dip) {
  const int clamped =
      std::max(kSidebarWidthMin, std::min(dip, kSidebarWidthMax));
  if (clamped == sidebar_width_) {
    return;
  }
  sidebar_width_ = clamped;
  SaveLayoutMode();  // The layout file carries the width too.
  for (const auto& [slot, controller] : windows_) {
    controller->LayoutContents();
    controller->RefreshTabs();
  }
}

void PhiCoordinator::ReorderSpace(const std::string& space_id, int new_index) {
  auto it = std::find_if(spaces_.begin(), spaces_.end(),
                         [&](const Space& s) { return s.id == space_id; });
  if (it == spaces_.end()) {
    return;
  }
  Space moved = *it;
  spaces_.erase(it);
  const int clamped =
      std::max(0, std::min(new_index, static_cast<int>(spaces_.size())));
  spaces_.insert(spaces_.begin() + clamped, std::move(moved));
  SaveSpaces();
  for (const auto& [slot, controller] : windows_) {
    controller->RefreshTabs();
  }
}

void PhiCoordinator::RequestBookmarkActiveTab(int64_t slot) {
  std::string profile_id, space_id;
  if (!framework_ || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return;
  }
  std::string title, url;
  std::optional<base::DictValue> state = base::JSONReader::ReadDict(
      framework_->NavStateJson(ResolveBrowser(slot)), base::JSON_PARSE_RFC);
  if (state) {
    if (const std::string* u = state->FindString("url")) {
      url = *u;
    }
    if (const std::string* t = state->FindString("title")) {
      title = *t;
    }
  }
  if (url.empty()) {
    return;  // Nothing to bookmark (the Mac disables the item then).
  }
  int fw = 0, fh = 0;
  const std::string favicon = CaptureFavicon(slot, &fw, &fh);
  const std::string guid =
      tab_store_.AddBookmark(profile_id, space_id, std::string(), title, url,
                             /*folder=*/false, favicon, fw, fh);
  if (!guid.empty()) {
    for (const auto& [s, controller] : windows_) {
      controller->RefreshTabs();
    }
  }
}

void PhiCoordinator::LoadPrefs() {
  HANDLE f = ::CreateFileW(prefs_path_.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    std::string json;
    char buf[1024];
    DWORD read = 0;
    while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
      json.append(buf, read);
    }
    ::CloseHandle(f);
    std::optional<base::DictValue> root =
        base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
    if (root) {
      saturation_ = root->FindInt("saturation").value_or(saturation_);
      always_show_full_url_ =
          root->FindBool("alwaysShowFullUrl").value_or(always_show_full_url_);
      developer_mode_ =
          root->FindBool("developerMode").value_or(developer_mode_);
      if (const std::string* behavior = root->FindString("newTabBehavior")) {
        new_tab_behavior_ = *behavior == "omnibox"
                                ? PhiNewTabBehavior::kOmnibox
                                : PhiNewTabBehavior::kNewTabPage;
      }
    }
  }
  // The palette depends on the saturation, so resolve it once both are known.
  palette_ = PhiResolvePalette(PhiThemeById(theme_id_), appearance_, saturation_);
  RebuildSpacePalettes();
}

void PhiCoordinator::SavePrefs() const {
  base::DictValue root;
  root.Set("saturation", saturation_);
  root.Set("alwaysShowFullUrl", always_show_full_url_);
  root.Set("developerMode", developer_mode_);
  root.Set("newTabBehavior", new_tab_behavior_ == PhiNewTabBehavior::kOmnibox
                                 ? "omnibox"
                                 : "newTabPage");
  std::string json;
  base::JSONWriter::Write(root, &json);
  HANDLE f = ::CreateFileW(prefs_path_.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD wrote = 0;
  ::WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &wrote, nullptr);
  ::CloseHandle(f);
}

int PhiCoordinator::Saturation() const {
  return saturation_;
}

void PhiCoordinator::SetSaturation(int value) {
  value = value < 0 ? 0 : (value > 100 ? 100 : value);
  if (value == saturation_) {
    return;
  }
  saturation_ = value;
  SavePrefs();
  ApplyTheme();
}

PhiNewTabBehavior PhiCoordinator::NewTabBehavior() const {
  return new_tab_behavior_;
}

void PhiCoordinator::SetNewTabBehavior(PhiNewTabBehavior behavior) {
  if (behavior == new_tab_behavior_) {
    return;
  }
  new_tab_behavior_ = behavior;
  SavePrefs();
  PhiSettingsWindow::Refresh();
}

bool PhiCoordinator::AlwaysShowFullUrl() const {
  return always_show_full_url_;
}

void PhiCoordinator::SetAlwaysShowFullUrl(bool enabled) {
  if (enabled == always_show_full_url_) {
    return;
  }
  always_show_full_url_ = enabled;
  SavePrefs();
  for (const auto& [slot, controller] : windows_) {
    controller->RefreshTabs();
  }
  PhiSettingsWindow::Refresh();
}

PhiPinScope PhiCoordinator::PinScope() const {
  return tab_store_.pin_scope();
}

void PhiCoordinator::SetPinScope(PhiPinScope scope) {
  if (!tab_store_.SetPinScope(scope)) {
    return;
  }
  for (const auto& [slot, controller] : windows_) {
    controller->RefreshTabs();
  }
  PhiSettingsWindow::Refresh();
}

std::vector<std::string> PhiCoordinator::SpaceSummaries() const {
  std::vector<std::string> out;
  for (const Space& space : spaces_) {
    out.push_back(space.name + "\t" + PhiIconDisplayText(space.icon) + "\t" +
                  space.profile_id + "\t" + space.id + "\t" + space.theme_id +
                  "\t" + base::NumberToString(space.saturation));
  }
  return out;
}

void PhiCoordinator::SetSpaceIcon(const std::string& space_id,
                                  const std::string& icon) {
  if (!PhiIconIsValid(icon)) {
    return;
  }
  for (Space& space : spaces_) {
    if (space.id != space_id) {
      continue;
    }
    space.icon = icon;
    SaveSpaces();
    for (const auto& [slot, controller] : windows_) {
      controller->RefreshTabs();
    }
    PhiSettingsWindow::Refresh();
    return;
  }
}

void PhiCoordinator::SetSpaceSaturation(const std::string& space_id,
                                        int value) {
  for (Space& space : spaces_) {
    if (space.id != space_id) {
      continue;
    }
    // -1 follows the app-wide slider; anything else clamps to its range.
    space.saturation = value < 0 ? -1 : (value > 100 ? 100 : value);
    SaveSpaces();
    RebuildSpacePalettes();
    for (const auto& [slot, controller] : windows_) {
      controller->ApplyPalette();
    }
    PhiSettingsWindow::Refresh();
    return;
  }
}

bool PhiCoordinator::RenameSpace(const std::string& space_id,
                                 const std::string& name) {
  if (name.empty()) {
    return false;
  }
  for (Space& space : spaces_) {
    if (space.id != space_id) {
      continue;
    }
    space.name = name;
    SaveSpaces();
    for (const auto& [slot, controller] : windows_) {
      controller->RefreshTabs();
    }
    PhiSettingsWindow::Refresh();
    return true;
  }
  return false;
}

bool PhiCoordinator::DeleteSpace(const std::string& space_id) {
  // The default Space cannot be deleted, as on Mac.
  if (space_id == "1") {
    return false;
  }
  auto it = std::find_if(spaces_.begin(), spaces_.end(),
                         [&](const Space& s) { return s.id == space_id; });
  if (it == spaces_.end()) {
    return false;
  }
  // Close the Space's browser wherever it is open; each controller switches
  // away first so a live window never keeps showing a deleted Space.
  for (const auto& [slot, controller] : windows_) {
    controller->CloseSpaceBrowser(space_id);
  }
  // The Space's bookmarks and space-scoped pins go with it (the Mac's
  // delete-confirmation promise).
  tab_store_.RemoveForSpace(space_id);
  spaces_.erase(it);
  SaveSpaces();
  RebuildSpacePalettes();
  for (const auto& [slot, controller] : windows_) {
    controller->RefreshTabs();
  }
  PhiSettingsWindow::Refresh();
  return true;
}

bool PhiCoordinator::DeleteProfileById(const std::string& profile_id) {
  // Default is permanent, and a profile still bound to a Space stays —
  // bindings are immutable, so the Spaces must go first.
  if (profile_id.empty() || profile_id == "Default" || !framework_) {
    return false;
  }
  for (const Space& space : spaces_) {
    if (space.profile_id == profile_id) {
      return false;
    }
  }
  framework_->DeleteProfile(profile_id.c_str());
  PhiSettingsWindow::Refresh();
  return true;
}

bool PhiCoordinator::RenameProfileById(const std::string& profile_id,
                                       const std::string& name) {
  if (profile_id.empty() || name.empty() || !framework_) {
    return false;
  }
  framework_->RenameProfile(profile_id.c_str(), name.c_str());
  PhiSettingsWindow::Refresh();
  return true;
}

bool PhiCoordinator::DeveloperMode() const {
  return developer_mode_;
}

void PhiCoordinator::SetDeveloperMode(bool enabled) {
  if (developer_mode_ == enabled) {
    return;
  }
  developer_mode_ = enabled;
  SavePrefs();
  PhiSettingsWindow::Refresh();
}

void PhiCoordinator::OpenBrowserSettings() {
  if (!windows_.empty()) {
    RequestOpen(windows_.begin()->first, "chrome://settings");
  }
}

std::vector<std::string> PhiCoordinator::ProfileSummaries() const {
  std::vector<std::string> out;
  if (!framework_) {
    return out;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return out;
  }
  for (const base::Value& value : parsed->GetList()) {
    if (!value.is_dict()) {
      continue;
    }
    const std::string* id = value.GetDict().FindString("id");
    const std::string* name = value.GetDict().FindString("name");
    if (id) {
      out.push_back(*id + "\t" + (name ? *name : *id));
    }
  }
  return out;
}

void PhiCoordinator::CreateProfileNamed(const std::string& name) {
  if (framework_ && !name.empty()) {
    framework_->CreateProfile(name.c_str());
  }
}

void PhiCoordinator::CreateSpaceNamed(const std::string& name) {
  if (name.empty()) {
    return;
  }
  base::DictValue payload;
  payload.Set("name", name);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  // The same path the control verb takes — one creation flow.
  ExecuteControlOnUi("agentSpace.spaces.create", json);
}

void PhiCoordinator::LoadTheme() {
  palette_ = PhiResolvePalette(PhiThemeById(theme_id_), appearance_, saturation_);
  HANDLE f = ::CreateFileW(theme_path_.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;  // First run: Pure, following the system appearance (the Mac
             // defaults).
  }
  std::string json;
  char buf[512];
  DWORD read = 0;
  while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
    json.append(buf, read);
  }
  ::CloseHandle(f);
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  if (const std::string* id = root->FindString("themeId")) {
    theme_id_ = PhiThemeById(*id).id;  // Normalises an unknown id to Pure.
  }
  if (const std::string* name = root->FindString("appearance")) {
    PhiAppearance parsed = PhiAppearance::kSystem;
    if (PhiAppearanceFromName(*name, &parsed)) {
      appearance_ = parsed;
    }
  }
  palette_ = PhiResolvePalette(PhiThemeById(theme_id_), appearance_);
  LOG(INFO) << "[PhiClient] theme: " << theme_id_ << " / "
            << PhiAppearanceName(appearance_);
}

void PhiCoordinator::SaveTheme() const {
  base::DictValue root;
  root.Set("themeId", theme_id_);
  root.Set("appearance", PhiAppearanceName(appearance_));
  std::string json;
  base::JSONWriter::Write(root, &json);
  HANDLE f = ::CreateFileW(theme_path_.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD wrote = 0;
  ::WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &wrote, nullptr);
  ::CloseHandle(f);
}

void PhiCoordinator::ApplyTheme() {
  palette_ = PhiResolvePalette(PhiThemeById(theme_id_), appearance_, saturation_);
  RebuildSpacePalettes();
  SaveTheme();
  for (const auto& [slot, controller] : windows_) {
    controller->ApplyPalette();
  }
  PhiSettingsWindow::Refresh();
}

void PhiCoordinator::RebuildSpacePalettes() {
  palette_by_space_.clear();
  for (const Space& space : spaces_) {
    if (space.theme_id.empty() && space.saturation < 0) {
      continue;  // Follows the app-wide theme.
    }
    const std::string& id = space.theme_id.empty() ? theme_id_ : space.theme_id;
    const int saturation =
        space.saturation < 0 ? saturation_ : space.saturation;
    palette_by_space_[space.id] =
        PhiResolvePalette(PhiThemeById(id), appearance_, saturation);
  }
}

const PhiPalette& PhiCoordinator::PaletteForSlot(int64_t slot) const {
  auto window = windows_.find(slot);
  if (window != windows_.end()) {
    auto found = palette_by_space_.find(window->second->active_space());
    if (found != palette_by_space_.end()) {
      return found->second;
    }
  }
  return palette_;
}

std::string PhiCoordinator::SpaceThemeId(const std::string& space_id) const {
  for (const Space& space : spaces_) {
    if (space.id == space_id) {
      return space.theme_id;
    }
  }
  return std::string();
}

void PhiCoordinator::SetSpaceTheme(const std::string& space_id,
                                   const std::string& theme_id) {
  for (Space& space : spaces_) {
    if (space.id != space_id) {
      continue;
    }
    // An empty id clears the override, putting the Space back on the app-wide
    // theme — the Mac's "no entry in spaceThemeIds" state.
    space.theme_id = theme_id.empty() ? std::string() : PhiThemeById(theme_id).id;
    SaveSpaces();
    RebuildSpacePalettes();
    for (const auto& [slot, controller] : windows_) {
      controller->ApplyPalette();
    }
    PhiSettingsWindow::Refresh();
    return;
  }
}

const PhiPalette& PhiCoordinator::Palette() const {
  return palette_;
}

std::string PhiCoordinator::ThemeId() const {
  return theme_id_;
}

PhiAppearance PhiCoordinator::Appearance() const {
  return appearance_;
}

void PhiCoordinator::SetThemeId(const std::string& id) {
  const std::string normalised = PhiThemeById(id).id;
  if (normalised == theme_id_) {
    return;
  }
  theme_id_ = normalised;
  ApplyTheme();
}

void PhiCoordinator::SetAppearance(PhiAppearance appearance) {
  if (appearance == appearance_) {
    return;
  }
  appearance_ = appearance;
  ApplyTheme();
}

void PhiCoordinator::RequestSettings() {
  PhiSettingsWindow::Open(this);
}

PhiLayoutMode PhiCoordinator::LayoutMode() const {
  return layout_mode_;
}

void PhiCoordinator::RequestLayoutMode(PhiLayoutMode mode) {
  if (layout_mode_ == mode) {
    return;
  }
  layout_mode_ = mode;
  SaveLayoutMode();
  // Every slot re-lays out: the layout is a client-wide setting, so two
  // windows side by side must never disagree.
  for (const auto& [slot, controller] : windows_) {
    controller->ApplyLayoutMode(mode);
  }
}

bool PhiCoordinator::TryFinishProfileSpace() {
  if (pending_profile_slot_ == 0 || !framework_) {
    return true;
  }
  if (++pending_profile_ticks_ > 40) {  // ~20s
    LOG(ERROR) << "[PhiClient] profile creation timed out";
    pending_profile_slot_ = 0;
    return true;
  }
  std::optional<base::Value> now = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!now || !now->is_list()) {
    return false;
  }
  for (const base::Value& v : now->GetList()) {
    if (!v.is_dict()) {
      continue;
    }
    const std::string* id = v.GetDict().FindString("id");
    if (!id || known_profiles_before_.count(*id)) {
      continue;
    }
    // The new profile: create its Space and enter it.
    base::DictValue payload;
    payload.Set("name", "Space " + std::to_string(next_space_id_));
    payload.Set("profileId", *id);
    std::string json;
    base::JSONWriter::Write(payload, &json);
    const std::string created =
        ExecuteControlOnUi("agentSpace.spaces.create", json);
    std::optional<base::DictValue> r =
        base::JSONReader::ReadDict(created, base::JSON_PARSE_RFC);
    const int64_t slot = pending_profile_slot_;
    pending_profile_slot_ = 0;
    if (r) {
      if (const std::string* sid = r->FindString("id")) {
        RequestSwitchSpace(slot, *sid);
      }
    }
    return true;
  }
  return false;
}

std::vector<PhiSpaceView> PhiCoordinator::Spaces() const {
  std::vector<PhiSpaceView> out;
  out.reserve(spaces_.size());
  for (const Space& space : spaces_) {
    PhiSpaceView view;
    view.id = space.id;
    view.name = space.name;
    view.profile_id = space.profile_id;
    view.icon = space.icon;
    view.icon_text = PhiIconDisplayText(space.icon);
    out.push_back(std::move(view));
  }
  return out;
}

void PhiCoordinator::RequestSwitchSpace(int64_t slot,
                                        const std::string& space_id) {
  base::DictValue payload;
  payload.Set("windowId", static_cast<double>(slot));
  payload.Set("spaceId", space_id);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  ExecuteControlOnUi("agentSpace.spaces.switch", json);
}

void PhiCoordinator::RequestCreateSpace(int64_t slot, bool new_profile) {
  // Same profile as the slot's current Space unless a new one was asked for.
  std::string profile_id = "Default";
  auto it = windows_.find(slot);
  if (it != windows_.end()) {
    const std::string& active = it->second->active_space();
    for (const Space& s : spaces_) {
      if (s.id == active) {
        profile_id = s.profile_id;
        break;
      }
    }
  }
  if (new_profile && framework_) {
    // Profile creation is asynchronous and the new basename is not known up
    // front. Snapshot the current profiles, ask for a new one, and finish the
    // Space (bound to that profile) when it appears — bindings are immutable,
    // so the Space must not be created against the wrong profile first.
    known_profiles_before_.clear();
    std::optional<base::Value> before = base::JSONReader::Read(
        framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
    if (before && before->is_list()) {
      for (const base::Value& v : before->GetList()) {
        if (v.is_dict()) {
          if (const std::string* id = v.GetDict().FindString("id")) {
            known_profiles_before_.insert(*id);
          }
        }
      }
    }
    pending_profile_slot_ = slot;
    pending_profile_ticks_ = 0;
    framework_->CreateProfile(
        ("Phi Space " + std::to_string(next_space_id_)).c_str());
    if (msg_hwnd_) {
      ::SetTimer(msg_hwnd_, 1, 500, nullptr);
    }
    return;
  }
  base::DictValue payload;
  payload.Set("name", "Space " + std::to_string(next_space_id_));
  payload.Set("profileId", profile_id);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  const std::string created = ExecuteControlOnUi("agentSpace.spaces.create", json);
  std::optional<base::DictValue> r =
      base::JSONReader::ReadDict(created, base::JSON_PARSE_RFC);
  if (r) {
    if (const std::string* id = r->FindString("id")) {
      RequestSwitchSpace(slot, *id);
    }
  }
}

PhiWindowController* PhiCoordinator::ControllerForSlot(int64_t slot) {
  auto it = windows_.find(slot);
  if (it == windows_.end() && slot == 0 && !windows_.empty()) {
    it = windows_.begin();  // No windowId: the first window.
  }
  return it != windows_.end() ? it->second.get() : nullptr;
}

int64_t PhiCoordinator::ResolveBrowser(int64_t slot_or_browser) const {
  auto it = windows_.find(slot_or_browser);
  if (it != windows_.end() && it->second->active_browser() != 0) {
    return it->second->active_browser();
  }
  return slot_or_browser;
}

std::string PhiCoordinator::ExecuteControlOnUi(const std::string& type,
                                               const std::string& payload_json) {
  // Runs on the UI thread — safe to touch windows_ and call framework_.
  base::DictValue out;
  if (type == "agentSpace.ping") {
    out.Set("ok", true);
  } else if (type == "agentSpace.list") {
    base::ListValue list;
    for (const auto& [window_id, controller] : windows_) {
      list.Append(static_cast<double>(window_id));
    }
    out.Set("windows", std::move(list));
  } else if (type == "agentSpace.newWindow") {
    int64_t source = windows_.empty() ? 0 : windows_.begin()->first;
    if (framework_) {
      framework_->NewWindow(source);
    }
    out.Set("ok", true);
  } else if (type == "agentSpace.close") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id = 0;
    if (p) {
      window_id = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
    }
    // Closing a SLOT means closing every Space browser it hosts, not just the
    // browser its id names — same reason the shell window's close box does.
    auto it = windows_.find(window_id);
    bool ok = false;
    if (it != windows_.end()) {
      PhiWindowController* controller = it->second.get();
      ok = true;  // Set first: the call below may erase `it`.
      controller->RequestClose();
    } else if (framework_ && window_id) {
      framework_->CloseBrowserWindow(window_id);
      ok = true;
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.navigate") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id = 0;
    std::string url;
    if (p) {
      window_id = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      if (const std::string* u = p->FindString("url")) {
        url = *u;
      }
    }
    bool ok = framework_ && window_id && !url.empty();
    if (ok) {
      framework_->Navigate(ResolveBrowser(window_id), url.c_str());
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.spaces.list") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    auto it = windows_.find(slot);
    base::ListValue list;
    for (const Space& space : spaces_) {
      base::DictValue s;
      s.Set("id", space.id);
      s.Set("name", space.name);
      s.Set("profileId", space.profile_id);
      s.Set("icon", space.icon);
      s.Set("themeId", space.theme_id);
      s.Set("iconText", PhiIconDisplayText(space.icon));
      s.Set("active",
            it != windows_.end() && it->second->active_space() == space.id);
      s.Set("open", it != windows_.end() && it->second->HasSpace(space.id));
      list.Append(std::move(s));
    }
    out.Set("spaces", std::move(list));
    out.Set("ok", true);
  } else if (type == "agentSpace.spaces.create") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string name, profile_id;
    if (p) {
      if (const std::string* n = p->FindString("name")) {
        name = *n;
      }
      if (const std::string* pid = p->FindString("profileId")) {
        profile_id = *pid;
      }
    }
    const std::string id = std::to_string(next_space_id_++);
    if (name.empty()) {
      name = "Space " + id;
    }
    if (profile_id.empty()) {
      profile_id = "Default";  // A Space always binds exactly one profile.
    }
    // A new Space draws a random catalog icon, as the Mac's create panel does.
    const auto& catalog = PhiIconCatalogAll();
    const std::string icon =
        "phi:" +
        std::string(catalog[base::RandGenerator(catalog.size())].id);
    spaces_.push_back({id, name, profile_id, icon});
    SaveSpaces();
    out.Set("id", id);
    out.Set("name", name);
    out.Set("profileId", profile_id);
    out.Set("ok", true);
  } else if (type == "agentSpace.spaces.switch") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot = 0;
    std::string space_id;
    if (p) {
      slot = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      if (const std::string* s = p->FindString("spaceId")) {
        space_id = *s;
      }
    }
    auto it = windows_.find(slot);
    auto space_it = std::find_if(spaces_.begin(), spaces_.end(),
                                 [&](const Space& s) { return s.id == space_id; });
    bool ok = false;
    if (it != windows_.end() && space_it != spaces_.end()) {
      if (it->second->HasSpace(space_id)) {
        ok = it->second->ShowSpace(space_id);
      } else if (framework_) {
        // First visit: create this Space's browser IN ITS PROFILE and attach
        // it to the slot. A cold profile loads asynchronously, so the pending
        // attach stays armed until OnBrowserWindowCreated consumes it.
        pending_slot_ = slot;
        pending_space_ = space_id;
        ok = framework_->CreateSpaceBrowserInProfile(
            space_it->profile_id.c_str());
        if (it->second->HasSpace(space_id)) {
          out.Set("pending", false);
        } else {
          out.Set("pending", true);  // Attaching when the browser arrives.
        }
      }
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.spaces.rename" ||
             type == "agentSpace.spaces.delete") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string space_id, name;
    if (p) {
      if (const std::string* s = p->FindString("spaceId")) {
        space_id = *s;
      }
      if (const std::string* n = p->FindString("name")) {
        name = *n;
      }
    }
    const bool ok = type == "agentSpace.spaces.rename"
                        ? RenameSpace(space_id, name)
                        : DeleteSpace(space_id);
    out.Set("ok", ok);
  } else if (type == "agentSpace.profiles.rename" ||
             type == "agentSpace.profiles.delete") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string id, name;
    if (p) {
      if (const std::string* i = p->FindString("id")) {
        id = *i;
      }
      if (const std::string* n = p->FindString("name")) {
        name = *n;
      }
    }
    const bool ok = type == "agentSpace.profiles.rename"
                        ? RenameProfileById(id, name)
                        : DeleteProfileById(id);
    out.Set("ok", ok);
  } else if (type == "agentSpace.profiles.list") {
    if (framework_) {
      std::optional<base::Value> v = base::JSONReader::Read(
          framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
      if (v && v->is_list()) {
        out.Set("profiles", std::move(*v));
      }
    }
    if (!out.contains("profiles")) {
      out.Set("profiles", base::ListValue());
    }
    out.Set("ok", true);
  } else if (type == "agentSpace.profiles.create") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string name;
    if (p) {
      if (const std::string* n = p->FindString("name")) {
        name = *n;
      }
    }
    if (framework_ && !name.empty()) {
      framework_->CreateProfile(name.c_str());
    }
    out.Set("ok", framework_ && !name.empty());
  } else if (type == "agentSpace.window.profile") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    if (framework_) {
      std::optional<base::DictValue> v = base::JSONReader::ReadDict(
          framework_->WindowProfileJson(ResolveBrowser(slot)),
          base::JSON_PARSE_RFC);
      if (v) {
        out = std::move(*v);
      }
    }
    out.Set("ok", out.contains("id"));
  } else if (type == "agentSpace.layout.get") {
    out.Set("mode", PhiLayoutModeName(layout_mode_));
    out.Set("ok", true);
  } else if (type == "agentSpace.layout.set") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    PhiLayoutMode mode = layout_mode_;
    bool ok = false;
    if (p) {
      if (const std::string* name = p->FindString("mode")) {
        ok = PhiLayoutModeFromName(*name, &mode);
      }
      if (std::optional<int> width = p->FindInt("sidebarWidth")) {
        RequestSidebarWidth(*width);
        ok = true;
      }
    }
    if (ok && p && p->FindString("mode")) {
      RequestLayoutMode(mode);
    }
    out.Set("mode", PhiLayoutModeName(layout_mode_));
    out.Set("sidebarWidth", sidebar_width_);
    out.Set("ok", ok);
  } else if (type == "agentSpace.pinned.scope.get") {
    out.Set("scope", PhiPinScopeName(tab_store_.pin_scope()));
    out.Set("ok", true);
  } else if (type == "agentSpace.pinned.scope.set") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    PhiPinScope scope = tab_store_.pin_scope();
    bool ok = false;
    if (p) {
      if (const std::string* name = p->FindString("scope")) {
        ok = PhiPinScopeFromName(*name, &scope);
      }
    }
    if (ok) {
      tab_store_.SetPinScope(scope);
      for (const auto& [slot, controller] : windows_) {
        controller->RefreshTabs();
      }
    }
    out.Set("scope", PhiPinScopeName(tab_store_.pin_scope()));
    out.Set("ok", ok);
  } else if (type == "agentSpace.pinned.list") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    std::string profile_id, space_id;
    base::ListValue list;
    if (OwnerForSlot(slot, &profile_id, &space_id)) {
      for (const PhiRow& row : tab_store_.PinnedFor(profile_id, space_id)) {
        base::DictValue entry;
        entry.Set("guid", row.guid);
        entry.Set("title", row.title);
        entry.Set("url", row.url);
        entry.Set("index", row.index);
        entry.Set("favicon", row.favicon_base64);
        entry.Set("faviconWidth", row.favicon_width);
        entry.Set("faviconHeight", row.favicon_height);
        list.Append(std::move(entry));
      }
    }
    out.Set("pinned", std::move(list));
    out.Set("scope", PhiPinScopeName(tab_store_.pin_scope()));
    out.Set("ok", true);
  } else if (type == "agentSpace.pinned.open") {
    // The Mac's openOrFocusPinnedTab, the same path a sidebar pin click
    // takes: focus the pin's bound live tab, or open one bound to the pin.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot = 0;
    std::string guid;
    if (p) {
      slot = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      if (const std::string* g = p->FindString("guid")) {
        guid = *g;
      }
    }
    PhiWindowController* controller = ControllerForSlot(slot);
    std::string profile_id, space_id, url;
    bool known = false;
    if (controller &&
        OwnerForSlot(controller->window_id(), &profile_id, &space_id)) {
      // Pins and sidebar bookmarks both bind (bookmarkGuidBacking) — resolve
      // the guid against either kind of stored row.
      for (const PhiRow& row : tab_store_.rows()) {
        if (row.guid == guid && row.type != PhiRowType::kBookmarkFolder) {
          url = row.url;
          known = true;
          break;
        }
      }
    }
    if (known) {
      controller->OpenOrFocusPinned(guid, url);
      out.Set("ok", true);
    } else {
      out.Set("ok", false);
      out.Set("error", "unknown pin");
    }
  } else if (type == "agentSpace.pinned.add") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot = 0;
    std::string title, url;
    if (p) {
      slot = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      if (const std::string* t = p->FindString("title")) {
        title = *t;
      }
      if (const std::string* u = p->FindString("url")) {
        url = *u;
      }
    }
    std::string profile_id, space_id, guid;
    if (OwnerForSlot(slot, &profile_id, &space_id)) {
      // No url given: pin what the slot is showing.
      if (url.empty() && framework_) {
        std::optional<base::DictValue> st = base::JSONReader::ReadDict(
            framework_->NavStateJson(ResolveBrowser(slot)), base::JSON_PARSE_RFC);
        if (st) {
          if (const std::string* u = st->FindString("url")) {
            url = *u;
          }
          if (title.empty()) {
            if (const std::string* t = st->FindString("title")) {
              title = *t;
            }
          }
        }
      }
      int fw = 0, fh = 0;
      // Only when we are pinning what the slot is SHOWING: the live favicon
      // belongs to the active tab, not to some other url the caller named.
      const std::string favicon =
          url == LiveUrl(slot) ? CaptureFavicon(slot, &fw, &fh) : std::string();
      guid = tab_store_.AddPinned(profile_id, space_id, title, url, favicon, fw,
                                  fh);
      // Report what was captured: a caller (or a test) can then tell "no icon
      // yet" apart from "the pin failed".
      out.Set("faviconWidth", fw);
      out.Set("faviconHeight", fh);
      out.Set("liveUrl", LiveUrl(slot));
    }
    if (!guid.empty()) {
      for (const auto& [s, controller] : windows_) {
        controller->RefreshTabs();
      }
    }
    out.Set("guid", guid);
    out.Set("ok", !guid.empty());
  } else if (type == "agentSpace.bookmarks.list") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot = 0;
    std::string parent;
    if (p) {
      slot = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      if (const std::string* g = p->FindString("parentGuid")) {
        parent = *g;
      }
    }
    std::string profile_id, space_id;
    base::ListValue list;
    if (OwnerForSlot(slot, &profile_id, &space_id)) {
      for (const PhiRow& row :
           tab_store_.BookmarksFor(profile_id, space_id, parent)) {
        base::DictValue entry;
        entry.Set("guid", row.guid);
        entry.Set("title", row.title);
        entry.Set("url", row.url);
        entry.Set("index", row.index);
        entry.Set("folder", row.type == PhiRowType::kBookmarkFolder);
        entry.Set("favicon", row.favicon_base64);
        entry.Set("faviconWidth", row.favicon_width);
        entry.Set("faviconHeight", row.favicon_height);
        list.Append(std::move(entry));
      }
    }
    out.Set("bookmarks", std::move(list));
    out.Set("ok", true);
  } else if (type == "agentSpace.bookmarks.add") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot = 0;
    std::string title, url, parent;
    bool folder = false;
    if (p) {
      slot = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      if (const std::string* t = p->FindString("title")) {
        title = *t;
      }
      if (const std::string* u = p->FindString("url")) {
        url = *u;
      }
      if (const std::string* g = p->FindString("parentGuid")) {
        parent = *g;
      }
      folder = p->FindBool("folder").value_or(false);
    }
    std::string profile_id, space_id, guid;
    if (OwnerForSlot(slot, &profile_id, &space_id)) {
      if (!folder && url.empty() && framework_) {
        std::optional<base::DictValue> st = base::JSONReader::ReadDict(
            framework_->NavStateJson(ResolveBrowser(slot)), base::JSON_PARSE_RFC);
        if (st) {
          if (const std::string* u = st->FindString("url")) {
            url = *u;
          }
          if (title.empty()) {
            if (const std::string* t = st->FindString("title")) {
              title = *t;
            }
          }
        }
      }
      int fw = 0, fh = 0;
      const std::string favicon =
          (!folder && url == LiveUrl(slot)) ? CaptureFavicon(slot, &fw, &fh)
                                            : std::string();
      guid = tab_store_.AddBookmark(profile_id, space_id, parent, title, url,
                                    folder, favicon, fw, fh);
    }
    if (!guid.empty()) {
      for (const auto& [s, controller] : windows_) {
        controller->RefreshTabs();
      }
    }
    out.Set("guid", guid);
    out.Set("ok", !guid.empty());
  } else if (type == "agentSpace.bookmarks.remove" ||
             type == "agentSpace.pinned.remove") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string guid;
    if (p) {
      if (const std::string* g = p->FindString("guid")) {
        guid = *g;
      }
    }
    // Route through RemoveStoredRow: it also clears the identity tag from
    // any tab bound to the row (verb and UI paths must behave alike).
    const bool ok = !guid.empty() && tab_store_.Contains(guid);
    if (ok) {
      RemoveStoredRow(guid);
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.emoji.groups") {
    // The catalog the Mac bundles, offered verbatim.
    base::ListValue groups;
    for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
      base::DictValue g;
      g.Set("name", group.name);
      g.Set("count", static_cast<int>(group.items.size()));
      groups.Append(std::move(g));
    }
    out.Set("groups", std::move(groups));
    out.Set("ok", true);
  } else if (type == "agentSpace.emoji.list") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string want;
    int limit = 64;
    if (p) {
      if (const std::string* g = p->FindString("group")) {
        want = *g;
      }
      limit = static_cast<int>(p->FindDouble("limit").value_or(64));
    }
    base::ListValue items;
    for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
      if (!want.empty() && group.name != want) {
        continue;
      }
      for (const PhiEmoji& emoji : group.items) {
        if (static_cast<int>(items.size()) >= limit) {
          break;
        }
        base::DictValue e;
        e.Set("id", emoji.id);
        e.Set("text", emoji.text);
        e.Set("name", emoji.name);
        items.Append(std::move(e));
      }
    }
    out.Set("emoji", std::move(items));
    out.Set("ok", true);
  } else if (type == "agentSpace.spaces.setTheme") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string space_id, theme_id;
    if (p) {
      if (const std::string* s2 = p->FindString("spaceId")) {
        space_id = *s2;
      }
      if (const std::string* t = p->FindString("themeId")) {
        theme_id = *t;
      }
    }
    // An empty id is legal: it clears the override.
    const bool known =
        theme_id.empty() || theme_id == PhiThemeById(theme_id).id;
    bool ok = false;
    if (known) {
      for (const Space& space : spaces_) {
        if (space.id == space_id) {
          SetSpaceTheme(space_id, theme_id);
          ok = true;
          break;
        }
      }
    }
    out.Set("themeId", SpaceThemeId(space_id));
    out.Set("ok", ok);
  } else if (type == "agentSpace.spaces.setIcon") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string space_id, icon;
    if (p) {
      if (const std::string* s = p->FindString("spaceId")) {
        space_id = *s;
      }
      if (const std::string* i = p->FindString("icon")) {
        icon = *i;
      }
    }
    bool ok = false;
    if (PhiIconIsValid(icon)) {
      for (Space& space : spaces_) {
        if (space.id != space_id) {
          continue;
        }
        space.icon = icon;
        SaveSpaces();
        ok = true;
        for (const auto& [slot, controller] : windows_) {
          controller->RefreshTabs();
        }
        break;
      }
    }
    out.Set("icon", icon);
    out.Set("ok", ok);
  } else if (type == "agentSpace.theme.get") {
    out.Set("themeId", theme_id_);
    out.Set("appearance", PhiAppearanceName(appearance_));
    base::ListValue names;
    for (const PhiTheme& theme : PhiBuiltInThemes()) {
      base::DictValue entry;
      entry.Set("id", theme.id);
      entry.Set("name", theme.name);
      names.Append(std::move(entry));
    }
    out.Set("themes", std::move(names));
    out.Set("dark", palette_.dark);
    out.Set("ok", true);
  } else if (type == "agentSpace.theme.set") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    bool ok = false;
    if (p) {
      if (const std::string* id = p->FindString("themeId")) {
        const bool known = *id == PhiThemeById(*id).id;
        if (known) {
          SetThemeId(*id);
          ok = true;
        }
      }
      if (const std::string* name = p->FindString("appearance")) {
        PhiAppearance parsed = PhiAppearance::kSystem;
        if (PhiAppearanceFromName(*name, &parsed)) {
          SetAppearance(parsed);
          ok = true;
        }
      }
    }
    out.Set("themeId", theme_id_);
    out.Set("appearance", PhiAppearanceName(appearance_));
    out.Set("dark", palette_.dark);
    out.Set("ok", ok);
  } else if (type == "agentSpace.settings.get") {
    // Everything the settings window shows, in one read.
    out.Set("section", PhiSettingsWindow::CurrentSection());
    base::ListValue sections;
    // The Mac's panes(), in its order; Developer only in developer mode.
    for (const char* name : {"account", "general", "profiles", "spaces", "ai",
                             "phiLink", "shortcuts"}) {
      sections.Append(name);
    }
    if (developer_mode_) {
      sections.Append("developer");
    }
    out.Set("sections", std::move(sections));
    out.Set("developerMode", developer_mode_);
    out.Set("themeId", theme_id_);
    out.Set("appearance", PhiAppearanceName(appearance_));
    out.Set("saturation", saturation_);
    out.Set("layout", PhiLayoutModeName(layout_mode_));
    out.Set("newTabBehavior", new_tab_behavior_ == PhiNewTabBehavior::kOmnibox
                                  ? "omnibox"
                                  : "newTabPage");
    out.Set("alwaysShowFullUrl", always_show_full_url_);
    out.Set("pinScope", PhiPinScopeName(tab_store_.pin_scope()));
    out.Set("ok", true);
  } else if (type == "agentSpace.settings.set") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    bool ok = false;
    if (p) {
      if (std::optional<int> saturation = p->FindInt("saturation")) {
        // Out-of-range is clamped, not rejected: a slider cannot overshoot.
        SetSaturation(*saturation);
        ok = true;
      }
      if (std::optional<bool> full = p->FindBool("alwaysShowFullUrl")) {
        SetAlwaysShowFullUrl(*full);
        ok = true;
      }
      if (const std::string* behavior = p->FindString("newTabBehavior")) {
        if (*behavior == "omnibox" || *behavior == "newTabPage") {
          SetNewTabBehavior(*behavior == "omnibox"
                                ? PhiNewTabBehavior::kOmnibox
                                : PhiNewTabBehavior::kNewTabPage);
          ok = true;
        }
      }
      if (std::optional<bool> dev = p->FindBool("developerMode")) {
        SetDeveloperMode(*dev);
        ok = true;
      }
      if (const std::string* section = p->FindString("section")) {
        // Developer exists only in developer mode — refuse it here too: the
        // window-side check cannot run before the window ever opens. The
        // refusal leaves `ok` untouched so a combined payload's other keys
        // still report their own success.
        if (*section != "developer" || developer_mode_) {
          ok = PhiSettingsWindow::ShowSection(*section) || ok;
        }
      }
    }
    out.Set("developerMode", developer_mode_);
    out.Set("saturation", saturation_);
    out.Set("alwaysShowFullUrl", always_show_full_url_);
    out.Set("newTabBehavior", new_tab_behavior_ == PhiNewTabBehavior::kOmnibox
                                  ? "omnibox"
                                  : "newTabPage");
    out.Set("section", PhiSettingsWindow::CurrentSection());
    out.Set("ok", ok);
  } else if (type == "agentSpace.settings.open") {
    RequestSettings();
    out.Set("hwnd", static_cast<double>(
                        reinterpret_cast<intptr_t>(PhiSettingsWindow::Handle())));
    out.Set("ok", PhiSettingsWindow::Handle() != nullptr);
  } else if (type == "agentSpace.window.layout") {
    // The chrome geometry a slot is rendering, so layout can be asserted
    // without going through pixels.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    auto it = windows_.find(slot);
    if (it != windows_.end()) {
      const RECT content = it->second->content_rect();
      base::DictValue rect;
      rect.Set("x", static_cast<int>(content.left));
      rect.Set("y", static_cast<int>(content.top));
      rect.Set("width", static_cast<int>(content.right - content.left));
      rect.Set("height", static_cast<int>(content.bottom - content.top));
      out.Set("content", std::move(rect));
      // Geometry is in PHYSICAL pixels; a caller comparing against design
      // constants has to scale them by this.
      out.Set("dpi", static_cast<int>(it->second->dpi()));
      const RECT url = it->second->url_field_rect();
      base::DictValue field;
      field.Set("x", static_cast<int>(url.left));
      field.Set("y", static_cast<int>(url.top));
      field.Set("width", static_cast<int>(url.right - url.left));
      field.Set("height", static_cast<int>(url.bottom - url.top));
      out.Set("urlField", std::move(field));
      out.Set("tabsTop", it->second->tabs_top());
      out.Set("rowHeight", it->second->tab_row_height());
      out.Set("rowGap", it->second->tab_row_gap());
      out.Set("titleBar", it->second->title_bar_height());
      out.Set("sidebarWidth", it->second->sidebar_width());
      out.Set("contentMargin", it->second->content_margin());
      const POINT frame = it->second->frame_offset();
      out.Set("frameLeft", static_cast<int>(frame.x));
      out.Set("frameTop", static_cast<int>(frame.y));
      out.Set("mode", PhiLayoutModeName(layout_mode_));
      out.Set("ok", true);
    } else {
      out.Set("ok", false);
    }
  } else if (type == "agentSpace.menu.list") {
    // The shell window's menu bar as the user would see it, dynamic rows
    // included — the automated stand-in for opening each menu by hand.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    PhiWindowController* controller = ControllerForSlot(slot);
    if (controller && controller->menu_bar()) {
      std::optional<base::Value> menus = base::JSONReader::Read(
          controller->menu_bar()->ListJson(), base::JSON_PARSE_RFC);
      if (menus) {
        out.Set("menus", std::move(*menus));
        out.Set("ok", true);
      } else {
        out.Set("ok", false);
      }
    } else {
      out.Set("ok", false);
    }
  } else if (type == "agentSpace.menu.click") {
    // Activates a menu item by its title path, e.g. ["File","New Tab"] or
    // ["Spaces","Edit Theme","Mint"]. Disabled items refuse, as a real click
    // would.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    std::vector<std::string> path;
    if (p) {
      if (const base::ListValue* list = p->FindList("path")) {
        for (const base::Value& v : *list) {
          if (v.is_string()) {
            path.push_back(v.GetString());
          }
        }
      }
    }
    auto it = windows_.find(slot);
    if (it == windows_.end() && slot == 0 && !windows_.empty()) {
      it = windows_.begin();
    }
    PhiWindowController* controller =
        it != windows_.end() ? it->second.get() : nullptr;
    out.Set("ok", controller && controller->menu_bar() && !path.empty() &&
                      controller->menu_bar()->InvokePath(path));
  } else if (type == "agentSpace.window.handle") {
    // The slot's shell HWND, so a test/screenshot tool can capture exactly the
    // window it is driving (several slots may exist).
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    auto it = windows_.find(slot);
    if (it != windows_.end() && it->second->shell_hwnd()) {
      out.Set("hwnd", static_cast<double>(
                          reinterpret_cast<intptr_t>(it->second->shell_hwnd())));
      out.Set("ok", true);
    } else {
      out.Set("ok", false);
    }
  } else if (type == "agentSpace.window.bounds") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    auto it = windows_.find(window_id);
    int x = 0, y = 0, w = 0, h = 0;
    if (it != windows_.end() && it->second->GetBounds(&x, &y, &w, &h)) {
      out.Set("x", x);
      out.Set("y", y);
      out.Set("width", w);
      out.Set("height", h);
      out.Set("ok", true);
    } else {
      out.Set("ok", false);
    }
  } else if (type == "agentSpace.window.setBounds") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id = 0;
    int x = 0, y = 0, w = 0, h = 0;
    if (p) {
      window_id = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      x = static_cast<int>(p->FindDouble("x").value_or(0));
      y = static_cast<int>(p->FindDouble("y").value_or(0));
      w = static_cast<int>(p->FindDouble("width").value_or(0));
      h = static_cast<int>(p->FindDouble("height").value_or(0));
    }
    auto it = windows_.find(window_id);
    bool ok = it != windows_.end() && w > 0 && h > 0 &&
              it->second->SetBounds(x, y, w, h);
    out.Set("ok", ok);
  } else if (type == "agentSpace.tabs.list") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    if (framework_ && window_id) {
      std::optional<base::Value> tabs = base::JSONReader::Read(
          framework_->ListTabsJson(ResolveBrowser(window_id)), base::JSON_PARSE_RFC);
      if (tabs && tabs->is_list()) {
        out.Set("tabs", std::move(*tabs));
      }
    }
    if (!out.contains("tabs")) {
      out.Set("tabs", base::ListValue());
    }
  } else if (type == "agentSpace.tabs.new") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id = 0;
    std::string url;
    if (p) {
      window_id = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      if (const std::string* u = p->FindString("url")) {
        url = *u;
      }
    }
    int index = (framework_ && window_id)
                    ? framework_->NewTab(ResolveBrowser(window_id), url.c_str())
                    : -1;
    out.Set("index", index);
    out.Set("ok", index >= 0);
  } else if (type == "agentSpace.nav.state") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    if (framework_ && window_id) {
      std::optional<base::DictValue> st = base::JSONReader::ReadDict(
          framework_->NavStateJson(ResolveBrowser(window_id)), base::JSON_PARSE_RFC);
      if (st) {
        out = std::move(*st);
      }
    }
  } else if (type == "agentSpace.nav.back" ||
             type == "agentSpace.nav.forward" ||
             type == "agentSpace.nav.reload" ||
             type == "agentSpace.nav.stop") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    const bool bypass_cache =
        p ? p->FindBool("bypassCache").value_or(false) : false;
    bool ok = false;
    if (framework_ && window_id) {
      if (type == "agentSpace.nav.back") {
        ok = framework_->GoBack(ResolveBrowser(window_id));
      } else if (type == "agentSpace.nav.forward") {
        ok = framework_->GoForward(ResolveBrowser(window_id));
      } else if (type == "agentSpace.nav.reload") {
        ok = framework_->Reload(ResolveBrowser(window_id), bypass_cache);
      } else {
        ok = framework_->StopLoading(ResolveBrowser(window_id));
      }
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.tabs.activate" ||
             type == "agentSpace.tabs.close" ||
             type == "agentSpace.tabs.split" ||
             type == "agentSpace.tabs.unsplit") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id = 0;
    int index = -1;
    if (p) {
      window_id = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      index = static_cast<int>(p->FindDouble("index").value_or(-1));
    }
    bool ok = false;
    if (framework_ && window_id && index >= 0) {
      const int64_t browser = ResolveBrowser(window_id);
      if (type == "agentSpace.tabs.activate") {
        ok = framework_->ActivateTab(browser, index);
      } else if (type == "agentSpace.tabs.close") {
        ok = framework_->CloseTab(browser, index);
      } else if (type == "agentSpace.tabs.split") {
        // Splits the ACTIVE tab with the tab at `index` (native
        // side-by-side; the sidebar shows one merged row).
        ok = framework_->SplitActiveWith(browser, index);
      } else {
        ok = framework_->Unsplit(browser, index);
      }
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.tabs.move") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id = 0;
    int from = -1, to = -1;
    if (p) {
      window_id = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      from = static_cast<int>(p->FindDouble("from").value_or(-1));
      to = static_cast<int>(p->FindDouble("to").value_or(-1));
    }
    out.Set("ok", framework_ && window_id && from >= 0 && to >= 0 &&
                      framework_->MoveTab(ResolveBrowser(window_id), from, to));
  } else if (type == "agentSpace.rows.reorder") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string guid;
    int index = -1;
    if (p) {
      if (const std::string* g = p->FindString("guid")) {
        guid = *g;
      }
      index = static_cast<int>(p->FindDouble("index").value_or(-1));
    }
    const bool ok = !guid.empty() && index >= 0 && tab_store_.Contains(guid);
    if (ok) {
      ReorderStoredRow(guid, index);
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.spaces.reorder") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string space_id;
    int index = -1;
    if (p) {
      if (const std::string* g = p->FindString("spaceId")) {
        space_id = *g;
      }
      index = static_cast<int>(p->FindDouble("index").value_or(-1));
    }
    const bool known =
        index >= 0 && std::any_of(spaces_.begin(), spaces_.end(),
                                  [&](const Space& sp) {
                                    return sp.id == space_id;
                                  });
    if (known) {
      ReorderSpace(space_id, index);
    }
    out.Set("ok", known);
  } else if (type == "agentSpace.tabs.group" ||
             type == "agentSpace.tabs.ungroup") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id = 0;
    int index = -1;
    if (p) {
      window_id = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      index = static_cast<int>(p->FindDouble("index").value_or(-1));
    }
    bool ok = false;
    if (framework_ && window_id && index >= 0) {
      const int64_t browser = ResolveBrowser(window_id);
      ok = (type == "agentSpace.tabs.group")
               ? framework_->AddTabToNewGroup(browser, index)
               : framework_->RemoveTabFromGroup(browser, index);
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.groups.list") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    const int64_t window_id =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    if (framework_ && window_id) {
      if (std::optional<base::Value> groups = base::JSONReader::Read(
              framework_->TabGroupsJson(ResolveBrowser(window_id)),
              base::JSON_PARSE_RFC)) {
        out.Set("groups", std::move(*groups));
      }
      out.Set("ok", true);
    } else {
      out.Set("ok", false);
    }
  } else if (type == "agentSpace.groups.rename" ||
             type == "agentSpace.groups.color" ||
             type == "agentSpace.groups.collapse" ||
             type == "agentSpace.groups.ungroup" ||
             type == "agentSpace.groups.close") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id = 0;
    std::string group_id, text;
    bool collapsed = false;
    if (p) {
      window_id = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      if (const std::string* g = p->FindString("groupId")) {
        group_id = *g;
      }
      if (const std::string* t = p->FindString("title")) {
        text = *t;
      }
      if (const std::string* c = p->FindString("color")) {
        text = *c;
      }
      collapsed = p->FindBool("collapsed").value_or(false);
    }
    bool ok = false;
    if (framework_ && window_id && !group_id.empty()) {
      const int64_t browser = ResolveBrowser(window_id);
      if (type == "agentSpace.groups.rename") {
        ok = framework_->RenameTabGroup(browser, group_id.c_str(),
                                        text.c_str());
      } else if (type == "agentSpace.groups.color") {
        ok = framework_->SetTabGroupColor(browser, group_id.c_str(),
                                          text.c_str());
      } else if (type == "agentSpace.groups.collapse") {
        ok = framework_->SetTabGroupCollapsed(browser, group_id.c_str(),
                                              collapsed);
      } else if (type == "agentSpace.groups.ungroup") {
        ok = framework_->UngroupTabGroup(browser, group_id.c_str());
      } else {
        ok = framework_->CloseTabGroup(browser, group_id.c_str());
      }
    }
    out.Set("ok", ok);
  } else {
    out.Set("error", "unknown type: " + type);
  }
  std::string s;
  base::JSONWriter::Write(out, &s);
  return s;
}

void PhiCoordinator::OnBrowserWindowCreated(int64_t window_id, void* hwnd) {
  LOG(INFO) << "[PhiClient] OnBrowserWindowCreated windowId=" << window_id
            << " hwnd=" << hwnd;
  // Option B: the client owns a shell window and reparents Chromium's content
  // in. The shell routes activation back through the framework.
  if (pending_slot_ != 0) {
    // A Space browser for an existing slot: attach, don't open a new shell.
    auto it = windows_.find(pending_slot_);
    if (it != windows_.end()) {
      it->second->AttachSpaceBrowser(pending_space_, window_id,
                                     static_cast<HWND>(hwnd));
      browser_to_slot_[window_id] = pending_slot_;
      pending_slot_ = 0;
      pending_space_.clear();
      return;
    }
    pending_slot_ = 0;
    pending_space_.clear();
  }
  auto controller = std::make_unique<PhiWindowController>(
      window_id, static_cast<HWND>(hwnd), framework_, this);
  controller->RefreshTabs();
  windows_[window_id] = std::move(controller);
  browser_to_slot_[window_id] = window_id;
}

HWND PhiCoordinator::ShellForFocus() const {
  HWND focus = ::GetFocus();
  if (!focus) {
    return nullptr;
  }
  HWND root = ::GetAncestor(focus, GA_ROOT);
  for (const auto& [slot, controller] : windows_) {
    if (controller->shell_hwnd() == root) {
      return root;
    }
  }
  return nullptr;
}

// static
LRESULT CALLBACK PhiCoordinator::KeyboardHookProc(int code,
                                                  WPARAM wparam,
                                                  LPARAM lparam) {
  if (code != HC_ACTION || (lparam & 0x80000000) ||  // Key-up.
      (lparam & 0x20000000)) {                       // Alt held.
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
  }
  PhiCoordinator* self = g_coordinator;
  if (!self) {
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
  }
  const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  // The menu bar's displayed shortcuts, reserved the way Chrome reserves
  // its browser shortcuts (they fire even while a page has focus). Edit
  // keys (Ctrl+C/V/X/Z/A/F...) stay with the web content.
  struct Accelerator {
    UINT vk;
    bool ctrl;
    bool shift;
    UINT command;
  };
  static constexpr Accelerator kAccelerators[] = {
      {'T', true, false, kCmdNewTab},
      {'N', true, false, kCmdNewWindow},
      {'N', true, true, kCmdNewIncognitoWindow},
      {'T', true, true, kCmdReopenClosedTab},
      {'W', true, false, kCmdCloseTab},
      {'W', true, true, kCmdCloseWindow},
      {'L', true, false, kCmdOpenLocation},
      {'R', true, false, kCmdReload},
      {'R', true, true, kCmdForceReload},
      {VK_F5, false, false, kCmdReload},
      {'D', true, false, kCmdBookmarkThisTab},
      {VK_OEM_COMMA, true, false, kCmdPreferences},
      {VK_TAB, true, false, kCmdNextTab},
      {VK_TAB, true, true, kCmdPreviousTab},
  };
  for (const Accelerator& accel : kAccelerators) {
    if (accel.vk != static_cast<UINT>(wparam) || accel.ctrl != ctrl ||
        accel.shift != shift) {
      continue;
    }
    if (HWND shell = self->ShellForFocus()) {
      ::PostMessageW(shell, WM_COMMAND, MAKEWPARAM(accel.command, 0), 0);
      return 1;  // Swallow: Chromium must not double-handle it.
    }
    break;
  }
  return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

void PhiCoordinator::OnTabsChanged(int64_t window_id) {
  auto slot_it = browser_to_slot_.find(window_id);
  const int64_t slot = slot_it != browser_to_slot_.end() ? slot_it->second
                                                         : window_id;
  auto it = windows_.find(slot);
  if (it != windows_.end()) {
    // Deferred: this fires from inside the tab-strip change it reports (a tab
    // "will be removed" is still in the strip), so the sidebar is repainted
    // once the browser state has settled.
    it->second->ScheduleRefresh();
  }
}

void PhiCoordinator::OnBrowserWindowClosing(int64_t window_id) {
  LOG(INFO) << "[PhiClient] OnBrowserWindowClosing windowId=" << window_id;
  auto slot_it = browser_to_slot_.find(window_id);
  if (slot_it == browser_to_slot_.end()) {
    windows_.erase(window_id);
    return;
  }
  const int64_t slot = slot_it->second;
  browser_to_slot_.erase(slot_it);
  auto it = windows_.find(slot);
  if (it == windows_.end()) {
    return;
  }
  // The slot survives while any Space browser remains.
  if (!it->second->ForgetBrowser(window_id)) {
    windows_.erase(it);
  }
}

}  // namespace phi_client
