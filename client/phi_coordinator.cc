// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_coordinator.h"

#include <windows.h>

#include <algorithm>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"

namespace phi_client {

PhiCoordinator::PhiCoordinator() = default;
PhiCoordinator::~PhiCoordinator() = default;

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

  control_server_.Start(
      base::UTF8ToWide(user_data_dir_utf8 ? user_data_dir_utf8 : ""), this);
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
    if (framework_ && window_id) {
      framework_->CloseBrowserWindow(window_id);
    }
    out.Set("ok", framework_ && window_id ? true : false);
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
    spaces_.push_back({id, name, profile_id});
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
             type == "agentSpace.tabs.close") {
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
      ok = (type == "agentSpace.tabs.activate")
               ? framework_->ActivateTab(ResolveBrowser(window_id), index)
               : framework_->CloseTab(ResolveBrowser(window_id), index);
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
      window_id, static_cast<HWND>(hwnd), framework_);
  controller->RefreshTabs();
  windows_[window_id] = std::move(controller);
  browser_to_slot_[window_id] = window_id;
}

void PhiCoordinator::OnTabsChanged(int64_t window_id) {
  auto slot_it = browser_to_slot_.find(window_id);
  const int64_t slot = slot_it != browser_to_slot_.end() ? slot_it->second
                                                         : window_id;
  auto it = windows_.find(slot);
  if (it != windows_.end()) {
    it->second->RefreshTabs();
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
