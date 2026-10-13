// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sync_setup_window.h"

#include <commctrl.h>

#include <algorithm>
#include <utility>

#include "base/json/json_writer.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_sync_key_service.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiSyncSetupWindow";
// The Mac's setup window: 720x560, also its minimum.
constexpr int kWidthDip = 720;
constexpr int kHeightDip = 560;
constexpr int kPadDip = 40;
constexpr int kButtonHeightDip = 32;
constexpr int kRowHeightDip = 40;
constexpr int kPillWidthDip = 280;
constexpr UINT kPollMs = 3000;
constexpr UINT kCountdownMs = 1000;
constexpr int64_t kLocalJoinLifetimeMs = 15 * 60 * 1000;
constexpr int kIdCodeField = 101;
// Hit keys: control * kKeyStride + row.
constexpr int kKeyStride = 1000;

PhiSyncSetupWindow* g_window = nullptr;

int64_t NowMs() {
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}

std::wstring W(const std::string& utf8) {
  return base::UTF8ToWide(utf8);
}

std::wstring Countdown(int64_t remaining_ms) {
  const int64_t seconds = std::max<int64_t>(0, (remaining_ms + 999) / 1000);
  const int64_t minutes = seconds / 60;
  std::wstring secs = base::NumberToWString(seconds % 60);
  if (secs.size() < 2) {
    secs = L"0" + secs;
  }
  return base::NumberToWString(minutes) + L":" + secs;
}

}  // namespace

// --- Lifetime ---------------------------------------------------------------------

// static
void PhiSyncSetupWindow::Open(Host* host, HWND anchor) {
  if (!host || !host->SyncKeys()) {
    return;
  }
  if (!g_window || !g_window->alive()) {
    g_window = new PhiSyncSetupWindow(host);
    if (!g_window->OpenWindow(anchor)) {
      delete g_window;
      g_window = nullptr;
      return;
    }
    g_window->BeginSetup();
  }
  // A minimized window comes back; SW_SHOW alone leaves it on the taskbar.
  ::ShowWindow(g_window->hwnd(),
               ::IsIconic(g_window->hwnd()) ? SW_RESTORE : SW_SHOW);
  ::SetForegroundWindow(g_window->hwnd());
}

// static
PhiSyncSetupWindow* PhiSyncSetupWindow::Current() {
  return g_window && g_window->alive() ? g_window : nullptr;
}

// static
void PhiSyncSetupWindow::CloseForAccountChange() {
  if (PhiSyncSetupWindow* window = Current()) {
    window->busy_ = false;  // the account is gone; nothing left to protect
    window->Destroy();
  }
}

// static
void PhiSyncSetupWindow::NoteProfileCreated(const std::string& display_name,
                                            const std::string& profile_id) {
  PhiSyncSetupWindow* window = Current();
  if (!window || window->page_ != Page::kSubmitting ||
      window->creating_name_ != display_name) {
    return;
  }
  if (profile_id.empty()) {
    window->busy_ = false;
    window->ShowError(PhiStrF("sync.setup.error.createProfileFailed",
                              {W(display_name)}));
    return;
  }
  window->pending_decisions_[window->creating_].local_profile_id = profile_id;
  ++window->creating_;
  window->CreateNextProfile();
}

PhiSyncSetupWindow::PhiSyncSetupWindow(Host* host) : host_(host) {}

PhiSyncSetupWindow::~PhiSyncSetupWindow() {
  if (field_brush_) {
    ::DeleteObject(field_brush_);
  }
  if (bg_brush_) {
    ::DeleteObject(bg_brush_);
  }
}

bool PhiSyncSetupWindow::OpenWindow(HWND anchor) {
  ClassSpec spec;
  spec.name = kClassName;
  Options options;
  options.caption_buttons = kPhiCaptionClose;
  options.dialog_keys = kPhiDialogNone;  // Escape / Return handled here
  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                       bounds, nullptr, PhiStr("sync.setup.windowTitle"),
                       options)) {
    LOG(ERROR) << "[SyncSetup] CreateWindowEx failed err=" << ::GetLastError();
    return false;
  }
  DeleteWhenDestroyed();
  OnColorsChanged();
  const SIZE outer = chrome().OuterSize(kWidthDip, kHeightDip);
  ::SetWindowPos(hwnd(), nullptr, 0, 0, outer.cx, outer.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  CenterOn(anchor && ::IsWindow(anchor) ? anchor : nullptr);
  poll_timer_ = NewTimer();
  countdown_ = NewTimer();
  copied_timer_ = NewTimer();
  ShowPage(Page::kWorking);
  return true;
}

void PhiSyncSetupWindow::OnNcDestroy() {
  poll_timer_.Stop();
  countdown_.Stop();
  copied_timer_.Stop();
  // Replies still in flight find nothing.
  alive_.reset();
  if (!request_id_.empty() && host_->SyncKeys()) {
    host_->SyncKeys()->CancelJoin(request_id_);
  }
  if (g_window == this) {
    g_window = nullptr;
  }
  host_->SyncSetupClosed();
  PhiSecondaryWindow::OnNcDestroy();
}

void PhiSyncSetupWindow::OnColorsChanged() {
  if (bg_brush_) {
    ::DeleteObject(bg_brush_);
  }
  bg_brush_ = ::CreateSolidBrush(colors().bg);
  if (field_brush_) {
    ::DeleteObject(field_brush_);
  }
  field_brush_ = ::CreateSolidBrush(colors().card);
  if (code_field_) {
    PhiPopupThemeControl(code_field_, colors().dark);
  }
  if (alive()) {
    Invalidate();
  }
}

bool PhiSyncSetupWindow::CanClose() const {
  // The Mac's deferSetup: nothing while an operation that must finish runs,
  // and never while the recovery code awaits its acknowledgement.
  return !busy_ && page_ != Page::kShowingCode && page_ != Page::kSubmitting;
}

void PhiSyncSetupWindow::FinishLater() {
  if (!CanClose()) {
    return;
  }
  CancelJoin();
  Destroy();
}

LRESULT PhiSyncSetupWindow::OnWindowMessage(UINT message,
                                            WPARAM wparam,
                                            LPARAM lparam,
                                            bool* handled) {
  switch (message) {
    case WM_CLOSE:
      *handled = true;
      FinishLater();
      return 0;
    case WM_SIZE:
      if (alive()) {
        Layout();
      }
      break;
    case WM_CTLCOLOREDIT:
      if (reinterpret_cast<HWND>(lparam) == code_field_) {
        *handled = true;
        const HDC dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(dc, colors().label);
        ::SetBkColor(dc, colors().card);
        return reinterpret_cast<LRESULT>(field_brush_);
      }
      break;
    case WM_COMMAND:
      if (LOWORD(wparam) == kIdCodeField && HIWORD(wparam) == EN_CHANGE) {
        input_error_.clear();
        Layout();
        Invalidate();
      }
      break;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(message, wparam, lparam, handled);
}

void PhiSyncSetupWindow::OnEscape() {
  FinishLater();
}

void PhiSyncSetupWindow::OnEnter() {
  switch (page_) {
    case Page::kEnterCode:
      SubmitRecoveryCode();
      break;
    case Page::kIntroduction:
      StartBootstrap();
      break;
    default:
      break;
  }
}

void PhiSyncSetupWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  if (code_field_) {
    ::SendMessageW(code_field_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(CodeFont()), TRUE);
  }
  Layout();
}

void PhiSyncSetupWindow::OnTimerFired(UINT_PTR id) {
  if (id == poll_timer_.id()) {
    Poll();
  } else if (id == countdown_.id()) {
    if (page_ == Page::kWaiting) {
      Invalidate();
    } else {
      countdown_.Stop();
    }
  } else if (id == copied_timer_.id()) {
    copied_timer_.Stop();
    copied_ = false;
    Layout();
    Invalidate();
  }
}

// --- Flow --------------------------------------------------------------------------

void PhiSyncSetupWindow::ShowPage(Page page) {
  page_ = page;
  ++generation_;
  if (page != Page::kWaiting) {
    poll_timer_.Stop();
    countdown_.Stop();
    poll_in_flight_ = false;
  }
  if (page != Page::kEnterCode) {
    input_error_.clear();
  }
  if (page == Page::kEnterCode) {
    EnsureCodeField();
    ::SetWindowTextW(code_field_, L"");
  }
  if (code_field_) {
    ::ShowWindow(code_field_, page == Page::kEnterCode ? SW_SHOW : SW_HIDE);
  }
  Layout();
  Invalidate();
  if (page == Page::kEnterCode) {
    ::SetFocus(code_field_);
  }
}

void PhiSyncSetupWindow::ShowError(const std::wstring& message) {
  error_message_ = message;
  ShowPage(Page::kError);
}

void PhiSyncSetupWindow::BeginSetup() {
  PhiSyncKeyService* keys = host_->SyncKeys();
  if (!keys) {
    Destroy();
    return;
  }
  ShowPage(Page::kWorking);
  const int generation = generation_;
  std::weak_ptr<int> alive = alive_;
  keys->BeginSetup([this, alive, generation](
                       PhiSyncKeyService::SetupRoute route,
                       keys::KeyStatus status) {
    if (alive.expired() || generation != generation_) {
      return;
    }
    switch (route) {
      case PhiSyncKeyService::SetupRoute::kDone:
        Destroy();  // Already paired: nothing to set up.
        return;
      case PhiSyncKeyService::SetupRoute::kReadyToPair:
        LoadPairing();
        return;
      case PhiSyncKeyService::SetupRoute::kNotSignedIn:
        ShowError(PhiStr("sync.setup.error.signInRequired"));
        return;
      case PhiSyncKeyService::SetupRoute::kChooseJoin:
        ShowPage(Page::kChooseJoin);
        return;
      case PhiSyncKeyService::SetupRoute::kIntroduction:
        ShowPage(Page::kIntroduction);
        return;
      case PhiSyncKeyService::SetupRoute::kNeedsReconfiguration:
        ShowError(PhiStr("sync.reconfigure.returnToSettings"));
        return;
      case PhiSyncKeyService::SetupRoute::kError:
        LOG(WARNING) << "[SyncSetup] setup check failed: "
                     << keys::KeyStatusName(status);
        ShowError(PhiStr("sync.setup.error.connection"));
        return;
    }
  });
}

void PhiSyncSetupWindow::StartBootstrap() {
  PhiSyncKeyService* keys = host_->SyncKeys();
  if (!keys || busy_) {
    return;
  }
  busy_ = true;
  ShowPage(Page::kWorking);
  std::weak_ptr<int> alive = alive_;
  keys->Bootstrap([this, alive](keys::KeyStatus status, std::string code) {
    if (alive.expired()) {
      return;
    }
    busy_ = false;
    if (status.ok()) {
      recovery_code_ = W(code);
      ShowPage(Page::kShowingCode);
    } else if (status.kind == keys::KeyErrorKind::kAlreadyInitialized) {
      // Another device set the account up meanwhile: join it instead.
      ShowPage(Page::kChooseJoin);
    } else {
      LOG(WARNING) << "[SyncSetup] bootstrap failed: "
                   << keys::KeyStatusName(status);
      ShowError(PhiStr("sync.setup.error.connection"));
    }
  });
}

void PhiSyncSetupWindow::StartJoinRequest() {
  PhiSyncKeyService* keys = host_->SyncKeys();
  if (!keys || busy_) {
    return;
  }
  busy_ = true;
  ShowPage(Page::kWorking);
  const int generation = generation_;
  std::weak_ptr<int> alive = alive_;
  keys->RequestJoin([this, alive, generation](keys::KeyStatus status,
                                              keys::JoinTicket ticket) {
    if (alive.expired()) {
      return;
    }
    busy_ = false;
    if (generation != generation_) {
      return;
    }
    if (status.ok()) {
      request_id_ = ticket.request_id;
      verification_code_ = W(ticket.verification_code);
      deadline_ms_ = NowMs() + kLocalJoinLifetimeMs;
      ShowPage(Page::kWaiting);
      poll_timer_.Start(kPollMs);
      countdown_.Start(kCountdownMs);
    } else if (status.kind == keys::KeyErrorKind::kTooManyPending) {
      ShowError(PhiStr("sync.setup.error.tooManyRequests"));
    } else if (status.kind != keys::KeyErrorKind::kCancelled) {
      LOG(WARNING) << "[SyncSetup] join request failed: "
                   << keys::KeyStatusName(status);
      ShowError(PhiStr("sync.setup.error.connection"));
    }
  });
}

void PhiSyncSetupWindow::Poll() {
  PhiSyncKeyService* keys = host_->SyncKeys();
  if (!keys || page_ != Page::kWaiting || poll_in_flight_ ||
      request_id_.empty()) {
    return;
  }
  poll_in_flight_ = true;
  const int generation = generation_;
  std::weak_ptr<int> alive = alive_;
  keys->PollJoin(request_id_, [this, alive, generation](keys::KeyStatus status,
                                                        keys::JoinPoll poll) {
    if (alive.expired() || generation != generation_) {
      return;
    }
    poll_in_flight_ = false;
    if (!status.ok()) {
      if (status.kind != keys::KeyErrorKind::kCancelled) {
        input_error_ = PhiStr("sync.setup.error.connection");
        Invalidate();
      }
      return;  // keep polling
    }
    input_error_.clear();
    switch (poll.state) {
      case keys::JoinPollState::kApproved:
        request_id_.clear();
        LoadPairing();
        return;
      case keys::JoinPollState::kDenied:
        request_id_.clear();
        ShowPage(Page::kDenied);
        return;
      case keys::JoinPollState::kExpired:
        request_id_.clear();
        ShowPage(Page::kExpired);
        return;
      case keys::JoinPollState::kPending:
        if (poll.deadline_ms > 0) {
          deadline_ms_ = poll.deadline_ms;
        }
        if (NowMs() > deadline_ms_) {
          CancelJoin();
          ShowPage(Page::kExpired);
          return;
        }
        Invalidate();
        return;
    }
  });
}

void PhiSyncSetupWindow::CancelJoin() {
  poll_timer_.Stop();
  countdown_.Stop();
  poll_in_flight_ = false;
  if (PhiSyncKeyService* keys = host_->SyncKeys()) {
    keys->CancelJoin(request_id_);
  }
  request_id_.clear();
}

void PhiSyncSetupWindow::ShowRecoveryEntry() {
  CancelJoin();
  ShowPage(Page::kEnterCode);
}

void PhiSyncSetupWindow::SubmitRecoveryCode() {
  PhiSyncKeyService* keys = host_->SyncKeys();
  const std::wstring code = CodeFieldText();
  if (!keys || busy_ || page_ != Page::kEnterCode || code.empty()) {
    return;
  }
  busy_ = true;
  input_error_.clear();
  Layout();
  Invalidate();
  std::weak_ptr<int> alive = alive_;
  keys->JoinWithRecoveryCode(
      base::WideToUTF8(code), [this, alive](keys::KeyStatus status) {
        if (alive.expired()) {
          return;
        }
        busy_ = false;
        if (status.ok()) {
          LoadPairing();
          return;
        }
        if (status.kind == keys::KeyErrorKind::kBadRecoveryCode) {
          input_error_ = PhiStr("sync.setup.error.badRecoveryCode");
        } else if (status.kind == keys::KeyErrorKind::kHttp &&
                   status.http_status == 401) {
          input_error_ = PhiStr("sync.setup.error.signInRequired");
        } else {
          LOG(WARNING) << "[SyncSetup] recovery join failed: "
                       << keys::KeyStatusName(status);
          input_error_ = PhiStr("sync.setup.error.connection");
        }
        Layout();
        Invalidate();
      });
}

void PhiSyncSetupWindow::LoadPairing() {
  PhiSyncKeyService* keys = host_->SyncKeys();
  if (!keys) {
    Destroy();
    return;
  }
  ShowPage(Page::kWorking);
  const int generation = generation_;
  std::weak_ptr<int> alive = alive_;
  keys->StartPairing([this, alive, generation](
                         keys::KeyStatus status,
                         keys::PairingCandidates candidates) {
    if (alive.expired() || generation != generation_) {
      return;
    }
    if (!status.ok()) {
      LOG(WARNING) << "[SyncSetup] pairing load failed: "
                   << keys::KeyStatusName(status);
      ShowError(PhiStr("sync.setup.error.profileLoadFailed"));
      return;
    }
    candidates_ = std::move(candidates);
    SeedSelections();
    // A brand-new account (nothing to match against) finishes by itself:
    // every local profile registers as new.
    if (candidates_.remotes.empty()) {
      ApplyDecisions();
      return;
    }
    ShowPage(Page::kProfiles);
    input_error_ = std::move(profiles_error_);
    profiles_error_.clear();
    Layout();
    Invalidate();
  });
}

void PhiSyncSetupWindow::ContinueProfiles() {
  if (page_ != Page::kProfiles || !AllRowsDecided()) {
    return;
  }
  pending_decisions_ = BuildDecisions();
  // "Create on this PC" rows first need their Chromium profiles; they are
  // created one at a time, in order, then everything applies at once.
  std::stable_partition(pending_decisions_.begin(), pending_decisions_.end(),
                        [](const keys::PairingDecision& d) {
                          return d.kind ==
                                 keys::PairingDecision::Kind::kCreateLocal;
                        });
  creating_ = 0;
  busy_ = true;
  ShowPage(Page::kSubmitting);
  CreateNextProfile();
}

void PhiSyncSetupWindow::CreateNextProfile() {
  if (creating_ < pending_decisions_.size() &&
      pending_decisions_[creating_].kind ==
          keys::PairingDecision::Kind::kCreateLocal &&
      pending_decisions_[creating_].local_profile_id.empty()) {
    creating_name_ = UniqueProfileName(pending_decisions_[creating_].display_name);
    host_->SyncSetupCreateProfile(creating_name_);
    return;  // NoteProfileCreated continues
  }
  creating_name_.clear();
  ApplyDecisions();
}

void PhiSyncSetupWindow::ApplyDecisions() {
  PhiSyncKeyService* keys = host_->SyncKeys();
  if (!keys) {
    Destroy();
    return;
  }
  std::vector<keys::PairingDecision> decisions =
      page_ == Page::kSubmitting ? pending_decisions_ : BuildDecisions();
  busy_ = true;
  if (page_ != Page::kSubmitting) {
    ShowPage(Page::kSubmitting);
  }
  std::weak_ptr<int> alive = alive_;
  keys->ApplyDecisions(std::move(decisions), [this,
                                              alive](keys::KeyStatus status) {
    if (alive.expired()) {
      return;
    }
    busy_ = false;
    pending_decisions_.clear();
    if (status.ok()) {
      Destroy();  // Sync starts: the keys reach Chromium now.
      return;
    }
    LOG(WARNING) << "[SyncSetup] applying decisions failed: "
                 << keys::KeyStatusName(status);
    // Reload so the page shows whatever did land, with the error.
    profiles_error_ = PhiStr("sync.setup.error.profileDecisionFailed");
    LoadPairing();
  });
}

// --- The Profiles model --------------------------------------------------------

void PhiSyncSetupWindow::SeedSelections() {
  // ProfilePairingModel.initialSelections: each local takes the first
  // readable remote whose name equals its own (each remote at most once),
  // else registers as new.
  local_choice_.assign(candidates_.locals.size(), kRegisterNew);
  std::vector<bool> used(candidates_.remotes.size(), false);
  for (size_t l = 0; l < candidates_.locals.size(); ++l) {
    for (size_t r = 0; r < candidates_.remotes.size(); ++r) {
      const keys::RemoteProfile& remote = candidates_.remotes[r];
      if (!used[r] && remote.name &&
          *remote.name == candidates_.locals[l].display_name) {
        local_choice_[l] = static_cast<int>(r);
        used[r] = true;
        break;
      }
    }
  }
  remote_choice_.assign(candidates_.remotes.size(), RemoteDecision());
}

bool PhiSyncSetupWindow::RemoteClaimedByLocal(int remote) const {
  return std::find(local_choice_.begin(), local_choice_.end(), remote) !=
         local_choice_.end();
}

bool PhiSyncSetupWindow::RemoteAssignmentValid(int remote) const {
  const RemoteDecision& d = remote_choice_[static_cast<size_t>(remote)];
  if (d.choice != RemoteChoice::kAssign || d.local < 0 ||
      static_cast<size_t>(d.local) >= local_choice_.size() ||
      local_choice_[static_cast<size_t>(d.local)] != kRegisterNew) {
    return false;
  }
  // One remote per local: the first remote assigned to it wins.
  for (int r = 0; r < remote; ++r) {
    const RemoteDecision& other = remote_choice_[static_cast<size_t>(r)];
    if (other.choice == RemoteChoice::kAssign && other.local == d.local &&
        !RemoteClaimedByLocal(r)) {
      return false;
    }
  }
  return true;
}

bool PhiSyncSetupWindow::AllRowsDecided() const {
  for (size_t r = 0; r < candidates_.remotes.size(); ++r) {
    const int remote = static_cast<int>(r);
    if (!candidates_.remotes[r].name || RemoteClaimedByLocal(remote)) {
      continue;  // read-only, or taken by a local's own picker
    }
    const RemoteDecision& d = remote_choice_[r];
    if (d.choice == RemoteChoice::kCreate) {
      continue;
    }
    if (!RemoteAssignmentValid(remote)) {
      return false;
    }
  }
  return true;
}

std::vector<keys::PairingDecision> PhiSyncSetupWindow::BuildDecisions() const {
  std::vector<keys::PairingDecision> out;
  std::vector<std::string> assigned(candidates_.locals.size());
  for (size_t r = 0; r < candidates_.remotes.size(); ++r) {
    const int remote = static_cast<int>(r);
    if (!candidates_.remotes[r].name || RemoteClaimedByLocal(remote)) {
      continue;
    }
    const RemoteDecision& d = remote_choice_[r];
    if (d.choice == RemoteChoice::kCreate) {
      keys::PairingDecision decision;
      decision.kind = keys::PairingDecision::Kind::kCreateLocal;
      decision.remote_uuid = candidates_.remotes[r].uuid;
      decision.display_name = *candidates_.remotes[r].name;
      out.push_back(std::move(decision));
    } else if (RemoteAssignmentValid(remote)) {
      assigned[static_cast<size_t>(d.local)] = candidates_.remotes[r].uuid;
    }
  }
  for (size_t l = 0; l < candidates_.locals.size(); ++l) {
    keys::PairingDecision decision;
    decision.local_profile_id = candidates_.locals[l].profile_id;
    decision.display_name = candidates_.locals[l].display_name;
    if (local_choice_[l] != kRegisterNew) {
      decision.kind = keys::PairingDecision::Kind::kAdopt;
      decision.remote_uuid =
          candidates_.remotes[static_cast<size_t>(local_choice_[l])].uuid;
    } else if (!assigned[l].empty()) {
      decision.kind = keys::PairingDecision::Kind::kAdopt;
      decision.remote_uuid = assigned[l];
    } else {
      decision.kind = keys::PairingDecision::Kind::kRegisterNew;
    }
    out.push_back(std::move(decision));
  }
  return out;
}

std::wstring PhiSyncSetupWindow::RemoteName(int remote) const {
  const keys::RemoteProfile& r =
      candidates_.remotes[static_cast<size_t>(remote)];
  if (r.name) {
    return W(*r.name);
  }
  return PhiStrF("sync.setup.profiles.unnamed", {W(r.uuid.substr(0, 8))});
}

std::string PhiSyncSetupWindow::UniqueProfileName(
    const std::string& raw) const {
  const std::string base =
      raw.find_first_not_of(" \t\r\n") == std::string::npos
          ? base::WideToUTF8(PhiStr("sync.setup.profiles.fallbackName"))
          : raw;
  if (!host_->SyncProfileNameExists(base)) {
    return base;
  }
  for (int suffix = 2; suffix <= 50; ++suffix) {
    const std::string candidate =
        base + " (" + base::NumberToString(suffix) + ")";
    if (!host_->SyncProfileNameExists(candidate)) {
      return candidate;
    }
  }
  return base + " (" + keys::NewLowercaseUuid().substr(0, 4) + ")";
}

void PhiSyncSetupWindow::PickLocal(int row) {
  const size_t l = static_cast<size_t>(row);
  HMENU menu = ::CreatePopupMenu();
  // The remotes this local may take: readable, not another local's pick.
  std::vector<int> ids;
  for (size_t r = 0; r < candidates_.remotes.size(); ++r) {
    const int remote = static_cast<int>(r);
    if (!candidates_.remotes[r].name ||
        (RemoteClaimedByLocal(remote) && local_choice_[l] != remote)) {
      continue;
    }
    ids.push_back(remote);
    // Profile names are the user's: an '&' is a character.
    ::AppendMenuW(menu, MF_STRING | (local_choice_[l] == remote ? MF_CHECKED : 0),
                  ids.size(), PhiEscapeMenuText(RemoteName(remote)).c_str());
  }
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  const UINT register_id = static_cast<UINT>(ids.size() + 1);
  ::AppendMenuW(menu,
                MF_STRING | (local_choice_[l] == kRegisterNew ? MF_CHECKED : 0),
                register_id, PhiStr("sync.setup.profiles.registerNew"));
  POINT pt = {rows_[l].pill_rect.left, rows_[l].pill_rect.bottom};
  ::ClientToScreen(hwnd(), &pt);
  const UINT picked = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN, pt.x, pt.y, 0,
      hwnd(), nullptr));
  ::DestroyMenu(menu);
  if (picked == 0) {
    return;
  }
  local_choice_[l] = picked == register_id
                         ? kRegisterNew
                         : ids[static_cast<size_t>(picked - 1)];
  Layout();
  Invalidate();
}

void PhiSyncSetupWindow::PickRemote(int row) {
  const size_t r = static_cast<size_t>(row);
  HMENU menu = ::CreatePopupMenu();
  const RemoteDecision& current = remote_choice_[r];
  const bool undecided = current.choice == RemoteChoice::kUndecided ||
                         (current.choice == RemoteChoice::kAssign &&
                          !RemoteAssignmentValid(row));
  constexpr UINT kChoose = 1;
  constexpr UINT kCreate = 2;
  constexpr UINT kAssignBase = 100;
  ::AppendMenuW(menu, MF_STRING | (undecided ? MF_CHECKED : 0), kChoose,
                PhiStr("sync.setup.profiles.choose"));
  ::AppendMenuW(
      menu,
      MF_STRING | (current.choice == RemoteChoice::kCreate ? MF_CHECKED : 0),
      kCreate, PhiStr("sync.setup.profiles.createHere"));
  // "Assign to X": locals still registering as new, not taken elsewhere.
  for (size_t l = 0; l < candidates_.locals.size(); ++l) {
    if (local_choice_[l] != kRegisterNew) {
      continue;
    }
    bool taken = false;
    for (size_t other = 0; other < remote_choice_.size(); ++other) {
      if (other != r && remote_choice_[other].choice == RemoteChoice::kAssign &&
          remote_choice_[other].local == static_cast<int>(l) &&
          RemoteAssignmentValid(static_cast<int>(other))) {
        taken = true;
      }
    }
    if (taken) {
      continue;
    }
    const bool checked = !undecided && current.choice == RemoteChoice::kAssign &&
                         current.local == static_cast<int>(l);
    ::AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0),
                  kAssignBase + static_cast<UINT>(l),
                  PhiStrF("sync.setup.profiles.assignTo",
                          {PhiEscapeMenuText(
                              W(candidates_.locals[l].display_name))})
                      .c_str());
  }
  const PickerRow& picker =
      rows_[static_cast<size_t>(remote_header_index_ + 1) + r];
  POINT pt = {picker.pill_rect.left, picker.pill_rect.bottom};
  ::ClientToScreen(hwnd(), &pt);
  const UINT picked = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN, pt.x, pt.y, 0,
      hwnd(), nullptr));
  ::DestroyMenu(menu);
  if (picked == 0) {
    return;
  }
  RemoteDecision next;
  if (picked == kCreate) {
    next.choice = RemoteChoice::kCreate;
  } else if (picked >= kAssignBase) {
    next.choice = RemoteChoice::kAssign;
    next.local = static_cast<int>(picked - kAssignBase);
  }
  remote_choice_[r] = next;
  Layout();
  Invalidate();
}

// --- Layout --------------------------------------------------------------------------

HFONT PhiSyncSetupWindow::TitleFont() {
  return fonts().Get(22, FW_SEMIBOLD);
}
HFONT PhiSyncSetupWindow::BodyFont() {
  return fonts().Get(14, FW_NORMAL);
}
HFONT PhiSyncSetupWindow::SmallFont() {
  return fonts().Get(12, FW_NORMAL);
}
HFONT PhiSyncSetupWindow::CodeFont() {
  PhiFontDesc desc;
  desc.size = 22;
  desc.weight = FW_SEMIBOLD;
  desc.face = L"Consolas";
  return fonts().Get(desc);
}

int PhiSyncSetupWindow::MeasureHeight(HFONT font,
                                      const std::wstring& text,
                                      int width) {
  if (text.empty()) {
    return 0;
  }
  HDC dc = ::GetDC(hwnd());
  HGDIOBJ old = ::SelectObject(dc, font);
  RECT r = {0, 0, width, 0};
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  ::SelectObject(dc, old);
  ::ReleaseDC(hwnd(), dc);
  return r.bottom - r.top;
}

std::wstring PhiSyncSetupWindow::TitleText() const {
  switch (page_) {
    case Page::kIntroduction:
      return PhiStr("sync.setup.intro.title");
    case Page::kShowingCode:
      return PhiStr("sync.setup.code.title");
    case Page::kChooseJoin:
      return PhiStr("sync.setup.join.title");
    case Page::kWaiting:
      return PhiStr("sync.setup.waiting.title");
    case Page::kEnterCode:
      return PhiStr("sync.setup.enter.title");
    case Page::kDenied:
      return PhiStr("sync.setup.denied.title");
    case Page::kExpired:
      return PhiStr("sync.setup.expired.title");
    case Page::kError:
      return PhiStr("sync.setup.error.title");
    case Page::kProfiles:
      return PhiStr("sync.setup.profiles.title");
    case Page::kWorking:
    case Page::kSubmitting:
      return std::wstring();
  }
  return std::wstring();
}

std::wstring PhiSyncSetupWindow::BodyText() const {
  switch (page_) {
    case Page::kIntroduction:
      return PhiStr("sync.setup.intro.body");
    case Page::kShowingCode:
      return PhiStr("sync.setup.code.body");
    case Page::kChooseJoin:
      return PhiStr("sync.setup.join.body");
    case Page::kWaiting:
      return PhiStr("sync.setup.waiting.body");
    case Page::kEnterCode:
      return PhiStr("sync.setup.enter.body");
    case Page::kDenied:
      return PhiStr("sync.setup.denied.body");
    case Page::kExpired:
      return PhiStr("sync.setup.expired.body");
    case Page::kError:
      return error_message_;
    case Page::kProfiles:
      return PhiStr("sync.setup.profiles.body");
    case Page::kWorking:
      return PhiStr("sync.setup.working");
    case Page::kSubmitting:
      return PhiStr("sync.setup.applying");
  }
  return std::wstring();
}

void PhiSyncSetupWindow::AddButtonRow(std::vector<Button> buttons, int bottom) {
  // Trailing-aligned, primary last (the Windows ContentDialog order).
  const RECT content = ContentRect();
  int right = content.right - Scaled(kPadDip);
  HDC dc = ::GetDC(hwnd());
  HGDIOBJ old = ::SelectObject(dc, BodyFont());
  for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
    SIZE size = {};
    ::GetTextExtentPoint32W(dc, it->text.c_str(),
                            static_cast<int>(it->text.size()), &size);
    const int width = std::max<int>(Scaled(100), size.cx + Scaled(32));
    it->rect = {right - width, bottom - Scaled(kButtonHeightDip), right, bottom};
    right -= width + Scaled(8);
  }
  ::SelectObject(dc, old);
  ::ReleaseDC(hwnd(), dc);
  for (Button& button : buttons) {
    buttons_.push_back(std::move(button));
  }
}

void PhiSyncSetupWindow::Layout() {
  if (!alive()) {
    return;
  }
  buttons_.clear();
  rows_.clear();
  remote_header_index_ = -1;
  title_rect_ = body_rect_ = code_rect_ = note_rect_ = error_rect_ =
      section_rect_ = RECT{};
  note_text_.clear();
  const RECT content = ContentRect();
  const int pad = Scaled(kPadDip);
  const int left = content.left + pad;
  const int width = (content.right - content.left) - 2 * pad;
  const int bottom = content.bottom - Scaled(28);
  int y = content.top + Scaled(24);

  const std::wstring title = TitleText();
  if (!title.empty()) {
    const int h = MeasureHeight(TitleFont(), title, width);
    title_rect_ = {left, y, left + width, y + h};
    y += h + Scaled(10);
  }
  const std::wstring body = BodyText();
  const int body_h = MeasureHeight(BodyFont(), body, width);
  body_rect_ = {left, y, left + width, y + body_h};
  y += body_h + Scaled(20);

  auto primary = [](Control control, const std::wstring& text,
                    bool enabled = true) {
    Button b;
    b.control = control;
    b.text = text;
    b.role = static_cast<int>(PhiPopupButtonRole::kPrimary);
    b.enabled = enabled;
    return b;
  };
  auto secondary = [](Control control, const std::wstring& text,
                      bool enabled = true) {
    Button b;
    b.control = control;
    b.text = text;
    b.role = static_cast<int>(PhiPopupButtonRole::kSecondary);
    b.enabled = enabled;
    return b;
  };

  switch (page_) {
    case Page::kWorking:
    case Page::kSubmitting:
      // The body line is the whole page, centred.
      body_rect_ = {left, content.top + (content.bottom - content.top) / 2 -
                              body_h / 2,
                    left + width,
                    content.top + (content.bottom - content.top) / 2 +
                        body_h / 2 + 1};
      break;
    case Page::kIntroduction:
      AddButtonRow({primary(Control::kContinue,
                            PhiStr("sync.setup.intro.continue"), !busy_)},
                   bottom);
      break;
    case Page::kShowingCode: {
      const int h = Scaled(64);
      code_rect_ = {left, y, left + width, y + h};
      AddButtonRow(
          {secondary(Control::kCopy, copied_ ? PhiStr("sync.setup.code.copied")
                                             : PhiStr("sync.setup.code.copy")),
           primary(Control::kSaved, PhiStr("sync.setup.code.saved"))},
          bottom);
      break;
    }
    case Page::kChooseJoin: {
      // Two stacked choices under the text, full width.
      Button a = primary(Control::kRequestApproval,
                         PhiStr("sync.setup.join.requestApproval"));
      a.rect = {left, y, left + width, y + Scaled(kButtonHeightDip + 4)};
      y += Scaled(kButtonHeightDip + 16);
      Button b = secondary(Control::kUseRecovery,
                           PhiStr("sync.setup.join.useRecoveryCode"));
      b.rect = {left, y, left + width, y + Scaled(kButtonHeightDip + 4)};
      buttons_.push_back(std::move(a));
      buttons_.push_back(std::move(b));
      break;
    }
    case Page::kWaiting: {
      code_rect_ = {left, y, left + width, y + Scaled(64)};
      y += Scaled(64) + Scaled(12);
      note_text_ = PhiStrF("sync.setup.waiting.expiresIn",
                           {Countdown(deadline_ms_ - NowMs())});
      note_rect_ = {left, y, left + width, y + Scaled(20)};
      y += Scaled(28);
      if (!input_error_.empty()) {
        error_rect_ = {left, y, left + width,
                       y + MeasureHeight(BodyFont(), input_error_, width)};
      }
      AddButtonRow({secondary(Control::kUseRecovery,
                              PhiStr("sync.setup.waiting.useRecoveryCode")),
                    secondary(Control::kCancelJoin,
                              PhiStr("sync.setup.waiting.cancel"))},
                   bottom);
      break;
    }
    case Page::kEnterCode: {
      code_rect_ = {left, y, left + width, y + Scaled(48)};
      if (code_field_) {
        ::MoveWindow(code_field_, code_rect_.left + Scaled(12),
                     code_rect_.top + Scaled(10),
                     (code_rect_.right - code_rect_.left) - Scaled(24),
                     (code_rect_.bottom - code_rect_.top) - Scaled(20), TRUE);
      }
      y += Scaled(48) + Scaled(12);
      if (!input_error_.empty()) {
        error_rect_ = {left, y, left + width,
                       y + MeasureHeight(BodyFont(), input_error_, width)};
      }
      AddButtonRow({primary(Control::kSubmit, PhiStr("sync.setup.enter.submit"),
                            !busy_ && !CodeFieldText().empty())},
                   bottom);
      break;
    }
    case Page::kDenied:
    case Page::kExpired:
      AddButtonRow({primary(Control::kTryAnother,
                            PhiStr("sync.setup.tryAnotherWay"))},
                   bottom);
      break;
    case Page::kError:
      AddButtonRow({primary(Control::kTryAgain, PhiStr("sync.setup.tryAgain"))},
                   bottom);
      break;
    case Page::kProfiles: {
      const int row_h = Scaled(kRowHeightDip);
      const int pill_w = Scaled(kPillWidthDip);
      auto add_row = [&](PickerRow row) {
        row.label_rect = {left, y, left + width - pill_w - Scaled(16), y + row_h};
        if (!row.value.empty()) {
          row.pill_rect = {left + width - pill_w, y + Scaled(4), left + width,
                           y + row_h - Scaled(4)};
        }
        rows_.push_back(std::move(row));
        y += row_h;
      };
      for (size_t l = 0; l < candidates_.locals.size(); ++l) {
        PickerRow row;
        row.control = Control::kLocalPick;
        row.key = static_cast<int>(l);
        row.label = W(candidates_.locals[l].display_name);
        row.value = local_choice_[l] == kRegisterNew
                        ? std::wstring(PhiStr("sync.setup.profiles.registerNew"))
                        : RemoteName(local_choice_[l]);
        add_row(std::move(row));
      }
      std::vector<size_t> unclaimed;
      for (size_t r = 0; r < candidates_.remotes.size(); ++r) {
        if (!RemoteClaimedByLocal(static_cast<int>(r))) {
          unclaimed.push_back(r);
        }
      }
      // Every remote keeps a row slot (hidden when a local claimed it) so
      // row indices stay stable for PickRemote.
      if (!candidates_.remotes.empty()) {
        y += Scaled(8);
        const int h = MeasureHeight(SmallFont(),
                                    PhiStr("sync.setup.profiles.unclaimed"),
                                    width);
        section_rect_ = unclaimed.empty()
                            ? RECT{}
                            : RECT{left, y, left + width, y + h};
        if (!unclaimed.empty()) {
          y += h + Scaled(6);
        }
        remote_header_index_ = static_cast<int>(rows_.size()) - 1;
        for (size_t r = 0; r < candidates_.remotes.size(); ++r) {
          PickerRow row;
          row.control = Control::kRemotePick;
          row.key = static_cast<int>(r);
          const bool claimed = RemoteClaimedByLocal(static_cast<int>(r));
          if (claimed) {
            rows_.push_back(std::move(row));  // placeholder, not drawn
            continue;
          }
          row.label = RemoteName(static_cast<int>(r));
          if (!candidates_.remotes[r].name) {
            row.note = PhiStr("sync.setup.profiles.unreadable");
          } else {
            const RemoteDecision& d = remote_choice_[r];
            if (d.choice == RemoteChoice::kCreate) {
              row.value = PhiStr("sync.setup.profiles.createHere");
            } else if (RemoteAssignmentValid(static_cast<int>(r))) {
              row.value = PhiStrF(
                  "sync.setup.profiles.assignTo",
                  {W(candidates_.locals[static_cast<size_t>(d.local)]
                         .display_name)});
            } else {
              row.value = PhiStr("sync.setup.profiles.choose");
            }
          }
          add_row(std::move(row));
        }
      }
      if (!input_error_.empty()) {
        y += Scaled(8);
        error_rect_ = {left, y, left + width,
                       y + MeasureHeight(BodyFont(), input_error_, width)};
      }
      AddButtonRow(
          {secondary(Control::kFinishLater, PhiStr("sync.setup.finishLater")),
           primary(Control::kProfilesContinue,
                   PhiStr("sync.setup.profiles.continue"), AllRowsDecided())},
          bottom);
      break;
    }
  }

  // Hits: buttons, then the pickers.
  ClearHits();
  for (const Button& button : buttons_) {
    AddHit(button.rect, static_cast<int>(button.control) * kKeyStride,
           button.text, PhiHitRole::kButton, false, button.enabled);
  }
  for (const PickerRow& row : rows_) {
    if (row.value.empty() || row.pill_rect.right <= row.pill_rect.left) {
      continue;
    }
    AddHit(row.pill_rect, static_cast<int>(row.control) * kKeyStride + row.key,
           row.label + L": " + row.value, PhiHitRole::kButton);
  }
}

// --- Paint ------------------------------------------------------------------------------

void PhiSyncSetupWindow::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  ::FillRect(dc, &client, bg_brush_);
  ::SetBkMode(dc, TRANSPARENT);
  const PhiPopupColors& c = colors();
  auto text = [&](HFONT font, COLORREF ink, const std::wstring& s, RECT r,
                  UINT format) {
    if (s.empty()) {
      return;
    }
    HGDIOBJ old = ::SelectObject(dc, font);
    ::SetTextColor(dc, ink);
    PhiDrawTextAA(dc, s.c_str(), static_cast<int>(s.size()), &r,
                  format | DT_NOPREFIX);
    ::SelectObject(dc, old);
  };

  text(TitleFont(), c.label_strong, TitleText(), title_rect_, DT_WORDBREAK);
  const bool centred = page_ == Page::kWorking || page_ == Page::kSubmitting;
  text(BodyFont(), centred ? c.secondary : c.label, BodyText(), body_rect_,
       DT_WORDBREAK | (centred ? DT_CENTER : 0));

  if (page_ == Page::kShowingCode || page_ == Page::kWaiting ||
      page_ == Page::kEnterCode) {
    PhiPopupPaintWell(dc, code_rect_, c, dpi());
    if (page_ != Page::kEnterCode) {
      text(CodeFont(), c.label_strong,
           page_ == Page::kShowingCode ? recovery_code_ : verification_code_,
           code_rect_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }
  text(SmallFont(), c.secondary, note_text_, note_rect_, DT_SINGLELINE);
  text(BodyFont(), c.error, input_error_, error_rect_, DT_WORDBREAK);
  if (page_ == Page::kProfiles) {
    text(SmallFont(), c.secondary, PhiStr("sync.setup.profiles.unclaimed"),
         section_rect_, DT_WORDBREAK);
    for (const PickerRow& row : rows_) {
      if (row.label.empty()) {
        continue;
      }
      RECT label = row.label_rect;
      if (!row.note.empty()) {
        label.bottom = label.top + (label.bottom - label.top) / 2 + Scaled(2);
        text(BodyFont(), c.label, row.label, label,
             DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS);
        RECT note = row.label_rect;
        note.top = label.bottom;
        note.right = client.right - Scaled(kPadDip);
        text(SmallFont(), c.secondary, row.note, note,
             DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS);
        continue;
      }
      text(BodyFont(), c.label, row.label, label,
           DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
      if (row.pill_rect.right > row.pill_rect.left) {
        // The ComboBox pill (Settings' kPopup look).
        PhiPopupStrokeRounded(dc, row.pill_rect, c.card, c.card_border,
                              Scaled(4));
        RECT value = row.pill_rect;
        value.left += Scaled(10);
        value.right -= Scaled(28);
        text(BodyFont(), c.label, row.value, value,
             DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT chevron = row.pill_rect;
        chevron.left = chevron.right - Scaled(26);
        text(fonts().Get(PhiFontDesc{10, FW_NORMAL, L"Segoe MDL2 Assets"}),
             c.secondary, L"", chevron,
             DT_SINGLELINE | DT_VCENTER | DT_CENTER);
      }
    }
  }
  for (size_t i = 0; i < buttons_.size(); ++i) {
    const Button& b = buttons_[i];
    const bool hot = hovered() >= 0 && static_cast<size_t>(hovered()) < hits().size() &&
                     hits()[static_cast<size_t>(hovered())].key ==
                         static_cast<int>(b.control) * kKeyStride;
    PhiPopupPaintButton(dc, b.rect, b.text,
                        static_cast<PhiPopupButtonRole>(b.role), hot, false,
                        b.enabled, c, BodyFont(), dpi());
  }
  PaintBand(dc);
}

void PhiSyncSetupWindow::OnHover(int, int) {
  Invalidate();
}

void PhiSyncSetupWindow::OnClick(int hit, POINT) {
  if (hit < 0 || static_cast<size_t>(hit) >= hits().size() ||
      !hits()[static_cast<size_t>(hit)].enabled) {
    return;
  }
  const int key = hits()[static_cast<size_t>(hit)].key;
  const Control control = static_cast<Control>(key / kKeyStride);
  const int row = key % kKeyStride;
  switch (control) {
    case Control::kContinue:
      StartBootstrap();
      break;
    case Control::kSaved:
      recovery_code_.clear();
      LoadPairing();
      break;
    case Control::kCopy:
      CopyToClipboard(recovery_code_);
      copied_ = true;
      copied_timer_.Start(1200);
      Layout();
      Invalidate();
      break;
    case Control::kRequestApproval:
      StartJoinRequest();
      break;
    case Control::kUseRecovery:
      ShowRecoveryEntry();
      break;
    case Control::kCancelJoin:
      CancelJoin();
      ShowPage(Page::kChooseJoin);
      break;
    case Control::kSubmit:
      SubmitRecoveryCode();
      break;
    case Control::kTryAnother:
      ShowPage(Page::kChooseJoin);
      break;
    case Control::kTryAgain:
      CancelJoin();
      BeginSetup();
      break;
    case Control::kFinishLater:
      FinishLater();
      break;
    case Control::kProfilesContinue:
      ContinueProfiles();
      break;
    case Control::kLocalPick:
      PickLocal(row);
      break;
    case Control::kRemotePick:
      PickRemote(row);
      break;
    case Control::kNone:
      break;
  }
}

// --- Pieces ------------------------------------------------------------------------------

void PhiSyncSetupWindow::EnsureCodeField() {
  if (code_field_) {
    return;
  }
  code_field_ = ::CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE,
      0, 0, 10, 10, hwnd(), reinterpret_cast<HMENU>(kIdCodeField),
      ::GetModuleHandleW(nullptr), nullptr);
  ::SetWindowSubclass(
      code_field_,
      [](HWND field, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR,
         DWORD_PTR ref) -> LRESULT {
        auto* self = reinterpret_cast<PhiSyncSetupWindow*>(ref);
        if (msg == WM_KEYDOWN && wparam == VK_RETURN) {
          self->SubmitRecoveryCode();
          return 0;
        }
        if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
          self->FinishLater();
          return 0;
        }
        if (msg == WM_CHAR && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
          return 0;
        }
        return ::DefSubclassProc(field, msg, wparam, lparam);
      },
      kIdCodeField, reinterpret_cast<DWORD_PTR>(this));
  ::SendMessageW(code_field_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(CodeFont()), TRUE);
  ::SendMessageW(code_field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(PhiStr("sync.setup.enter.placeholder")));
  PhiPopupThemeControl(code_field_, colors().dark);
}

std::wstring PhiSyncSetupWindow::CodeFieldText() const {
  if (!code_field_) {
    return std::wstring();
  }
  const int length = ::GetWindowTextLengthW(code_field_);
  std::wstring out(static_cast<size_t>(length) + 1, L'\0');
  ::GetWindowTextW(code_field_, out.data(), length + 1);
  out.resize(static_cast<size_t>(length));
  const size_t first = out.find_first_not_of(L" \t");
  const size_t last = out.find_last_not_of(L" \t");
  return first == std::wstring::npos ? std::wstring()
                                     : out.substr(first, last - first + 1);
}

void PhiSyncSetupWindow::CopyToClipboard(const std::wstring& value) {
  PhiCopySecretToClipboard(hwnd(), value);
}

namespace {

// The clipboard state PhiCopySecretToClipboard left, for its timed clear.
DWORD g_secret_clipboard_sequence = 0;
UINT_PTR g_secret_clear_timer = 0;

// A registered format carrying one DWORD (the clipboard-history opt-outs).
// The clipboard must be open.
void SetClipboardDword(const wchar_t* format_name, DWORD value) {
  const UINT format = ::RegisterClipboardFormatW(format_name);
  if (!format) {
    return;
  }
  HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
  if (!mem) {
    return;
  }
  if (void* dest = ::GlobalLock(mem)) {
    *static_cast<DWORD*>(dest) = value;
    ::GlobalUnlock(mem);
    if (::SetClipboardData(format, mem)) {
      return;  // The system owns it now.
    }
  }
  ::GlobalFree(mem);
}

void CALLBACK ClearSecretClipboard(HWND, UINT, UINT_PTR id, DWORD) {
  ::KillTimer(nullptr, id);
  if (id != g_secret_clear_timer) {
    return;
  }
  g_secret_clear_timer = 0;
  // Anything copied since (by the user, by another app) is left alone.
  if (::GetClipboardSequenceNumber() != g_secret_clipboard_sequence) {
    return;
  }
  if (::OpenClipboard(nullptr)) {
    ::EmptyClipboard();
    ::CloseClipboard();
  }
}

}  // namespace

bool PhiCopySecretToClipboard(HWND owner,
                              const std::wstring& text,
                              UINT clear_after_ms) {
  if (text.empty() || !::OpenClipboard(owner)) {
    return false;
  }
  ::EmptyClipboard();
  bool written = false;
  const size_t count = text.size() + 1;
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, count * sizeof(wchar_t))) {
    if (void* dest = ::GlobalLock(mem)) {
      // SAFETY: the allocation above is exactly `count` wchar_t.
      auto span = UNSAFE_BUFFERS(
          base::span<wchar_t>(static_cast<wchar_t*>(dest), count));
      std::copy(text.begin(), text.end(), span.begin());
      span[text.size()] = L'\0';
      ::GlobalUnlock(mem);
      if (::SetClipboardData(CF_UNICODETEXT, mem)) {
        mem = nullptr;  // The system owns it now.
        written = true;
      }
    }
    if (mem) {
      ::GlobalFree(mem);
    }
  }
  if (written) {
    // Not into Win+V history, not to other devices, not to monitors
    // (learn.microsoft.com, "Clipboard Formats": cloud clipboard and
    // clipboard history formats).
    SetClipboardDword(L"ExcludeClipboardContentFromMonitorProcessing", 0);
    SetClipboardDword(L"CanIncludeInClipboardHistory", 0);
    SetClipboardDword(L"CanUploadToCloudClipboard", 0);
  }
  ::CloseClipboard();
  if (!written) {
    return false;
  }
  g_secret_clipboard_sequence = ::GetClipboardSequenceNumber();
  if (g_secret_clear_timer) {
    ::KillTimer(nullptr, g_secret_clear_timer);
  }
  g_secret_clear_timer =
      ::SetTimer(nullptr, 0, clear_after_ms, &ClearSecretClipboard);
  return true;
}

// --- The harness -----------------------------------------------------------------------

// static
const char* PhiSyncSetupWindow::PageName(Page page) {
  switch (page) {
    case Page::kWorking:
      return "working";
    case Page::kIntroduction:
      return "introduction";
    case Page::kShowingCode:
      return "showingRecoveryCode";
    case Page::kChooseJoin:
      return "chooseJoinMethod";
    case Page::kWaiting:
      return "waitingForApproval";
    case Page::kEnterCode:
      return "enteringRecoveryCode";
    case Page::kDenied:
      return "joinDenied";
    case Page::kExpired:
      return "joinExpired";
    case Page::kError:
      return "error";
    case Page::kProfiles:
      return "profiles";
    case Page::kSubmitting:
      return "submitting";
  }
  return "unknown";
}

std::string PhiSyncSetupWindow::StateJson() const {
  base::DictValue out;
  out.Set("page", PageName(page_));
  out.Set("busy", busy_);
  out.Set("canClose", CanClose());
  out.Set("title", base::WideToUTF8(TitleText()));
  if (!input_error_.empty()) {
    out.Set("inputError", base::WideToUTF8(input_error_));
  }
  if (page_ == Page::kError) {
    out.Set("error", base::WideToUTF8(error_message_));
  }
  if (page_ == Page::kWaiting) {
    out.Set("verificationCode", base::WideToUTF8(verification_code_));
    out.Set("requestId", request_id_);
    out.Set("expiresInMs", static_cast<double>(deadline_ms_ - NowMs()));
  }
  if (page_ == Page::kShowingCode) {
    // The harness is the user here: it must be able to save the code.
    out.Set("recoveryCode", base::WideToUTF8(recovery_code_));
  }
  if (page_ == Page::kProfiles) {
    base::ListValue locals;
    for (size_t l = 0; l < candidates_.locals.size(); ++l) {
      base::DictValue row;
      row.Set("profileId", candidates_.locals[l].profile_id);
      row.Set("name", candidates_.locals[l].display_name);
      row.Set("choice", local_choice_[l] == kRegisterNew
                            ? std::string("new")
                            : base::NumberToString(local_choice_[l]));
      locals.Append(std::move(row));
    }
    base::ListValue remotes;
    for (size_t r = 0; r < candidates_.remotes.size(); ++r) {
      base::DictValue row;
      row.Set("uuid", candidates_.remotes[r].uuid);
      row.Set("name", base::WideToUTF8(RemoteName(static_cast<int>(r))));
      row.Set("readable", candidates_.remotes[r].name.has_value());
      row.Set("claimedByLocal", RemoteClaimedByLocal(static_cast<int>(r)));
      const RemoteDecision& d = remote_choice_[r];
      row.Set("choice", d.choice == RemoteChoice::kCreate ? "create"
                        : RemoteAssignmentValid(static_cast<int>(r))
                            ? "assign:" + base::NumberToString(d.local)
                            : std::string("undecided"));
      remotes.Append(std::move(row));
    }
    out.Set("locals", std::move(locals));
    out.Set("remotes", std::move(remotes));
    out.Set("canContinue", AllRowsDecided());
  }
  std::string json;
  base::JSONWriter::Write(out, &json);
  return json;
}

std::string PhiSyncSetupWindow::Drive(const std::string& action,
                                      const std::string& value) {
  auto on = [&](Page page) { return page_ == page; };
  if (action == "continue" && on(Page::kIntroduction)) {
    StartBootstrap();
  } else if (action == "saved" && on(Page::kShowingCode)) {
    recovery_code_.clear();
    LoadPairing();
  } else if (action == "copy" && on(Page::kShowingCode)) {
    CopyToClipboard(recovery_code_);
  } else if (action == "requestApproval" && on(Page::kChooseJoin)) {
    StartJoinRequest();
  } else if (action == "useRecovery" &&
             (on(Page::kChooseJoin) || on(Page::kWaiting))) {
    ShowRecoveryEntry();
  } else if (action == "cancel" && on(Page::kWaiting)) {
    CancelJoin();
    ShowPage(Page::kChooseJoin);
  } else if (action == "code" && on(Page::kEnterCode)) {
    ::SetWindowTextW(code_field_, base::UTF8ToWide(value).c_str());
  } else if (action == "submit" && on(Page::kEnterCode)) {
    SubmitRecoveryCode();
  } else if (action == "tryAnother" && (on(Page::kDenied) || on(Page::kExpired))) {
    ShowPage(Page::kChooseJoin);
  } else if (action == "tryAgain" && on(Page::kError)) {
    CancelJoin();
    BeginSetup();
  } else if (action == "local" && on(Page::kProfiles)) {
    int row = 0;
    const size_t colon = value.find(':');
    if (colon == std::string::npos ||
        !base::StringToInt(value.substr(0, colon), &row) || row < 0 ||
        static_cast<size_t>(row) >= local_choice_.size()) {
      return "invalid_value";
    }
    const std::string pick = value.substr(colon + 1);
    int remote = 0;
    if (pick == "new") {
      local_choice_[static_cast<size_t>(row)] = kRegisterNew;
    } else if (base::StringToInt(pick, &remote) && remote >= 0 &&
               static_cast<size_t>(remote) < candidates_.remotes.size() &&
               candidates_.remotes[static_cast<size_t>(remote)].name) {
      local_choice_[static_cast<size_t>(row)] = remote;
    } else {
      return "invalid_value";
    }
    Layout();
    Invalidate();
  } else if (action == "remote" && on(Page::kProfiles)) {
    int row = 0;
    const size_t colon = value.find(':');
    if (colon == std::string::npos ||
        !base::StringToInt(value.substr(0, colon), &row) || row < 0 ||
        static_cast<size_t>(row) >= remote_choice_.size()) {
      return "invalid_value";
    }
    const std::string pick = value.substr(colon + 1);
    RemoteDecision next;
    int local = 0;
    if (pick == "create") {
      next.choice = RemoteChoice::kCreate;
    } else if (pick.rfind("assign:", 0) == 0 &&
               base::StringToInt(pick.substr(7), &local) && local >= 0 &&
               static_cast<size_t>(local) < local_choice_.size()) {
      next.choice = RemoteChoice::kAssign;
      next.local = local;
    } else if (pick != "none") {
      return "invalid_value";
    }
    remote_choice_[static_cast<size_t>(row)] = next;
    Layout();
    Invalidate();
  } else if (action == "continueProfiles" && on(Page::kProfiles)) {
    if (!AllRowsDecided()) {
      return "undecided";
    }
    ContinueProfiles();
  } else if (action == "finishLater") {
    if (!CanClose()) {
      return "busy";
    }
    FinishLater();
  } else {
    return action == "continue" || action == "saved" || action == "copy" ||
                   action == "requestApproval" || action == "useRecovery" ||
                   action == "cancel" || action == "code" ||
                   action == "submit" || action == "tryAnother" ||
                   action == "tryAgain" || action == "local" ||
                   action == "remote" || action == "continueProfiles"
               ? "wrong_page"
               : "unknown_action";
  }
  return std::string();
}

}  // namespace phi_client
