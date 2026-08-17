// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_coordinator.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "base/rand_util.h"
#include "phibrowser-win/client/phi_agent_overlay.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_extensions_popover.h"
#include "phibrowser-win/client/phi_menu_bar.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_tab_switcher.h"
#include "phibrowser-win/client/phi_window_capture.h"

namespace phi_client {

namespace {
// The one coordinator, reachable from the static keyboard hook. The client
// creates exactly one for the process lifetime.
PhiCoordinator* g_coordinator = nullptr;

// Splits a tab-separated row (the URLRuleRows() encoding). Keeps empty fields
// so positional indices stay stable.
std::vector<std::string> SplitTabRow(const std::string& value) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t tab = value.find('\t', start);
    if (tab == std::string::npos) {
      parts.push_back(value.substr(start));
      break;
    }
    parts.push_back(value.substr(start, tab - start));
    start = tab + 1;
  }
  return parts;
}

// The customisable shortcuts (the Mac Shortcuts pane's editable rows), in the
// pane's display order. `name` is the stable prefs key; the chord here is the
// default the menu bar advertises. Everything NOT in this table (Quit, zoom,
// devtools, Ctrl+Tab's MRU switcher, Ctrl+1..9 Spaces...) stays fixed.
struct RebindableShortcut {
  const char* name;
  const wchar_t* group;
  const wchar_t* label;
  UINT command;
  PhiShortcutChord def;
};
constexpr RebindableShortcut kRebindableShortcuts[] = {
      {"newTab", L"File", L"New Tab", kCmdNewTab, {'T', true, false, false}},
      {"newWindow", L"File", L"New Window", kCmdNewWindow,
       {'N', true, false, false}},
      {"newIncognitoWindow", L"File", L"New Incognito Window",
       kCmdNewIncognitoWindow, {'N', true, true, false}},
      {"reopenClosedTab", L"File", L"Reopen Closed Tab", kCmdReopenClosedTab,
       {'T', true, true, false}},
      {"openLocation", L"File", L"Open Location", kCmdOpenLocation,
       {'L', true, false, false}},
      {"closeTab", L"File", L"Close Tab", kCmdCloseTab,
       {'W', true, false, false}},
      {"closeWindow", L"File", L"Close Window", kCmdCloseWindow,
       {'W', true, true, false}},
      {"copyUrl", L"Edit", L"Copy URL", kCmdCopyUrl,
       {'C', true, true, false}},
      {"reload", L"View", L"Reload This Page", kCmdReload,
       {'R', true, false, false}},
      {"forceReload", L"View", L"Force Reload This Page", kCmdForceReload,
       {'R', true, true, false}},
      {"toggleSidebar", L"View", L"Toggle Sidebar", kCmdToggleSidebar,
       {'S', true, false, false}},
      {"toggleBookmarkBar", L"View", L"Always Show Bookmark Bar",
       kCmdAlwaysShowBookmarkBar, {'B', true, true, false}},
      {"showFullHistory", L"History", L"Show Full History",
       kCmdShowFullHistory, {'H', true, false, false}},
      {"bookmarkThisTab", L"Bookmarks", L"Bookmark This Tab",
       kCmdBookmarkThisTab, {'D', true, false, false}},
      {"bookmarkAllTabs", L"Bookmarks", L"Bookmark All Tabs",
       kCmdBookmarkAllTabs, {'D', true, true, false}},
      {"downloads", L"Window", L"Downloads", kCmdDownloads,
       {'J', true, false, false}},
      {"searchTabs", L"Window", L"Search Tabs", kCmdSearchTabs,
       {'A', true, true, false}},
      {"nextSpace", L"Spaces", L"Next Space", kCmdNextSpace,
       {VK_RIGHT, true, false, true}},
      {"previousSpace", L"Spaces", L"Previous Space", kCmdPreviousSpace,
       {VK_LEFT, true, false, true}},
};
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
constexpr UINT WM_PHI_AUTHORIZE = WM_APP + 2;

// A connection-consent decision marshaled the same way as ControlCall.
struct AuthorizeCall {
  RAW_PTR_EXCLUSION const PhiAgentPeer* peer = nullptr;
  bool allowed = false;
};

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
  agent_grants_path_ = user_data_dir + L"\\PhiAgentGrants.json";
  credential_grants_path_ = user_data_dir + L"\\PhiCredentialGrants.json";
  url_rules_path_ = user_data_dir + L"\\PhiUrlRules.json";
  credentials_dir_ = user_data_dir;
  // Prefer the real vault: the out-of-process Bitwarden helper, which owns the
  // user's actual vault (and links the GPL SDK on its own side of the process
  // boundary). The local DPAPI store is loaded only as the dev/test fallback
  // for machines with no helper installed.
  // PHI_VAULT_HELPER overrides the helper binary (dev/test only — the shipped
  // path is PhiBitwardenHelper.exe beside our own exe). It names a PATH TO AN
  // EXE, never arguments, so the no-arguments spawn contract is intact.
  std::wstring helper_override;
  {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length =
        ::GetEnvironmentVariableW(L"PHI_VAULT_HELPER", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
      helper_override.assign(buffer, length);
    }
  }
  if (!vault_helper_.Start(helper_override)) {
    LOG(INFO) << "[PhiVault] no helper installed — using the local dev vault";
    credentials_.Load(user_data_dir);
  }
  LoadSpaces();
  LoadUrlRules();
  LoadLayoutMode();
  LoadTheme();
  LoadPrefs();
  LoadAgentGrants();
  LoadCredentialGrants();
  PushRestorePreference();  // Into the loaded profiles' startup prefs.
  tab_store_.Load(user_data_dir + L"\\PhiTabData.json");
  PushSpaceMenu();  // The page context menu's "Open Link in Space" list.

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
    if (self && wparam == 2) {
      self->SweepAgentSpaces();  // Agent-space keep-alive sweep (10s).
      return 0;
    }
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
  if (msg == WM_PHI_AUTHORIZE) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    auto* call = reinterpret_cast<AuthorizeCall*>(lparam);
    if (self && call && call->peer) {
      call->allowed = self->AuthorizeAgentPeerOnUi(*call->peer);
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
  if (type == "agentSpace.profiles.create" && profile_create_pending_) {
    // Creation is under way on the UI thread; wait for the basename HERE (an
    // I/O thread) and fold it into the reply, so the skill's
    // `const {profileId} = await createProfile(...)` holds.
    const DWORD waited =
        ::WaitForSingleObject(profile_create_done_, 15000);
    profile_create_pending_ = false;
    if (waited == WAIT_OBJECT_0 && !created_profile_id_.empty()) {
      std::optional<base::DictValue> body =
          base::JSONReader::ReadDict(call.result, base::JSON_PARSE_RFC);
      if (body) {
        body->Set("profileId", created_profile_id_);
        std::string patched;
        base::JSONWriter::Write(*body, &patched);
        return patched;
      }
    }
  }
  return call.result;
}

void PhiCoordinator::OnProfileCreated(const char* profile_id) {
  created_profile_id_ = profile_id ? profile_id : "";
  if (profile_create_done_) {
    ::SetEvent(profile_create_done_);  // Releases the waiting I/O thread.
  }
}

bool PhiCoordinator::AuthorizeAgentPeer(const PhiAgentPeer& peer) {
  // I/O thread; the decision (and any consent dialog) runs on the UI thread.
  if (!msg_hwnd_) {
    return false;
  }
  AuthorizeCall call;
  call.peer = &peer;
  ::SendMessageW(msg_hwnd_, WM_PHI_AUTHORIZE, 0,
                 reinterpret_cast<LPARAM>(&call));
  return call.allowed;
}

bool PhiCoordinator::AuthorizeAgentPeerOnUi(const PhiAgentPeer& peer) {
  // An unidentifiable peer (no pid, or the process died before it could be
  // opened) is refused outright: consent must name WHO is asking.
  if (peer.image_path.empty()) {
    LOG(WARNING) << "[PhiAgent] refused unidentified pipe peer (pid "
                 << peer.pid << ")";
    return false;
  }
  std::wstring key = peer.image_path;
  for (wchar_t& c : key) {
    c = towlower(c);
  }
  if (agent_grants_.count(key) || session_allowed_peers_.count(key)) {
    return true;
  }
  if (session_denied_peers_.count(key)) {
    return false;  // Asked and denied this run — no dialog spam per request.
  }
  // The consent dialog — the Mac's AgentAccessApprovalAlert. Owned by a
  // shell window when one exists; a MessageBox fallback (allow-once only)
  // covers the pre-window startup connection.
  const std::wstring message =
      peer.image_path + L"\n(process " + std::to_wstring(peer.pid) +
      L") wants to control Phi — tabs, navigation, and page content.";
  HWND owner = nullptr;
  for (const auto& [slot, controller] : windows_) {
    if (controller->shell_hwnd()) {
      owner = controller->shell_hwnd();
      break;
    }
  }
  int chosen = -1;
  if (owner) {
    chosen = PhiPromptChoose(owner, L"Agent access request", message.c_str(),
                             {L"Allow Once", L"Always Allow", L"Deny"}, 0);
  } else {
    chosen = ::MessageBoxW(nullptr,
                           (message + L"\n\nAllow this connection?").c_str(),
                           L"Phi — Agent access request",
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                               MB_SETFOREGROUND) == IDYES
                 ? 0
                 : 2;
  }
  if (chosen == 0) {
    session_allowed_peers_.insert(key);
    return true;
  }
  if (chosen == 1) {
    agent_grants_.insert(key);
    SaveAgentGrants();
    PhiSettingsWindow::Refresh();
    return true;
  }
  session_denied_peers_.insert(key);
  return false;
}

std::vector<std::string> PhiCoordinator::AgentGrantRows() const {
  std::vector<std::string> rows;
  for (const std::wstring& path : agent_grants_) {
    rows.push_back(base::WideToUTF8(path));
  }
  return rows;
}

void PhiCoordinator::RemoveAgentGrant(int index) {
  if (index < 0 || index >= static_cast<int>(agent_grants_.size())) {
    return;
  }
  auto it = agent_grants_.begin();
  std::advance(it, index);
  agent_grants_.erase(it);
  SaveAgentGrants();
}

std::vector<std::string> PhiCoordinator::CredentialGrantRows() const {
  std::vector<std::string> rows;
  for (const auto& [scope, kind] : credential_grants_) {
    rows.push_back(scope + "\t" +
                   (kind == CredentialAccessKind::kReveal ? "reveal" : "fill"));
  }
  return rows;
}

void PhiCoordinator::RemoveCredentialGrant(int index) {
  if (index < 0 || index >= static_cast<int>(credential_grants_.size())) {
    return;
  }
  auto it = credential_grants_.begin();
  std::advance(it, index);
  // Drop any live timed grant for the same scope too — revoking must not
  // leave a ten-minute window standing behind the removed row.
  credential_timed_grants_.erase(it->first);
  credential_grants_.erase(it);
  SaveCredentialGrants();
}

std::vector<std::string> PhiCoordinator::CredentialAuditRows() const {
  return credential_audit_;
}

void PhiCoordinator::LoadAgentGrants() {
  HANDLE f = ::CreateFileW(agent_grants_path_.c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
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
  if (const base::ListValue* always = root->FindList("always")) {
    for (const base::Value& v : *always) {
      if (v.is_string()) {
        std::wstring key = base::UTF8ToWide(v.GetString());
        for (wchar_t& c : key) {
          c = towlower(c);
        }
        agent_grants_.insert(std::move(key));
      }
    }
  }
}

void PhiCoordinator::SaveAgentGrants() const {
  base::DictValue root;
  base::ListValue always;
  for (const std::wstring& path : agent_grants_) {
    always.Append(base::WideToUTF8(path));
  }
  root.Set("always", std::move(always));
  std::string json;
  base::JSONWriter::Write(root, &json);
  HANDLE f = ::CreateFileW(agent_grants_path_.c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD wrote = 0;
  ::WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &wrote,
              nullptr);
  ::CloseHandle(f);
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
    const std::string* color = d.FindString("colorHex");
    loaded.push_back({*id, name ? *name : ("Space " + *id),
                      profile && !profile->empty() ? *profile : "Default",
                      icon && PhiIconIsValid(*icon) ? *icon
                                                    : kPhiDefaultSpaceIcon,
                      theme ? *theme : std::string(),
                      d.FindInt("saturation").value_or(-1),
                      color ? *color : std::string()});
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
    d.Set("colorHex", space.color_hex);
    list.Append(std::move(d));
  }
  base::DictValue root;
  root.Set("spaces", std::move(list));
  std::string json;
  base::JSONWriter::Write(root, &json);

  HANDLE f = ::CreateFileW(spaces_path_.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD wrote = 0;
    ::WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &wrote,
                nullptr);
    ::CloseHandle(f);
  }
  // Every Space-list change reaches here; keep the page context menu's
  // "Open Link in Space" list in step.
  PushSpaceMenu();
}

void PhiCoordinator::PushSpaceMenu() const {
  if (!framework_) {
    return;
  }
  base::ListValue list;
  for (const Space& space : spaces_) {
    base::DictValue d;
    d.Set("id", space.id);
    d.Set("name", space.name);
    // The session id of the Space's open browser (any slot), so the menu
    // can exclude the Space the user is already looking at.
    int64_t browser_id = 0;
    for (const auto& [slot, controller] : windows_) {
      browser_id = controller->BrowserIdForSpace(space.id);
      if (browser_id) {
        break;
      }
    }
    d.Set("windowId", static_cast<double>(browser_id));
    list.Append(std::move(d));
  }
  std::string json;
  base::JSONWriter::Write(list, &json);
  framework_->SetSpaceMenu(json.c_str());
  // The routing table shares this trigger set (rules edits add one more),
  // the Mac's pushSpaceStateToChromium coupling.
  PushRoutingTable();
}

void PhiCoordinator::PushRoutingTable() const {
  if (!framework_) {
    return;
  }
  base::DictValue root;
  base::ListValue rules;
  int sort_order = 0;
  for (const UrlRule& rule : url_rules_) {
    // Incognito rules are stored and listed but stay OUT of the live router
    // until the client grows Incognito Spaces — a match would cancel the
    // navigation with nowhere to send it.
    if (rule.space_id == "space.incognito") {
      continue;
    }
    base::DictValue d;
    d.Set("spaceId", rule.space_id);
    d.Set("host", rule.host);
    if (!rule.path_prefix.empty()) {
      d.Set("pathPrefix", rule.path_prefix);
    }
    d.Set("ask", rule.ask);
    d.Set("sortOrder", sort_order++);
    rules.Append(std::move(d));
  }
  root.Set("rules", std::move(rules));
  // Each Space's VISIBLE browser only — a Space hidden behind a slot's
  // active sibling is deliberately absent, so a match on it resolves to
  // OnRouteURLToSpace (which re-enters ShowSpace) instead of a direct open
  // into an off-screen browser.
  base::DictValue map;
  for (const auto& [slot, controller] : windows_) {
    const std::string& space_id = controller->active_space();
    const int64_t browser_id = controller->active_browser();
    if (space_id.empty() || !browser_id || map.Find(space_id)) {
      continue;
    }
    map.Set(space_id, static_cast<double>(browser_id));
  }
  root.Set("spaceWindows", std::move(map));
  std::string json;
  base::JSONWriter::Write(root, &json);
  framework_->SetSpaceRoutingTable(json.c_str());
}

void PhiCoordinator::LoadUrlRules() {
  HANDLE f = ::CreateFileW(url_rules_path_.c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;  // First run: no rules.
  }
  std::string json;
  char buf[4096];
  DWORD read = 0;
  while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
    json.append(buf, read);
  }
  ::CloseHandle(f);
  std::optional<base::Value> root =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!root || !root->is_list()) {
    return;
  }
  url_rules_.clear();
  for (const base::Value& v : root->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    if (!d) {
      continue;
    }
    const std::string* space_id = d->FindString("spaceId");
    const std::string* host = d->FindString("host");
    if (!space_id || space_id->empty() || !host || host->empty()) {
      continue;
    }
    UrlRule rule;
    rule.space_id = *space_id;
    rule.host = *host;
    if (const std::string* prefix = d->FindString("pathPrefix")) {
      rule.path_prefix = *prefix;
    }
    rule.ask = d->FindBool("ask").value_or(false);
    url_rules_.push_back(std::move(rule));
  }
}

void PhiCoordinator::SaveUrlRules() const {
  base::ListValue list;
  for (const UrlRule& rule : url_rules_) {
    base::DictValue d;
    d.Set("spaceId", rule.space_id);
    d.Set("host", rule.host);
    if (!rule.path_prefix.empty()) {
      d.Set("pathPrefix", rule.path_prefix);
    }
    d.Set("ask", rule.ask);
    list.Append(std::move(d));
  }
  std::string json;
  base::JSONWriter::Write(list, &json);
  HANDLE f = ::CreateFileW(url_rules_path_.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD wrote = 0;
    ::WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &wrote,
                nullptr);
    ::CloseHandle(f);
  }
}

void PhiCoordinator::OnOpenLinkInSpace(int64_t source_window_id,
                                       const char* space_id_c,
                                       const char* url_c) {
  const std::string space_id = space_id_c ? space_id_c : "";
  const std::string url = url_c ? url_c : "";
  if (space_id.empty() || url.empty() || !framework_) {
    return;
  }
  RouteUrlToSpace(space_id, url, source_window_id);
}

void PhiCoordinator::OnRouteURLToSpace(const char* space_id_c,
                                       const char* url_c,
                                       int64_t source_window_id) {
  // A silent URL rule matched but the target Space had no visible browser;
  // the navigation is already cancelled, so surface the Space and re-open
  // the URL there (never silently dropped).
  const std::string space_id = space_id_c ? space_id_c : "";
  const std::string url = url_c ? url_c : "";
  if (space_id.empty() || url.empty() || !framework_) {
    return;
  }
  RouteUrlToSpace(space_id, url, source_window_id);
}

void PhiCoordinator::OnAskSpaceForURL(const char* url_c,
                                      const char* default_space_id_c,
                                      int64_t source_window_id,
                                      bool source_is_new_tab) {
  const std::string url = url_c ? url_c : "";
  const std::string default_space_id =
      default_space_id_c ? default_space_id_c : "";
  if (url.empty() || !framework_ || spaces_.empty()) {
    return;
  }
  // The source slot's shell owns the (modal) chooser; its active Space is
  // the "keep here" target.
  auto slot_it = browser_to_slot_.find(source_window_id);
  const int64_t slot = slot_it != browser_to_slot_.end() ? slot_it->second : 0;
  auto window_it = windows_.find(slot);
  PhiWindowController* controller =
      window_it != windows_.end() ? window_it->second.get() : nullptr;
  const std::string current_space_id =
      controller ? controller->active_space() : std::string();

  std::vector<std::wstring> options;
  int default_index = 0;
  for (size_t i = 0; i < spaces_.size(); ++i) {
    std::wstring label = base::UTF8ToWide(spaces_[i].name);
    if (spaces_[i].id == current_space_id) {
      label += L"  (current)";
    }
    options.push_back(std::move(label));
    if (spaces_[i].id == default_space_id) {
      default_index = static_cast<int>(i);
    }
  }
  const int chosen = PhiPromptChoose(
      controller ? controller->shell_hwnd() : nullptr, L"Open in Space",
      base::UTF8ToWide(url).c_str(), options, default_index);
  const std::string chosen_space =
      chosen >= 0 && chosen < static_cast<int>(spaces_.size())
          ? spaces_[chosen].id
          : std::string();
  if (chosen_space.empty() || chosen_space == current_space_id) {
    // Keep it here. The cancelled navigation was replacing the active tab
    // when it started from a fresh new tab — re-issue it in place there;
    // otherwise open a fresh foreground tab (the Mac split). Both bypass
    // routing so the same rule cannot re-prompt.
    if (source_is_new_tab) {
      framework_->NavigateActiveTabBypassingSpaceRouting(source_window_id,
                                                         url.c_str());
    } else {
      framework_->OpenTabBypassingSpaceRouting(source_window_id, url.c_str());
    }
    if (controller) {
      controller->RefreshTabs();
    }
    return;
  }
  RouteUrlToSpace(chosen_space, url, source_window_id);
}

void PhiCoordinator::RouteUrlToSpace(const std::string& space_id,
                                     const std::string& url,
                                     int64_t source_window_id) {
  // A slot that already holds the Space: switch to it there and open the
  // link (the Mac activates the Space's window and opens the tab). The open
  // bypasses URL routing — the URL may be the very navigation a rule just
  // re-homed, and re-matching it would loop.
  for (const auto& [slot, controller] : windows_) {
    if (!controller->HasSpace(space_id)) {
      continue;
    }
    controller->ShowSpace(space_id);
    framework_->OpenTabBypassingSpaceRouting(controller->active_browser(),
                                             url.c_str());
    if (controller->shell_hwnd()) {
      ::SetForegroundWindow(controller->shell_hwnd());
    }
    controller->RefreshTabs();
    PushSpaceMenu();  // The visible-Space routing map changed.
    return;
  }
  // Cold Space: create its browser in its profile; the pending attach opens
  // the link once the browser arrives (the spaces.switch machinery).
  auto space_it =
      std::find_if(spaces_.begin(), spaces_.end(),
                   [&](const Space& s) { return s.id == space_id; });
  if (space_it == spaces_.end()) {
    return;
  }
  auto slot_it = browser_to_slot_.find(source_window_id);
  int64_t slot = slot_it != browser_to_slot_.end() ? slot_it->second : 0;
  if (!slot && !windows_.empty()) {
    slot = windows_.begin()->first;
  }
  if (!slot) {
    return;
  }
  pending_slot_ = slot;
  pending_space_ = space_id;
  pending_open_url_ = url;
  framework_->CreateSpaceBrowserInProfile(space_it->profile_id.c_str());
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
  view.url_b = row.url_b;
  view.title_b = row.title_b;
  view.partner_guid = row.split_partner_guid;
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
  // The menu path: pinning MOVES the tab into the cell (the Mac's
  // togglePin binds); the bookmark item is an unbound copy
  // (addTabToRootBookmarks).
  StoreTabInternal(slot, index, as_bookmark, /*insert_index=*/-1,
                   /*bind=*/!as_bookmark);
}

void PhiCoordinator::RequestDropTab(int64_t slot, int index, bool as_bookmark,
                                    int insert_index) {
  // The drag path always converts — the tab moves into the row it becomes.
  StoreTabInternal(slot, index, as_bookmark, insert_index, /*bind=*/true);
}

void PhiCoordinator::StoreTabInternal(int64_t slot, int index,
                                      bool as_bookmark, int insert_index,
                                      bool bind) {
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
    if (insert_index >= 0) {
      tab_store_.Reorder(guid, insert_index);
    }
    if (bind) {
      // The Mac's updateTabCustomValue: the live tab carries the row's
      // guid and moves into its cell.
      framework_->SetTabTag(browser, index, guid.c_str());
    }
    for (const auto& [s2, controller] : windows_) {
      controller->RefreshTabs();
    }
  }
}

void PhiCoordinator::RequestStoreSplit(int64_t slot, int index_a, int index_b,
                                       bool as_bookmark) {
  // The menu path: a split bookmark is an unbound copy (bindLiveSplit
  // false); a pinned pair always binds.
  StoreSplitInternal(slot, index_a, index_b, as_bookmark,
                     /*insert_index=*/-1, /*bind_bookmark=*/false);
}

void PhiCoordinator::RequestDropSplit(int64_t slot, int index_a, int index_b,
                                      bool as_bookmark, int insert_index) {
  // The drag path binds either way (the Mac's bindLiveSplit: true).
  StoreSplitInternal(slot, index_a, index_b, as_bookmark, insert_index,
                     /*bind_bookmark=*/true);
}

void PhiCoordinator::StoreSplitInternal(int64_t slot, int index_a,
                                        int index_b, bool as_bookmark,
                                        int insert_index,
                                        bool bind_bookmark) {
  std::string profile_id, space_id;
  if (!framework_ || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return;
  }
  const int64_t browser = ResolveBrowser(slot);
  struct Pane {
    std::string title, url, favicon;
    int favicon_width = 0, favicon_height = 0;
  };
  const auto capture = [&](int index, Pane* pane) {
    std::optional<base::Value> tabs = base::JSONReader::Read(
        framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
    if (tabs && tabs->is_list()) {
      for (const base::Value& v : tabs->GetList()) {
        if (v.is_dict() &&
            v.GetDict().FindInt("index").value_or(-1) == index) {
          if (const std::string* t = v.GetDict().FindString("title")) {
            pane->title = *t;
          }
          if (const std::string* u = v.GetDict().FindString("url")) {
            pane->url = *u;
          }
          break;
        }
      }
    }
    int width = 0, height = 0;
    const unsigned char* pixels =
        framework_->TabFaviconBgra(browser, index, &width, &height);
    if (pixels && width > 0 && height > 0) {
      const size_t bytes =
          static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
      // SAFETY: TabFaviconBgra reports the buffer's own dimensions.
      const auto view =
          UNSAFE_BUFFERS(base::span<const uint8_t>(pixels, bytes));
      pane->favicon = base::Base64Encode(view);
      pane->favicon_width = width;
      pane->favicon_height = height;
    }
  };
  Pane a, b;
  capture(index_a, &a);
  capture(index_b, &b);
  if (a.url.empty() || b.url.empty()) {
    return;
  }
  if (as_bookmark) {
    // The Mac's addSplitBookmarkFromTab: ONE row keyed on the primary
    // pane, the second pane riding secondaryUrl/-Title (suppressed when it
    // duplicates the primary's). The drag path binds the live split to the
    // row (bindLiveSplit); the menu leaves it an independent copy.
    const std::string guid = tab_store_.AddBookmark(
        profile_id, space_id, std::string(), a.title, a.url,
        /*folder=*/false, a.favicon, a.favicon_width, a.favicon_height);
    if (!guid.empty()) {
      tab_store_.SetRowSecondary(guid, b.url,
                                 b.title == a.title ? std::string()
                                                    : b.title);
      if (insert_index >= 0) {
        tab_store_.Reorder(guid, insert_index);
      }
      if (bind_bookmark) {
        framework_->SetTabTag(browser, index_a, guid.c_str());
      }
    }
  } else {
    // The Mac's pinSplit: TWO pinned rows (primary first, so it renders as
    // the left pane), cross-linked, with BOTH live panes bound by tag.
    const std::string guid_a =
        tab_store_.AddPinned(profile_id, space_id, a.title, a.url, a.favicon,
                             a.favicon_width, a.favicon_height);
    const std::string guid_b =
        tab_store_.AddPinned(profile_id, space_id, b.title, b.url, b.favicon,
                             b.favicon_width, b.favicon_height);
    if (!guid_a.empty() && !guid_b.empty()) {
      tab_store_.LinkSplitPair(guid_a, guid_b);
      if (insert_index >= 0) {
        tab_store_.Reorder(guid_a, insert_index);
        tab_store_.Reorder(guid_b, insert_index + 1);
      }
      framework_->SetTabTag(browser, index_a, guid_a.c_str());
      framework_->SetTabTag(browser, index_b, guid_b.c_str());
    }
  }
  for (const auto& [s2, controller] : windows_) {
    controller->RefreshTabs();
  }
}

void PhiCoordinator::RemoveStoredRow(const std::string& guid) {
  // A pinned-split member takes its partner row (the store removes both);
  // capture the partner guid FIRST so its tag can be cleared too.
  const std::string partner = tab_store_.SplitPartner(guid);
  if (tab_store_.Remove(guid)) {
    // Clear the identity tag from any tab bound to the removed row(s) — it
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
          if (tag && (*tag == guid ||
                      (!partner.empty() && *tag == partner))) {
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

bool PhiCoordinator::RenameStoredRow(const std::string& guid,
                                     const std::string& name) {
  if (!tab_store_.RenameRow(guid, name)) {
    return false;
  }
  for (const auto& [slot, controller] : windows_) {
    controller->RefreshTabs();
  }
  PhiSettingsWindow::Refresh();
  return true;
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

void PhiCoordinator::MoveStoredRow(const std::string& guid,
                                   const std::string& new_parent_guid,
                                   int new_index) {
  // The store refuses non-bookmarks, foreign parents, and cycles.
  if (tab_store_.Move(guid, new_parent_guid, new_index)) {
    for (const auto& [slot, controller] : windows_) {
      controller->RefreshTabs();
    }
    PhiSettingsWindow::Refresh();
  }
}

std::string PhiCoordinator::AddBookmarkFolder(int64_t slot,
                                              const std::string& parent_guid,
                                              const std::string& title) {
  std::string profile_id, space_id;
  if (title.empty() || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return std::string();
  }
  const std::string guid = tab_store_.AddBookmark(
      profile_id, space_id, parent_guid, title, std::string(),
      /*folder=*/true, std::string(), 0, 0);
  if (!guid.empty()) {
    for (const auto& [s2, controller] : windows_) {
      controller->RefreshTabs();
    }
    PhiSettingsWindow::Refresh();
  }
  return guid;
}

std::string PhiCoordinator::AddBookmarkRow(int64_t slot,
                                           const std::string& parent_guid,
                                           const std::string& title,
                                           const std::string& url) {
  std::string profile_id, space_id;
  if (url.empty() || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return std::string();
  }
  const std::string guid = tab_store_.AddBookmark(
      profile_id, space_id, parent_guid, !title.empty() ? title : url, url,
      /*folder=*/false, std::string(), 0, 0);
  if (!guid.empty()) {
    for (const auto& [s2, controller] : windows_) {
      controller->RefreshTabs();
    }
    PhiSettingsWindow::Refresh();
  }
  return guid;
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
      restore_last_session_ =
          root->FindBool("restoreLastSession").value_or(restore_last_session_);
      always_show_bookmark_bar_ = root->FindBool("alwaysShowBookmarkBar")
                                      .value_or(always_show_bookmark_bar_);
      bookmark_bar_on_new_tab_ = root->FindBool("bookmarkBarOnNewTab")
                                     .value_or(bookmark_bar_on_new_tab_);
      developer_mode_ =
          root->FindBool("developerMode").value_or(developer_mode_);
      if (const std::string* behavior = root->FindString("newTabBehavior")) {
        new_tab_behavior_ = *behavior == "omnibox"
                                ? PhiNewTabBehavior::kOmnibox
                                : PhiNewTabBehavior::kNewTabPage;
      }
      if (const base::DictValue* shortcuts = root->FindDict("shortcuts")) {
        for (const auto [name, value] : *shortcuts) {
          const base::DictValue* chord = value.GetIfDict();
          if (!chord) {
            continue;
          }
          shortcut_overrides_[name] = {
              static_cast<UINT>(chord->FindInt("vk").value_or(0)),
              chord->FindBool("ctrl").value_or(false),
              chord->FindBool("shift").value_or(false),
              chord->FindBool("alt").value_or(false)};
        }
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
  root.Set("restoreLastSession", restore_last_session_);
  root.Set("alwaysShowBookmarkBar", always_show_bookmark_bar_);
  root.Set("bookmarkBarOnNewTab", bookmark_bar_on_new_tab_);
  root.Set("developerMode", developer_mode_);
  root.Set("newTabBehavior", new_tab_behavior_ == PhiNewTabBehavior::kOmnibox
                                 ? "omnibox"
                                 : "newTabPage");
  if (!shortcut_overrides_.empty()) {
    base::DictValue shortcuts;
    for (const auto& [name, chord] : shortcut_overrides_) {
      base::DictValue entry;
      entry.Set("vk", static_cast<int>(chord.vk));
      entry.Set("ctrl", chord.ctrl);
      entry.Set("shift", chord.shift);
      entry.Set("alt", chord.alt);
      shortcuts.Set(name, std::move(entry));
    }
    root.Set("shortcuts", std::move(shortcuts));
  }
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

bool PhiCoordinator::NewTabOpensOmnibox() const {
  return new_tab_behavior_ == PhiNewTabBehavior::kOmnibox;
}

void PhiCoordinator::SetNewTabBehavior(PhiNewTabBehavior behavior) {
  if (behavior == new_tab_behavior_) {
    return;
  }
  new_tab_behavior_ = behavior;
  SavePrefs();
  PhiSettingsWindow::Refresh();
}

PhiShortcutChord PhiCoordinator::EffectiveChord(const std::string& name) const {
  auto it = shortcut_overrides_.find(name);
  if (it != shortcut_overrides_.end()) {
    return it->second;
  }
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (name == def.name) {
      return def.def;
    }
  }
  return {};
}

UINT PhiCoordinator::MatchRebindableChord(UINT vk,
                                          bool ctrl,
                                          bool shift,
                                          bool alt) const {
  const PhiShortcutChord pressed{vk, ctrl, shift, alt};
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    const PhiShortcutChord chord = EffectiveChord(def.name);
    if (chord.vk && chord == pressed) {
      return def.command;
    }
  }
  return 0;
}

std::vector<std::string> PhiCoordinator::ShortcutRows() const {
  std::vector<std::string> rows;
  const auto fixed = [&](const char* group, const char* label,
                         const char* keys) {
    rows.push_back(std::string(group) + "\t" + label + "\t" + keys + "\t\t0");
  };
  const auto rebindable = [&](const RebindableShortcut& def) {
    rows.push_back(base::WideToUTF8(def.group) + "\t" +
                   base::WideToUTF8(def.label) + "\t" +
                   base::WideToUTF8(PhiFormatChord(EffectiveChord(def.name))) +
                   "\t" + def.name + "\t" +
                   (shortcut_overrides_.count(def.name) ? "1" : "0"));
  };
  // The pane's groups in Mac order; a group's rebindable rows first, then its
  // fixed, informational ones (chords the hook special-cases or that other
  // machinery advertises — shown but not editable).
  fixed("App", "Preferences", "Ctrl+,");
  fixed("App", "Quit Phi", "Ctrl+Shift+Q");
  const std::array<const wchar_t*, 8> kGroups = {
      L"File", L"Edit", L"View", L"History", L"Bookmarks",
      L"Window", L"Tab", L"Spaces"};
  for (const wchar_t* group : kGroups) {
    for (const RebindableShortcut& def : kRebindableShortcuts) {
      if (std::wstring(def.group) == group) {
        rebindable(def);
      }
    }
    if (std::wstring(group) == L"History") {
      fixed("History", "Back", "Alt+Left");
      fixed("History", "Forward", "Alt+Right");
    } else if (std::wstring(group) == L"Tab") {
      fixed("Tab", "Select Next Tab", "Ctrl+Tab");
      fixed("Tab", "Select Previous Tab", "Ctrl+Shift+Tab");
    }
  }
  return rows;
}

void PhiCoordinator::RebindShortcut(const std::string& name,
                                    UINT vk,
                                    bool ctrl,
                                    bool shift,
                                    bool alt) {
  const RebindableShortcut* target = nullptr;
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (name == def.name) {
      target = &def;
      break;
    }
  }
  if (!target || !vk) {
    return;
  }
  const PhiShortcutChord chord{vk, ctrl, shift, alt};
  // Steal the chord from whichever rebindable currently answers to it — the
  // stolen shortcut goes UNBOUND (vk 0) until given a new chord or restored.
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (name != def.name && EffectiveChord(def.name) == chord) {
      shortcut_overrides_[def.name] = {};
    }
  }
  // Rebinding back to the default is a restore, not an override.
  if (chord == target->def) {
    shortcut_overrides_.erase(name);
  } else {
    shortcut_overrides_[name] = chord;
  }
  SavePrefs();
}

void PhiCoordinator::RestoreAllShortcuts() {
  if (shortcut_overrides_.empty()) {
    return;
  }
  shortcut_overrides_.clear();
  SavePrefs();
}

bool PhiCoordinator::AnyShortcutCustomized() const {
  return !shortcut_overrides_.empty();
}

void PhiCoordinator::FocusWindowForBrowser(int64_t browser_window_id) {
  auto slot_it = browser_to_slot_.find(browser_window_id);
  if (slot_it == browser_to_slot_.end()) {
    return;
  }
  auto it = windows_.find(slot_it->second);
  if (it == windows_.end()) {
    return;
  }
  PhiWindowController* controller = it->second.get();
  if (controller->active_browser() != browser_window_id) {
    // The browser backs a HIDDEN Space of that slot — surface it first.
    for (const std::string& space : controller->space_order()) {
      if (controller->BrowserIdForSpace(space) == browser_window_id) {
        controller->ShowSpace(space);
        break;
      }
    }
  }
  if (controller->shell_hwnd()) {
    ::SetForegroundWindow(controller->shell_hwnd());
  }
  controller->RefreshTabs();
}

void PhiCoordinator::RequestTabToSpace(int64_t slot,
                                       int index,
                                       const std::string& space_id,
                                       bool move) {
  auto it = windows_.find(slot);
  if (it == windows_.end() || !framework_) {
    return;
  }
  PhiWindowController* source = it->second.get();
  if (source->active_space() == space_id) {
    return;
  }
  const int64_t source_browser =
      source->active_browser() ? source->active_browser() : slot;
  // The tab's URL — the clone and cold-Space paths re-open it.
  std::string url;
  std::optional<base::Value> tabs = base::JSONReader::Read(
      framework_->ListTabsJson(source_browser), base::JSON_PARSE_RFC);
  if (tabs && tabs->is_list()) {
    for (const base::Value& v : tabs->GetList()) {
      const base::DictValue* d = v.GetIfDict();
      if (d && d->FindInt("index").value_or(-1) == index) {
        if (const std::string* u = d->FindString("url")) {
          url = *u;
        }
        break;
      }
    }
  }
  // An open target Space (any slot) gets the real move.
  int64_t target_browser = 0;
  for (const auto& [other_slot, controller] : windows_) {
    if (controller->is_agent() || controller->is_incognito()) {
      continue;
    }
    if (const int64_t id = controller->BrowserIdForSpace(space_id)) {
      target_browser = id;
      break;
    }
  }
  if (move && target_browser) {
    if (framework_->MoveTabToWindow(source_browser, index, target_browser)) {
      FocusWindowForBrowser(target_browser);
      source->RefreshTabs();
    }
    return;
  }
  if (url.empty()) {
    return;
  }
  if (target_browser) {
    framework_->OpenTabBypassingSpaceRouting(target_browser, url.c_str());
    FocusWindowForBrowser(target_browser);
  } else {
    // Cold Space: surface it and open there (the routed-URL tail).
    RouteUrlToSpace(space_id, url, source_browser);
  }
  if (move) {
    framework_->CloseTab(source_browser, index);
  }
  source->RefreshTabs();
}

bool PhiCoordinator::RestoreLastSession() const {
  return restore_last_session_;
}

void PhiCoordinator::SetRestoreLastSession(bool enabled) {
  if (enabled == restore_last_session_) {
    return;
  }
  restore_last_session_ = enabled;
  SavePrefs();
  PushRestorePreference();
  PhiSettingsWindow::Refresh();
}

void PhiCoordinator::PushRestorePreference() {
  // Every LOADED profile takes the app-wide choice; profiles loading later
  // get it pushed again from the paths that load them.
  if (!framework_) {
    return;
  }
  std::optional<base::Value> list = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!list || !list->is_list()) {
    return;
  }
  for (const base::Value& v : list->GetList()) {
    if (!v.is_dict()) {
      continue;
    }
    const std::string* id = v.GetDict().FindString("id");
    if (id && v.GetDict().FindBool("loaded").value_or(false)) {
      framework_->SetRestoreOnStartup(id->c_str(), restore_last_session_);
    }
  }
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
  // delete-confirmation promise) — and its URL rules, which would otherwise
  // route into a Space that no longer exists.
  tab_store_.RemoveForSpace(space_id);
  std::erase_if(url_rules_,
                [&](const UrlRule& r) { return r.space_id == space_id; });
  ++url_rules_generation_;  // Wire rule ids regenerate on every write.
  SaveUrlRules();
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
  // Refuse an id the framework does not know — the rename callback is
  // fire-and-forget, so the check has to happen here.
  std::optional<base::Value> profiles = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  bool known = false;
  if (profiles && profiles->is_list()) {
    for (const base::Value& v : profiles->GetList()) {
      const base::DictValue* d = v.GetIfDict();
      const std::string* id = d ? d->FindString("id") : nullptr;
      if (id && *id == profile_id) {
        known = true;
        break;
      }
    }
  }
  if (!known) {
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

void PhiCoordinator::OpenBrowserPage(const std::string& url) {
  if (!windows_.empty()) {
    RequestOpen(windows_.begin()->first, url);
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

std::vector<std::string> PhiCoordinator::SearchEngines(
    const std::string& profile_id) const {
  std::vector<std::string> out;
  if (!framework_) {
    return out;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_->ListSearchEnginesJson(profile_id.c_str()),
      base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return out;
  }
  for (const base::Value& value : parsed->GetList()) {
    if (!value.is_dict()) {
      continue;
    }
    const std::string* keyword = value.GetDict().FindString("keyword");
    const std::string* name = value.GetDict().FindString("name");
    if (!keyword) {
      continue;
    }
    out.push_back(*keyword + "\t" + (name && !name->empty() ? *name
                                                            : *keyword) +
                  "\t" +
                  (value.GetDict().FindBool("default").value_or(false)
                       ? "1"
                       : "0"));
  }
  return out;
}

bool PhiCoordinator::SetSearchEngine(const std::string& profile_id,
                                     const std::string& keyword) {
  return framework_ &&
         framework_->SetDefaultSearchEngine(profile_id.c_str(),
                                            keyword.c_str());
}

std::string PhiCoordinator::DownloadDirectory(
    const std::string& profile_id) const {
  return framework_ ? framework_->DownloadDirectoryUtf8(profile_id.c_str())
                    : std::string();
}

bool PhiCoordinator::SetDownloadDirectory(const std::string& profile_id,
                                          const std::string& dir) {
  return framework_ &&
         framework_->SetDownloadDirectory(profile_id.c_str(), dir.c_str());
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

std::vector<std::string> PhiCoordinator::URLRuleRows() const {
  std::vector<std::string> rows;
  rows.reserve(url_rules_.size());
  for (const UrlRule& rule : url_rules_) {
    rows.push_back(rule.space_id + "\t" + rule.host + "\t" +
                   rule.path_prefix + "\t" + (rule.ask ? "1" : "0"));
  }
  return rows;
}

void PhiCoordinator::AddURLRule(const std::string& space_id,
                                const std::string& host,
                                const std::string& path_prefix,
                                bool ask) {
  if (space_id.empty() || host.empty()) {
    return;
  }
  url_rules_.push_back({space_id, host, path_prefix, ask});
  ++url_rules_generation_;  // Wire rule ids regenerate on every write.
  SaveUrlRules();
  PushRoutingTable();
}

void PhiCoordinator::SetURLRulePattern(int index,
                                       const std::string& host,
                                       const std::string& path_prefix) {
  if (index < 0 || index >= static_cast<int>(url_rules_.size()) ||
      host.empty()) {
    return;
  }
  url_rules_[static_cast<size_t>(index)].host = host;
  url_rules_[static_cast<size_t>(index)].path_prefix = path_prefix;
  ++url_rules_generation_;  // Wire rule ids regenerate on every write.
  SaveUrlRules();
  PushRoutingTable();
}

void PhiCoordinator::SetURLRuleTarget(int index,
                                      const std::string& space_id,
                                      bool ask) {
  if (index < 0 || index >= static_cast<int>(url_rules_.size()) ||
      space_id.empty()) {
    return;
  }
  url_rules_[static_cast<size_t>(index)].space_id = space_id;
  url_rules_[static_cast<size_t>(index)].ask = ask;
  ++url_rules_generation_;  // Wire rule ids regenerate on every write.
  SaveUrlRules();
  PushRoutingTable();
}

void PhiCoordinator::RemoveURLRule(int index) {
  if (index < 0 || index >= static_cast<int>(url_rules_.size())) {
    return;
  }
  url_rules_.erase(url_rules_.begin() + index);
  ++url_rules_generation_;  // Wire rule ids regenerate on every write.
  SaveUrlRules();
  PushRoutingTable();
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
    COLORREF accent = 0;
    const bool has_hex = PhiParseColorHex(space.color_hex, &accent);
    if (space.theme_id.empty() && space.saturation < 0 && !has_hex) {
      continue;  // Follows the app-wide theme.
    }
    const int saturation =
        space.saturation < 0 ? saturation_ : space.saturation;
    if (space.theme_id.empty() && has_hex) {
      // No pinned theme but a wire colorHex: the hex drives the Space's tint
      // (the Mac CreateSpacePanel's "colorHex drives the sidebar tint"
      // contract — Spaces created by the skill carry only the hex).
      PhiTheme custom = PhiThemeById(std::string());  // Pure's grounds.
      custom.light.overlay = accent;
      custom.light.accent = accent;
      custom.dark.overlay = accent;
      custom.dark.accent = accent;
      palette_by_space_[space.id] =
          PhiResolvePalette(custom, appearance_, saturation);
      continue;
    }
    const std::string& id = space.theme_id.empty() ? theme_id_ : space.theme_id;
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

void PhiCoordinator::RequestSpaceIconPicker(const std::string& space_id) {
  PhiSettingsWindow::Open(this);
  PhiSettingsWindow::ShowSpacePicker(space_id);
}

int64_t PhiCoordinator::SlotAtScreenPoint(POINT screen,
                                          int64_t exclude_slot) const {
  // WindowFromPoint respects z-order; a hit on the browser child (or any
  // shell child) normalizes to its top-level shell.
  const HWND at = ::GetAncestor(::WindowFromPoint(screen), GA_ROOT);
  if (!at) {
    return 0;
  }
  for (const auto& [slot, controller] : windows_) {
    if (slot != exclude_slot && controller->shell_hwnd() == at) {
      return slot;
    }
  }
  return 0;
}

bool PhiCoordinator::RequestDropTabOnWindow(int64_t source_slot,
                                            int index,
                                            int b_index,
                                            int64_t target_slot) {
  auto source = windows_.find(source_slot);
  auto target = windows_.find(target_slot);
  if (source == windows_.end() || target == windows_.end() || !framework_) {
    return false;
  }
  // Never across the off-the-record boundary.
  if (source->second->is_incognito() != target->second->is_incognito()) {
    return false;
  }
  const int64_t source_browser = source->second->active_browser()
                                     ? source->second->active_browser()
                                     : source_slot;
  const int64_t target_browser = target->second->active_browser()
                                     ? target->second->active_browser()
                                     : target_slot;
  // A split pair moves both members; the HIGHER index first so the lower
  // one's position survives the removal.
  bool ok = true;
  if (b_index >= 0) {
    ok = framework_->MoveTabToWindow(source_browser, std::max(index, b_index),
                                     target_browser);
    ok = framework_->MoveTabToWindow(source_browser, std::min(index, b_index),
                                     target_browser) &&
         ok;
  } else {
    ok = framework_->MoveTabToWindow(source_browser, index, target_browser);
  }
  if (ok) {
    source->second->RefreshTabs();
    target->second->RefreshTabs();
  }
  return ok;
}

bool PhiCoordinator::AlwaysShowBookmarkBar() const {
  return always_show_bookmark_bar_;
}

void PhiCoordinator::SetAlwaysShowBookmarkBar(bool enabled) {
  if (enabled == always_show_bookmark_bar_) {
    return;
  }
  always_show_bookmark_bar_ = enabled;
  SavePrefs();
  // The bar takes/releases a band of the content rect in every window.
  for (const auto& [slot, controller] : windows_) {
    controller->LayoutContents();
    controller->RefreshTabs();
  }
}

bool PhiCoordinator::BookmarkBarOnNewTab() const {
  return bookmark_bar_on_new_tab_;
}

void PhiCoordinator::SetBookmarkBarOnNewTab(bool enabled) {
  if (enabled == bookmark_bar_on_new_tab_) {
    return;
  }
  bookmark_bar_on_new_tab_ = enabled;
  SavePrefs();
  for (const auto& [slot, controller] : windows_) {
    controller->LayoutContents();
    controller->RefreshTabs();
  }
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

std::vector<PhiAgentPipView> PhiCoordinator::AgentSpaces() const {
  std::vector<PhiAgentPipView> out;
  for (const auto& [task_id, space] : agent_spaces_.entries()) {
    PhiAgentPipView pip;
    pip.task_id = task_id;
    pip.caption = space.caption;
    pip.watched = task_id == watched_agent_task_;
    out.push_back(std::move(pip));
  }
  return out;
}

void PhiCoordinator::RequestWatchAgentSpace(const std::string& task_id) {
  PhiAgentSpaces::Space* space = agent_spaces_.Find(task_id);
  if (!space) {
    return;
  }
  auto controller_for = [&](const std::string& id) -> PhiWindowController* {
    PhiAgentSpaces::Space* s = agent_spaces_.Find(id);
    if (!s) {
      return nullptr;
    }
    auto it = windows_.find(s->window_id);
    return it != windows_.end() ? it->second.get() : nullptr;
  };
  // Clicking the watched pip again parks it; clicking a different one swaps.
  if (watched_agent_task_ == task_id) {
    if (PhiWindowController* c = controller_for(task_id)) {
      c->SetAgentVisible(false);
    }
    watched_agent_task_.clear();
  } else {
    if (!watched_agent_task_.empty()) {
      if (PhiWindowController* prev = controller_for(watched_agent_task_)) {
        prev->SetAgentVisible(false);
      }
    }
    if (PhiWindowController* c = controller_for(task_id)) {
      c->SetAgentVisible(true);
    }
    watched_agent_task_ = task_id;
  }
  RefreshAgentOverlay();
  RefreshUserWindows();
}

void PhiCoordinator::RefreshAgentOverlay() {
  if (watched_agent_task_.empty()) {
    PhiAgentOverlay::Hide();
    return;
  }
  PhiAgentSpaces::Space* space = agent_spaces_.Find(watched_agent_task_);
  if (!space) {
    PhiAgentOverlay::Hide();
    return;
  }
  auto it = windows_.find(space->window_id);
  if (it == windows_.end()) {
    PhiAgentOverlay::Hide();
    return;
  }
  const std::wstring label =
      space->agent_name.empty() ? L"Agent"
                                : base::UTF8ToWide(space->agent_name);
  const bool agent_owns =
      space->ownership == PhiAgentSpaces::Ownership::kAgent;
  std::wstring caption = base::UTF8ToWide(space->caption);
  if (caption.empty()) {
    caption = agent_owns ? L"Agent is working\u2026" : L"Agent paused";
  }
  if (space->status == "failed") {
    caption = L"\u26A0 " + caption;
  }
  PhiAgentOverlay::Show(it->second.get(), watched_agent_task_, label, caption,
                        agent_owns);
}

void PhiCoordinator::RequestAgentTakeControl(const std::string& task_id) {
  if (!agent_spaces_.Find(task_id)) {
    return;
  }
  agent_spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kUser,
                             static_cast<uint64_t>(::GetTickCount64()));
  PushOwnershipChanged(task_id);  // The driver must learn it lost control.
  RefreshAgentOverlay();
}

void PhiCoordinator::RequestAgentHandBack(const std::string& task_id) {
  if (!agent_spaces_.Find(task_id)) {
    return;
  }
  agent_spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kAgent,
                             static_cast<uint64_t>(::GetTickCount64()));
  PushOwnershipChanged(task_id);
  RefreshAgentOverlay();
}

void PhiCoordinator::RequestAgentFinish(const std::string& task_id) {
  // Ends the task outright — the pill's escape hatch when an agent is stuck.
  CloseAgentSpace(task_id);
  if (watched_agent_task_ == task_id) {
    watched_agent_task_.clear();
  }
  PhiAgentOverlay::Hide();
  RefreshUserWindows();
}

void PhiCoordinator::RefreshUserWindows() {
  for (const auto& [slot, controller] : windows_) {
    if (!controller->is_agent()) {
      controller->RefreshTabs();
    }
  }
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
  // The phi-browser skill's agent-space lifecycle (create/ping/complete/
  // ownership/log/messages/…) is served first; it owns the taskId-keyed
  // model. `agentSpace.list` deliberately falls THROUGH so both the skill's
  // `tasks` view and the box probes' `windows` view are merged below.
  {
    std::optional<base::DictValue> payload =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    if (HandleAgentSpaceVerb(type, payload ? &*payload : nullptr,
                             ::GetTickCount64(), &out)) {
      std::string s;
      base::JSONWriter::Write(out, &s);
      return s;
    }
    if (type.rfind("credentials.", 0) == 0 &&
        HandleCredentialsVerb(type, payload ? &*payload : nullptr, &out)) {
      std::string s;
      base::JSONWriter::Write(out, &s);
      return s;
    }
    // The Mac skill's dialect (tabGroups/splitView/downloads/pinnedTabs/
    // bookmark-tree/spaces.* forms). Claims name-colliding verbs only for
    // skill-shaped payloads, so the client-UI handlers below still serve
    // their own callers.
    if (HandleSkillDialectVerb(type, payload ? &*payload : nullptr, &out)) {
      std::string s;
      base::JSONWriter::Write(out, &s);
      return s;
    }
  }
  if (type == "agentSpace.ping") {
    out.Set("ok", true);
  } else if (type == "agentSpace.list") {
    base::ListValue list;
    for (const auto& [window_id, controller] : windows_) {
      list.Append(static_cast<double>(window_id));
    }
    out.Set("windows", std::move(list));
    // The skill reads `tasks`; the box probes read `windows`. Ship both.
    out.Set("tasks", agent_spaces_.TasksJson(::GetTickCount64()));
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
    int sort_order = 0;
    for (const Space& space : spaces_) {
      base::DictValue s;
      // The client-UI shape (id/icon/themeId) AND the skill's management shape
      // (spaceId/colorHex/iconName/sortOrder/isDefault/isActive) — one list
      // serves the box probes and the phi-browser skill both.
      s.Set("id", space.id);
      s.Set("spaceId", space.id);
      s.Set("name", space.name);
      s.Set("profileId", space.profile_id);
      s.Set("icon", space.icon);
      s.Set("iconName", space.icon);
      s.Set("colorHex", space.color_hex);
      s.Set("themeId", space.theme_id);
      s.Set("iconText", PhiIconDisplayText(space.icon));
      s.Set("sortOrder", sort_order++);
      s.Set("isDefault", space.id == "1");
      const bool active =
          it != windows_.end() && it->second->active_space() == space.id;
      s.Set("active", active);
      s.Set("isActive", active);
      s.Set("open", it != windows_.end() && it->second->HasSpace(space.id));
      list.Append(std::move(s));
    }
    out.Set("spaces", std::move(list));
    out.Set("ok", true);
  } else if (type == "agentSpace.spaces.create") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string name, profile_id, color_hex, icon_name;
    bool activate = false;
    if (p) {
      if (const std::string* n = p->FindString("name")) {
        name = *n;
      }
      if (const std::string* pid = p->FindString("profileId")) {
        profile_id = *pid;
      }
      if (const std::string* c = p->FindString("colorHex")) {
        color_hex = *c;
      }
      if (const std::string* i = p->FindString("iconName")) {
        icon_name = *i;
      }
      activate = p->FindBool("activate").value_or(false);
    }
    const std::string id = std::to_string(next_space_id_++);
    if (name.empty()) {
      name = "Space " + id;
    }
    if (profile_id.empty()) {
      profile_id = "Default";  // A Space always binds exactly one profile.
    }
    // A named icon when the skill gave a valid one; a random catalog draw
    // otherwise, as the Mac's create panel does.
    const auto& catalog = PhiIconCatalogAll();
    const std::string icon =
        PhiIconIsValid(icon_name)
            ? icon_name
            : "phi:" +
                  std::string(catalog[base::RandGenerator(catalog.size())].id);
    Space space{id, name, profile_id, icon};
    space.color_hex = color_hex;
    spaces_.push_back(std::move(space));
    SaveSpaces();
    RebuildSpacePalettes();  // A wire colorHex tints the new Space.
    if (activate) {
      // Surface it in the user's focused window — explicit opt-in only (the
      // skill's default create never yanks the user's window).
      PhiWindowController* focused = FocusedUserController();
      bool pending = false, cold = false;
      SwitchSlotToSpace(focused ? focused->window_id() : 0, id, &pending,
                        &cold);
    }
    out.Set("id", id);
    out.Set("spaceId", id);
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
    bool pending = false, cold = false;
    const bool ok = SwitchSlotToSpace(slot, space_id, &pending, &cold);
    if (cold) {
      out.Set("pending", pending);  // Attaching when the browser arrives.
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
      // Both key spellings: the box probes' {id,name} and the Mac skill's
      // {profileId,displayName}.
      if (const std::string* i = p->FindString("id")) {
        id = *i;
      } else if (const std::string* i2 = p->FindString("profileId")) {
        id = *i2;
      }
      if (const std::string* n = p->FindString("name")) {
        name = *n;
      } else if (const std::string* n2 = p->FindString("displayName")) {
        name = *n2;
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
      } else if (const std::string* n2 = p->FindString("displayName")) {
        name = *n2;  // The Mac skill's key spelling.
      }
    }
    bool ok = framework_ && !name.empty();
    if (ok) {
      // Creation is fire-and-forget in the framework, so the duplicate
      // display-name refusal has to happen here.
      std::optional<base::Value> profiles = base::JSONReader::Read(
          framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
      if (profiles && profiles->is_list()) {
        for (const base::Value& v : profiles->GetList()) {
          const base::DictValue* d = v.GetIfDict();
          const std::string* existing = d ? d->FindString("name") : nullptr;
          if (existing && *existing == name) {
            ok = false;
            out.Set("error", "duplicate_name");
            break;
          }
        }
      }
    }
    if (ok && profile_create_pending_) {
      ok = false;  // One at a time: the completion has a single slot.
      out.Set("error", "busy");
    }
    if (ok) {
      if (!profile_create_done_) {
        profile_create_done_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      }
      ::ResetEvent(profile_create_done_);
      created_profile_id_.clear();
      profile_create_pending_ = true;
      framework_->CreateProfile(name.c_str());
    }
    out.Set("ok", ok);
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
        entry.Set("splitPartnerGuid", row.split_partner_guid);
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
        entry.Set("secondaryUrl", row.url_b);
        entry.Set("secondaryTitle", row.title_b);
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
    const PhiRow* row = tab_store_.Find(guid);
    const bool ok = !guid.empty() && row != nullptr;
    if (ok) {
      // The owner rides the response so the skill can settle against the
      // right Space's list after the row is gone.
      out.Set("spaceId", row->space_id);
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
    // (There is no Phi Link pane on Mac — IMChannels exists in its source
    // but is never mounted.)
    for (const char* name : {"account", "general", "profiles", "spaces", "ai",
                             "shortcuts"}) {
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
    out.Set("restoreLastSession", restore_last_session_);
    out.Set("pinScope", PhiPinScopeName(tab_store_.pin_scope()));
    // With a profileId, the per-profile browsing settings too.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    if (p) {
      if (const std::string* profile_id = p->FindString("profileId")) {
        base::ListValue engines;
        for (const std::string& row : SearchEngines(*profile_id)) {
          engines.Append(row);
        }
        out.Set("searchEngines", std::move(engines));
        out.Set("downloadDirectory", DownloadDirectory(*profile_id));
      }
    }
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
      if (std::optional<bool> restore = p->FindBool("restoreLastSession")) {
        SetRestoreLastSession(*restore);
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
      // Per-profile browsing settings, keyed by profileId.
      if (const std::string* profile_id = p->FindString("profileId")) {
        if (const std::string* keyword = p->FindString("searchEngine")) {
          ok = SetSearchEngine(*profile_id, *keyword) || ok;
          PhiSettingsWindow::Refresh();
        }
        if (const std::string* dir = p->FindString("downloadDirectory")) {
          ok = SetDownloadDirectory(*profile_id, *dir) || ok;
          PhiSettingsWindow::Refresh();
        }
      }
    }
    out.Set("developerMode", developer_mode_);
    out.Set("saturation", saturation_);
    out.Set("alwaysShowFullUrl", always_show_full_url_);
    out.Set("restoreLastSession", restore_last_session_);
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
  } else if (type == "agentSpace.tabs.storeSplit") {
    // Stores a live split the row menu's two ways: a pinned pair
    // (`asBookmark` false) or a split bookmark.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    bool ok = false;
    if (p) {
      const int64_t window_id =
          static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
      const int index_a = static_cast<int>(
          p->FindDouble("indexA").value_or(-1));
      const int index_b = static_cast<int>(
          p->FindDouble("indexB").value_or(-1));
      if (window_id && index_a >= 0 && index_b >= 0) {
        RequestStoreSplit(window_id, index_a, index_b,
                          p->FindBool("asBookmark").value_or(false));
        ok = true;
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

bool PhiCoordinator::HandleAgentSpaceVerb(const std::string& type,
                                          const base::DictValue* payload,
                                          uint64_t now_ms,
                                          base::DictValue* out) {
  auto task_id_of = [&]() -> std::string {
    if (const std::string* t = payload ? payload->FindString("taskId")
                                       : nullptr) {
      return *t;
    }
    return std::string();
  };

  if (type == "agentSpace.create") {
    const std::string task_id = task_id_of();
    if (task_id.empty()) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    // Re-bind to an existing agent Space if its window is still alive.
    if (PhiAgentSpaces::Space* existing = agent_spaces_.Find(task_id)) {
      if (windows_.count(existing->window_id)) {
        agent_spaces_.Create(task_id, existing->window_id,
                             existing->profile_id, existing->persistent,
                             now_ms);
        out->Set("ok", true);
        out->Set("spaceId", existing->space_id);
        out->Set("windowId", static_cast<double>(existing->window_id));
        out->Set("ownership",
                 existing->ownership == PhiAgentSpaces::Ownership::kUser
                     ? "user"
                     : "agent");
        return true;
      }
      agent_spaces_.Remove(task_id);  // Stale record; recreate below.
    }
    std::string profile_id;
    if (const std::string* p = payload ? payload->FindString("profileId")
                                       : nullptr) {
      profile_id = *p;
    } else if (const std::string* pn =
                   payload ? payload->FindString("profileName") : nullptr) {
      profile_id = *pn;
    }
    const bool persistent =
        payload ? payload->FindBool("persistent").value_or(false) : false;
    if (!framework_) {
      out->Set("ok", false);
      out->Set("error", "create_failed");
      return true;
    }
    // Create the hidden agent browser; OnBrowserWindowCreated fires
    // re-entrantly and stamps last_agent_window_id_.
    last_agent_window_id_ = 0;
    pending_agent_task_ = task_id;
    const int64_t window_id =
        framework_->CreateAgentBrowser(profile_id.c_str());
    pending_agent_task_.clear();
    const int64_t adopted = window_id ? window_id : last_agent_window_id_;
    if (!adopted) {
      out->Set("ok", false);
      out->Set("error", "create_failed");
      return true;
    }
    PhiAgentSpaces::Space& space = agent_spaces_.Create(
        task_id, adopted, profile_id, persistent, now_ms);
    // The driving agent's name rides the create call; the watch-mode pill
    // shows it so a watching user sees WHO holds the Space, not just "an
    // agent" (the Mac's AgentDriverBadge).
    if (const std::string* agent_name =
            payload ? payload->FindString("agentName") : nullptr) {
      space.agent_name = *agent_name;
    }
    // Start the keep-alive sweep once (id 2, every 10s — the Mac sweep tick).
    if (msg_hwnd_) {
      ::SetTimer(msg_hwnd_, 2, 10000, nullptr);
    }
    RefreshUserWindows();  // A new agent pip appears in the switcher.
    out->Set("ok", true);
    out->Set("spaceId", space.space_id);
    out->Set("windowId", static_cast<double>(space.window_id));
    out->Set("ownership", "agent");
    return true;
  }

  if (type == "agentSpace.ping") {
    const std::string task_id = task_id_of();
    if (task_id.empty()) {
      return false;  // The generic (task-less) ping handles the box probes.
    }
    if (!agent_spaces_.Find(task_id)) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    const int ttl = payload ? static_cast<int>(
                                  payload->FindDouble("ttlSeconds").value_or(0))
                            : 0;
    agent_spaces_.Ping(task_id, ttl, now_ms);
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.complete") {
    const std::string task_id = task_id_of();
    if (task_id.empty()) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    PhiAgentSpaces::Space* space = agent_spaces_.Find(task_id);
    if (!space) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    // `keep` defaults true; a persistent Space keeps its window, an ephemeral
    // one is torn down. (Persistent Spaces are not yet surfaced in a switcher
    // on Windows, so `keep` on a persistent Space just leaves the hidden
    // window alive for the next re-bind.)
    const bool keep = payload ? payload->FindBool("keep").value_or(true)
                              : true;
    if (space->persistent && keep) {
      space->status = "completed";
      space->caption.clear();
    } else {
      CloseAgentSpace(task_id);
    }
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.getOwnership") {
    const std::string task_id = task_id_of();
    PhiAgentSpaces::Space* space =
        task_id.empty() ? nullptr : agent_spaces_.Find(task_id);
    // Unknown/absent task answers "none" (never unknown_task), matching Mac.
    out->Set("owner", !space ? "none"
             : space->ownership == PhiAgentSpaces::Ownership::kUser ? "user"
                                                                    : "agent");
    return true;
  }

  if (type == "agentSpace.handoff") {
    const std::string task_id = task_id_of();
    if (!agent_spaces_.Find(task_id)) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    agent_spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kUser,
                              now_ms);
    PushOwnershipChanged(task_id);
    RefreshAgentOverlay();
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.takeover") {
    const std::string task_id = task_id_of();
    if (!agent_spaces_.Find(task_id)) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    agent_spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kAgent,
                              now_ms);
    PushOwnershipChanged(task_id);
    RefreshAgentOverlay();
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.setState") {
    const std::string task_id = task_id_of();
    if (task_id.empty()) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    if (!agent_spaces_.Find(task_id)) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    const std::string* caption =
        payload ? payload->FindString("caption") : nullptr;
    const std::string* state =
        payload ? payload->FindString("state") : nullptr;
    agent_spaces_.SetState(task_id, caption, state, now_ms);
    RefreshAgentOverlay();  // The pill shows the caption the agent just set.
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.log") {
    const std::string task_id = task_id_of();
    if (task_id.empty() || !payload) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    if (!agent_spaces_.Find(task_id)) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    // Accept either a single-entry object (top-level `text`) or an `entries`
    // array; we store rendered lines for a future transcript panel.
    bool any = false;
    if (const base::ListValue* entries = payload->FindList("entries")) {
      for (const base::Value& v : *entries) {
        const base::DictValue* d = v.GetIfDict();
        const std::string* text = d ? d->FindString("text") : nullptr;
        if (text) {
          agent_spaces_.AppendLog(task_id, *text);
          any = true;
        }
      }
    } else if (const std::string* text = payload->FindString("text")) {
      agent_spaces_.AppendLog(task_id, *text);
      any = true;
    }
    if (!any) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    // Refresh keep-alive unless touch:false.
    if (payload->FindBool("touch").value_or(true)) {
      agent_spaces_.Ping(task_id, 0, now_ms);
    }
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.markError") {
    const std::string task_id = task_id_of();
    if (task_id.empty()) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    PhiAgentSpaces::Space* space = agent_spaces_.Find(task_id);
    if (!space) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    const std::string* message =
        payload ? payload->FindString("message") : nullptr;
    space->status = "failed";
    agent_spaces_.AppendLog(task_id, message ? *message : "error");
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.readUserMessages") {
    const std::string task_id = task_id_of();
    if (task_id.empty()) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    if (!agent_spaces_.Find(task_id)) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    out->Set("messages", agent_spaces_.DrainMessages(task_id));
    out->Set("ok", true);  // Every other verb answers with it.
    return true;
  }

  if (type == "agentSpace.userMessage") {
    // Inbound console message injection (used by the mirror / tests); the Mac
    // wire has this as an event, but accepting it as a request lets the
    // console flow work without a push channel.
    const std::string task_id = task_id_of();
    const std::string* text = payload ? payload->FindString("text") : nullptr;
    if (task_id.empty() || !text) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    const std::string id = agent_spaces_.PushMessage(
        task_id, *text, base::Time::Now().InMillisecondsSinceUnixEpoch());
    out->Set("ok", !id.empty());
    if (!id.empty()) {
      out->Set("id", id);
    } else {
      out->Set("error", "unknown_task");
    }
    return true;
  }

  if (type == "agentSpace.openTab") {
    const std::string task_id = task_id_of();
    const std::string* url = payload ? payload->FindString("url") : nullptr;
    PhiAgentSpaces::Space* space =
        task_id.empty() ? nullptr : agent_spaces_.Find(task_id);
    if (task_id.empty() || !url || url->empty()) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    if (!space || !framework_) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    framework_->NewTab(space->window_id, url->c_str());
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.listProfiles") {
    base::ListValue profiles;
    if (framework_) {
      std::optional<base::Value> parsed = base::JSONReader::Read(
          framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
      if (parsed && parsed->is_list()) {
        for (const base::Value& v : parsed->GetList()) {
          const base::DictValue* d = v.GetIfDict();
          if (!d) {
            continue;
          }
          base::DictValue e;
          const std::string* id = d->FindString("id");
          const std::string* name = d->FindString("name");
          e.Set("profileId", id ? *id : "");
          e.Set("displayName", name ? *name : (id ? *id : ""));
          e.Set("agentSpacesAllowed", true);
          profiles.Append(std::move(e));
        }
      }
    }
    out->Set("profiles", std::move(profiles));
    return true;
  }

  if (type == "agentSpace.panelSize") {
    // No watcher panel on Windows yet; report the agent surface's content
    // size so callers that scale to it get a sane value.
    out->Set("width", 1400);
    out->Set("height", 900);
    return true;
  }

  if (type == "agentSpace.cursor" || type == "agentSpace.effect") {
    // The user-facing cursor glide / click ripple overlays live on the
    // watcher; the hidden window has none. Accept and no-op so the skill's
    // input mirroring never errors.
    const std::string task_id = task_id_of();
    if (task_id.empty() || !agent_spaces_.Find(task_id)) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.captureWindow") {
    // Whole-window capture (screenshotBrowser): render the client-drawn
    // chrome and composite the CDP web capture the caller already took over
    // the content rect — the hidden agent window's DComp surface cannot be
    // read back, exactly as the Mac's GPU surface cannot.
    const std::string task_id = task_id_of();
    const std::string* out_path =
        payload ? payload->FindString("outPath") : nullptr;
    if (task_id.empty() || !out_path || out_path->empty()) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    PhiAgentSpaces::Space* space = agent_spaces_.Find(task_id);
    if (!space || !space->window_id) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    auto it = windows_.find(space->window_id);
    if (it == windows_.end() || !it->second->shell_hwnd()) {
      out->Set("ok", false);
      out->Set("error", "no_window");
      return true;
    }
    const std::string* web_path =
        payload ? payload->FindString("webPath") : nullptr;
    if (!PhiCaptureWindowToPng(it->second->shell_hwnd(),
                               it->second->content_rect(),
                               web_path ? base::UTF8ToWide(*web_path)
                                        : std::wstring(),
                               base::UTF8ToWide(*out_path))) {
      out->Set("ok", false);
      out->Set("error", "capture_failed");
      return true;
    }
    out->Set("ok", true);
    out->Set("path", *out_path);
    return true;
  }

  // --- URL routing rules (the skill's listUrlRules/addUrlRule/...) ---------
  // Rule "id" is "g<generation>-<index>" — the generation bumps on every
  // rule write, so ids REGENERATE across writes (the Mac contract; callers
  // must re-list before update/delete).
  auto space_exists = [&](const std::string& id) {
    // 'space.incognito' is the routing wire's Incognito target — always a
    // legal rule target even though it is not a listed Space.
    return id == "space.incognito" ||
           std::any_of(spaces_.begin(), spaces_.end(),
                       [&](const Space& s) { return s.id == id; });
  };
  if (type == "agentSpace.urlRules.list") {
    base::ListValue rules;
    const std::vector<std::string> rows = URLRuleRows();
    for (size_t i = 0; i < rows.size(); ++i) {
      const std::vector<std::string> parts = SplitTabRow(rows[i]);
      base::DictValue d;
      d.Set("id", "g" + base::NumberToString(url_rules_generation_) + "-" +
                      base::NumberToString(i));
      d.Set("spaceId", parts.size() > 0 ? parts[0] : "");
      d.Set("host", parts.size() > 1 ? parts[1] : "");
      if (parts.size() > 2 && !parts[2].empty()) {
        d.Set("pathPrefix", parts[2]);
      } else {
        d.Set("pathPrefix", base::Value());
      }
      d.Set("ask", parts.size() > 3 && parts[3] == "1");
      d.Set("sortOrder", static_cast<int>(i));
      rules.Append(std::move(d));
    }
    out->Set("ok", true);
    out->Set("rules", std::move(rules));
    return true;
  }
  if (type == "agentSpace.urlRules.add") {
    const std::string* space_id =
        payload ? payload->FindString("spaceId") : nullptr;
    const std::string* host = payload ? payload->FindString("host") : nullptr;
    if (!space_id || space_id->empty() || !host || host->empty()) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    if (!space_exists(*space_id)) {
      out->Set("ok", false);
      out->Set("error", "unknown_space");
      return true;
    }
    const std::string* prefix =
        payload ? payload->FindString("pathPrefix") : nullptr;
    const bool ask = payload ? payload->FindBool("ask").value_or(false) : false;
    AddURLRule(*space_id, base::ToLowerASCII(*host),
               prefix ? *prefix : std::string(), ask);
    out->Set("ok", true);
    return true;
  }
  if (type == "agentSpace.urlRules.update" ||
      type == "agentSpace.urlRules.delete") {
    int index = -1;
    if (const std::string* id = payload ? payload->FindString("id") : nullptr) {
      const std::string current_gen =
          "g" + base::NumberToString(url_rules_generation_) + "-";
      if (id->rfind(current_gen, 0) == 0) {
        base::StringToInt(id->substr(current_gen.size()), &index);
      } else if (id->find('-') == std::string::npos) {
        base::StringToInt(*id, &index);  // Legacy bare-index probes.
      } else {
        // A prior generation's id: the list has been rewritten since it was
        // read, so it no longer names anything.
        out->Set("ok", false);
        out->Set("error", "unknown_rule");
        return true;
      }
    }
    const std::vector<std::string> rows = URLRuleRows();
    if (index < 0 || index >= static_cast<int>(rows.size())) {
      out->Set("ok", false);
      out->Set("error",
               index < 0 ? "invalid_payload" : "unknown_rule");
      return true;
    }
    if (type == "agentSpace.urlRules.delete") {
      RemoveURLRule(index);
      out->Set("ok", true);
      return true;
    }
    // Update: host/pathPrefix change the pattern; spaceId/ask change the
    // target. Absent fields keep their current value.
    const std::vector<std::string> parts = SplitTabRow(rows[index]);
    std::string space_id = parts.size() > 0 ? parts[0] : "";
    std::string host = parts.size() > 1 ? parts[1] : "";
    std::string prefix = parts.size() > 2 ? parts[2] : "";
    bool ask = parts.size() > 3 && parts[3] == "1";
    if (const std::string* h = payload ? payload->FindString("host") : nullptr) {
      host = base::ToLowerASCII(*h);
    }
    // pathPrefix: key presence is meaningful (present clears/sets, absent
    // keeps) — but our storage can't distinguish; treat present as set.
    if (payload && payload->Find("pathPrefix")) {
      const std::string* p = payload->FindString("pathPrefix");
      prefix = p ? *p : "";
    }
    if (const std::string* s =
            payload ? payload->FindString("spaceId") : nullptr) {
      if (!space_exists(*s)) {
        out->Set("ok", false);
        out->Set("error", "unknown_space");
        return true;
      }
      space_id = *s;
    }
    if (payload && payload->Find("ask")) {
      ask = payload->FindBool("ask").value_or(ask);
    }
    SetURLRulePattern(index, host, prefix);
    SetURLRuleTarget(index, space_id, ask);
    out->Set("ok", true);
    return true;
  }

  return false;  // Not an agent-space lifecycle verb.
}

void PhiCoordinator::CloseAgentSpace(const std::string& task_id) {
  PhiAgentSpaces::Space* space = agent_spaces_.Find(task_id);
  if (!space) {
    return;
  }
  const int64_t window_id = space->window_id;
  agent_spaces_.Remove(task_id);
  if (watched_agent_task_ == task_id) {
    watched_agent_task_.clear();
  }
  auto it = windows_.find(window_id);
  if (it != windows_.end()) {
    it->second->RequestClose();  // Fires OnBrowserWindowClosing → cleanup.
  } else if (framework_ && window_id) {
    framework_->CloseBrowserWindow(window_id);
  }
  if (agent_spaces_.empty() && msg_hwnd_) {
    ::KillTimer(msg_hwnd_, 2);
  }
  RefreshUserWindows();  // The agent pip disappears.
}

void PhiCoordinator::SweepAgentSpaces() {
  for (const std::string& task_id : agent_spaces_.Expired(::GetTickCount64())) {
    LOG(INFO) << "[PhiClient] agent space expired: " << task_id;
    CloseAgentSpace(task_id);
  }
  if (agent_spaces_.empty() && msg_hwnd_) {
    ::KillTimer(msg_hwnd_, 2);
  }
}

namespace {

// Pulls a credential query out of a payload: either a nested `query`
// {domain|id|search|username} or a top-level `domain`. Precedence
// domain -> id -> search matches the Mac parser.
struct CredentialQuery {
  std::string domain;
  std::string id;
  std::string search;
  std::string username;
  bool valid = false;
};

CredentialQuery ParseCredentialQuery(const base::DictValue* payload) {
  CredentialQuery q;
  if (!payload) {
    return q;
  }
  const base::DictValue* query = payload->FindDict("query");
  auto pick = [&](const char* key) -> std::string {
    if (query) {
      if (const std::string* s = query->FindString(key)) {
        return *s;
      }
    }
    return std::string();
  };
  q.domain = pick("domain");
  q.id = pick("id");
  q.search = pick("search");
  q.username = pick("username");
  if (q.domain.empty() && !query) {
    if (const std::string* d = payload->FindString("domain")) {
      q.domain = *d;
    }
  }
  q.valid = !q.domain.empty() || !q.id.empty() || !q.search.empty();
  return q;
}

// The non-secret candidate list for an ambiguous match.
base::ListValue CandidateList(
    const std::vector<const PhiCredentialItem*>& items) {
  base::ListValue out;
  for (const PhiCredentialItem* item : items) {
    base::DictValue d;
    if (!item->credential_id.empty()) {
      d.Set("credentialId", item->credential_id);
    }
    d.Set("type", item->type);
    if (!item->name.empty()) {
      d.Set("name", item->name);
    }
    if (!item->username.empty()) {
      d.Set("username", item->username);
    }
    if (!item->uri.empty()) {
      d.Set("uri", item->uri);
    }
    if (!item->domain.empty()) {
      d.Set("domain", item->domain);
    }
    out.Append(std::move(d));
  }
  return out;
}

}  // namespace

std::string PhiCoordinator::HelperStatus() {
  if (!vault_helper_.running()) {
    return std::string();
  }
  std::string error;
  std::optional<base::DictValue> result =
      vault_helper_.Call("status", base::DictValue(), &error);
  if (!result) {
    return std::string();
  }
  const std::string* status = result->FindString("status");
  return status ? *status : std::string();
}

std::optional<base::DictValue> PhiCoordinator::HelperLookup(
    const std::string& domain,
    const std::string& id,
    const std::string& search,
    const std::string& username,
    std::string* error) {
  base::DictValue query;
  if (!domain.empty()) {
    query.Set("domain", domain);
    if (!username.empty()) {
      query.Set("username", username);
    }
  } else if (!id.empty()) {
    query.Set("id", id);
  } else {
    query.Set("search", search);
  }
  base::DictValue params;
  params.Set("query", std::move(query));
  return vault_helper_.Call("lookup", params, error);
}

bool PhiCoordinator::ResolveCredentialItem(const std::string& domain,
                                           const std::string& id,
                                           const std::string& search,
                                           const std::string& username,
                                           base::DictValue* item,
                                           base::DictValue* out) {
  if (vault_helper_.running()) {
    std::string error;
    std::optional<base::DictValue> result =
        HelperLookup(domain, id, search, username, &error);
    if (!result) {
      // The helper refused or died. Never silently fall through to the dev
      // vault here: that would answer a real-vault question from a different
      // store.
      out->Set("ok", false);
      out->Set("error", error == "helper_unavailable" ? "not_ready"
                                                      : "provider_error");
      if (error == "helper_unavailable") {
        out->Set("status", "unavailable");
      }
      return false;
    }
    if (result->FindBool("ambiguous").value_or(false)) {
      out->Set("ok", false);
      out->Set("error", "ambiguous");
      out->Set("matches", result->FindInt("matches").value_or(0));
      if (base::ListValue* candidates = result->FindList("candidates")) {
        out->Set("candidates", candidates->Clone());
      }
      return false;
    }
    base::DictValue* hit = result->FindBool("found").value_or(false)
                               ? result->FindDict("item")
                               : nullptr;
    if (!hit) {
      out->Set("ok", false);
      out->Set("error", "not_found");
      return false;
    }
    *item = hit->Clone();
    return true;
  }

  // Dev/test fallback: the local DPAPI store.
  if (!credentials_.present()) {
    out->Set("ok", false);
    out->Set("error", "not_ready");
    out->Set("status", "logged_out");
    return false;
  }
  std::vector<const PhiCredentialItem*> matches =
      credentials_.Match(domain, id, search, username);
  if (matches.empty()) {
    out->Set("ok", false);
    out->Set("error", "not_found");
    return false;
  }
  if (matches.size() > 1) {
    out->Set("ok", false);
    out->Set("error", "ambiguous");
    out->Set("matches", static_cast<int>(matches.size()));
    out->Set("candidates", CandidateList(matches));
    return false;
  }
  const PhiCredentialItem* found = matches.front();
  item->Set("type", found->type);
  item->Set("name", found->name);
  if (!found->credential_id.empty()) {
    item->Set("credentialId", found->credential_id);
  }
  if (!found->username.empty()) {
    item->Set("username", found->username);
  }
  if (!found->password.empty()) {
    item->Set("password", found->password);
  }
  if (!found->uri.empty()) {
    item->Set("uri", found->uri);
  }
  if (!found->domain.empty()) {
    item->Set("domain", found->domain);
  }
  if (!found->notes.empty()) {
    item->Set("notes", found->notes);
  }
  return true;
}

void PhiCoordinator::PushOwnershipChanged(const std::string& task_id) {
  PhiAgentSpaces::Space* space = agent_spaces_.Find(task_id);
  if (!space) {
    return;  // Ended task: its disappearance from list() is the signal.
  }
  base::DictValue payload;
  payload.Set("taskId", task_id);
  payload.Set("owner", space->ownership == PhiAgentSpaces::Ownership::kUser
                           ? "user"
                           : "agent");
  std::string payload_json;
  base::JSONWriter::Write(payload, &payload_json);
  control_server_.BroadcastEvent("agentSpace.ownershipChanged", payload_json);
}

bool PhiCoordinator::CredentialGrantCovers(const std::string& scope,
                                           CredentialAccessKind kind) const {
  // A REVEAL grant covers everything; anything else must match the kind
  // exactly — a fill approval must never satisfy a reveal request.
  const auto covers = [kind](CredentialAccessKind granted) {
    return granted == CredentialAccessKind::kReveal || granted == kind;
  };
  auto always = credential_grants_.find(scope);
  if (always != credential_grants_.end() && covers(always->second)) {
    return true;
  }
  auto timed = credential_timed_grants_.find(scope);
  if (timed != credential_timed_grants_.end() &&
      ::GetTickCount64() < timed->second.expires_tick &&
      covers(timed->second.kind)) {
    return true;
  }
  return false;
}

bool PhiCoordinator::PromptCredentialApproval(const std::wstring& detail,
                                              CredentialAccessKind kind,
                                              const std::string& scope) {
  const bool reveal = kind == CredentialAccessKind::kReveal;
  if (CredentialGrantCovers(scope, kind)) {
    AppendCredentialAudit(scope, kind, "allowed (standing approval)");
    return true;
  }
  // The exposure banner is the load-bearing line, as on Mac: say plainly
  // whether the secret stays inside Phi or crosses to the agent.
  const std::wstring exposure =
      reveal ? L"The secret will be GIVEN TO THE AGENT."
             : L"Phi types the secret into the page. The agent never sees it.";
  const std::wstring text = detail + L"\n\n" + exposure;
  HWND owner = nullptr;
  for (const auto& [slot, controller] : windows_) {
    if (controller->shell_hwnd()) {
      owner = controller->shell_hwnd();
      break;
    }
  }
  int chosen = -1;
  if (owner) {
    chosen = PhiPromptChoose(
        owner, L"Credential request", text.c_str(),
        {L"Allow once", L"Allow for 10 minutes", L"Always allow", L"Deny"},
        0);
  } else {
    // No window yet: the MessageBox fallback offers allow-ONCE only — a
    // standing grant must never be given through a degraded dialog.
    chosen = ::MessageBoxW(nullptr, (text + L"\n\nAllow this once?").c_str(),
                           L"Phi — Credential request",
                           MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND |
                               MB_TOPMOST | MB_DEFBUTTON2) == IDYES
                 ? 0
                 : 3;
  }
  if (chosen == 1) {
    TimedCredentialGrant grant;
    grant.kind = kind;
    grant.expires_tick = ::GetTickCount64() + 10 * 60 * 1000;
    credential_timed_grants_[scope] = grant;
    AppendCredentialAudit(scope, kind, "allowed (10 minutes)");
    return true;
  }
  if (chosen == 2) {
    // A stronger kind never downgrades an existing grant.
    auto existing = credential_grants_.find(scope);
    if (existing == credential_grants_.end() ||
        kind == CredentialAccessKind::kReveal) {
      credential_grants_[scope] = kind;
    }
    SaveCredentialGrants();
    PhiSettingsWindow::Refresh();
    AppendCredentialAudit(scope, kind, "allowed (always)");
    return true;
  }
  if (chosen == 0) {
    AppendCredentialAudit(scope, kind, "allowed (once)");
    return true;
  }
  AppendCredentialAudit(scope, kind, "denied");
  return false;
}

void PhiCoordinator::AppendCredentialAudit(const std::string& scope,
                                           CredentialAccessKind kind,
                                           const char* decision) {
  SYSTEMTIME now = {};
  ::GetLocalTime(&now);
  char stamp[32] = {};
  ::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d", now.wYear,
             now.wMonth, now.wDay, now.wHour, now.wMinute);
  credential_audit_.insert(
      credential_audit_.begin(),
      std::string(stamp) + "\t" +
          (kind == CredentialAccessKind::kReveal ? "reveal" : "fill") + "\t" +
          scope + "\t" + decision);
  constexpr size_t kMaxAudit = 50;
  if (credential_audit_.size() > kMaxAudit) {
    credential_audit_.resize(kMaxAudit);
  }
  SaveCredentialGrants();
  PhiSettingsWindow::Refresh();
}

void PhiCoordinator::LoadCredentialGrants() {
  HANDLE f = ::CreateFileW(credential_grants_path_.c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
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
  if (const base::DictValue* grants = root->FindDict("grants")) {
    for (const auto [scope, value] : *grants) {
      if (value.is_string()) {
        credential_grants_[scope] = value.GetString() == "reveal"
                                        ? CredentialAccessKind::kReveal
                                        : CredentialAccessKind::kFill;
      }
    }
  }
  if (const base::ListValue* audit = root->FindList("audit")) {
    for (const base::Value& v : *audit) {
      if (v.is_string()) {
        credential_audit_.push_back(v.GetString());
      }
    }
  }
}

void PhiCoordinator::SaveCredentialGrants() const {
  base::DictValue root;
  base::DictValue grants;
  for (const auto& [scope, kind] : credential_grants_) {
    grants.Set(scope, kind == CredentialAccessKind::kReveal ? "reveal"
                                                            : "fill");
  }
  root.Set("grants", std::move(grants));
  base::ListValue audit;
  for (const std::string& line : credential_audit_) {
    audit.Append(line);
  }
  root.Set("audit", std::move(audit));
  std::string json;
  base::JSONWriter::Write(root, &json);
  HANDLE f = ::CreateFileW(credential_grants_path_.c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD wrote = 0;
  ::WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &wrote,
              nullptr);
  ::CloseHandle(f);
}

bool PhiCoordinator::HandleCredentialsVerb(const std::string& type,
                                           const base::DictValue* payload,
                                           base::DictValue* out) {
  if (type == "credentials.status") {
    // The helper (the user's real vault) answers when it is running; the dev
    // store only speaks for itself.
    const std::string helper_status = HelperStatus();
    if (!helper_status.empty()) {
      // Helper wire strings -> the skill's status vocabulary.
      std::string wire = "unavailable";
      if (helper_status == "unlocked") {
        wire = "ready";
      } else if (helper_status == "locked") {
        wire = "locked";
      } else if (helper_status == "loggedOut" ||
                 helper_status == "notConfigured") {
        wire = "logged_out";
      } else if (helper_status == "notInstalled") {
        wire = "not_installed";
      }
      out->Set("ok", true);
      out->Set("status", wire);
      out->Set("ready", wire == "ready");
      return true;
    }
    out->Set("ok", true);
    out->Set("status", credentials_.present() ? "ready" : "logged_out");
    out->Set("ready", credentials_.ready());
    return true;
  }

  if (type == "credentials.getTotp") {
    // A TOTP code is a SECOND FACTOR handed to the agent — a reveal
    // exposure, approved like any other, and only the real vault helper can
    // generate one (the dev store holds no seed, and the helper never
    // encodes the seed itself — it returns a generated code).
    if (!vault_helper_.running()) {
      out->Set("ok", false);
      out->Set("error", "totp_not_supported");
      return true;
    }
    CredentialQuery q = ParseCredentialQuery(payload);
    if (!q.valid) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    // Resolve first, so the approval names the item the code belongs to.
    base::DictValue item;
    if (!ResolveCredentialItem(q.domain, q.id, q.search, q.username, &item,
                               out)) {
      return true;  // `out` already carries the wire error body.
    }
    const std::string* name = item.FindString("name");
    const std::string* item_domain = item.FindString("domain");
    const std::string scope =
        name && !name->empty()
            ? *name
            : (item_domain ? *item_domain : q.domain);
    const std::wstring detail = base::UTF8ToWide(
        "An agent is requesting a one-time code (2FA) for:\n\n" + scope);
    if (!PromptCredentialApproval(detail, CredentialAccessKind::kReveal,
                                  scope)) {
      out->Set("ok", false);
      out->Set("error", "user_denied");
      return true;
    }
    base::DictValue query;
    if (!q.domain.empty()) {
      query.Set("domain", q.domain);
      if (!q.username.empty()) {
        query.Set("username", q.username);
      }
    } else if (!q.id.empty()) {
      query.Set("id", q.id);
    } else {
      query.Set("search", q.search);
    }
    base::DictValue params;
    params.Set("query", std::move(query));
    std::string error;
    std::optional<base::DictValue> result =
        vault_helper_.Call("getTotp", params, &error);
    const std::string* code = result ? result->FindString("totp") : nullptr;
    if (!code || code->empty()) {
      out->Set("ok", false);
      out->Set("error", result ? "totp_unavailable" : "vault_error");
      return true;
    }
    out->Set("ok", true);
    out->Set("totp", *code);
    return true;
  }

  if (type == "credentials.get") {
    // A fill request is never satisfied by a reveal.
    const std::string* mode = payload ? payload->FindString("mode") : nullptr;
    if (mode && *mode == "fill") {
      out->Set("ok", false);
      out->Set("error", "fill_requires_autofill");
      return true;
    }
    CredentialQuery q = ParseCredentialQuery(payload);
    if (!q.valid) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    base::DictValue item;
    if (!ResolveCredentialItem(q.domain, q.id, q.search, q.username, &item,
                               out)) {
      return true;  // `out` already carries the wire error body.
    }
    auto field_of = [&](const char* key) -> std::string {
      const std::string* value = item.FindString(key);
      return value ? *value : std::string();
    };
    const std::string item_type =
        field_of("type").empty() ? "login" : field_of("type");
    const std::string scope =
        field_of("name").empty() ? field_of("domain") : field_of("name");
    std::wstring detail = base::UTF8ToWide(
        "An agent is requesting a credential from your Phi vault:\n\n" +
        scope +
        (field_of("username").empty() ? "" : "  (" + field_of("username") + ")"));
    if (const std::string* purpose =
            payload ? payload->FindString("purpose") : nullptr) {
      if (!purpose->empty()) {
        detail += L"\n" + base::UTF8ToWide(*purpose);
      }
    }
    // A read hands the secret TO the agent — the reveal exposure.
    if (!PromptCredentialApproval(detail, CredentialAccessKind::kReveal,
                                  scope)) {
      out->Set("ok", false);
      out->Set("error", "user_denied");
      return true;
    }
    // Build the credential by the requested (or type-default) field set.
    std::vector<std::string> fields;
    if (const base::ListValue* f =
            payload ? payload->FindList("fields") : nullptr) {
      for (const base::Value& v : *f) {
        if (v.is_string()) {
          fields.push_back(v.GetString());
        }
      }
    }
    if (fields.empty()) {
      if (item_type == "note") {
        fields = {"notes"};
      } else {
        fields = {"username", "password", "uri", "domain"};
      }
    }
    base::DictValue cred;
    cred.Set("type", item_type);
    cred.Set("name", field_of("name"));
    for (const std::string& field : fields) {
      // Only the asked-for (or type-default) fields cross to the agent — and
      // never a TOTP seed: the helper does not encode one at all.
      const std::string value = field_of(field.c_str());
      if (!value.empty() && field != "totp") {
        cred.Set(field, value);
      }
    }
    out->Set("ok", true);
    out->Set("credential", std::move(cred));
    out->Set("matches", 1);
    return true;
  }

  if (type == "credentials.autofill") {
    const std::string* token = payload ? payload->FindString("token") : nullptr;
    const std::string* target_id =
        payload ? payload->FindString("targetId") : nullptr;
    const std::string* field_p =
        payload ? payload->FindString("field") : nullptr;
    const std::string field = field_p ? *field_p : "password";
    const bool allow_cross_origin =
        payload ? payload->FindBool("allowCrossOrigin").value_or(false)
                : false;
    if (!token || token->empty() || !target_id || target_id->empty() ||
        (field != "password" && field != "username")) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    CredentialQuery q = ParseCredentialQuery(payload);
    if (!q.valid) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    if (!framework_) {
      out->Set("ok", false);
      out->Set("error", "autofill_not_available");
      return true;
    }
    // Resolve the page host up front for the origin binding.
    const std::string page_url = framework_->TargetUrlForCdpTarget(
        target_id->c_str());
    const std::string page_host = PhiCredentialVault::HostOfUri(page_url);
    if (page_host.empty()) {
      out->Set("ok", false);
      out->Set("error", "page_not_found");
      return true;
    }
    base::DictValue item;
    if (!ResolveCredentialItem(q.domain, q.id, q.search, q.username, &item,
                               out)) {
      return true;  // `out` already carries the wire error body.
    }
    auto field_of = [&](const char* key) -> std::string {
      const std::string* v = item.FindString(key);
      return v ? *v : std::string();
    };
    const std::string item_type =
        field_of("type").empty() ? "login" : field_of("type");
    if (item_type != "login") {
      out->Set("ok", false);
      out->Set("error", "not_a_login");
      out->Set("type", item_type);
      return true;
    }
    const std::string value =
        field == "username" ? field_of("username") : field_of("password");
    if (value.empty()) {
      out->Set("ok", false);
      out->Set("error", "field_unavailable");
      out->Set("field", field);
      return true;
    }
    // Origin binding: the page must belong to the credential's site unless the
    // caller explicitly allowed a cross-origin fill.
    const std::string cred_host =
        field_of("domain").empty()
            ? PhiCredentialVault::HostOfUri(field_of("uri"))
            : field_of("domain");
    if (!allow_cross_origin &&
        !PhiCredentialVault::HostMatches(page_host, cred_host)) {
      out->Set("ok", false);
      out->Set("error", "origin_mismatch");
      out->Set("pageHost", page_host);
      return true;
    }
    const std::string scope =
        field_of("name").empty() ? cred_host : field_of("name");
    const std::wstring detail = base::UTF8ToWide(
        "An agent is requesting a credential fill:\n\n" + scope +
        "  → " + field + " on " + page_host);
    if (!PromptCredentialApproval(detail, CredentialAccessKind::kFill,
                                  scope)) {
      out->Set("ok", false);
      out->Set("error", "user_denied");
      return true;
    }
    // The secret goes client vault -> framework -> page DOM, never to the
    // agent. FillTaggedField returns false only when the tab is gone.
    if (!framework_->FillTaggedField(target_id->c_str(), token->c_str(),
                                     field.c_str(), value.c_str())) {
      out->Set("ok", false);
      out->Set("error", "target_not_found");
      return true;
    }
    out->Set("ok", true);
    out->Set("filled", true);
    out->Set("field", field);
    out->Set("matches", 1);
    return true;
  }

  return false;
}

void PhiCoordinator::OnBrowserWindowCreated(int64_t window_id, void* hwnd) {
  LOG(INFO) << "[PhiClient] OnBrowserWindowCreated windowId=" << window_id
            << " hwnd=" << hwnd;
  // Option B: the client owns a shell window and reparents Chromium's content
  // in. The shell routes activation back through the framework.
  if (!pending_agent_task_.empty()) {
    // An agent Space: host the browser OFF-SCREEN as a hidden agent surface.
    // This runs re-entrantly inside framework_->CreateAgentBrowser, so the
    // control handler sees `last_agent_window_id_` set on return.
    auto controller = std::make_unique<PhiWindowController>(
        window_id, static_cast<HWND>(hwnd), framework_, this, /*agent=*/true);
    controller->RefreshTabs();
    windows_[window_id] = std::move(controller);
    browser_to_slot_[window_id] = window_id;
    last_agent_window_id_ = window_id;
    return;
  }
  if (pending_slot_ != 0) {
    // A Space browser for an existing slot: attach, don't open a new shell.
    auto it = windows_.find(pending_slot_);
    if (it != windows_.end()) {
      it->second->AttachSpaceBrowser(pending_space_, window_id,
                                     static_cast<HWND>(hwnd));
      browser_to_slot_[window_id] = pending_slot_;
      // "Open Link in Space" / a routed URL into a cold Space: the link
      // opens once the Space's browser exists. Bypassing URL routing — the
      // URL may be the navigation a rule just re-homed HERE, and the table
      // rebuilt below races the open, so exempt it outright.
      if (!pending_open_url_.empty()) {
        if (framework_) {
          framework_->OpenTabBypassingSpaceRouting(window_id,
                                                   pending_open_url_.c_str());
        }
        pending_open_url_.clear();
      }
      pending_slot_ = 0;
      pending_space_.clear();
      PushSpaceMenu();  // The Space gained a window id.
      return;
    }
    pending_slot_ = 0;
    pending_space_.clear();
    pending_open_url_.clear();
  }
  // An off-the-record browser (IDC_NEW_INCOGNITO_WINDOW) becomes an
  // INCOGNITO shell: fixed-dark, no Spaces/pins/bookmarks, unrouted.
  bool incognito = false;
  if (framework_) {
    std::optional<base::DictValue> profile = base::JSONReader::ReadDict(
        framework_->WindowProfileJson(window_id), base::JSON_PARSE_RFC);
    incognito = profile && profile->FindBool("incognito").value_or(false);
  }
  auto controller = std::make_unique<PhiWindowController>(
      window_id, static_cast<HWND>(hwnd), framework_, this, /*agent=*/false,
      incognito);
  controller->RefreshTabs();
  windows_[window_id] = std::move(controller);
  browser_to_slot_[window_id] = window_id;
  if (!incognito) {
    PushSpaceMenu();  // The slot's Space gained a window id.
  }
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
  PhiCoordinator* self = g_coordinator;
  if (code != HC_ACTION || !self) {
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
  }
  if (lparam & 0x80000000) {  // Key-up.
    // Releasing Ctrl commits an in-flight MRU switch (the Mac's ⌃⇥ HUD).
    if (wparam == VK_CONTROL && PhiTabSwitcher::IsActive()) {
      if (HWND shell = self->ShellForFocus()) {
        ::PostMessageW(shell, kMsgPhiSwitcherCommit, 0, 0);
      } else {
        PhiTabSwitcher::Cancel();
      }
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
  }
  const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool alt = (lparam & 0x20000000) != 0;  // The context code bit.
  if (wparam == VK_TAB && ctrl && !alt) {
    // Ctrl+Tab drives the MRU switcher HUD, not a plain next-tab hop.
    if (HWND shell = self->ShellForFocus()) {
      ::PostMessageW(shell, shift ? kMsgPhiSwitcherPrev : kMsgPhiSwitcherNext,
                     0, 0);
      return 1;
    }
  }
  // The REBINDABLE shortcuts first (their effective chords carry the user's
  // overrides), so a custom chord shadows any fixed one it collides with.
  if (const UINT command = self->MatchRebindableChord(
          static_cast<UINT>(wparam), ctrl, shift, alt)) {
    if (HWND shell = self->ShellForFocus()) {
      ::PostMessageW(shell, WM_COMMAND, MAKEWPARAM(command, 0), 0);
      return 1;
    }
  }
  // The menu bar's remaining, FIXED shortcuts, reserved the way Chrome
  // reserves its browser shortcuts (they fire even while a page has focus).
  // Edit keys (Ctrl+C/V/X/Z/A/F...) stay with the web content; everything a
  // user can rebind lives in kRebindableShortcuts, not here.
  struct Accelerator {
    UINT vk;
    bool ctrl;
    bool shift;
    bool alt;
    UINT command;
  };
  static constexpr Accelerator kAccelerators[] = {
      {'Q', true, true, false, kCmdQuit},
      {'O', true, false, false, kCmdOpenFile},
      {'P', true, false, false, kCmdPrint},
      {VK_F5, false, false, false, kCmdReload},
      {'U', true, false, false, kCmdViewSource},
      {'I', true, true, false, kCmdDevTools},
      {'J', true, true, false, kCmdJsConsole},
      // F11 deliberately unbound — see the menu bar's IDC_FULLSCREEN note.
      {'0', true, false, false, kCmdActualSize},
      {VK_OEM_PLUS, true, false, false, kCmdZoomIn},
      {VK_ADD, true, false, false, kCmdZoomIn},
      {VK_OEM_MINUS, true, false, false, kCmdZoomOut},
      {VK_SUBTRACT, true, false, false, kCmdZoomOut},
      {VK_OEM_COMMA, true, false, false, kCmdPreferences},
      // Ctrl+Tab / Ctrl+Shift+Tab are handled ABOVE — the MRU switcher.
      // Alt+Left/Right/Home — the history chords Chrome reserves.
      {VK_LEFT, false, false, true, kCmdBack},
      {VK_RIGHT, false, false, true, kCmdForward},
      {VK_HOME, false, false, true, kCmdHome},
      // Ctrl+1..9 switches Spaces (the Mac's ⌃1..⌃9; the Spaces menu and
      // the pips' hover cards advertise these).
      {'1', true, false, false, kCmdSpace1},
      {'2', true, false, false, kCmdSpace2},
      {'3', true, false, false, kCmdSpace3},
      {'4', true, false, false, kCmdSpace4},
      {'5', true, false, false, kCmdSpace5},
      {'6', true, false, false, kCmdSpace6},
      {'7', true, false, false, kCmdSpace7},
      {'8', true, false, false, kCmdSpace8},
      {'9', true, false, false, kCmdSpace9},
  };
  for (const Accelerator& accel : kAccelerators) {
    if (accel.vk != static_cast<UINT>(wparam) || accel.ctrl != ctrl ||
        accel.shift != shift || accel.alt != alt) {
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

void PhiCoordinator::OnExtensionsChanged(int64_t window_id) {
  PhiExtensionsPopover::RefreshOpen();
  // The address capsule's pinned-extension icons follow the same change
  // notification (a pin toggle, an install, an icon decode).
  for (auto& [slot, controller] : windows_) {
    controller->RefreshFieldExtensions();
  }
}

void PhiCoordinator::OnContentFullscreenChanged(int64_t window_id,
                                                bool fullscreen) {
  auto slot_it = browser_to_slot_.find(window_id);
  const int64_t slot = slot_it != browser_to_slot_.end() ? slot_it->second
                                                         : window_id;
  auto it = windows_.find(slot);
  if (it != windows_.end()) {
    it->second->OnContentFullscreenChanged(window_id, fullscreen);
  }
}

void PhiCoordinator::OnTargetURLChanged(int64_t window_id, const char* url) {
  auto slot_it = browser_to_slot_.find(window_id);
  const int64_t slot = slot_it != browser_to_slot_.end() ? slot_it->second
                                                         : window_id;
  auto it = windows_.find(slot);
  if (it != windows_.end()) {
    it->second->OnTargetURLChanged(window_id,
                                   base::UTF8ToWide(url ? url : ""));
  }
}

void PhiCoordinator::OnOmniboxResults(int64_t window_id, const char* json) {
  auto slot_it = browser_to_slot_.find(window_id);
  const int64_t slot = slot_it != browser_to_slot_.end() ? slot_it->second
                                                         : window_id;
  auto it = windows_.find(slot);
  if (it != windows_.end()) {
    it->second->OnOmniboxResults(window_id, json ? json : "");
  }
}

void PhiCoordinator::OnImportCompleted(const char* source, bool success) {
  PhiWindowController* focused = FocusedUserController();
  HWND owner = focused ? focused->shell_hwnd() : nullptr;
  if (!success) {
    ::MessageBoxW(owner, L"The import did not finish. Close Google Chrome "
                         L"and try again.",
                  L"Import", MB_OK | MB_ICONWARNING);
    return;
  }
  // History/cookies/passwords landed in the profile directly; bookmarks
  // landed in the CHROMIUM model, which the Phi sidebar never shows — copy
  // them into the client store under one folder (the Mac's
  // saveChromiumBookmarksToLocalStore step).
  int copied = 0;
  std::string profile_id, space_id;
  if (focused && framework_ &&
      OwnerForSlot(focused->window_id(), &profile_id, &space_id)) {
    const int64_t browser = focused->active_browser()
                                ? focused->active_browser()
                                : focused->window_id();
    std::optional<base::Value> tree = base::JSONReader::Read(
        framework_->AllBookmarksJson(browser), base::JSON_PARSE_RFC);
    if (tree && tree->is_list() && !tree->GetList().empty()) {
      const std::string root = tab_store_.AddBookmark(
          profile_id, space_id, std::string(), "Imported from Chrome",
          std::string(), /*folder=*/true, std::string(), 0, 0);
      const auto copy = [&](const auto& self, const base::ListValue& nodes,
                            const std::string& parent) -> void {
        for (const base::Value& v : nodes) {
          const base::DictValue* d = v.GetIfDict();
          if (!d) {
            continue;
          }
          const std::string* title = d->FindString("title");
          const std::string* url = d->FindString("url");
          if (d->FindBool("folder").value_or(false)) {
            const base::ListValue* children = d->FindList("children");
            if (!children || children->empty()) {
              continue;  // The permanent nodes arrive empty when unused.
            }
            const std::string guid = tab_store_.AddBookmark(
                profile_id, space_id, parent,
                title && !title->empty() ? *title : "Folder", std::string(),
                /*folder=*/true, std::string(), 0, 0);
            self(self, *children, guid);
          } else if (url && !url->empty()) {
            tab_store_.AddBookmark(profile_id, space_id, parent,
                                   title && !title->empty() ? *title : *url,
                                   *url, /*folder=*/false, std::string(), 0,
                                   0);
            ++copied;
          }
        }
      };
      copy(copy, tree->GetList(), root);
      if (copied == 0) {
        tab_store_.Remove(root);  // Nothing under it — no empty shell.
      }
    }
  }
  for (const auto& [slot, controller] : windows_) {
    controller->RefreshTabs();
  }
  PhiSettingsWindow::Refresh();
  std::wstring message = L"Import finished.";
  if (copied > 0) {
    message += L" " + std::to_wstring(copied) +
               L" bookmarks were added to \"Imported from Chrome\".";
  }
  ::MessageBoxW(owner, message.c_str(), L"Import",
                MB_OK | MB_ICONINFORMATION);
}

void PhiCoordinator::OnPlaceholderModeChanged(int64_t window_id,
                                              bool entered) {
  auto slot_it = browser_to_slot_.find(window_id);
  const int64_t slot = slot_it != browser_to_slot_.end() ? slot_it->second
                                                         : window_id;
  auto it = windows_.find(slot);
  if (it != windows_.end()) {
    it->second->OnPlaceholderModeChanged(window_id, entered);
  }
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
  // An agent Space's hidden window closing (via complete(), the user closing
  // the surfaced window, or a crash) drops its record — the skill learns the
  // task ended from its absence in the list.
  if (PhiAgentSpaces::Space* space = agent_spaces_.FindByWindow(window_id)) {
    const std::string task_id = space->task_id;
    agent_spaces_.Remove(task_id);
    if (watched_agent_task_ == task_id) {
      watched_agent_task_.clear();
    }
    windows_.erase(window_id);
    browser_to_slot_.erase(window_id);
    if (agent_spaces_.empty() && msg_hwnd_) {
      ::KillTimer(msg_hwnd_, 2);
    }
    RefreshUserWindows();
    return;
  }
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
  PushSpaceMenu();  // A Space lost its window id.
}

bool PhiCoordinator::SwitchSlotToSpace(int64_t slot,
                                       const std::string& space_id,
                                       bool* pending,
                                       bool* cold) {
  auto it = windows_.find(slot);
  if (it == windows_.end() && slot == 0 && !windows_.empty()) {
    it = windows_.begin();  // The verb convention: 0 means "the window".
  }
  auto space_it =
      std::find_if(spaces_.begin(), spaces_.end(),
                   [&](const Space& s) { return s.id == space_id; });
  if (it == windows_.end() || space_it == spaces_.end()) {
    return false;
  }
  if (it->second->HasSpace(space_id)) {
    const bool ok = it->second->ShowSpace(space_id);
    if (ok) {
      PushSpaceMenu();  // The visible-Space routing map changed.
    }
    return ok;
  }
  if (!framework_) {
    return false;
  }
  // First visit: create this Space's browser IN ITS PROFILE and attach it to
  // the slot. A cold profile loads asynchronously, so the pending attach
  // stays armed until OnBrowserWindowCreated consumes it.
  if (cold) {
    *cold = true;
  }
  pending_slot_ = it->first;
  pending_space_ = space_id;
  const bool ok =
      framework_->CreateSpaceBrowserInProfile(space_it->profile_id.c_str());
  if (pending) {
    *pending = !it->second->HasSpace(space_id);
  }
  return ok;
}

int64_t PhiCoordinator::BrowserForSpace(const std::string& space_id) const {
  if (space_id.empty()) {
    return 0;
  }
  for (const auto& [slot, controller] : windows_) {
    const int64_t browser = controller->BrowserIdForSpace(space_id);
    if (browser) {
      return browser;
    }
  }
  return 0;
}

int64_t PhiCoordinator::ResolveScopeBrowser(const base::DictValue* payload,
                                            std::string* error) {
  if (const std::string* task =
          payload ? payload->FindString("taskId") : nullptr) {
    PhiAgentSpaces::Space* space = agent_spaces_.Find(*task);
    if (!space) {
      *error = "unknown_task";
      return 0;
    }
    return ResolveBrowser(space->window_id);
  }
  if (const std::string* space_id =
          payload ? payload->FindString("spaceId") : nullptr) {
    const int64_t browser = BrowserForSpace(*space_id);
    if (!browser) {
      *error = "space_not_open";
    }
    return browser;
  }
  *error = "invalid_payload";
  return 0;
}

PhiWindowController* PhiCoordinator::FocusedUserController() {
  const HWND foreground = ::GetForegroundWindow();
  PhiWindowController* first_user = nullptr;
  for (const auto& [slot, controller] : windows_) {
    if (controller->is_agent()) {
      continue;
    }
    if (foreground && controller->shell_hwnd() == foreground) {
      return controller.get();
    }
    if (!first_user) {
      first_user = controller.get();
    }
  }
  return first_user;
}

bool PhiCoordinator::DialectOwner(const std::string& profile_id_or_empty,
                                  std::string* profile_id,
                                  std::string* space_id) {
  if (!profile_id_or_empty.empty()) {
    *profile_id = profile_id_or_empty;
    // Prefer a window whose active Space runs this profile (its space is the
    // natural owner under space scope); fall back to the profile's first
    // Space, then to no Space (profile/app-scoped pins still resolve).
    for (const auto& [slot, controller] : windows_) {
      if (controller->is_agent()) {
        continue;
      }
      std::string p, s;
      if (OwnerForSlot(slot, &p, &s) && p == *profile_id) {
        *space_id = s;
        return true;
      }
    }
    for (const Space& space : spaces_) {
      if (space.profile_id == *profile_id) {
        *space_id = space.id;
        return true;
      }
    }
    space_id->clear();
    return true;
  }
  PhiWindowController* focused = FocusedUserController();
  return focused &&
         OwnerForSlot(focused->window_id(), profile_id, space_id);
}

bool PhiCoordinator::HandleSkillDialectVerb(const std::string& type,
                                            const base::DictValue* payload,
                                            base::DictValue* out) {
  auto fail = [&](const std::string& code) {
    out->Set("ok", false);
    out->Set("error", code);
    return true;
  };
  // The window's tab rows (ListTabsJson parsed), or an empty list.
  auto tabs_of = [&](int64_t browser) {
    base::ListValue tabs;
    if (framework_ && browser) {
      std::optional<base::Value> parsed = base::JSONReader::Read(
          framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
      if (parsed && parsed->is_list()) {
        tabs = std::move(*parsed).TakeList();
      }
    }
    return tabs;
  };
  auto refresh_all = [&]() {
    for (const auto& [slot, controller] : windows_) {
      controller->RefreshTabs();
    }
  };

  if (type == "agentSpace.resolveTabIds") {
    base::ListValue ids;
    if (const base::ListValue* targets =
            payload ? payload->FindList("targetIds") : nullptr) {
      for (const base::Value& v : *targets) {
        ids.Append(framework_ && v.is_string()
                       ? framework_->TabIdForCdpTarget(v.GetString().c_str())
                       : -1);
      }
    }
    out->Set("tabIds", std::move(ids));
    out->Set("ok", true);
    return true;
  }

  // --- Tab groups, the skill dialect (scope + stable tab ids) --------------
  if (type.rfind("agentSpace.tabGroups.", 0) == 0) {
    const std::string op = type.substr(21);
    std::string error;
    const int64_t browser = ResolveScopeBrowser(payload, &error);
    if (!browser || !framework_) {
      return fail(error.empty() ? "space_not_open" : error);
    }
    const std::string* token = payload ? payload->FindString("token")
                                       : nullptr;
    const base::ListValue* tab_ids = payload ? payload->FindList("tabIds")
                                             : nullptr;
    std::string tab_ids_json = "[]";
    if (tab_ids) {
      base::JSONWriter::Write(*tab_ids, &tab_ids_json);
    }
    if (op == "list") {
      base::ListValue groups;
      std::optional<base::Value> parsed = base::JSONReader::Read(
          framework_->TabGroupsJson(browser), base::JSON_PARSE_RFC);
      const base::ListValue tabs = tabs_of(browser);
      if (parsed && parsed->is_list()) {
        for (const base::Value& v : parsed->GetList()) {
          const base::DictValue* g = v.GetIfDict();
          const std::string* id = g ? g->FindString("id") : nullptr;
          if (!id) {
            continue;
          }
          base::DictValue row;
          row.Set("token", *id);
          if (const std::string* title = g->FindString("title")) {
            row.Set("title", *title);
          }
          if (const std::string* color = g->FindString("color")) {
            row.Set("color", *color);
          }
          row.Set("collapsed", g->FindBool("collapsed").value_or(false));
          base::ListValue member_ids;
          for (const base::Value& t : tabs) {
            const base::DictValue* tab = t.GetIfDict();
            const std::string* group =
                tab ? tab->FindString("groupId") : nullptr;
            if (group && *group == *id) {
              member_ids.Append(tab->FindInt("id").value_or(-1));
            }
          }
          row.Set("tabIds", std::move(member_ids));
          groups.Append(std::move(row));
        }
      }
      out->Set("groups", std::move(groups));
      out->Set("ok", true);
      return true;
    }
    if (op == "create") {
      const std::string created =
          framework_->CreateTabGroupForTabs(browser, tab_ids_json.c_str());
      if (created.empty()) {
        return fail("create_failed");
      }
      if (const std::string* title =
              payload ? payload->FindString("title") : nullptr) {
        framework_->RenameTabGroup(browser, created.c_str(), title->c_str());
      }
      if (const std::string* color =
              payload ? payload->FindString("color") : nullptr) {
        framework_->SetTabGroupColor(browser, created.c_str(),
                                     color->c_str());
      }
      out->Set("token", created);
      out->Set("ok", true);
      return true;
    }
    if (op == "update") {
      if (!token) {
        return fail("invalid_payload");
      }
      bool ok = true;
      if (const std::string* title =
              payload ? payload->FindString("title") : nullptr) {
        ok &= framework_->RenameTabGroup(browser, token->c_str(),
                                         title->c_str());
      }
      if (const std::string* color =
              payload ? payload->FindString("color") : nullptr) {
        ok &= framework_->SetTabGroupColor(browser, token->c_str(),
                                           color->c_str());
      }
      if (std::optional<bool> collapsed =
              payload ? payload->FindBool("collapsed") : std::nullopt) {
        ok &= framework_->SetTabGroupCollapsed(browser, token->c_str(),
                                               *collapsed);
      }
      out->Set("ok", ok);
      return true;
    }
    if (op == "addTabs") {
      if (!token) {
        return fail("invalid_payload");
      }
      out->Set("ok", framework_->AddTabsToGroup(browser, token->c_str(),
                                                tab_ids_json.c_str()));
      return true;
    }
    if (op == "removeTabs") {
      out->Set("ok", framework_->RemoveTabsFromGroupByIds(
                         browser, tab_ids_json.c_str()));
      return true;
    }
    if (op == "ungroup" || op == "close") {
      if (!token) {
        return fail("invalid_payload");
      }
      out->Set("ok", op == "ungroup"
                         ? framework_->UngroupTabGroup(browser,
                                                       token->c_str())
                         : framework_->CloseTabGroup(browser,
                                                     token->c_str()));
      return true;
    }
    return fail("invalid_payload");
  }

  // --- Split view, the skill dialect ----------------------------------------
  if (type.rfind("agentSpace.splitView.", 0) == 0) {
    const std::string op = type.substr(21);
    std::string error;
    const int64_t browser = ResolveScopeBrowser(payload, &error);
    if (!browser || !framework_) {
      return fail(error.empty() ? "space_not_open" : error);
    }
    const std::string* split_id = payload ? payload->FindString("splitId")
                                          : nullptr;
    if (op == "list") {
      base::ListValue splits;
      std::optional<base::Value> parsed = base::JSONReader::Read(
          framework_->SplitViewsJson(browser), base::JSON_PARSE_RFC);
      if (parsed && parsed->is_list()) {
        for (const base::Value& v : parsed->GetList()) {
          const base::DictValue* s = v.GetIfDict();
          const std::string* id = s ? s->FindString("id") : nullptr;
          if (!id) {
            continue;
          }
          base::DictValue row;
          row.Set("splitId", *id);
          if (const std::string* layout = s->FindString("layout")) {
            row.Set("layout", *layout);
          }
          row.Set("ratio", s->FindDouble("ratio").value_or(0.5));
          row.Set("primaryTabId", s->FindInt("primaryTabId").value_or(-1));
          row.Set("secondaryTabId",
                  s->FindInt("secondaryTabId").value_or(-1));
          splits.Append(std::move(row));
        }
      }
      out->Set("splits", std::move(splits));
      out->Set("ok", true);
      return true;
    }
    if (op == "create") {
      const int primary =
          payload ? payload->FindInt("primaryTabId").value_or(-1) : -1;
      const int secondary =
          payload ? payload->FindInt("secondaryTabId").value_or(-1) : -1;
      const std::string* layout = payload ? payload->FindString("layout")
                                          : nullptr;
      const std::string created = framework_->CreateSplitViewForTabs(
          browser, primary, secondary, layout ? layout->c_str() : "");
      if (created.empty()) {
        return fail("create_failed");
      }
      out->Set("splitId", created);
      out->Set("ok", true);
      return true;
    }
    if (!split_id) {
      return fail("invalid_payload");
    }
    if (op == "update") {
      const std::string* layout = payload ? payload->FindString("layout")
                                          : nullptr;
      const double ratio =
          payload ? payload->FindDouble("ratio").value_or(-1.0) : -1.0;
      out->Set("ok", framework_->UpdateSplitView(
                         browser, split_id->c_str(),
                         layout ? layout->c_str() : "", ratio));
      return true;
    }
    if (op == "swap") {
      out->Set("ok", framework_->SwapSplitView(browser, split_id->c_str()));
      return true;
    }
    if (op == "remove") {
      out->Set("ok",
               framework_->RemoveSplitById(browser, split_id->c_str()));
      return true;
    }
    return fail("invalid_payload");
  }

  // --- Downloads (observe and control only — never open) --------------------
  if (type.rfind("agentSpace.downloads.", 0) == 0) {
    const std::string op = type.substr(21);
    std::string error;
    const int64_t browser = ResolveScopeBrowser(payload, &error);
    if (!browser || !framework_) {
      return fail(error.empty() ? "space_not_open" : error);
    }
    if (op == "list") {
      std::optional<base::Value> parsed = base::JSONReader::Read(
          framework_->DownloadsJsonForAgent(browser), base::JSON_PARSE_RFC);
      out->Set("downloads", parsed && parsed->is_list()
                                ? std::move(*parsed)
                                : base::Value(base::ListValue()));
      out->Set("ok", true);
      return true;
    }
    const std::string* guid = payload ? payload->FindString("guid") : nullptr;
    if (!guid || guid->empty()) {
      return fail("invalid_payload");
    }
    if (op == "get") {
      std::optional<base::Value> parsed = base::JSONReader::Read(
          framework_->DownloadsJsonForAgent(browser), base::JSON_PARSE_RFC);
      if (parsed && parsed->is_list()) {
        for (const base::Value& v : parsed->GetList()) {
          const base::DictValue* d = v.GetIfDict();
          const std::string* g = d ? d->FindString("guid") : nullptr;
          if (g && *g == *guid) {
            out->Set("download", d->Clone());
            out->Set("ok", true);
            return true;
          }
        }
      }
      return fail("unknown_download");
    }
    if (op == "pause" || op == "resume" || op == "cancel" || op == "remove") {
      if (!framework_->DownloadActionByGuid(browser, guid->c_str(),
                                            op.c_str())) {
        return fail("unknown_download");
      }
      out->Set("ok", true);
      return true;
    }
    return fail("invalid_payload");
  }

  // --- User-Space tabs and focus (the skill's spaces.* family) --------------
  if (type == "agentSpace.spaces.listTabs") {
    const std::string* space_id = payload ? payload->FindString("spaceId")
                                          : nullptr;
    if (!space_id || space_id->empty()) {
      return fail("invalid_payload");
    }
    const int64_t browser = BrowserForSpace(*space_id);
    if (!browser || !framework_) {
      return fail("space_not_open");
    }
    base::ListValue rows;
    for (const base::Value& v : tabs_of(browser)) {
      const base::DictValue* tab = v.GetIfDict();
      if (!tab) {
        continue;
      }
      base::DictValue row;
      row.Set("tabId", tab->FindInt("id").value_or(-1));
      if (const std::string* url = tab->FindString("url")) {
        row.Set("url", *url);
      }
      if (const std::string* title = tab->FindString("title")) {
        row.Set("title", *title);
      }
      row.Set("active", tab->FindBool("active").value_or(false));
      rows.Append(std::move(row));
    }
    out->Set("tabs", std::move(rows));
    out->Set("windowId", static_cast<double>(browser));
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.spaces.openTab") {
    const std::string* space_id = payload ? payload->FindString("spaceId")
                                          : nullptr;
    const std::string* url = payload ? payload->FindString("url") : nullptr;
    if (!space_id || space_id->empty() || !url || url->empty()) {
      return fail("invalid_payload");
    }
    const int64_t browser = BrowserForSpace(*space_id);
    if (!browser || !framework_) {
      return fail("space_not_open");
    }
    const bool activate =
        payload->FindBool("activate").value_or(true);
    int previous_active = -1;
    if (!activate) {
      for (const base::Value& v : tabs_of(browser)) {
        const base::DictValue* tab = v.GetIfDict();
        if (tab && tab->FindBool("active").value_or(false)) {
          previous_active = tab->FindInt("index").value_or(-1);
          break;
        }
      }
    }
    // Bypassed like every client-side deliberate open: a URL rule pointing
    // at another Space must not bounce a tab the skill placed HERE.
    framework_->OpenTabBypassingSpaceRouting(browser, url->c_str());
    if (!activate && previous_active >= 0) {
      // The new tab opened foreground; hand the selection back. The old
      // active tab's index is unchanged by an append/adjacent insert.
      framework_->ActivateTab(browser, previous_active);
    }
    out->Set("windowId", static_cast<double>(browser));
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.spaces.focus") {
    // A surfaced agent window in the foreground answers as the agent Space.
    const HWND foreground = ::GetForegroundWindow();
    for (const auto& [slot, controller] : windows_) {
      if (!controller->is_agent() || !foreground ||
          controller->shell_hwnd() != foreground) {
        continue;
      }
      PhiAgentSpaces::Space* agent = agent_spaces_.FindByWindow(slot);
      if (!agent) {
        break;
      }
      out->Set("spaceId", agent->space_id);
      out->Set("spaceName", "Agent Space");
      out->Set("isAgentSpace", true);
      out->Set("isIncognito", false);
      out->Set("windowId", static_cast<double>(ResolveBrowser(slot)));
      out->Set("ok", true);
      return true;
    }
    PhiWindowController* controller = FocusedUserController();
    if (!controller) {
      return fail("no_window");
    }
    const std::string space_id = controller->active_space();
    std::string name = space_id;
    for (const Space& space : spaces_) {
      if (space.id == space_id) {
        name = space.name;
        break;
      }
    }
    out->Set("spaceId", space_id);
    out->Set("spaceName", name);
    out->Set("isAgentSpace", false);
    out->Set("isIncognito", controller->is_incognito());
    const int64_t browser = controller->active_browser();
    if (browser) {
      out->Set("windowId", static_cast<double>(browser));
      for (const base::Value& v : tabs_of(browser)) {
        const base::DictValue* tab = v.GetIfDict();
        if (!tab || !tab->FindBool("active").value_or(false)) {
          continue;
        }
        base::DictValue row;
        row.Set("tabId", tab->FindInt("id").value_or(-1));
        if (const std::string* url = tab->FindString("url")) {
          row.Set("url", *url);
        }
        if (const std::string* title = tab->FindString("title")) {
          row.Set("title", *title);
        }
        out->Set("tab", std::move(row));
        break;
      }
    }
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.spaces.activate") {
    const std::string* space_id = payload ? payload->FindString("spaceId")
                                          : nullptr;
    if (!space_id || space_id->empty()) {
      return fail("invalid_payload");
    }
    // A slot already hosting the Space surfaces it there; otherwise the
    // user's focused window takes it (cold-spawning the browser).
    for (const auto& [slot, controller] : windows_) {
      if (controller->is_agent() || !controller->HasSpace(*space_id)) {
        continue;
      }
      const bool ok = controller->ShowSpace(*space_id);
      if (ok) {
        PushSpaceMenu();
        if (controller->shell_hwnd()) {
          ::SetForegroundWindow(controller->shell_hwnd());
        }
        controller->RefreshTabs();
      }
      out->Set("ok", ok);
      return true;
    }
    PhiWindowController* focused = FocusedUserController();
    if (!focused) {
      return fail("no_window");
    }
    bool pending = false, cold = false;
    const bool ok =
        SwitchSlotToSpace(focused->window_id(), *space_id, &pending, &cold);
    if (!ok) {
      return fail("unknown_space");
    }
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.spaces.update") {
    const std::string* space_id = payload ? payload->FindString("spaceId")
                                          : nullptr;
    if (!space_id || space_id->empty()) {
      return fail("invalid_payload");
    }
    Space* entry = nullptr;
    for (Space& space : spaces_) {
      if (space.id == *space_id) {
        entry = &space;
        break;
      }
    }
    if (!entry) {
      return fail("unknown_space");
    }
    bool ok = true;
    if (const std::string* name = payload->FindString("name")) {
      ok &= RenameSpace(*space_id, *name);
    }
    if (const std::string* icon = payload->FindString("iconName")) {
      if (PhiIconIsValid(*icon)) {
        entry->icon = *icon;
        SaveSpaces();
        refresh_all();
      } else {
        ok = false;
      }
    }
    if (const std::string* color = payload->FindString("colorHex")) {
      entry->color_hex = *color;
      SaveSpaces();
      // The hex drives the Space's tint when no theme is pinned.
      RebuildSpacePalettes();
      for (const auto& [slot, controller] : windows_) {
        controller->ApplyPalette();
      }
    }
    out->Set("spaceId", *space_id);
    out->Set("ok", ok);
    return true;
  }

  // --- Pinned tabs, the skill dialect (per-profile records) -----------------
  if (type.rfind("agentSpace.pinnedTabs.", 0) == 0) {
    const std::string op = type.substr(22);
    const std::string* given_profile =
        payload ? payload->FindString("profileId") : nullptr;
    if (op == "list") {
      std::string profile_id, space_id;
      if (!DialectOwner(given_profile ? *given_profile : "", &profile_id,
                        &space_id)) {
        return fail("no_window");
      }
      base::ListValue rows;
      for (const PhiRow& row : tab_store_.PinnedFor(profile_id, space_id)) {
        base::DictValue entry;
        entry.Set("guid", row.guid);
        entry.Set("url", row.url);
        entry.Set("title", row.title);
        entry.Set("index", row.index);
        entry.Set("profileId",
                  row.profile_id.empty() ? profile_id : row.profile_id);
        rows.Append(std::move(entry));
      }
      out->Set("pinnedTabs", std::move(rows));
      out->Set("ok", true);
      return true;
    }
    if (op == "add") {
      const std::string* url = payload ? payload->FindString("url") : nullptr;
      if (!url || url->empty()) {
        return fail("invalid_payload");
      }
      std::string profile_id, space_id;
      if (!DialectOwner(given_profile ? *given_profile : "", &profile_id,
                        &space_id)) {
        return fail("no_window");
      }
      const std::string* title = payload->FindString("title");
      const std::string guid = tab_store_.AddPinned(
          profile_id, space_id, title ? *title : "", *url,
          /*favicon_base64=*/"", 0, 0);
      if (guid.empty()) {
        return fail("create_failed");
      }
      if (std::optional<int> index = payload->FindInt("index")) {
        tab_store_.Reorder(guid, *index);
      }
      refresh_all();
      out->Set("guid", guid);
      const PhiRow* row = tab_store_.Find(guid);
      out->Set("profileId", row && !row->profile_id.empty()
                                ? row->profile_id
                                : profile_id);
      out->Set("ok", true);
      return true;
    }
    const std::string* guid = payload ? payload->FindString("guid") : nullptr;
    if (!guid || !tab_store_.Contains(*guid)) {
      return fail("unknown_pin");
    }
    if (op == "update") {
      bool ok = true;
      if (const std::string* title = payload->FindString("title")) {
        ok &= RenameStoredRow(*guid, *title);
      }
      if (const std::string* url = payload->FindString("url")) {
        ok &= tab_store_.SetRowUrl(*guid, *url);
      }
      refresh_all();
      out->Set("ok", ok);
      return true;
    }
    if (op == "remove") {
      RemoveStoredRow(*guid);
      out->Set("ok", true);
      return true;
    }
    return fail("invalid_payload");
  }

  // --- Bookmarks, the skill's Space-addressed tree forms ---------------------
  // list/add collide with the client-UI dialect (windowId-scoped, flat);
  // the skill's payloads carry no windowId, which is the claim test.
  const bool bookmarks_tree_shape =
      type.rfind("agentSpace.bookmarks.", 0) == 0 &&
      (!payload || !payload->contains("windowId"));
  if (bookmarks_tree_shape &&
      (type == "agentSpace.bookmarks.list" ||
       type == "agentSpace.bookmarks.add" ||
       type == "agentSpace.bookmarks.addFolder" ||
       type == "agentSpace.bookmarks.update" ||
       type == "agentSpace.bookmarks.move")) {
    const std::string op = type.substr(21);
    // The target Space: an explicit spaceId, else the default Space.
    const Space* target = nullptr;
    if (const std::string* space_id =
            payload ? payload->FindString("spaceId") : nullptr) {
      for (const Space& space : spaces_) {
        if (space.id == *space_id) {
          target = &space;
          break;
        }
      }
      if (!target) {
        return fail("unknown_space");
      }
    } else if (!spaces_.empty()) {
      target = &spaces_.front();
      for (const Space& space : spaces_) {
        if (space.id == "1") {  // isDefault, when it still exists.
          target = &space;
          break;
        }
      }
    }
    if (op == "update" || op == "move") {
      const std::string* guid = payload ? payload->FindString("guid")
                                        : nullptr;
      const PhiRow* row = guid ? tab_store_.Find(*guid) : nullptr;
      if (!row || (row->type != PhiRowType::kBookmark &&
                   row->type != PhiRowType::kBookmarkFolder)) {
        return fail("unknown_bookmark");
      }
      const std::string owner_space = row->space_id;
      bool ok = true;
      if (op == "update") {
        if (const std::string* title = payload->FindString("title")) {
          ok &= RenameStoredRow(*guid, *title);
        }
        if (const std::string* url = payload->FindString("url")) {
          ok &= tab_store_.SetRowUrl(*guid, *url);
        }
      } else {
        const std::string* parent = payload->FindString("parentGuid");
        ok = tab_store_.Move(*guid, parent ? *parent : "",
                             payload->FindInt("index").value_or(-1));
      }
      refresh_all();
      out->Set("spaceId", owner_space);
      out->Set("ok", ok);
      return true;
    }
    if (!target) {
      return fail("unknown_space");
    }
    const std::string profile_id = target->profile_id;
    const std::string space_id = target->id;
    if (op == "list") {
      // Folders carry `children`, leaves carry `url` — the Mac tree shape.
      auto build = [&](auto&& self,
                       const std::string& parent) -> base::ListValue {
        base::ListValue nodes;
        for (const PhiRow& row :
             tab_store_.BookmarksFor(profile_id, space_id, parent)) {
          base::DictValue node;
          node.Set("guid", row.guid);
          node.Set("title", row.title);
          node.Set("index", row.index);
          if (row.type == PhiRowType::kBookmarkFolder) {
            node.Set("children", self(self, row.guid));
          } else {
            node.Set("url", row.url);
          }
          nodes.Append(std::move(node));
        }
        return nodes;
      };
      out->Set("bookmarks", build(build, ""));
      out->Set("spaceId", space_id);
      out->Set("ok", true);
      return true;
    }
    if (op == "add" || op == "addFolder") {
      const bool folder = op == "addFolder";
      const std::string* url = payload ? payload->FindString("url") : nullptr;
      const std::string* title = payload ? payload->FindString("title")
                                         : nullptr;
      if (folder ? (!title || title->empty()) : (!url || url->empty())) {
        return fail("invalid_payload");
      }
      std::string parent;
      if (const std::string* p =
              payload ? payload->FindString("parentGuid") : nullptr) {
        const PhiRow* parent_row = tab_store_.Find(*p);
        if (!parent_row || parent_row->type != PhiRowType::kBookmarkFolder ||
            parent_row->space_id != space_id) {
          return fail("unknown_folder");
        }
        parent = *p;
      }
      const std::string guid = tab_store_.AddBookmark(
          profile_id, space_id, parent, title ? *title : "",
          folder ? "" : *url, folder, /*favicon_base64=*/"", 0, 0);
      if (guid.empty()) {
        return fail("create_failed");
      }
      if (std::optional<int> index =
              payload ? payload->FindInt("index") : std::nullopt) {
        tab_store_.Reorder(guid, *index);
      }
      refresh_all();
      out->Set("guid", guid);
      out->Set("spaceId", space_id);
      out->Set("ok", true);
      return true;
    }
  }

  return false;
}

}  // namespace phi_client
