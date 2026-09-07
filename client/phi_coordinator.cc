// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_telemetry.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <optional>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "third_party/abseil-cpp/absl/functional/overload.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "base/rand_util.h"
#include "phibrowser-win/client/phi_agent_overlay.h"
#include "phibrowser-win/client/phi_feedback_outbox.h"
#include "phibrowser-win/client/phi_updater.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_extensions_popover.h"
#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_menu_bar.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_tab_switcher.h"
#include "phibrowser-win/client/phi_window_capture.h"

namespace phi_client {

namespace {
// The one coordinator, reachable from the static keyboard hook. The client
// creates exactly one for the process lifetime.
PhiCoordinator* g_coordinator = nullptr;

// The wire name of a tab group colour — the TabGroupsJson vocabulary, so
// the skill's groups.list reads the same whether it came from the model or
// the framework's own serialisation.
const char* GroupColorName(phi_framework::TabGroupColor color) {
  using phi_framework::TabGroupColor;
  switch (color) {
    case TabGroupColor::kGrey:
      return "grey";
    case TabGroupColor::kBlue:
      return "blue";
    case TabGroupColor::kRed:
      return "red";
    case TabGroupColor::kYellow:
      return "yellow";
    case TabGroupColor::kGreen:
      return "green";
    case TabGroupColor::kPink:
      return "pink";
    case TabGroupColor::kPurple:
      return "purple";
    case TabGroupColor::kCyan:
      return "cyan";
    case TabGroupColor::kOrange:
      return "orange";
  }
  return "grey";
}

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
      // Ctrl+Alt+R, the Mac's Cmd-Opt-R: Safari's reader chord (Cmd-Shift-R)
      // is Chromium's hard reload on both platforms.
      {"toggleReader", L"View", L"Toggle Reader View", kCmdToggleReader,
       {'R', true, false, true}},
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
  // Every secondary window follows the app palette from here on.
  PhiSetPopupPalette(&palette_);
}

PhiCoordinator::~PhiCoordinator() {
  if (mouse_hook_) {
    ::UnhookWindowsHookEx(mouse_hook_);
  }
  if (keyboard_hook_) {
    ::UnhookWindowsHookEx(keyboard_hook_);
  }
  g_coordinator = nullptr;
}

namespace {
constexpr wchar_t kMsgClassName[] = L"PhiClientControlMsg";
constexpr UINT WM_PHI_CONTROL = WM_APP + 1;
constexpr UINT WM_PHI_AUTHORIZE = WM_APP + 2;
// A worker thread finished changing the Auth0 session state.
constexpr UINT WM_PHI_AUTH_CHANGED = WM_APP + 3;
// (WM_APP + 4 was WM_PHI_READER_SWEEP: the reader-panel settle now runs
// straight from OnWindowChanged, which contract rule 1 delivers after commit
// from a fresh task, coalesced per window — the deferral bought nothing the
// framework does not already do.)
// Reader View: hand a reader panel's page back to its origin window as a tab.
// Still DEFERRED: the panel's constructor runs RefreshState, and a panel
// born on a non-reader page would ask for this from inside
// OnBrowserWindowCreated, where creating a tab and closing a browser are
// forbidden (contract rule 2). wparam carries the panel window id.
constexpr UINT WM_PHI_READER_EXPAND = WM_APP + 5;
// Reader View: close a panel browser whose surface tab has gone. DEFERRED:
// OnPlaceholderModeChanged carries no rule-1 promise — it is reported from
// inside the tab removal that caused it, where closing a browser trips
// `!observers_.is_notifying_observers()`. wparam carries the panel window id.
constexpr UINT WM_PHI_READER_RETIRE = WM_APP + 6;

// How long the I/O thread waits for the UI thread to run a marshalled call.
// Generous on purpose: a consent or credential prompt can sit for minutes,
// and the skill itself waits up to 180 s for consent.
constexpr DWORD kUiCallTimeoutMs = 180 * 1000;
}  // namespace

// The request marshalled from the control server's I/O thread to the UI
// thread. POSTED, never sent: a cross-thread SendMessage is serviced by
// whatever message loop the UI thread happens to be pumping — a context
// menu's, a prompt's — so a verb could run, and destroy a controller, under
// that controller's own stack frame (docs/architecture-review.md §4 item
// 11). A posted message is only dispatched from the main loop, and
// RunOrDeferUiCall parks it when a nested loop is up.
//
// Heap-owned by TWO references, the I/O waiter's and the UI thread's, so
// whichever finishes last frees it: a waiter that gave up (timeout) must
// not pull the call from under a UI thread still about to run it, and the
// UI thread must not free a result the waiter is about to read.
struct PhiUiCall {
  enum class Kind { kControl, kAuthorize };
  Kind kind = Kind::kControl;
  // kControl: the verb and its stringified payload / reply.
  std::string type;
  std::string payload;
  std::string result;
  // kAuthorize: the peer asking for consent and the answer.
  PhiAgentPeer peer;
  bool allowed = false;
  // Manual-reset; signalled once the UI thread has run the call.
  HANDLE done = nullptr;
  std::atomic<int> refs{2};

  void Release() {
    if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (done) {
        ::CloseHandle(done);
      }
      delete this;
    }
  }
};

namespace {
// Allocates a call with its event; null when the event could not be made.
PhiUiCall* NewUiCall(PhiUiCall::Kind kind) {
  PhiUiCall* call = new PhiUiCall;
  call->kind = kind;
  call->done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!call->done) {
    delete call;
    return nullptr;
  }
  return call;
}

// Waits for the UI thread to run `call` (I/O thread). True when it did;
// false on timeout, in which case the UI side still owns its reference and
// will run and free the call on its own later — the waiter must not touch
// the call after this returns false beyond releasing its own reference.
bool WaitUiCall(PhiUiCall* call) {
  return ::WaitForSingleObject(call->done, kUiCallTimeoutMs) == WAIT_OBJECT_0;
}
}  // namespace

void PhiCoordinator::OnFrameworkReady(phi_framework::PhiFramework* framework,
                                     const char* user_data_dir_utf8) {
  framework_ = framework;

  // Start the outbound telemetry pipeline. It reads phi_telemetry.json from
  // this directory and stays entirely inert — spooling nothing, sending
  // nothing — until a collector is named there.
  if (user_data_dir_utf8) {
    PhiTelemetry::Get().Start(base::UTF8ToWide(user_data_dir_utf8));
  }

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
  mouse_hook_ = ::SetWindowsHookExW(WH_MOUSE, &MouseHookProc, nullptr,
                                    ::GetCurrentThreadId());

  // The operating mask: a hold changing repaints the owning shell, and the
  // reclaimed set is pushed to the browser, which then REFUSES drive commands
  // aimed at those tabs (the refusal is the browser's, so a driver cannot
  // decline to honour the takeover).
  agent_drives_.set_changed_callback([this](int tab_id) {
    registry_.InvalidateAll();
  });
  agent_drives_.set_reclaimed_callback([this](const std::vector<int>& tabs) {
    if (framework_) {
      framework_->SetUserReclaimedTabs(tabs.empty() ? nullptr : tabs.data(),
                                       static_cast<int>(tabs.size()));
    }
  });
  if (msg_hwnd_) {
    ::SetTimer(msg_hwnd_, kAgentDriveSweepTimer,
               PhiAgentDriveRegistry::kSweepIntervalMs, nullptr);
  }

  const std::wstring user_data_dir =
      base::UTF8ToWide(user_data_dir_utf8 ? user_data_dir_utf8 : "");
  // The ghost view's mutations ride the coalesced-save timer like every
  // other client store; FlushDirtyStores writes the restore file.
  ghost_spaces_.set_dirty_callback(
      [this] { ScheduleSave(StoreDomain::kRestore); });
  credentials_dir_ = user_data_dir;
  // The ONLY credential provider a release build knows is the real vault:
  // the out-of-process Bitwarden helper, which owns the user's actual vault
  // (and links the GPL SDK on its own side of the process boundary). With
  // no helper installed every credentials verb answers not_installed.
  // A --dev-hooks build (phi_features.h) adds two doors: PHI_VAULT_HELPER
  // replaces the helper binary (a PATH TO AN EXE, never arguments, so the
  // no-arguments spawn contract is intact), and the local DPAPI dev vault
  // stands in when no helper is installed.
  std::wstring helper_override;
  if constexpr (kPhiDevHooks) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length =
        ::GetEnvironmentVariableW(L"PHI_VAULT_HELPER", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
      helper_override.assign(buffer, length);
    }
  }
  if (!vault_helper_.Start(helper_override)) {
    if constexpr (kPhiDevHooks) {
      LOG(INFO) << "[PhiVault] no helper installed — using the local dev "
                   "vault (dev hooks build)";
      credentials_.Load(user_data_dir);
    } else {
      LOG(INFO) << "[PhiVault] no helper installed — credentials unavailable";
    }
  }
  LoadStores(user_data_dir);
  tab_store_.Load(user_data_dir + L"\\PhiTabData.json");
  MirrorPinScope();  // The tab store's file is the scope's truth.
  // The framework mirrors (restore-on-startup, peek, display language) are
  // NOT pushed here: no profile is loaded yet, and OnProfileLoaded — which
  // follows this callback for every profile — is their one push point.
  Transition(AppPhase::kStoresLoaded, "loaded");
  PushSpaceMenu();  // The page context menu's "Open Link in Space" list.

  control_server_.Start(user_data_dir, this, framework_);

  // Background update checks (WinSparkle over the Mac's appcast pipeline).
  // A no-op when WinSparkle.dll is not deployed beside the client.
  PhiUpdater::Init();

  // Restore the Auth0 session (validated in the background; the message
  // refreshes the settings pane whenever the state settles).
  auth_.Load(user_data_dir, msg_hwnd_, WM_PHI_AUTH_CHANGED);
  // The feedback outbox: reports queued by earlier runs resume uploading
  // once the account is known (the Mac's FeedbackOutboxUploader.start).
  PhiFeedbackOutbox::Get().Start(user_data_dir, &auth_);
  SyncFeedbackAccount();
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
    if (!self) {
      return 0;
    }
    switch (wparam) {
      case kAgentSpaceSweepTimer:
        self->SweepAgentSpaces();  // Agent-space keep-alive sweep (10s).
        return 0;
      case kAgentDriveSweepTimer:
        // Retires holds whose driver went quiet and takeovers that aged out.
        self->agent_drives_.Sweep();
        return 0;
      case kDeferredUiCallTimer:
        self->DrainDeferredUiCalls();
        return 0;
      case kSaveTimer:
        self->FlushDirtyStores();
        return 0;
      case kColdStartFallbackTimer:
        // The replay had its say and no user window disarmed this: the
        // cold start is settled either way (the rescue opens a shell when
        // the eager set matched nothing).
        ::KillTimer(hwnd, kColdStartFallbackTimer);
        self->cold_start_armed_ = false;
        self->SettleRestore("fallback");
        return 0;
      default:
        return 0;
    }
  }
  if (msg == WM_PHI_CONTROL || msg == WM_PHI_AUTHORIZE) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    auto* call = reinterpret_cast<PhiUiCall*>(lparam);
    if (!call) {
      return 0;
    }
    if (self) {
      self->RunOrDeferUiCall(call);
    } else {
      // No coordinator behind the window any more: answer "refused" so the
      // waiter does not sit out the whole timeout, and drop our reference.
      call->result = "{\"ok\":false,\"error\":\"not_ready\"}";
      ::SetEvent(call->done);
      call->Release();
    }
    return 0;
  }
  if (msg == WM_QUERYENDSESSION || msg == WM_ENDSESSION) {
    // Logoff / shutdown: Windows gives no second chance after WM_ENDSESSION,
    // so flush on the query already (idempotent; the session may still be
    // cancelled, and a flushed store is never wrong). Message-only windows
    // do get these — they are broadcast to every top-level window of the
    // session, HWND_MESSAGE children included on modern Windows — and the
    // shells do not handle them themselves.
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && (msg == WM_QUERYENDSESSION || wparam)) {
      if (self->phase_ < AppPhase::kClosing) {
        self->Transition(AppPhase::kClosing, "endsession");
      }
      self->FlushForShutdown();
    }
    return msg == WM_QUERYENDSESSION ? TRUE : 0;
  }
  if (msg == WM_PHI_READER_RETIRE) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const BrowserId panel{static_cast<int64_t>(wparam)};
    if (self && self->surfaces_.Find(panel) && self->framework_) {
      self->framework_->CloseBrowserWindow(panel.value);
    }
    return 0;
  }
  if (msg == WM_PHI_READER_EXPAND) {
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->FinishReaderPanelExpand(BrowserId{static_cast<int64_t>(wparam)});
    }
    return 0;
  }
  if (msg == WM_PHI_AUTH_CHANGED) {
    // The sign-in/refresh worker settled; the Account pane re-reads state,
    // and the feedback outbox learns whose jobs to work on.
    PhiSettingsWindow::Refresh();
    auto* self = reinterpret_cast<PhiCoordinator*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      self->SyncFeedbackAccount();
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void PhiCoordinator::RunOrDeferUiCall(PhiUiCall* call) {
  // A nested modal loop is pumping (a context menu, a prompt, a MessageBox
  // — every one wrapped in PhiModalScope): do NOT run the verb inside it.
  // Park it and poll for the loop to unwind. A non-empty queue also parks
  // newcomers, so calls run in arrival order.
  if (PhiModalDepth() > 0 || draining_ui_calls_ ||
      !deferred_ui_calls_.empty()) {
    deferred_ui_calls_.push_back(call);
    if (msg_hwnd_) {
      ::SetTimer(msg_hwnd_, kDeferredUiCallTimer, kDeferredUiCallPollMs,
                 nullptr);
    }
    return;
  }
  RunUiCall(call);
}

void PhiCoordinator::RunUiCall(PhiUiCall* call) {
  switch (call->kind) {
    case PhiUiCall::Kind::kControl:
      call->result = ExecuteControlOnUi(call->type, call->payload,
                                        ControlCaller::kPipe);
      break;
    case PhiUiCall::Kind::kAuthorize:
      call->allowed = AuthorizeAgentPeerOnUi(call->peer);
      break;
  }
  ::SetEvent(call->done);
  call->Release();  // The UI side's reference; the waiter holds the other.
}

void PhiCoordinator::DrainDeferredUiCalls() {
  // The timer also ticks INSIDE the modal loop it is waiting out (a nested
  // loop pumps every message); the depth check is what keeps the calls
  // parked until it has unwound. A drained call may itself open a prompt
  // (credentials, consent): the loop re-checks after each one and leaves
  // the rest for the next tick.
  if (PhiModalDepth() > 0 || draining_ui_calls_) {
    return;
  }
  draining_ui_calls_ = true;
  while (!deferred_ui_calls_.empty() && PhiModalDepth() == 0) {
    PhiUiCall* call = deferred_ui_calls_.front();
    deferred_ui_calls_.pop_front();
    RunUiCall(call);
  }
  draining_ui_calls_ = false;
  if (deferred_ui_calls_.empty() && msg_hwnd_) {
    ::KillTimer(msg_hwnd_, kDeferredUiCallTimer);
  }
}

void PhiCoordinator::FlushForShutdown() {
  // Coalesced saves land now rather than on a timer that will never fire.
  // The restore file is written whether or not it is dirty: the last-active
  // Space is re-derived here from the live window, and the write is cheap
  // insurance for a ghost record noted inside the replay.
  restore_.last_active_space_id = LastActiveSpaceId();  // The live window's.
  dirty_stores_ |= StoreBit(StoreDomain::kRestore);
  FlushDirtyStores();
  // The spool and the outbox worker: both are best-effort and idempotent
  // (a second call finds no thread to join and rewrites the same spool).
  PhiTelemetry::Get().Stop();
  PhiFeedbackOutbox::Get().Stop();
  PhiUpdater::Shutdown();
  if (phase_ < AppPhase::kFlushed) {
    Transition(AppPhase::kFlushed, "flushed");
  }
}

// static
const char* PhiCoordinator::AppPhaseName(AppPhase phase) {
  switch (phase) {
    case AppPhase::kBooting:
      return "booting";
    case AppPhase::kStoresLoaded:
      return "storesLoaded";
    case AppPhase::kRestorePlanned:
      return "restorePlanned";
    case AppPhase::kRestoring:
      return "restoring";
    case AppPhase::kSettled:
      return "settled";
    case AppPhase::kRunning:
      return "running";
    case AppPhase::kClosing:
      return "closing";
    case AppPhase::kFlushed:
      return "flushed";
  }
  return "unknown";
}

bool PhiCoordinator::Transition(AppPhase next, const char* reason) {
  if (next == phase_) {
    return false;
  }
  if (next < phase_) {
    // Refused, not clamped: a phase is a promise the rest of the coordinator
    // relies on (stores loaded, restore decided, flushed), and a caller that
    // wants to go back has misread where the app is.
    LOG(WARNING) << "[PhiLifecycle] refused " << AppPhaseName(phase_) << " -> "
                 << AppPhaseName(next) << " (" << reason << ")";
    return false;
  }
  LOG(INFO) << "[PhiLifecycle] " << AppPhaseName(phase_) << " -> "
            << AppPhaseName(next) << " (" << reason << ")";
  phase_ = next;
  return true;
}

void PhiCoordinator::ScheduleSave(StoreDomain domain) {
  const bool armed = dirty_stores_ != 0;
  dirty_stores_ |= StoreBit(domain);
  if (phase_ >= AppPhase::kFlushed || !msg_hwnd_) {
    // Past the flush nothing will run the timer, and before the message
    // window exists nothing can: write now rather than lose it.
    FlushDirtyStores();
    return;
  }
  if (!armed) {
    ::SetTimer(msg_hwnd_, kSaveTimer, kSaveDelayMs, nullptr);
  }
}

void PhiCoordinator::FlushDirtyStores() {
  if (msg_hwnd_) {
    ::KillTimer(msg_hwnd_, kSaveTimer);
  }
  // Taken before writing: a Save* that marks a domain dirty again (none
  // does today) must land in the next flush, not loop this one.
  const uint32_t dirty = dirty_stores_;
  dirty_stores_ = 0;
  if (dirty & StoreBit(StoreDomain::kSettings)) {
    settings_store_.Save(settings_);
  }
  if (dirty & StoreBit(StoreDomain::kSpaces)) {
    spaces_store_.Save(spaces_model_);
    // Every Space-list or rule change reaches here; keep the page context
    // menu's "Open Link in Space" list and the routing table in step.
    PushSpaceMenu();
  }
  if (dirty & StoreBit(StoreDomain::kRestore)) {
    restore_store_.Save(restore_);
  }
  if (dirty & StoreBit(StoreDomain::kConsent)) {
    consent_store_.Save(consent_);
  }
}

std::string PhiCoordinator::HandleControlCommand(
    const std::string& type,
    const std::string& payload_json) {
  // Called on the control server's I/O thread. Marshal to the UI thread by
  // POSTING (see PhiUiCall) and wait for its event; the UI side runs
  // ExecuteControlOnUi from the main loop only.
  if (!msg_hwnd_) {
    return "{\"error\":\"not ready\"}";
  }
  PhiUiCall* call = NewUiCall(PhiUiCall::Kind::kControl);
  if (!call) {
    return "{\"ok\":false,\"error\":\"internal\"}";
  }
  call->type = type;
  call->payload = payload_json;
  if (!::PostMessageW(msg_hwnd_, WM_PHI_CONTROL, 0,
                      reinterpret_cast<LPARAM>(call))) {
    call->Release();  // The UI side's reference: it will never take it.
    call->Release();
    return "{\"ok\":false,\"error\":\"not_ready\"}";
  }
  if (!WaitUiCall(call)) {
    call->Release();
    return "{\"ok\":false,\"error\":\"timeout\"}";
  }
  std::string result = std::move(call->result);
  call->Release();
  // A deferred reply: the UI side registered a slot and started the work
  // (a reader extraction, a window or profile creation the framework will
  // announce later); wait for it HERE, on the I/O thread, and hand back
  // what it stored. The wait outlasts kCreateTimeoutMs plus a sweep tick,
  // so a lost creation answers create_timeout rather than timeout.
  if (result.find("\"pendingReply\"") != std::string::npos) {
    std::optional<base::DictValue> body =
        base::JSONReader::ReadDict(result, base::JSON_PARSE_RFC);
    const std::optional<int> id = body ? body->FindInt("pendingReply")
                                       : std::nullopt;
    if (id) {
      return WaitPendingReply(*id, 40000);
    }
  }
  return result;
}

void PhiCoordinator::OnProfileLoaded(const char* profile_id) {
  const std::string id = profile_id ? profile_id : "";
  LOG(INFO) << "[PhiClient] OnProfileLoaded " << id;
  if (!framework_ || id.empty()) {
    return;
  }
  // Per profile: Chromium's SessionStartupPref is the profile's own copy
  // of the app-wide "Restore last session" choice, so each one takes it as
  // it loads (a cold Space switch, CreateProfile, RequestBrowser included).
  framework_->SetRestoreOnStartup(id.c_str(), settings_.restore_last_session);
  if (global_mirrors_pushed_) {
    return;
  }
  global_mirrors_pushed_ = true;
  // Global, once: the peek switches live on the framework, not a profile;
  // the display language reconciles a PhiSettings.json edited while the
  // browser was closed — Chromium read its own locale pref long before
  // this, and the first window is still to come, so it comes up in the
  // chosen language rather than flashing the old one.
  PushPeekPreferences();
  PushDisplayLanguageToChromium();
}

void PhiCoordinator::OnProfileCreated(const char* profile_id) {
  const std::string id = profile_id ? profile_id : "";
  LOG(INFO) << "[PhiClient] OnProfileCreated " << (id.empty() ? "(failed)" : id);
  if (pending_profiles_.empty()) {
    return;  // Settings' "New Profile" (CreateProfileNamed) asks for nothing.
  }
  // The request this profile answers: by the display name it was asked
  // under, else the oldest — the framework reports the name only once the
  // attributes entry exists, which a failed creation never reaches.
  auto pending = pending_profiles_.end();
  const std::string name = id.empty() ? std::string() : ProfileDisplayName(id);
  if (!name.empty()) {
    for (auto it = pending_profiles_.begin(); it != pending_profiles_.end();
         ++it) {
      if (it->display_name == name) {
        pending = it;
        break;
      }
    }
  }
  if (pending == pending_profiles_.end()) {
    pending = pending_profiles_.begin();
  }
  const PendingProfile request = std::move(*pending);
  pending_profiles_.erase(pending);
  UpdateSweepTimer();
  if (id.empty()) {
    LOG(ERROR) << "[PhiClient] profile creation failed for '"
               << request.display_name << "'";
    if (request.pending_reply_id) {
      CompletePendingReply(request.pending_reply_id,
                           "{\"ok\":false,\"error\":\"create_failed\"}");
    }
    return;
  }
  switch (request.then) {
    case PendingProfile::Then::kReplyOnly: {
      base::DictValue out;
      out.Set("ok", true);
      out.Set("profileId", id);
      std::string json;
      base::JSONWriter::Write(out, &json);
      CompletePendingReply(request.pending_reply_id, std::move(json));
      return;
    }
    case PendingProfile::Then::kFillForm:
      // The create-Space form asked for it: the form's picker selects it,
      // and the Space follows when the form is submitted.
      if (PhiWindowController* controller = ControllerForSlot(request.slot)) {
        controller->OnSpaceFormProfileCreated(id);
      }
      return;
    case PendingProfile::Then::kOpenSpace: {
      // "+ Space & profile": the Space binds the new profile — bindings are
      // immutable, which is why the Space waited for the basename — and the
      // slot enters it (a RequestBrowser, since the profile has no window).
      base::DictValue payload;
      payload.Set("name",
                  "Space " + std::to_string(spaces_model_.next_space_id));
      payload.Set("profileId", id);
      std::string json;
      base::JSONWriter::Write(payload, &json);
      const std::string created = ExecuteControlOnUi(
          "agentSpace.spaces.create", json, ControlCaller::kInternal);
      std::optional<base::DictValue> r =
          base::JSONReader::ReadDict(created, base::JSON_PARSE_RFC);
      const std::string* space_id = r ? r->FindString("id") : nullptr;
      if (space_id) {
        RequestSwitchSpace(request.slot.value, *space_id);
      }
      return;
    }
  }
}

void PhiCoordinator::RequestProfile(PendingProfile pending) {
  if (!framework_ || pending.display_name.empty()) {
    if (pending.pending_reply_id) {
      CompletePendingReply(pending.pending_reply_id,
                           "{\"ok\":false,\"error\":\"create_failed\"}");
    }
    return;
  }
  pending.deadline_tick = ::GetTickCount64() + kCreateTimeoutMs;
  const std::string name = pending.display_name;
  pending_profiles_.push_back(std::move(pending));
  UpdateSweepTimer();
  framework_->CreateProfile(name.c_str());
}

std::string PhiCoordinator::ProfileDisplayName(
    const std::string& profile_id) const {
  if (!framework_ || profile_id.empty()) {
    return std::string();
  }
  std::optional<base::Value> profiles = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!profiles || !profiles->is_list()) {
    return std::string();
  }
  for (const base::Value& v : profiles->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    const std::string* id = d ? d->FindString("id") : nullptr;
    const std::string* name = d ? d->FindString("name") : nullptr;
    if (id && *id == profile_id) {
      return name ? *name : std::string();
    }
  }
  return std::string();
}

void PhiCoordinator::UpdateSweepTimer() {
  if (!msg_hwnd_) {
    return;
  }
  const bool needed = !agent_spaces_.empty() || !shadow_windows_.empty() ||
                      registry_.HasPending() || !pending_profiles_.empty();
  if (needed) {
    // Re-arming a running timer only restarts its interval; a sweep delayed
    // by one tick is harmless, a sweep that never runs is not.
    ::SetTimer(msg_hwnd_, kAgentSpaceSweepTimer, kAgentSpaceSweepIntervalMs,
               nullptr);
  } else {
    ::KillTimer(msg_hwnd_, kAgentSpaceSweepTimer);
  }
}

int PhiCoordinator::RegisterPendingReply(bool document, bool include_html) {
  base::AutoLock lock(pending_replies_lock_);
  const int id = next_pending_reply_++;
  PendingReply& slot = pending_replies_[id];
  slot.event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  slot.document = document;
  slot.include_html = include_html;
  return id;
}

void PhiCoordinator::CompletePendingReply(int id, std::string json) {
  base::AutoLock lock(pending_replies_lock_);
  auto it = pending_replies_.find(id);
  if (it == pending_replies_.end()) {
    return;  // The waiter timed out and released the slot.
  }
  it->second.json = std::move(json);
  ::SetEvent(it->second.event);
}

std::string PhiCoordinator::WaitPendingReply(int id, DWORD timeout_ms) {
  HANDLE event = nullptr;
  {
    base::AutoLock lock(pending_replies_lock_);
    auto it = pending_replies_.find(id);
    if (it == pending_replies_.end()) {
      return "{\"ok\":false,\"error\":\"internal\"}";
    }
    event = it->second.event;
  }
  const DWORD waited = ::WaitForSingleObject(event, timeout_ms);
  std::string json;
  {
    base::AutoLock lock(pending_replies_lock_);
    auto it = pending_replies_.find(id);
    if (it != pending_replies_.end()) {
      json = std::move(it->second.json);
      pending_replies_.erase(it);
    }
  }
  ::CloseHandle(event);
  if (waited != WAIT_OBJECT_0 || json.empty()) {
    return "{\"ok\":false,\"error\":\"timeout\"}";
  }
  return json;
}

namespace {

// A standalone HTML document around a distilled article — the skill's
// saveArticle() writes this to disk. A <base> keeps the article's relative
// image URLs resolving against the source; images are NOT inlined (the
// Mac's inliner is its own service), so the file opens with the network.
std::string BuildReaderDocument(const std::string& title,
                                const std::string& source_url,
                                const std::string& html) {
  const std::string safe_title = base::EscapeForHTML(title);
  return "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n<title>" +
         safe_title + "</title>\n<base href=\"" +
         base::EscapeForHTML(source_url) +
         "\">\n<meta name=\"viewport\" content=\"width=device-width, "
         "initial-scale=1\">\n<style>\n"
         "body{margin:0;background:#fbfaf7;color:#1c1c1e;"
         "font:18px/1.65 -apple-system,\"Segoe UI\",system-ui,sans-serif}\n"
         "main{max-width:680px;margin:0 auto;padding:48px 24px 96px}\n"
         "h1{font-size:2em;line-height:1.2;margin:0 0 .4em}\n"
         ".source{color:#6b6b70;font-size:.85em;margin:0 0 2em;"
         "word-break:break-all}\n"
         "img,video{max-width:100%;height:auto}\n"
         "pre{overflow:auto;background:#f0efe9;padding:12px;"
         "border-radius:8px}\n"
         "a{color:#2f6fd6}\n</style></head>\n<body><main>\n<h1>" +
         safe_title + "</h1>\n<p class=\"source\"><a href=\"" +
         base::EscapeForHTML(source_url) + "\">" +
         base::EscapeForHTML(source_url) + "</a></p>\n<article>\n" + html +
         "\n</article>\n</main></body></html>\n";
}

// "<title>.html" with the characters a filename cannot carry replaced.
std::string ReaderSuggestedFileName(const std::string& title) {
  std::string name;
  for (unsigned char c : title) {
    if (c < 32 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) !=
                      std::string_view::npos) {
      name.push_back('-');
    } else {
      name.push_back(static_cast<char>(c));
    }
  }
  while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
    name.pop_back();
  }
  if (name.size() > 80) {
    name.resize(80);
  }
  return (name.empty() ? std::string("phi-article") : name) + ".html";
}

}  // namespace

void PhiCoordinator::OnReaderArticleExtracted(int request_id,
                                              const char* json) {
  // Shape the framework's article into the verb's reply. The Mac's fields
  // that come from ITS extraction ladder (rung, coverage, byline, siteName,
  // lang) are answered honestly: the rung is the distiller, coverage is
  // unknown, and a page the distiller finds nothing in is
  // no_article_detected exactly as on the Mac.
  bool document = false;
  bool include_html = true;
  {
    base::AutoLock lock(pending_replies_lock_);
    auto it = pending_replies_.find(request_id);
    if (it == pending_replies_.end()) {
      return;
    }
    document = it->second.document;
    include_html = it->second.include_html;
  }
  std::optional<base::DictValue> article =
      base::JSONReader::ReadDict(json ? json : "{}", base::JSON_PARSE_RFC);
  base::DictValue out;
  if (!article || !article->FindBool("ok").value_or(false)) {
    out.Set("ok", false);
    const std::string* error = article ? article->FindString("error")
                                       : nullptr;
    out.Set("error", error ? *error : "no_article_detected");
  } else {
    const std::string* title = article->FindString("title");
    const std::string* source = article->FindString("sourceURL");
    const std::string* html = article->FindString("contentHTML");
    out.Set("ok", true);
    out.Set("title", title ? *title : "");
    out.Set("sourceURL", source ? *source : "");
    // The reader extension's ladder knows which rung produced the article,
    // how much of the page it covered, who wrote it and which site rule
    // fired; the framework's distiller knows none of that, so each field
    // rides along only when its source supplied it. An answer with no rung
    // came from the distiller and says so.
    const std::string* rung = article->FindString("rung");
    out.Set("rung", rung ? *rung : "distiller");
    for (const char* key : {"byline", "siteName", "lang", "rule"}) {
      if (const std::string* value = article->FindString(key)) {
        out.Set(key, *value);
      }
    }
    if (std::optional<double> coverage = article->FindDouble("coverage")) {
      out.Set("coverage", *coverage);
    }
    if (std::optional<int> blocks = article->FindInt("codeBlockCount")) {
      out.Set("codeBlockCount", *blocks);
    }
    out.Set("isComplete", true);
    if (document) {
      out.Set("suggestedFileName",
              ReaderSuggestedFileName(title ? *title : ""));
      out.Set("document", BuildReaderDocument(title ? *title : "",
                                              source ? *source : "",
                                              html ? *html : ""));
    } else {
      out.Set("htmlLength", article->FindInt("htmlLength").value_or(0));
      out.Set("pageCount", article->FindInt("pageCount").value_or(1));
      if (include_html) {
        out.Set("contentHTML", html ? *html : "");
      }
    }
  }
  std::string reply;
  base::JSONWriter::Write(out, &reply);
  CompletePendingReply(request_id, std::move(reply));
}

bool PhiCoordinator::AuthorizeAgentPeer(const PhiAgentPeer& peer) {
  // I/O thread; the decision (and any consent dialog) runs on the UI thread,
  // posted like a verb so it never lands inside a nested loop. While one
  // consent dialog is up every later request waits in the deferred queue,
  // and a request for the same image path then finds that dialog's answer
  // in the session memory (AuthorizeAgentPeerOnUi) instead of raising a
  // second dialog — one prompt per program, however many connections it
  // opens at once.
  if (!msg_hwnd_) {
    return false;
  }
  PhiUiCall* call = NewUiCall(PhiUiCall::Kind::kAuthorize);
  if (!call) {
    return false;
  }
  call->peer = peer;
  if (!::PostMessageW(msg_hwnd_, WM_PHI_AUTHORIZE, 0,
                      reinterpret_cast<LPARAM>(call))) {
    call->Release();
    call->Release();
    return false;
  }
  if (!WaitUiCall(call)) {
    call->Release();
    return false;  // Nobody answered in time: no consent.
  }
  const bool allowed = call->allowed;
  call->Release();
  return allowed;
}

namespace {
// Programs whose image path names an INTERPRETER, not a program: a grant on
// node.exe would pre-approve every Node script that ever runs, including
// one dropped by a bad package. Those get Allow Once / Deny only.
bool IsInterpreterImage(const std::wstring& lowercased_path) {
  static constexpr std::wstring_view kInterpreters[] = {
      L"node.exe",   L"python.exe",     L"python3.exe", L"pwsh.exe",
      L"powershell.exe", L"cmd.exe",    L"bun.exe",     L"deno.exe",
  };
  const size_t cut = lowercased_path.find_last_of(L"\\/");
  const std::wstring_view base =
      cut == std::wstring::npos
          ? std::wstring_view(lowercased_path)
          : std::wstring_view(lowercased_path).substr(cut + 1);
  for (std::wstring_view name : kInterpreters) {
    if (base == name) {
      return true;
    }
  }
  return false;
}
}  // namespace

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
  if (consent_.agent_always_paths.count(key) ||
      session_allowed_peers_.count(key)) {
    return true;
  }
  if (session_denied_peers_.count(key)) {
    return false;  // Asked and denied this run — no dialog spam per request.
  }
  // The consent dialog — the Mac's AgentAccessApprovalAlert. Owned by a
  // shell window when one exists; a MessageBox fallback (allow-once only)
  // covers the pre-window startup connection. The text says what the
  // grant actually covers: the agent's own Spaces always, the user's own
  // ONLY while the Developer-pane toggle is on.
  std::wstring message =
      peer.image_path + L"\n(process " + std::to_wstring(peer.pid) +
      L") wants to connect to Phi as an agent. It can open its own agent "
      L"Spaces and drive them.";
  if (settings_.allow_agent_user_spaces) {
    message += L"\nIt can also read and change your own Spaces.";
  }
  HWND owner = nullptr;
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (controller->shell_hwnd()) {
      owner = controller->shell_hwnd();
      break;
    }
  }
  // An interpreter never gets "Always Allow" (see IsInterpreterImage).
  // Deny is the default action either way, so a stray Enter refuses.
  const bool interpreter = IsInterpreterImage(key);
  std::vector<std::wstring> options = {L"Allow Once"};
  if (!interpreter) {
    options.push_back(L"Always Allow");
  }
  options.push_back(L"Deny");
  const int deny_index = static_cast<int>(options.size()) - 1;
  int chosen = -1;
  if (owner) {
    // Deny preselected AND Cancel holding Return: Enter refuses; only a
    // deliberate click on Continue (or a row double-click) approves.
    chosen = PhiPromptChoose(owner, L"Agent access request", message.c_str(),
                             options, deny_index, L"Continue", L"Cancel",
                             /*cancel_is_default=*/true);
  } else {
    PhiModalScope modal;
    chosen = ::MessageBoxW(nullptr,
                           (message + L"\n\nAllow this connection?").c_str(),
                           L"Phi — Agent access request",
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                               MB_SETFOREGROUND) == IDYES
                 ? 0
                 : deny_index;
  }
  if (chosen == 0) {
    session_allowed_peers_.insert(key);
    return true;
  }
  if (chosen == 1 && !interpreter) {
    consent_.agent_always_paths.insert(key);
    ScheduleSave(StoreDomain::kConsent);
    PhiSettingsWindow::Refresh();
    return true;
  }
  session_denied_peers_.insert(key);
  return false;
}

std::vector<std::string> PhiCoordinator::AgentGrantRows() const {
  std::vector<std::string> rows;
  for (const std::wstring& path : consent_.agent_always_paths) {
    rows.push_back(base::WideToUTF8(path));
  }
  return rows;
}

void PhiCoordinator::RemoveAgentGrant(int index) {
  if (index < 0 ||
      index >= static_cast<int>(consent_.agent_always_paths.size())) {
    return;
  }
  auto it = consent_.agent_always_paths.begin();
  std::advance(it, index);
  consent_.agent_always_paths.erase(it);
  ScheduleSave(StoreDomain::kConsent);
}

std::vector<std::string> PhiCoordinator::CredentialGrantRows() const {
  std::vector<std::string> rows;
  for (const auto& [scope, kind] : consent_.credential_grants) {
    rows.push_back(scope + "\t" +
                   (kind == CredentialAccessKind::kReveal ? "reveal" : "fill"));
  }
  return rows;
}

void PhiCoordinator::RemoveCredentialGrant(int index) {
  if (index < 0 ||
      index >= static_cast<int>(consent_.credential_grants.size())) {
    return;
  }
  auto it = consent_.credential_grants.begin();
  std::advance(it, index);
  // Drop any live timed grant under the same scope too (their keys are
  // "<scope>|<page host>") — revoking must not leave a ten-minute window
  // standing behind the removed row.
  const std::string scope = it->first;
  std::erase_if(credential_timed_grants_, [&](const auto& entry) {
    return entry.first == scope ||
           entry.first.compare(0, scope.size() + 1, scope + "|") == 0;
  });
  consent_.credential_grants.erase(it);
  ScheduleSave(StoreDomain::kConsent);
}

std::vector<std::string> PhiCoordinator::CredentialAuditRows() const {
  return consent_.credential_audit;
}

void PhiCoordinator::PushSpaceMenu() const {
  if (!framework_) {
    return;
  }
  base::ListValue list;
  for (const Space& space : spaces_model_.spaces) {
    base::DictValue d;
    d.Set("id", space.id);
    d.Set("name", space.name);
    // The session id of the Space's open browser (any slot), so the menu
    // can exclude the Space the user is already looking at.
    BrowserId browser_id;
    for (PhiWindowController* controller : registry_.Controllers()) {
      browser_id = controller->BrowserIdForSpace(space.id);
      if (browser_id.valid()) {
        break;
      }
    }
    d.Set("windowId", static_cast<double>(browser_id.value));
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
  for (const UrlRule& rule : spaces_model_.url_rules) {
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
  for (PhiWindowController* controller : registry_.Controllers()) {
    const std::string& space_id = controller->active_space();
    const BrowserId browser_id = controller->active_browser_id();
    if (space_id.empty() || !browser_id.valid() || map.Find(space_id)) {
      continue;
    }
    map.Set(space_id, static_cast<double>(browser_id.value));
  }
  root.Set("spaceWindows", std::move(map));
  std::string json;
  base::JSONWriter::Write(root, &json);
  framework_->SetSpaceRoutingTable(json.c_str());
}

void PhiCoordinator::OnOpenLinkInSpace(int64_t source_window_id,
                                       const char* space_id_c,
                                       const char* url_c) {
  const std::string space_id = space_id_c ? space_id_c : "";
  const std::string url = url_c ? url_c : "";
  if (space_id.empty() || url.empty() || !framework_) {
    return;
  }
  RouteUrlToSpace(space_id, url, BrowserId{source_window_id});
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
  RouteUrlToSpace(space_id, url, BrowserId{source_window_id});
}

void PhiCoordinator::OnAskSpaceForURL(const char* url_c,
                                      const char* default_space_id_c,
                                      int64_t source_window_id,
                                      bool source_is_new_tab) {
  const std::string url = url_c ? url_c : "";
  const std::string default_space_id =
      default_space_id_c ? default_space_id_c : "";
  if (url.empty() || !framework_ || spaces_model_.spaces.empty()) {
    return;
  }
  // The source slot's shell owns the (modal) chooser; its active Space is
  // the "keep here" target.
  const BrowserId source{source_window_id};
  PhiWindowController* controller = registry_.ControllerForBrowser(source);
  const std::string current_space_id =
      controller ? controller->active_space() : std::string();

  std::vector<std::wstring> options;
  int default_index = 0;
  for (size_t i = 0; i < spaces_model_.spaces.size(); ++i) {
    std::wstring label = base::UTF8ToWide(spaces_model_.spaces[i].name);
    if (spaces_model_.spaces[i].id == current_space_id) {
      label += L"  (current)";
    }
    options.push_back(std::move(label));
    if (spaces_model_.spaces[i].id == default_space_id) {
      default_index = static_cast<int>(i);
    }
  }
  const int chosen = PhiPromptChoose(
      controller ? controller->shell_hwnd() : nullptr, L"Open in Space",
      base::UTF8ToWide(url).c_str(), options, default_index);
  const std::string chosen_space =
      chosen >= 0 && chosen < static_cast<int>(spaces_model_.spaces.size())
          ? spaces_model_.spaces[chosen].id
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
      controller->ScheduleRefresh();
    }
    return;
  }
  RouteUrlToSpace(chosen_space, url, source);
}

void PhiCoordinator::RouteUrlToSpace(const std::string& space_id,
                                     const std::string& url,
                                     BrowserId source_window) {
  // A slot that already holds the Space: switch to it there and open the
  // link (the Mac activates the Space's window and opens the tab). The open
  // bypasses URL routing — the URL may be the very navigation a rule just
  // re-homed, and re-matching it would loop.
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (!controller->HasSpace(space_id)) {
      continue;
    }
    controller->ShowSpace(space_id);
    framework_->OpenTabBypassingSpaceRouting(
        controller->active_browser_id().value, url.c_str());
    if (controller->shell_hwnd()) {
      ::SetForegroundWindow(controller->shell_hwnd());
    }
    controller->ScheduleRefresh();
    PushSpaceMenu();  // The visible-Space routing map changed.
    return;
  }
  // Cold Space: ask for its browser in its profile; the link opens once the
  // browser attaches (the spaces.switch machinery), so a request already in
  // flight for this slot and Space just gains the url.
  auto space_it =
      std::find_if(spaces_model_.spaces.begin(), spaces_model_.spaces.end(),
                   [&](const Space& s) { return s.id == space_id; });
  if (space_it == spaces_model_.spaces.end()) {
    return;
  }
  const SlotId slot =
      registry_.SlotFor(source_window).value_or(registry_.FirstUserSlot());
  if (!slot.valid()) {
    return;
  }
  if (PendingCreation* in_flight = registry_.FindPendingSpace(slot, space_id)) {
    in_flight->open_urls.push_back(url);
    return;
  }
  PendingCreation pending;
  pending.slot = slot;
  pending.space_id = space_id;
  pending.open_urls.push_back(url);
  RequestCreation(phi_framework::WindowKind::kSpace, space_it->profile_id,
                  /*incognito=*/false, BrowserId(), std::move(pending));
}

bool PhiCoordinator::OwnerForSlot(SlotId slot, std::string* profile_id,
                                  std::string* space_id) const {
  PhiWindowController* controller = registry_.Get(slot);
  if (!controller) {
    return false;
  }
  *space_id = controller->active_space();
  *profile_id = "Default";
  for (const Space& space : spaces_model_.spaces) {
    if (space.id == *space_id) {
      *profile_id = space.profile_id;
      break;
    }
  }
  return true;
}

std::string PhiCoordinator::LiveUrl(SlotId slot) const {
  const phi_framework::WindowSnapshot* window =
      window_model_.Get(ResolveBrowser(slot));
  return window ? window->nav.url : std::string();
}

std::string PhiCoordinator::CaptureFavicon(SlotId slot, int* width,
                                           int* height) {
  *width = 0;
  *height = 0;
  if (!framework_) {
    return std::string();
  }
  const BrowserId browser = ResolveBrowser(slot);
  const TabId active = ActiveTabId(browser);
  if (!active.valid()) {
    return std::string();
  }
  const unsigned char* pixels = framework_->TabFaviconBgraById(
      browser.value, active.value, width, height);
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
  view.icon = row.icon;
  view.layout = row.layout;
  if (!row.favicon_base64.empty()) {
    std::string decoded;
    if (base::Base64Decode(row.favicon_base64, &decoded)) {
      view.favicon_bgra.assign(decoded.begin(), decoded.end());
      view.favicon_width = row.favicon_width;
      view.favicon_height = row.favicon_height;
    }
  }
  if (!row.favicon_b_base64.empty()) {
    std::string decoded;
    if (base::Base64Decode(row.favicon_b_base64, &decoded)) {
      view.favicon_b_bgra.assign(decoded.begin(), decoded.end());
      view.favicon_b_width = row.favicon_b_width;
      view.favicon_b_height = row.favicon_b_height;
    }
  }
  return view;
}

}  // namespace

std::vector<PhiStoredRowView> PhiCoordinator::PinnedRows(int64_t slot) const {
  std::vector<PhiStoredRowView> out;
  std::string profile_id, space_id;
  if (OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
    for (const PhiRow& row : tab_store_.PinnedFor(profile_id, space_id)) {
      out.push_back(StoredRowView(row));
      UpgradeFavicon(SlotId{slot}, row.url, &out.back());
    }
  }
  return out;
}

std::vector<PhiStoredRowView> PhiCoordinator::BookmarkRows(
    int64_t slot,
    const std::string& parent_guid) const {
  std::vector<PhiStoredRowView> out;
  std::string profile_id, space_id;
  if (OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
    for (const PhiRow& row :
         tab_store_.BookmarksFor(profile_id, space_id, parent_guid)) {
      out.push_back(StoredRowView(row));
      UpgradeFavicon(SlotId{slot}, row.url, &out.back());
    }
  }
  return out;
}

std::vector<SidebarStoredItem> PhiCoordinator::SidebarStoredItems(
    int64_t slot) const {
  std::vector<SidebarStoredItem> items;
  std::string profile_id, space_id;
  if (!OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) return items;
  for (const PhiRow& row : tab_store_.PinnedFor(profile_id, space_id)) {
    items.push_back({row.guid, SidebarStoredKind::kPin, {}, row.index,
                     !row.url_b.empty()});
  }
  // Read the entire owned tree, including children of collapsed folders.
  // Expansion is presentation state and cannot decide whether a tab binds.
  for (const PhiRow& row : tab_store_.rows()) {
    if (row.profile_id != profile_id || row.space_id != space_id) continue;
    if (row.type == PhiRowType::kBookmark ||
        row.type == PhiRowType::kBookmarkFolder) {
      items.push_back({row.guid,
                       row.type == PhiRowType::kBookmarkFolder
                           ? SidebarStoredKind::kFolder
                           : SidebarStoredKind::kBookmark,
                       row.parent_guid, row.index, !row.url_b.empty()});
    }
  }
  return items;
}

std::map<int, SidebarSurface> PhiCoordinator::AttachedSurfaces(
    int64_t browser) const {
  return surfaces_.Surfaces(BrowserId{browser});
}

// ---- The sidebar's one mutator (docs/sidebar-architecture.md §5) ---------

namespace {

std::string IdsJson(int a, int b) {
  std::string json = "[" + std::to_string(a);
  if (b != 0 && b != a) {
    json += "," + std::to_string(b);
  }
  return json + "]";
}

}  // namespace

std::string PhiCoordinator::StoreTabsById(SlotId slot,
                                          TabId tab_a,
                                          TabId tab_b,
                                          bool as_bookmark,
                                          const std::string& parent_guid,
                                          int insert_index,
                                          bool bind) {
  std::string profile_id, space_id;
  if (!framework_ || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return std::string();
  }
  const BrowserId browser = ResolveBrowser(slot);
  const phi_framework::TabSnapshot* a = window_model_.FindTab(browser, tab_a);
  if (!a || a->url.empty()) {
    return std::string();
  }
  const bool split = tab_b.valid() && tab_b != tab_a;
  const phi_framework::TabSnapshot* b =
      split ? window_model_.FindTab(browser, tab_b) : nullptr;
  if (split && (!b || b->url.empty())) {
    return std::string();
  }
  struct Icon {
    std::string base64;
    int width = 0, height = 0;
  };
  const auto favicon = [&](TabId tab_id) {
    Icon icon;
    const unsigned char* pixels = framework_->TabFaviconBgraById(
        browser.value, tab_id.value, &icon.width, &icon.height);
    if (pixels && icon.width > 0 && icon.height > 0) {
      const size_t bytes = static_cast<size_t>(icon.width) *
                           static_cast<size_t>(icon.height) * 4;
      // SAFETY: TabFaviconBgraById reports the buffer's own dimensions.
      const auto view =
          UNSAFE_BUFFERS(base::span<const uint8_t>(pixels, bytes));
      icon.base64 = base::Base64Encode(view);
    } else {
      icon.width = 0;
      icon.height = 0;
    }
    return icon;
  };
  const Icon icon_a = favicon(tab_a);
  const std::string layout = split ? LiveSplitLayout(browser, tab_a)
                                   : std::string();
  // Pins have no tree; a bookmark lands under `parent_guid`.
  const std::string guid =
      as_bookmark
          ? tab_store_.AddBookmark(profile_id, space_id, parent_guid, a->title,
                                   a->url, /*folder=*/false, icon_a.base64,
                                   icon_a.width, icon_a.height)
          : tab_store_.AddPinned(profile_id, space_id, a->title, a->url,
                                 icon_a.base64, icon_a.width, icon_a.height);
  if (guid.empty()) {
    return guid;
  }
  if (split) {
    const Icon icon_b = favicon(tab_b);
    tab_store_.SetRowSecondary(guid, b->url, b->title, icon_b.base64,
                               icon_b.width, icon_b.height);
    if (!layout.empty()) {
      tab_store_.SetRowLayout(guid, layout);
    }
  }
  if (insert_index >= 0) {
    tab_store_.Reorder(guid, insert_index);
  }
  if (bind) {
    // The Mac's updateTabCustomValue: the live tab(s) carry the cell's
    // guid and move into it.
    TagTab(browser, tab_a.value,
                              TagFor(guid, false).c_str());
    if (split) {
      TagTab(browser, tab_b.value,
                                TagFor(guid, true).c_str());
    }
  }
  registry_.InvalidateAll();
  return guid;
}

void PhiCoordinator::TagTab(BrowserId browser, int tab_id, const char* tag) {
  // A tag is snapshot state. The framework marks the window dirty and a
  // push follows, but the rebuild this gesture posts may run BEFORE that
  // push lands (a posted message vs a Chromium task), so pull now: the
  // binding must be right on the very next frame, not one frame later.
  if (!framework_) {
    return;
  }
  framework_->SetTabTagById(browser.value, tab_id, tag);
  window_model_.Refresh(framework_, browser);
}

TabId PhiCoordinator::OpenCell(BrowserId browser, const std::string& guid) {
  const PhiRow* row = tab_store_.Find(guid);
  if (!framework_ || !row || row->url.empty() ||
      row->type == PhiRowType::kBookmarkFolder) {
    return TabId();
  }
  // Copied out: the store may move under the tag writes below.
  const std::string url_b = row->url_b;
  const std::string layout = row->layout;
  const TabId a = framework_->OpenTab(browser, row->url.c_str());
  if (!a.valid()) {
    return TabId();
  }
  TagTab(browser, a.value, TagFor(guid, false).c_str());
  if (!url_b.empty()) {
    const TabId b = framework_->OpenTab(browser, url_b.c_str());
    if (b.valid()) {
      TagTab(browser, b.value,
                                TagFor(guid, true).c_str());
      framework_->CreateSplitViewForTabs(
          browser.value, a.value, b.value,
          layout.empty() ? "vertical" : layout.c_str());
    }
  }
  return a;
}

bool PhiCoordinator::ApplyMove(SlotId slot,
                               const SidebarMove& move,
                               const SidebarTree& tree) {
  const SidebarKey& key = move.key;
  const SidebarPlace& place = move.place;
  if (!framework_ || key.empty() || MoveIsNoop(tree, key, place)) {
    return false;
  }
  const BrowserId browser = ResolveBrowser(slot);
  const bool live = key.kind == SidebarKey::Kind::kTab ||
                    key.kind == SidebarKey::Kind::kSplit;
  const bool stored = key.kind == SidebarKey::Kind::kCell ||
                      key.kind == SidebarKey::Kind::kFolder;
  const SidebarCell* cell = stored ? tree.CellFor(key.id) : nullptr;
  if (stored && !cell) {
    return false;
  }
  const auto refresh = [&] { registry_.InvalidateAll(); };
  if (place.section == SidebarPlace::Section::kTabs) {
    const int anchor = PlaceAnchorTab(tree, place);
    const char* group = place.group_explicit ? place.group_id.c_str() : nullptr;
    if (live) {
      // A reorder (the pair moves as a unit inside the framework), possibly
      // joining or leaving a group on the way.
      const bool ok = framework_->MoveTabBefore(
          browser.value, TabOfKey(tree, key, 0), anchor, group);
      refresh();
      return ok;
    }
    if (key.kind != SidebarKey::Kind::kCell) {
      return false;  // Folders and groups never become tabs.
    }
    // Unpin / un-bookmark into the list (the Mac's movePinnedTabOut /
    // moveBookmarkOut(toNormalTabs:)): the cell's tab(s) — opened first
    // when it was closed — become plain and land at the anchor; the cell
    // goes.
    TabId tab{cell->binding.tab_a ? cell->binding.tab_a
                                  : cell->binding.tab_b};
    if (!tab.valid()) {
      tab = OpenCell(browser, key.id);
    }
    RemoveStoredRow(key.id);  // Clears the tags; the tabs stay plain.
    if (tab.valid() && (anchor != 0 || group)) {
      framework_->MoveTabBefore(browser.value, tab.value, anchor, group);
    }
    refresh();
    return true;
  }
  // A stored destination: the pinned grid or a bookmark folder.
  const bool as_bookmark = place.section == SidebarPlace::Section::kBookmarks;
  const std::string parent = as_bookmark ? place.parent_guid : std::string();
  if (live) {
    // The drag conversions (the Mac's moveNormalTab(toPinnd:/toBookmark:)):
    // the tab(s) become a bound cell at the anchor.
    const TabId tab_b{key.kind == SidebarKey::Kind::kSplit
                          ? TabOfKey(tree, key, 1)
                          : 0};
    return !StoreTabsById(slot, TabId{TabOfKey(tree, key, 0)}, tab_b,
                          as_bookmark, parent,
                          PlaceSiblingIndex(tree, place, ""),
                          /*bind=*/true)
                .empty();
  }
  const bool is_pin = cell->kind == SidebarStoredKind::kPin;
  if (is_pin == !as_bookmark) {
    // Same section: a reorder (pins) or a tree move (bookmarks, folders).
    const int index = PlaceSiblingIndex(tree, place, key.id);
    const bool ok = is_pin ? tab_store_.Reorder(key.id, index)
                           : tab_store_.Move(key.id, parent, index);
    if (ok) {
      refresh();
      PhiSettingsWindow::Refresh();
    }
    return ok;
  }
  if (cell->kind == SidebarStoredKind::kFolder) {
    return false;  // A folder cannot become a pin.
  }
  // Across the two stored sections: the row changes kind, its live panes
  // following it (the Mac's movePinnedTabOut(toBookmark:) and
  // moveBookmarkOut(toPinnedTabs:)).
  return ConvertStoredRow(slot, key.id, as_bookmark, parent,
                          PlaceSiblingIndex(tree, place, ""));
}

bool PhiCoordinator::Apply(int64_t slot_number,
                           const SidebarCommand& command,
                           const SidebarTree& tree) {
  if (!framework_) {
    return false;
  }
  const SlotId slot{slot_number};
  const BrowserId browser = ResolveBrowser(slot);
  const auto refresh = [&] { registry_.InvalidateAll(); };
  // The live split a key stands for: a split row's token, or a split cell's
  // live split when both panes are open.
  const auto split_of = [&](const SidebarKey& key) -> std::string {
    if (key.kind == SidebarKey::Kind::kSplit) {
      return key.id;
    }
    if (key.kind == SidebarKey::Kind::kCell) {
      if (const SidebarCell* cell = tree.CellFor(key.id)) {
        return cell->binding.split_id;
      }
    }
    if (key.kind == SidebarKey::Kind::kTab) {
      if (const phi_framework::TabSnapshot* tab =
              window_model_.FindTab(browser, TabId{key.tab_id()})) {
        return tab->split_id;
      }
    }
    return std::string();
  };
  // A key's tab, opening a closed cell when the command needs a live one.
  const auto live_or_open = [&](const SidebarKey& key) {
    int tab = TabOfKey(tree, key, 0);
    if (tab == 0 && key.kind == SidebarKey::Kind::kCell) {
      tab = OpenCell(browser, key.id).value;
    }
    return tab;
  };
  return std::visit(
      absl::Overload{
          [&](const SidebarMove& move) { return ApplyMove(slot, move, tree); },
          [&](const SidebarActivate& cmd) {
            const int tab = TabOfKey(tree, cmd.key, cmd.pane);
            if (tab == 0) {
              return false;  // A closed cell: the controller opens it.
            }
            const bool ok = framework_->ActivateTabById(browser.value, tab);
            refresh();
            return ok;
          },
          [&](const SidebarClose& cmd) {
            bool ok = false;
            for (int pane : {1, 0}) {  // Pane B first; ids hold either way.
              if (cmd.pane >= 0 && cmd.pane != pane) {
                continue;
              }
              const int tab = TabOfKey(tree, cmd.key, pane);
              if (tab != 0) {
                ok = framework_->CloseTabById(browser.value, tab) || ok;
              }
            }
            refresh();
            return ok;
          },
          [&](const SidebarRemoveCell& cmd) {
            if (cmd.key.kind != SidebarKey::Kind::kCell &&
                cmd.key.kind != SidebarKey::Kind::kFolder) {
              return false;
            }
            RemoveStoredRow(cmd.key.id);
            return true;
          },
          [&](const SidebarFormSplit& cmd) {
            const int a = live_or_open(cmd.a);
            const int b = live_or_open(cmd.b);
            if (a == 0 || b == 0 || a == b) {
              return false;
            }
            // A split never lives in a stored cell by accident: both panes
            // become plain first (their cells stay as closed placeholders).
            TagTab(browser, a, "");
            TagTab(browser, b, "");
            const char* created = framework_->CreateSplitViewForTabs(
                browser.value, a, b,
                cmd.layout.empty() ? "vertical" : cmd.layout.c_str());
            refresh();
            return created && *created;
          },
          [&](const SidebarBreakSplit& cmd) {
            const std::string split_id = split_of(cmd.key);
            if (split_id.empty()) {
              return false;
            }
            if (cmd.key.kind == SidebarKey::Kind::kCell) {
              // The cell stays as a closed split placeholder.
              for (int pane : {0, 1}) {
                if (const int tab = TabOfKey(tree, cmd.key, pane)) {
                  TagTab(browser, tab, "");
                }
              }
            }
            const bool ok = framework_->RemoveSplitById(browser.value, split_id.c_str());
            refresh();
            return ok;
          },
          [&](const SidebarSwapPanes& cmd) {
            const std::string split_id = split_of(cmd.key);
            bool ok = false;
            if (!split_id.empty()) {
              ok = framework_->SwapSplitView(browser.value, split_id.c_str());
            }
            if (cmd.key.kind == SidebarKey::Kind::kCell) {
              // The stored panes and their tags follow the live order.
              const int a = TabOfKey(tree, cmd.key, 0);
              const int b = TabOfKey(tree, cmd.key, 1);
              ok = tab_store_.SwapPanes(cmd.key.id) || ok;
              if (a) TagTab(browser, a, TagFor(cmd.key.id, true).c_str());
              if (b) TagTab(browser, b, TagFor(cmd.key.id, false).c_str());
            }
            refresh();
            return ok;
          },
          [&](const SidebarSetLayout& cmd) {
            bool ok = false;
            const std::string split_id = split_of(cmd.key);
            if (!split_id.empty()) {
              ok = framework_->UpdateSplitView(browser.value, split_id.c_str(),
                                               cmd.layout.c_str(), -1.0);
            }
            // The stored cell remembers the axis so the split reopens with
            // it — the cell itself, or the cell a split row's pane is bound to.
            std::string guid;
            if (cmd.key.kind == SidebarKey::Kind::kCell) {
              guid = cmd.key.id;
            } else {
              const int tab = TabOfKey(tree, cmd.key, 0);
              const auto owner = tree.owner_of.find(tab);
              if (owner != tree.owner_of.end()) {
                guid = owner->second.id;
              }
            }
            if (!guid.empty()) {
              ok = tab_store_.SetRowLayout(guid, cmd.layout) || ok;
            }
            refresh();
            return ok;
          },
          [&](const SidebarJoinGroup& cmd) {
            const int a = TabOfKey(tree, cmd.key, 0);
            if (a == 0) {
              return false;
            }
            const std::string ids = IdsJson(a, TabOfKey(tree, cmd.key, 1));
            bool ok;
            if (cmd.group_id.empty()) {
              const char* created =
                  framework_->CreateTabGroupForTabs(browser.value, ids.c_str());
              ok = created && *created;
            } else {
              ok = framework_->AddTabsToGroup(browser.value, cmd.group_id.c_str(),
                                              ids.c_str());
            }
            refresh();
            return ok;
          },
          [&](const SidebarLeaveGroup& cmd) {
            const int a = TabOfKey(tree, cmd.key, 0);
            if (a == 0) {
              return false;
            }
            const std::string ids = IdsJson(a, TabOfKey(tree, cmd.key, 1));
            const bool ok = framework_->RemoveTabsFromGroupByIds(browser.value, ids.c_str());
            refresh();
            return ok;
          },
          [&](const SidebarSetGroupCollapsed& cmd) {
            if (cmd.key.kind != SidebarKey::Kind::kGroup) {
              return false;
            }
            const bool ok = framework_->SetTabGroupCollapsed(
                browser.value, cmd.key.id.c_str(), cmd.collapsed);
            refresh();
            return ok;
          },
          [&](const SidebarBookmarkCopy& cmd) {
            const TabId tab_b{cmd.key.kind == SidebarKey::Kind::kSplit
                                  ? TabOfKey(tree, cmd.key, 1)
                                  : 0};
            return !StoreTabsById(slot, TabId{TabOfKey(tree, cmd.key, 0)},
                                  tab_b, /*as_bookmark=*/true, std::string(),
                                  /*insert_index=*/-1, /*bind=*/false)
                        .empty();
          },
          [&](const SidebarCloseSurface& cmd) {
            bool any = false;
            for (int pane : {0, 1}) {
              const TabId tab{TabOfKey(tree, cmd.key, pane)};
              if (tab.valid() && surfaces_.ForOrigin(tab)) {
                CloseSurfacesForOrigin(tab);
                any = true;
              }
            }
            return any;
          },
          [&](const SidebarMoveToSpace& cmd) {
            if (cmd.key.kind == SidebarKey::Kind::kCell) {
              return MoveStoredRowToSpace(cmd.key.id, cmd.space_id, cmd.clone);
            }
            const TabId tab{TabOfKey(tree, cmd.key, 0)};
            if (!window_model_.FindTab(browser, tab)) {
              return false;
            }
            RequestTabToSpace(slot, tab, cmd.space_id, !cmd.clone);
            return true;
          },
      },
      command);
}

void PhiCoordinator::UpgradeFavicon(SlotId slot,
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
      ResolveBrowser(slot).value, url.c_str(), &width, &height);
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
    framework_->OpenTab(ResolveBrowser(slot), url.c_str());
  }
}

std::string PhiCoordinator::LiveSplitLayout(BrowserId browser,
                                            TabId tab) const {
  const phi_framework::SplitSnapshot* split =
      window_model_.SplitOfTab(browser, tab);
  if (!split) {
    return std::string();
  }
  return split->layout == phi_framework::SplitLayout::kHorizontal
             ? "horizontal"
             : "vertical";
}

void PhiCoordinator::StoreSplitInternal(SlotId slot, TabId tab_a,
                                        TabId tab_b, bool as_bookmark,
                                        int insert_index,
                                        bool bind_bookmark) {
  std::string profile_id, space_id;
  if (!framework_ || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return;
  }
  const BrowserId browser = ResolveBrowser(slot);
  struct Pane {
    std::string title, url, favicon;
    int favicon_width = 0, favicon_height = 0;
  };
  const auto capture = [&](TabId tab, Pane* pane) {
    const phi_framework::TabSnapshot* live = window_model_.FindTab(browser, tab);
    if (!live) {
      return;
    }
    pane->title = live->title;
    pane->url = live->url;
    int width = 0, height = 0;
    const unsigned char* pixels = framework_->TabFaviconBgraById(
        browser.value, tab.value, &width, &height);
    if (pixels && width > 0 && height > 0) {
      const size_t bytes =
          static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
      // SAFETY: TabFaviconBgraById reports the buffer's own dimensions.
      const auto view =
          UNSAFE_BUFFERS(base::span<const uint8_t>(pixels, bytes));
      pane->favicon = base::Base64Encode(view);
      pane->favicon_width = width;
      pane->favicon_height = height;
    }
  };
  Pane a, b;
  capture(tab_a, &a);
  capture(tab_b, &b);
  if (a.url.empty() || b.url.empty()) {
    return;
  }
  // Remember the arrangement the live split is in, so pinning or
  // bookmarking a STACKED split reopens it stacked (the Mac persists the
  // layout on the stored rows, not just on live ones).
  std::string layout = LiveSplitLayout(browser, tab_a);
  // ONE row per split cell, pin or bookmark alike (docs/sidebar-architecture
  // §4.1): pane A is the row, pane B rides url_b/title_b/favicon_b. A live
  // pane binds by tag — TagFor(guid, false) / TagFor(guid, true). A pinned
  // split always binds; a split bookmark binds only on the drag path
  // (the Mac's bindLiveSplit), the menu leaves an independent copy.
  const std::string guid =
      as_bookmark
          ? tab_store_.AddBookmark(profile_id, space_id, std::string(),
                                   a.title, a.url, /*folder=*/false,
                                   a.favicon, a.favicon_width,
                                   a.favicon_height)
          : tab_store_.AddPinned(profile_id, space_id, a.title, a.url,
                                 a.favicon, a.favicon_width,
                                 a.favicon_height);
  if (!guid.empty()) {
    tab_store_.SetRowSecondary(guid, b.url, b.title, b.favicon,
                               b.favicon_width, b.favicon_height);
    if (insert_index >= 0) {
      tab_store_.Reorder(guid, insert_index);
    }
    if (!layout.empty()) {
      tab_store_.SetRowLayout(guid, layout);
    }
    if (!as_bookmark || bind_bookmark) {
      TagTab(browser, tab_a.value,
                                TagFor(guid, false).c_str());
      TagTab(browser, tab_b.value,
                                TagFor(guid, true).c_str());
    }
  }
  registry_.InvalidateAll();
}

// Every live tab, in every browser the model knows, whose tag binds it to
// the stored row `guid` — either pane. Across every browser because a
// profile- or app-scoped pin can be open in any of them.
std::vector<PhiCoordinator::BoundTab> PhiCoordinator::TabsBoundTo(
    const std::string& guid) const {
  std::vector<BoundTab> bound;
  for (const BrowserId browser : window_model_.Browsers()) {
    for (const phi_framework::TabSnapshot& tab :
         window_model_.Get(browser)->tabs) {
      SidebarTag parsed;
      if (ParseTag(tab.tag, &parsed) && parsed.guid == guid) {
        bound.push_back({browser, tab.id, parsed.pane_b});
      }
    }
  }
  return bound;
}

void PhiCoordinator::RemoveStoredRow(const std::string& guid) {
  if (tab_store_.Remove(guid)) {
    // Clear the identity tag from any tab bound to the removed row (either
    // pane) — it returns to the tab list as a normal tab, and a stale tag
    // would keep it excluded from tab groups (the Phi-managed rule).
    if (framework_) {
      for (const BoundTab& bound : TabsBoundTo(guid)) {
        TagTab(bound.browser, bound.tab.value, "");
      }
    }
    registry_.InvalidateAll();
    PhiSettingsWindow::Refresh();
  }
}

bool PhiCoordinator::SetStoredRowIcon(const std::string& guid,
                                     const std::string& icon) {
  if (!tab_store_.SetRowIcon(guid, icon)) {
    return false;
  }
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
  return true;
}

bool PhiCoordinator::SetStoredRowUrl(const std::string& guid,
                                    const std::string& url) {
  if (!tab_store_.SetRowUrl(guid, url)) {
    return false;
  }
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
  return true;
}

bool PhiCoordinator::IsTabAgentDriven(int tab_id) const {
  return agent_drives_.IsMasked(tab_id);
}

bool PhiCoordinator::IsTabReclaimed(int tab_id) const {
  return agent_drives_.IsReclaimed(tab_id);
}

void PhiCoordinator::RequestReclaimTab(int tab_id) {
  agent_drives_.Reclaim(tab_id);
}

void PhiCoordinator::RequestReleaseTab(int tab_id) {
  agent_drives_.Release(tab_id);
}

std::string PhiCoordinator::LastActiveSpaceId() const {
  // A live slot's current Space wins when there is one (a reopen mid-run);
  // at cold start there is no window yet, so answer what PhiRestore.json
  // remembers from the last run — falling back to the default Space when
  // that id was deleted since.
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (controller->is_agent()) {
      continue;
    }
    const std::string& active = controller->active_space();
    if (!active.empty()) {
      return active;
    }
  }
  if (!restore_.last_active_space_id.empty() &&
      std::any_of(spaces_model_.spaces.begin(), spaces_model_.spaces.end(),
                  [&](const Space& s) {
                    return s.id == restore_.last_active_space_id;
                  })) {
    return restore_.last_active_space_id;
  }
  return DefaultSpaceId();  // What a fresh shell opens with.
}

void PhiCoordinator::NoteActiveSpace(const std::string& space_id) {
  // Coalesced: a switch is one keypress, and the store rewrites the whole
  // file. The save timer folds a burst into one write; a shutdown flushes
  // it early (FlushForShutdown).
  if (space_id.empty() || space_id == restore_.last_active_space_id) {
    return;
  }
  restore_.last_active_space_id = space_id;
  ScheduleSave(StoreDomain::kRestore);
}

void PhiCoordinator::OpenDefaultShellIfNoUserWindow(const char* reason) {
  if (registry_.FirstUserSlot().valid() || !framework_) {
    return;  // The user has a window; nothing to rescue.
  }
  // A restore that produced no window would leave the process alive with
  // nothing on screen (lane 4 §6 item 6). Ask for a browser in the default
  // Space's profile with NO slot to attach to, which is how a request says
  // "host it as a new shell" (AttachRequestedSpaceBrowser). One at a time:
  // a second call before the first arrives would open two.
  std::string profile = "Default";
  const std::string default_space = DefaultSpaceId();
  for (const Space& space : spaces_model_.spaces) {
    if (space.id == default_space) {
      profile = space.profile_id;
      break;
    }
  }
  if (registry_.FindPendingSpace(SlotId(), default_space)) {
    return;
  }
  LOG(WARNING) << "[PhiClient] no user window after restore (" << reason
               << "); opening a default shell in " << profile;
  PendingCreation pending;
  pending.space_id = default_space;
  RequestCreation(phi_framework::WindowKind::kSpace, profile,
                  /*incognito=*/false, BrowserId(), std::move(pending));
}

void PhiCoordinator::OnColdStartRestorePlan(
    std::vector<int>* eager_window_ids,
    bool* armed) {
  // Refuse to gate until the Space records are actually loaded. Arming with
  // an empty set here would park every window the user expected to see; an
  // unfiltered replay is the correct fallback, and `*armed` arrives false.
  if (!eager_window_ids || !armed || ghost_spaces_.Empty()) {
    LOG(INFO) << "[PhiClient] ColdStartRestorePlan: NOT armed (records "
              << (ghost_spaces_.Empty() ? "not loaded" : "loaded") << ")";
    Transition(AppPhase::kRestorePlanned, "unfiltered");
    return;
  }
  Transition(AppPhase::kRestorePlanned, "armed");
  // The active Space's window is rebuilt now; every other saved window parks
  // and materializes when the user switches to it.
  *eager_window_ids = ghost_spaces_.EagerWindowIds(LastActiveSpaceId());
  *armed = true;
  LOG(INFO) << "[PhiClient] ColdStartRestorePlan: armed space="
            << LastActiveSpaceId() << " eager="
            << (eager_window_ids->empty() ? 0 : eager_window_ids->front());
  // A cold start has no settlement callback, so the "eager set matched
  // nothing" case (the ids named windows the session file no longer holds:
  // every window parks, none appears) is caught by a timer: once the
  // replay has had its say and no user window exists, open a default
  // shell. A user window arriving disarms it (OnBrowserWindowCreated).
  cold_start_armed_ = true;
  if (msg_hwnd_) {
    ::SetTimer(msg_hwnd_, kColdStartFallbackTimer, kColdStartFallbackMs,
               nullptr);
  }
}

void PhiCoordinator::OnColdStartParkedWindows(const char* parked_json,
                                              const char* reporting_profile) {
  // Same shape as the reopen receipt's `parked`, so it feeds the one path.
  // Wrapped to match, since the cold start reports the list bare.
  base::DictValue receipt;
  std::optional<base::Value> parked = base::JSONReader::Read(
      parked_json ? parked_json : "[]", base::JSON_PARSE_RFC);
  if (parked && parked->is_list()) {
    receipt.Set("parked", std::move(*parked));
  } else {
    receipt.Set("parked", base::ListValue());
  }
  std::string json;
  base::JSONWriter::Write(receipt, &json);
  LOG(INFO) << "[PhiClient] ColdStartParked profile=" << reporting_profile
            << " parked=" << (parked_json ? parked_json : "(null)");
  ghost_spaces_.ReplaceFromReceipt(json);
  PushSpaceMenu();
  if (phase_ < AppPhase::kRestoring) {
    Transition(AppPhase::kRestoring, "parked");
  }
  if (phase_ >= AppPhase::kSettled) {
    // The diversion landed AFTER the restore settled (a user shell's first
    // snapshot beat it): the settle-time orphan drop found nothing, so run
    // it for this receipt — the parked windows no Space claims must not be
    // replayed as loose windows by the next unfiltered restore.
    DropOrphanGhosts();
  }
  if (cold_start_armed_ && msg_hwnd_) {
    // The diversion landed; give the eager window (if any) a moment to be
    // built before deciding nothing came.
    ::SetTimer(msg_hwnd_, kColdStartFallbackTimer, kColdStartSettleMs,
               nullptr);
  }
}

void PhiCoordinator::SettleRestore(const char* reason) {
  // Parked windows no Space claims (the Space was deleted while its window
  // sat parked) would be replayed as loose windows by the next unfiltered
  // restore; drop them once the receipt is in, whichever path brought it.
  DropOrphanGhosts();
  // A restore that produced no user window (the eager set matched nothing,
  // nothing restored, the cold start parked everything) leaves the process
  // alive with nothing on screen: open a default shell. A no-op when the
  // user has a window, so every path may ask.
  OpenDefaultShellIfNoUserWindow(reason);
  if (phase_ < AppPhase::kSettled) {
    Transition(AppPhase::kSettled, reason);
    // Nothing sits between the two today; kept as distinct phases so a
    // post-settle step (a deferred migration, say) has a slot to land in.
    Transition(AppPhase::kRunning, "settled");
  }
}

void PhiCoordinator::DropOrphanGhosts() {
  for (const PhiGhostSpaces::Remembered& orphan : ghost_spaces_.Orphans()) {
    if (framework_) {
      framework_->DropGhostWindow(orphan.profile.c_str(),
                                  static_cast<int>(orphan.window_id.value));
    }
  }
  ghost_spaces_.ClearOrphans();
}

void PhiCoordinator::OnTabDistillable(int64_t window_id, int tab_id,
                                     bool distillable) {
  // Route to the shell hosting this browser (every browser a slot shows,
  // its original included, is bound in the registry).
  PhiWindowController* controller =
      registry_.ControllerForBrowser(BrowserId{window_id});
  LOG(INFO) << "[PhiClient] Distillable win=" << window_id << " tab=" << tab_id
            << " d=" << distillable << " routed=" << (controller != nullptr);
  if (controller) {
    controller->NoteTabDistillable(TabId{tab_id}, distillable);
  }
}

void PhiCoordinator::NoteTabDistillable(int64_t window_id, int tab_id,
                                        bool distillable) {
  OnTabDistillable(window_id, tab_id, distillable);
}

void PhiCoordinator::OnExtensionMessage(const char* request_id,
                                        const char* sender_id,
                                        const char* type,
                                        const char* payload_json) {
  const std::string id = request_id ? request_id : "";
  const std::string sender = sender_id ? sender_id : "";
  const std::string message_type = type ? type : "";
  if (reader_.HandleExtensionMessage(
          id, sender, message_type,
          payload_json && *payload_json ? payload_json : "{}")) {
    return;
  }
  // Nothing here speaks this type. Fail it rather than leave the extension
  // waiting out its own timeout for an answer that will never come.
  LOG(INFO) << "[PhiClient] unhandled extension message " << message_type
            << " from " << sender;
  if (framework_ && !id.empty()) {
    framework_->FailExtensionMessage(id.c_str(), "Unhandled message type");
  }
}

void PhiCoordinator::OnOpenReaderView(int64_t window_id, int64_t tab_id) {
  // Right-click "Open in Reading Mode" in the page's own context menu.
  // ENTERS rather than toggles: the item is only reachable by right-clicking
  // the LIVE page, which is what is on screen when the reader is closed.
  const int tab = static_cast<int>(tab_id);
  LOG(INFO) << "[PhiClient] context menu reader win=" << window_id
            << " tab=" << tab;
  if (reader_.HasSurface(tab)) {
    return;
  }
  reader_.Open(tab);
}

void PhiCoordinator::RespondToExtension(const std::string& request_id,
                                        const std::string& response_json) {
  if (framework_ && !request_id.empty()) {
    framework_->RespondToExtensionMessage(request_id.c_str(),
                                          response_json.c_str());
  }
}

bool PhiCoordinator::BroadcastToExtensions(const std::string& type,
                                           const std::string& payload_json) {
  return framework_ && framework_->BroadcastToExtensions(type.c_str(),
                                                         payload_json.c_str());
}

PhiWindowController* PhiCoordinator::WindowShowingTab(TabId tab_id) {
  const std::optional<BrowserId> browser = window_model_.BrowserOfTab(tab_id);
  if (!browser) {
    return nullptr;
  }
  PhiWindowController* window = registry_.ControllerForBrowser(*browser);
  // A slot hosts N Space browsers; only the one on screen "shows" the tab.
  return window && window->active_browser_id() == *browser ? window : nullptr;
}

void PhiCoordinator::OnReaderStyleChanged() {
  SyncReaderStyleToModel();
  ScheduleSave(StoreDomain::kSettings);
}

bool PhiCoordinator::PanelizeReaderSurface(TabId origin_tab_id) {
  if (!framework_ || HasReaderPanelForOrigin(origin_tab_id)) {
    return false;
  }
  PhiWindowController* window = WindowShowingTab(origin_tab_id);
  if (!window) {
    return false;
  }
  const BrowserId browser = ResolveBrowser(window->slot());
  TabId surface_tab_id;
  if (const phi_framework::WindowSnapshot* snapshot =
          window_model_.Get(browser)) {
    for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
      if (tab.surface == phi_framework::TabSurface::kReader &&
          tab.origin == origin_tab_id) {
        surface_tab_id = tab.id;
        break;
      }
    }
  }
  if (!surface_tab_id.valid()) {
    // Not an error: the framework panels a surface at insertion (see
    // DivertReaderSurfaceAfterInsert); by the time the extension reports
    // the open, the tab is usually already out of this strip.
    return false;
  }
  const BrowserId panel{framework_->MoveTabToReaderPanel(
      browser.value, surface_tab_id.value, origin_tab_id.value)};
  if (!panel.valid()) {
    LOG(WARNING) << "[PhiClient] could not panel reader surface "
                 << surface_tab_id.value;
    return false;
  }
  if (PhiSurfacePanel* panelled = surfaces_.Find(panel)) {
    panelled->BindSurfaceTab(surface_tab_id.value);
  }
  // Chromium foregrounded the surface when the extension created it, and
  // moving it out leaves whatever neighbour it landed on active. Hand the
  // strip back to the origin so tab-scoped commands keep targeting the
  // article the reader is showing — the Mac's setAsActiveTab step.
  framework_->ActivateTabById(browser.value, origin_tab_id.value);
  window->ScheduleRefresh();
  return true;
}

bool PhiCoordinator::HasReaderPanelForOrigin(TabId origin_tab_id) const {
  return surfaces_.ForOrigin(origin_tab_id, SidebarSurface::kReader) != nullptr;
}

void PhiCoordinator::CloseReaderPanelForOrigin(TabId origin_tab_id) {
  CloseSurfacesForOrigin(origin_tab_id, SidebarSurface::kReader);
}

void PhiCoordinator::CloseSurfacesForOrigin(TabId origin_tab_id,
                                            SidebarSurface kind) {
  if (!framework_) {
    return;
  }
  // Snapshot first: closing a browser runs OnBrowserWindowClosing, which
  // detaches the record from the registry.
  std::vector<BrowserId> doomed;
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (panel.origin_tab_id() == origin_tab_id.value &&
        (kind == SidebarSurface::kContent || panel.kind() == kind)) {
      doomed.push_back(BrowserId{panel.window_id()});
    }
  });
  for (const BrowserId window : doomed) {
    // Closing the window closes the surface tab with it; the reader
    // extension drops its cache on tabs.onRemoved, so both sides agree.
    framework_->CloseBrowserWindow(window.value);
  }
}

void PhiCoordinator::ExpandReaderPanelIntoTab(int64_t panel_window_id,
                                              const std::string& url) {
  const BrowserId panel{panel_window_id};
  if (!surfaces_.Find(panel)) {
    return;
  }
  pending_reader_expands_[panel] = url;
  if (msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_READER_EXPAND,
                   static_cast<WPARAM>(panel.value), 0);
  }
}

void PhiCoordinator::FinishReaderPanelExpand(BrowserId panel_window) {
  auto pending = pending_reader_expands_.find(panel_window);
  if (pending == pending_reader_expands_.end()) {
    return;
  }
  const std::string url = pending->second;
  pending_reader_expands_.erase(pending);
  PhiSurfacePanel* reader = surfaces_.Find(panel_window);
  if (!reader || !framework_) {
    return;
  }
  const BrowserId origin_window{reader->origin_window_id()};
  // Like Peek's expand, this RE-OPENS the url rather than moving the live tab
  // across: the destination has only just started loading, and moving the tab
  // out would empty the panel browser mid-move.
  framework_->OpenTab(origin_window, url.c_str());
  framework_->CloseBrowserWindow(panel_window.value);
}

void PhiCoordinator::ApplyLegacyTagAliases(BrowserId window_id) {
  const auto& aliases = tab_store_.legacy_tag_aliases();
  const phi_framework::WindowSnapshot* snapshot = window_model_.Get(window_id);
  if (aliases.empty() || !framework_ || !snapshot) {
    return;
  }
  std::vector<std::string> applied;
  for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
    if (tab.tag.empty()) {
      continue;
    }
    const auto alias = aliases.find(tab.tag);
    if (alias == aliases.end()) {
      continue;
    }
    TagTab(window_id, tab.id.value,
                              alias->second.c_str());
    applied.push_back(alias->first);
  }
  const bool changed = !applied.empty();
  for (const std::string& old_guid : applied) {
    tab_store_.DropLegacyTagAlias(old_guid);
  }
  if (changed) {
    registry_.InvalidateAll();
  }
}

void PhiCoordinator::UpdateReaderPanelsForWindow(BrowserId window_id) {
  const phi_framework::WindowSnapshot* snapshot = window_model_.Get(window_id);
  if (!framework_ || !snapshot) {
    return;
  }
  const TabId active_tab = ActiveTabId(window_id);
  // The framework panels a reader surface at insertion and closes husks
  // (DivertReaderSurfaceAfterInsert); this is the FALLBACK for one it could
  // not place — a live surface (the extension reported it up, or an open is
  // still in flight) goes into a panel over its origin, anything else is a
  // husk and closes, as the Mac does on sight.
  for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
    if (tab.surface != phi_framework::TabSurface::kReader) {
      continue;
    }
    const TabId origin = tab.origin;
    if (!origin.valid() || HasReaderPanelForOrigin(origin)) {
      continue;
    }
    if (reader_.HasSurface(origin.value) ||
        reader_.HasPendingOpen(origin.value)) {
      PanelizeReaderSurface(origin);
      return;  // The strip changed either way; its push re-runs this.
    }
    LOG(INFO) << "[PhiClient] closing orphaned reader surface tab in window "
              << window_id.value;
    framework_->CloseTabById(window_id.value, tab.id.value);
    return;  // The close's own push re-runs this on the new strip.
  }
  // Every surface over this window, both kinds: an origin that left the
  // strip — closed, torn off, moved to another Space — takes its surface
  // with it (the Mac's origin watch); the rest re-fit and show only over
  // the active tab.
  std::vector<BrowserId> orphaned;
  for (PhiSurfacePanel* panel : surfaces_.OverWindow(window_id)) {
    if (!window_model_.FindTab(
            window_id, TabId{static_cast<int32_t>(panel->origin_tab_id())})) {
      orphaned.push_back(BrowserId{panel->window_id()});
      continue;
    }
    panel->Reposition();
    panel->SetVisibleForActiveTab(active_tab.value);
  }
  for (const BrowserId panel_id : orphaned) {
    LOG(INFO) << "[PhiClient] surface " << panel_id.value
              << " origin left the strip — closing";
    if (framework_) {
      framework_->CloseBrowserWindow(panel_id.value);
    }
  }
}

TabId PhiCoordinator::ActiveTabId(BrowserId browser) const {
  const phi_framework::TabSnapshot* active = window_model_.ActiveTab(browser);
  return active ? active->id : TabId();
}

void PhiCoordinator::OnReaderOfferabilityChanged(TabId tab_id) {
  // The reader button is laid out from this verdict, so the window showing
  // the tab has to repaint. At the next idle: the report can arrive during a
  // navigation the strip has not caught up with yet.
  if (PhiWindowController* window = WindowShowingTab(tab_id)) {
    window->ScheduleRefresh();
  }
}

bool PhiCoordinator::IsReaderOfferable(int tab_id,
                                       const std::string& url) const {
  return reader_.IsOfferable(tab_id, url);
}

bool PhiCoordinator::IsReaderActive(int tab_id,
                                    const std::string& url) const {
  return PhiReaderBridge::IsReaderPageUrl(url) || reader_.HasSurface(tab_id);
}

void PhiCoordinator::RequestToggleReaderView(int tab_id,
                                             const std::string& url) {
  reader_.Toggle(tab_id, url);
}

void PhiCoordinator::RequestCloseSurfaces(int tab_id) {
  CloseSurfacesForOrigin(TabId{tab_id});
}

void PhiCoordinator::RaiseReaderPanels(int64_t slot) {
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (registry_.SlotFor(BrowserId{panel.origin_window_id()}) ==
        SlotId{slot}) {
      panel.Raise();
    }
  });
}

void PhiCoordinator::NoteContentLayoutChanged(int64_t slot) {
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (registry_.SlotFor(BrowserId{panel.origin_window_id()}) ==
        SlotId{slot}) {
      panel.Reposition();
    }
  });
}

void PhiCoordinator::NoteSlotActiveSpace(const std::string& space_id) {
  NoteActiveSpace(space_id);
}

void PhiCoordinator::NoteSpaceWindow(const std::string& space_id,
                                     int64_t browser_window_id) {
  std::string profile = "Default";
  for (const Space& space : spaces_model_.spaces) {
    if (space.id == space_id) {
      profile = space.profile_id;
      break;
    }
  }
  ghost_spaces_.NoteLiveWindow(space_id, profile, BrowserId{browser_window_id});
}

void PhiCoordinator::OnAnalyticsEventCaptured(const char* module,
                                             const char* event_name,
                                             const char* properties_json) {
  if (!module || !event_name) {
    return;
  }
  // Out to the collector (a no-op until one is configured), and into the ring
  // below so the harness can see what crossed regardless.
  PhiTelemetry::Get().RecordEvent(module, event_name,
                                  properties_json ? properties_json : "{}");
  AnalyticsEvent event;
  event.module = module;
  event.name = event_name;
  event.properties_json = properties_json ? properties_json : "{}";
  // The name and module go to the log; the PROPERTIES do not. The component's
  // privacy contract keeps event data out of logs, and this is a log the user
  // can read off disk.
  VLOG(1) << "[analytics] " << event.module << "/" << event.name;
  analytics_events_.push_back(std::move(event));
  while (analytics_events_.size() > kAnalyticsRingSize) {
    analytics_events_.pop_front();
  }
}

void PhiCoordinator::OnSessionRestoreSettled(const char* receipt_json) {
  last_restore_receipt_ = receipt_json ? receipt_json : "";
  // REPLACE, never merge — the receipt describes the whole ghost store.
  ghost_spaces_.ReplaceFromReceipt(last_restore_receipt_);
  PushSpaceMenu();
  // The receipt's verdict names the settle reason: an armed eager set that
  // matched nothing parks the whole session with no window up, and a replay
  // that never started restores nothing either. SettleRestore rescues both
  // (and is a no-op rescue when a window is up).
  std::optional<base::DictValue> receipt =
      base::JSONReader::ReadDict(last_restore_receipt_, base::JSON_PARSE_RFC);
  const bool matched_nothing =
      receipt && receipt->FindBool("eagerFilterMatchedNothing").value_or(false);
  const bool restored_any =
      receipt && receipt->FindBool("restoredAny").value_or(true);
  SettleRestore(matched_nothing ? "matchednothing"
                                : (restored_any ? "receipt" : "nothingrestored"));
}

// --- Agent operating mask for the user's own Spaces ------------------------
// The browser is the only thing that can see an agent driving one of these
// tabs, so these three are its report channel (phi::PhiUserSpaceDrive). The
// registry owns the lifecycle rules; this just relays.
void PhiCoordinator::OnAgentDroveUserSpaceTab(int64_t tab_id,
                                              int64_t window_id,
                                              int64_t session_id) {
  agent_drives_.OnDrove(static_cast<int>(tab_id), window_id, session_id);
}

void PhiCoordinator::OnAgentObservedUserSpaceTab(int64_t tab_id,
                                                 int64_t window_id,
                                                 int64_t session_id) {
  agent_drives_.OnObserved(static_cast<int>(tab_id), window_id, session_id);
}

void PhiCoordinator::OnAgentStoppedUserSpaceTab(int64_t tab_id,
                                                int64_t session_id) {
  agent_drives_.OnStopped(static_cast<int>(tab_id), session_id);
}

bool PhiCoordinator::SetStoredRowLayout(const std::string& guid,
                                       const std::string& layout) {
  if (!tab_store_.SetRowLayout(guid, layout)) {
    return false;
  }
  registry_.InvalidateAll();
  return true;
}

bool PhiCoordinator::RenameStoredRow(const std::string& guid,
                                     const std::string& name) {
  if (!tab_store_.RenameRow(guid, name)) {
    return false;
  }
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
  return true;
}

std::string PhiCoordinator::AddBookmarkFolder(int64_t slot,
                                              const std::string& parent_guid,
                                              const std::string& title) {
  std::string profile_id, space_id;
  if (title.empty() || !OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
    return std::string();
  }
  const std::string guid = tab_store_.AddBookmark(
      profile_id, space_id, parent_guid, title, std::string(),
      /*folder=*/true, std::string(), 0, 0);
  if (!guid.empty()) {
    registry_.InvalidateAll();
    PhiSettingsWindow::Refresh();
  }
  return guid;
}

std::string PhiCoordinator::AddBookmarkRow(int64_t slot,
                                           const std::string& parent_guid,
                                           const std::string& title,
                                           const std::string& url) {
  std::string profile_id, space_id;
  if (url.empty() || !OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
    return std::string();
  }
  const std::string guid = tab_store_.AddBookmark(
      profile_id, space_id, parent_guid, !title.empty() ? title : url, url,
      /*folder=*/false, std::string(), 0, 0);
  if (!guid.empty()) {
    registry_.InvalidateAll();
    PhiSettingsWindow::Refresh();
  }
  return guid;
}

bool PhiCoordinator::MoveStoredRowToSpace(const std::string& guid,
                                          const std::string& space_id,
                                          bool clone) {
  const PhiRow* found = tab_store_.Find(guid);
  if (!found || found->type != PhiRowType::kBookmark || found->url.empty() ||
      found->space_id == space_id) {
    return false;  // Folders and pins do not travel between Spaces.
  }
  const PhiRow row = *found;  // AddBookmark may reallocate the store's rows.
  const Space* target = nullptr;
  for (const Space& space : spaces_model_.spaces) {
    if (space.id == space_id) {
      target = &space;
      break;
    }
  }
  if (!target) {
    return false;
  }
  // The copy first, at the target's root, carrying the favicon and — for a
  // split bookmark — the second pane and its arrangement; then, for a move,
  // the original goes (RemoveStoredRow also frees the bound tab's tag).
  const std::string added = tab_store_.AddBookmark(
      target->profile_id, space_id, std::string(), row.title, row.url,
      /*folder=*/false, row.favicon_base64, row.favicon_width,
      row.favicon_height);
  if (added.empty()) {
    return false;
  }
  if (!row.url_b.empty()) {
    tab_store_.SetRowSecondary(added, row.url_b, row.title_b,
                               row.favicon_b_base64, row.favicon_b_width,
                               row.favicon_b_height);
  }
  if (!row.layout.empty()) {
    tab_store_.SetRowLayout(added, row.layout);
  }
  if (!row.icon.empty()) {
    tab_store_.SetRowIcon(added, row.icon);
  }
  if (!clone) {
    RemoveStoredRow(guid);
  }
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
  return true;
}

int PhiCoordinator::SidebarWidth() const {
  return settings_.sidebar_width;
}

void PhiCoordinator::RequestSidebarWidth(int dip) {
  const int clamped =
      std::max(kSidebarWidthMin, std::min(dip, kSidebarWidthMax));
  if (clamped == settings_.sidebar_width) {
    return;
  }
  settings_.sidebar_width = clamped;
  ScheduleSave(StoreDomain::kSettings);
  for (PhiWindowController* controller : registry_.Controllers()) {
    controller->LayoutContents();
  }
  registry_.InvalidateAll();
}

void PhiCoordinator::ReorderSpace(const std::string& space_id, int new_index) {
  auto it = std::find_if(
      spaces_model_.spaces.begin(), spaces_model_.spaces.end(),
      [&](const Space& s) { return s.id == space_id; });
  if (it == spaces_model_.spaces.end()) {
    return;
  }
  Space moved = *it;
  spaces_model_.spaces.erase(it);
  const int clamped =
      std::max(0, std::min(new_index,
                           static_cast<int>(spaces_model_.spaces.size())));
  spaces_model_.spaces.insert(spaces_model_.spaces.begin() + clamped,
                              std::move(moved));
  ScheduleSave(StoreDomain::kSpaces);
  registry_.InvalidateAll();
}

void PhiCoordinator::RequestBookmarkActiveTab(int64_t slot_number) {
  const SlotId slot{slot_number};
  std::string profile_id, space_id;
  if (!framework_ || !OwnerForSlot(slot, &profile_id, &space_id)) {
    return;
  }
  std::string title, url;
  if (const phi_framework::WindowSnapshot* window =
          window_model_.Get(ResolveBrowser(slot))) {
    url = window->nav.url;
    title = window->nav.title;
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
    registry_.InvalidateAll();
  }
}

namespace {

// The reader's live style (PhiReaderStyle, a base::Value codec) and the
// model's (STL, so the header stays on the host) are the same eight
// fields; the copy lives here, in one place each way.
model::ReaderStyle ReaderStyleToModel(const PhiReaderStyle& style) {
  model::ReaderStyle reader;
  reader.font_size = style.font_size;
  reader.width = style.width;
  reader.theme = style.theme;
  reader.typeface = style.typeface;
  reader.line_height = style.line_height;
  reader.letter_spacing = style.letter_spacing;
  reader.shows_images = style.shows_images;
  reader.highlights_links = style.highlights_links;
  return reader;
}

void ReaderStyleFromModel(const model::ReaderStyle& reader,
                          PhiReaderStyle* style) {
  style->font_size = reader.font_size;
  style->width = reader.width;
  style->theme = reader.theme;
  style->typeface = reader.typeface;
  style->line_height = reader.line_height;
  style->letter_spacing = reader.letter_spacing;
  style->shows_images = reader.shows_images;
  style->highlights_links = reader.highlights_links;
}

}  // namespace

void PhiCoordinator::LoadStores(const std::wstring& user_data_dir) {
  settings_store_.Init(user_data_dir);
  spaces_store_.Init(user_data_dir);
  restore_store_.Init(user_data_dir);
  consent_store_.Init(user_data_dir);
  // Each Load leaves its model usable: decoded, imported from the
  // pre-phase-3 files on a first run over an old profile, or defaults over
  // a quarantined file (the truth-owner rules are in phi_app_model.h).
  spaces_store_.Load(&spaces_model_);
  settings_store_.Load(&settings_);
  restore_store_.Load(&restore_);
  if (consent_store_.Load(&consent_) == PhiJsonFileStatus::kCorrupt) {
    // Fail closed (lane 5 §4.6): a file whose seal did not check out is
    // quarantined beside its replacement and the model holds NO grants, so
    // every agent connection and credential request prompts as on a first
    // run until the user grants again.
    LOG(ERROR) << "[PhiClient] consent store unreadable; standing grants "
                  "dropped until re-granted";
  }
  LOG(INFO) << "[PhiClient] stores loaded: " << spaces_model_.spaces.size()
            << " spaces, " << spaces_model_.url_rules.size()
            << " rules, layout " << PhiLayoutModeName(settings_.layout_mode)
            << ", theme " << settings_.theme_id << " / "
            << PhiAppearanceName(Appearance());
  // What the loaded settings drive outside the model. The language first
  // and before any chrome is built, so the first window comes up in the
  // chosen language rather than flashing English; an empty preference
  // follows the system.
  PhiSetLanguage(settings_.language);
  ReaderStyleFromModel(settings_.reader, &reader_.mutable_style());
  // The palette depends on the theme AND the saturation, so it is resolved
  // once both are known.
  palette_ = PhiResolvePalette(PhiThemeById(settings_.theme_id), Appearance(),
                               settings_.saturation);
  RebuildSpacePalettes();
  // The consent reaches the pipeline the moment it is known: Start() ran
  // before this file was read, and every event until now was dropped.
  PhiTelemetry::Get().SetEnabled(settings_.analytics_enabled);
}

void PhiCoordinator::SyncReaderStyleToModel() {
  settings_.reader = ReaderStyleToModel(reader_.style());
}

void PhiCoordinator::MirrorPinScope() {
  if (settings_.pin_scope == tab_store_.pin_scope()) {
    return;
  }
  settings_.pin_scope = tab_store_.pin_scope();
  ScheduleSave(StoreDomain::kSettings);
}

std::string PhiCoordinator::LanguagePreference() const {
  return PhiLanguagePreference();
}

void PhiCoordinator::PushDisplayLanguageToChromium() {
  if (!framework_) {
    return;
  }
  // The preference, not the active locale: an empty tag is the instruction to
  // follow the operating system, and Chromium resolves that for itself — it
  // ships far more languages than the client's catalog does.
  const char* applied =
      framework_->SetDisplayLanguage(PhiLanguagePreference().c_str());
  LOG(INFO) << "[PhiClient] chromium display language -> "
            << (applied && *applied ? applied : "(unresolved)");
}

void PhiCoordinator::SetLanguagePreference(const std::string& tag) {
  const bool active_changed = PhiSetLanguage(tag);
  // The catalog's normalised preference is what the file carries.
  settings_.language = PhiLanguagePreference();
  ScheduleSave(StoreDomain::kSettings);
  PushDisplayLanguageToChromium();
  // Every piece of chrome caches the strings it was built with, so the whole
  // UI is rebuilt: menu bars are recreated from their (new) titles. Only when
  // the ACTIVE locale moved — picking Japanese on a Japanese machine changes
  // the preference without changing a single string.
  if (active_changed) {
    for (PhiWindowController* controller : registry_.Controllers()) {
      controller->RebuildMenuBar();
      if (HWND shell = controller->shell_hwnd()) {
        ::InvalidateRect(shell, nullptr, TRUE);
      }
    }
    registry_.InvalidateAll();
  }
  // The picker shows the preference, which changed either way.
  PhiSettingsWindow::Refresh();
}

int PhiCoordinator::Saturation() const {
  return settings_.saturation;
}

void PhiCoordinator::SetSaturation(int value) {
  value = value < 0 ? 0 : (value > 100 ? 100 : value);
  if (value == settings_.saturation) {
    return;
  }
  settings_.saturation = value;
  ScheduleSave(StoreDomain::kSettings);
  ApplyTheme();
}

PhiNewTabBehavior PhiCoordinator::NewTabBehavior() const {
  // Same enumerators in the same order (phi_app_model.h): the model's twin
  // exists only to keep that header off windows.h.
  return static_cast<PhiNewTabBehavior>(settings_.new_tab_behavior);
}

bool PhiCoordinator::NewTabOpensOmnibox() const {
  return NewTabBehavior() == PhiNewTabBehavior::kOmnibox;
}

void PhiCoordinator::SetNewTabBehavior(PhiNewTabBehavior behavior) {
  if (behavior == NewTabBehavior()) {
    return;
  }
  settings_.new_tab_behavior = static_cast<model::NewTabBehavior>(behavior);
  ScheduleSave(StoreDomain::kSettings);
  PhiSettingsWindow::Refresh();
}

PhiShortcutChord PhiCoordinator::EffectiveChord(const std::string& name) const {
  auto it = settings_.shortcut_overrides.find(name);
  if (it != settings_.shortcut_overrides.end()) {
    const model::ShortcutChord& chord = it->second;
    return {chord.vk, chord.ctrl, chord.shift, chord.alt};
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
                   (settings_.shortcut_overrides.count(def.name) ? "1" : "0"));
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
      settings_.shortcut_overrides[def.name] = {};
    }
  }
  // Rebinding back to the default is a restore, not an override.
  if (chord == target->def) {
    settings_.shortcut_overrides.erase(name);
  } else {
    settings_.shortcut_overrides[name] = {chord.vk, chord.ctrl, chord.shift,
                                          chord.alt};
  }
  ScheduleSave(StoreDomain::kSettings);
}

void PhiCoordinator::RestoreAllShortcuts() {
  if (settings_.shortcut_overrides.empty()) {
    return;
  }
  settings_.shortcut_overrides.clear();
  ScheduleSave(StoreDomain::kSettings);
}

bool PhiCoordinator::AnyShortcutCustomized() const {
  return !settings_.shortcut_overrides.empty();
}

void PhiCoordinator::FocusWindowForBrowser(int64_t browser_window_id) {
  const BrowserId browser{browser_window_id};
  PhiWindowController* controller = registry_.ControllerForBrowser(browser);
  if (!controller) {
    return;
  }
  if (controller->active_browser_id() != browser) {
    // The browser backs a HIDDEN Space of that slot — surface it first.
    for (const std::string& space : controller->space_order()) {
      if (controller->BrowserIdForSpace(space) == browser) {
        controller->ShowSpace(space);
        break;
      }
    }
  }
  if (controller->shell_hwnd()) {
    ::SetForegroundWindow(controller->shell_hwnd());
  }
  controller->ScheduleRefresh();
}

void PhiCoordinator::RequestTabToSpace(SlotId slot,
                                       TabId tab,
                                       const std::string& space_id,
                                       bool move) {
  PhiWindowController* source = registry_.Get(slot);
  if (!source || !framework_) {
    return;
  }
  if (source->active_space() == space_id) {
    return;
  }
  const BrowserId source_browser = ResolveBrowser(slot);
  // The tab's URL — the clone and cold-Space paths re-open it.
  std::string url;
  if (const phi_framework::TabSnapshot* live =
          window_model_.FindTab(source_browser, tab)) {
    url = live->url;
  }
  // An open target Space (any slot) gets the real move.
  BrowserId target_browser;
  for (PhiWindowController* controller : registry_.UserControllers()) {
    target_browser = controller->BrowserIdForSpace(space_id);
    if (target_browser.valid()) {
      break;
    }
  }
  if (move && target_browser.valid()) {
    if (framework_->MoveTabToWindowById(source_browser, tab, target_browser)) {
      FocusWindowForBrowser(target_browser.value);
      source->ScheduleRefresh();
    }
    return;
  }
  if (url.empty()) {
    return;
  }
  if (target_browser.valid()) {
    framework_->OpenTabBypassingSpaceRouting(target_browser.value,
                                             url.c_str());
    FocusWindowForBrowser(target_browser.value);
  } else {
    // Cold Space: surface it and open there (the routed-URL tail).
    RouteUrlToSpace(space_id, url, source_browser);
  }
  if (move) {
    framework_->CloseTabById(source_browser.value, tab.value);
  }
  source->ScheduleRefresh();
}

bool PhiCoordinator::RestoreLastSession() const {
  return settings_.restore_last_session;
}

void PhiCoordinator::SetRestoreLastSession(bool enabled) {
  if (enabled == settings_.restore_last_session) {
    return;
  }
  settings_.restore_last_session = enabled;
  ScheduleSave(StoreDomain::kSettings);
  PushRestorePreference();
  PhiSettingsWindow::Refresh();
}

void PhiCoordinator::PushRestorePreference() {
  // The ON-CHANGE push: every LOADED profile takes the new app-wide choice.
  // A profile loading later gets it from OnProfileLoaded, never from the
  // path that happened to load it.
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
      framework_->SetRestoreOnStartup(id->c_str(),
                                      settings_.restore_last_session);
    }
  }
}

bool PhiCoordinator::AnalyticsEnabled() const {
  return settings_.analytics_enabled;
}

void PhiCoordinator::SetAnalyticsEnabled(bool enabled) {
  if (enabled == settings_.analytics_enabled) {
    return;
  }
  settings_.analytics_enabled = enabled;
  ScheduleSave(StoreDomain::kSettings);
  PhiTelemetry::Get().SetEnabled(enabled);
  PhiSettingsWindow::Refresh();
}

bool PhiCoordinator::AlwaysShowFullUrl() const {
  return settings_.always_show_full_url;
}

void PhiCoordinator::SetAlwaysShowFullUrl(bool enabled) {
  if (enabled == settings_.always_show_full_url) {
    return;
  }
  settings_.always_show_full_url = enabled;
  ScheduleSave(StoreDomain::kSettings);
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
}

bool PhiCoordinator::ShowTabPreviews() const {
  return settings_.show_tab_previews;
}

void PhiCoordinator::SetShowTabPreviews(bool enabled) {
  if (enabled == settings_.show_tab_previews) {
    return;
  }
  settings_.show_tab_previews = enabled;
  ScheduleSave(StoreDomain::kSettings);
  PhiSettingsWindow::Refresh();
}

bool PhiCoordinator::PeekViewEnabled() const {
  return settings_.peek_view_enabled;
}

void PhiCoordinator::SetPeekViewEnabled(bool enabled) {
  if (enabled == settings_.peek_view_enabled) {
    return;
  }
  settings_.peek_view_enabled = enabled;
  ScheduleSave(StoreDomain::kSettings);
  PushPeekPreferences();
  PhiSettingsWindow::Refresh();
}

bool PhiCoordinator::AutoPeekViewEnabled() const {
  return settings_.auto_peek_view_enabled;
}

void PhiCoordinator::SetAutoPeekViewEnabled(bool enabled) {
  if (enabled == settings_.auto_peek_view_enabled) {
    return;
  }
  settings_.auto_peek_view_enabled = enabled;
  ScheduleSave(StoreDomain::kSettings);
  PushPeekPreferences();
  PhiSettingsWindow::Refresh();
}

void PhiCoordinator::PushPeekPreferences() {
  if (framework_) {
    framework_->SetPeekPreferences(settings_.peek_view_enabled,
                                   settings_.auto_peek_view_enabled);
  }
}

PhiPinScope PhiCoordinator::PinScope() const {
  return tab_store_.pin_scope();
}

void PhiCoordinator::SetPinScope(PhiPinScope scope) {
  if (!tab_store_.SetPinScope(scope)) {
    return;
  }
  MirrorPinScope();
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
}

std::vector<std::string> PhiCoordinator::SpaceSummaries() const {
  std::vector<std::string> out;
  for (const Space& space : spaces_model_.spaces) {
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
  for (Space& space : spaces_model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    space.icon = icon;
    ScheduleSave(StoreDomain::kSpaces);
    registry_.InvalidateAll();
    PhiSettingsWindow::Refresh();
    return;
  }
}

int PhiCoordinator::SpaceSaturation(const std::string& space_id) const {
  for (const Space& space : spaces_model_.spaces) {
    if (space.id == space_id) {
      return space.saturation;  // -1 follows the app-wide slider.
    }
  }
  return -1;
}

void PhiCoordinator::SetSpaceSaturation(const std::string& space_id,
                                        int value) {
  for (Space& space : spaces_model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    // -1 follows the app-wide slider; anything else clamps to its range.
    space.saturation = value < 0 ? -1 : (value > 100 ? 100 : value);
    ScheduleSave(StoreDomain::kSpaces);
    RebuildSpacePalettes();
    for (PhiWindowController* controller : registry_.Controllers()) {
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
  for (Space& space : spaces_model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    space.name = name;
    ScheduleSave(StoreDomain::kSpaces);
    registry_.InvalidateAll();
    PhiSettingsWindow::Refresh();
    return true;
  }
  return false;
}

bool PhiCoordinator::DeleteSpace(const std::string& space_id) {
  // Only the LAST Space is undeletable (the Mac's last-Space rule); the
  // default Space itself may go, handing its role on below.
  if (!CanDeleteSpace(space_id)) {
    return false;
  }
  auto it = std::find_if(
      spaces_model_.spaces.begin(), spaces_model_.spaces.end(),
      [&](const Space& s) { return s.id == space_id; });
  if (it == spaces_model_.spaces.end()) {
    return false;
  }
  if (space_id == spaces_model_.default_space_id) {
    // The default role passes to the first REMAINING Space (strip order),
    // before the windows retreat so anything landing on the default lands
    // on the successor; the Spaces save below persists the hand-off.
    for (const Space& space : spaces_model_.spaces) {
      if (space.id != space_id) {
        spaces_model_.default_space_id = space.id;
        break;
      }
    }
    LOG(INFO) << "[PhiClient] default Space role handed to "
              << spaces_model_.default_space_id;
  }
  // A slot whose ONLY browser is the deleted Space's would be torn down by
  // the close below (a window closes with its last browser). Retreat it to
  // the default Space first — spawning that Space's browser into the slot
  // (synchronous for a loaded profile) — the Mac's retreat-then-close order.
  // Now reachable for the default Space itself, whose browser is usually the
  // slot's original one.
  const std::string retreat = DefaultSpaceId();
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (controller->is_agent() || !controller->HasSpace(space_id) ||
        retreat == space_id) {
      continue;
    }
    bool has_other = false;
    for (const Space& space : spaces_model_.spaces) {
      if (space.id != space_id && controller->HasSpace(space.id)) {
        has_other = true;
        break;
      }
    }
    if (!has_other) {
      bool pending = false, cold = false;
      SwitchSlotToSpace(controller->slot(), retreat, &pending, &cold);
    }
  }
  // Close the Space's browser wherever it is open; each controller switches
  // away first so a live window never keeps showing a deleted Space.
  for (PhiWindowController* controller : registry_.Controllers()) {
    controller->CloseSpaceBrowser(space_id);
  }
  // The Space's bookmarks and space-scoped pins go with it (the Mac's
  // delete-confirmation promise) — and its URL rules, which would otherwise
  // route into a Space that no longer exists.
  tab_store_.RemoveForSpace(space_id);
  std::erase_if(spaces_model_.url_rules,
                [&](const UrlRule& r) { return r.space_id == space_id; });
  ++spaces_model_.url_rules_generation;  // Wire ids regenerate on every write.
  // Its ghost record goes too — and, when the Space existed only as a
  // PARKED window from the last session, that window is dropped on the
  // session side, or the next unfiltered restore would replay it as a
  // loose window of a Space that no longer exists.
  const bool was_parked = ghost_spaces_.Parked(space_id) != nullptr;
  const PhiGhostSpaces::Remembered ghost = ghost_spaces_.Forget(space_id);
  if (was_parked && ghost.window_id.valid() && framework_) {
    framework_->DropGhostWindow(ghost.profile.c_str(),
                                static_cast<int>(ghost.window_id.value));
  }
  spaces_model_.spaces.erase(it);
  // The list, the default role and the rules are one file: one save.
  ScheduleSave(StoreDomain::kSpaces);
  RebuildSpacePalettes();
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
  return true;
}

std::string PhiCoordinator::DefaultSpaceId() const {
  for (const Space& space : spaces_model_.spaces) {
    if (space.id == spaces_model_.default_space_id) {
      return spaces_model_.default_space_id;
    }
  }
  return spaces_model_.spaces.empty() ? std::string("1")
                                      : spaces_model_.spaces.front().id;
}

bool PhiCoordinator::CanDeleteSpace(const std::string& space_id) const {
  const bool known =
      std::any_of(spaces_model_.spaces.begin(), spaces_model_.spaces.end(),
                  [&](const Space& s) { return s.id == space_id; });
  return known && spaces_model_.spaces.size() > 1;
}

bool PhiCoordinator::SetSpaceProfile(const std::string& space_id,
                                     const std::string& profile_id) {
  if (space_id.empty() || profile_id.empty() || !framework_) {
    return false;
  }
  auto it = std::find_if(
      spaces_model_.spaces.begin(), spaces_model_.spaces.end(),
      [&](const Space& s) { return s.id == space_id; });
  if (it == spaces_model_.spaces.end() || it->profile_id == profile_id) {
    return false;
  }
  // The default Space keeps its binding: it is what a fresh shell's FIRST
  // browser lands on, and that browser exists before any Space is chosen —
  // the Mac refuses the same Space for the same reason.
  const std::string retreat = DefaultSpaceId();
  if (space_id == retreat) {
    return false;
  }
  // A window's profile is baked in at spawn, so the Space's browser cannot
  // be re-pointed: it is closed and respawned in the new profile with its
  // open tabs reopened there (the Mac's changeProfile promise).
  std::vector<std::string> urls;
  SlotId respawn_slot;
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (controller->is_agent() || !controller->HasSpace(space_id)) {
      continue;
    }
    if (!respawn_slot.valid()) {
      respawn_slot = controller->slot();
      if (const phi_framework::WindowSnapshot* window =
              window_model_.Get(controller->BrowserIdForSpace(space_id))) {
        for (const phi_framework::TabSnapshot& tab : window->tabs) {
          if (!tab.url.empty()) {
            urls.push_back(tab.url);
          }
        }
      }
    }
    // A slot whose ONLY browser is this Space's would be torn down by the
    // close below (a window closes with its last browser) — retreat it to
    // the default Space first, the delete path's order.
    bool has_other = false;
    for (const Space& space : spaces_model_.spaces) {
      if (space.id != space_id && controller->HasSpace(space.id)) {
        has_other = true;
        break;
      }
    }
    if (!has_other) {
      bool pending = false, cold = false;
      SwitchSlotToSpace(controller->slot(), retreat, &pending, &cold);
    }
  }
  for (PhiWindowController* controller : registry_.Controllers()) {
    controller->CloseSpaceBrowser(space_id);
  }
  it->profile_id = profile_id;
  ScheduleSave(StoreDomain::kSpaces);
  // The rows the Space owns move with it — the Mac's "bookmarks stay with
  // the Space". Pins held at profile or app scope carry no Space and stay
  // with their profile, so a re-profiled Space picks the new one's up.
  tab_store_.RehomeSpace(space_id, profile_id);
  // Any parked window from the previous session belongs to the OLD profile;
  // materializing it now would reopen the Space where it no longer lives.
  ghost_spaces_.ClearParked(space_id);
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
  PushSpaceMenu();  // The wire's Space list carries the binding.
  if (respawn_slot.valid()) {
    // The Space has no browser any more, so this is a request; the tabs
    // ride on it and reopen once the replacement attaches.
    bool pending = false, cold = false;
    SwitchSlotToSpace(respawn_slot, space_id, &pending, &cold, &urls);
  }
  LOG(INFO) << "[PhiClient] Space " << space_id << " re-bound to profile "
            << profile_id;
  return true;
}

bool PhiCoordinator::DeleteProfileById(const std::string& profile_id) {
  // Default is permanent, and a profile still bound to a Space stays — the
  // Spaces must be re-profiled or deleted first.
  if (profile_id.empty() || profile_id == "Default" || !framework_) {
    return false;
  }
  for (const Space& space : spaces_model_.spaces) {
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
  return settings_.developer_mode;
}

void PhiCoordinator::SetDeveloperMode(bool enabled) {
  if (settings_.developer_mode == enabled) {
    return;
  }
  settings_.developer_mode = enabled;
  ScheduleSave(StoreDomain::kSettings);
  PhiSettingsWindow::Refresh();
}

bool PhiCoordinator::AllowAgentUserSpaces() const {
  return settings_.allow_agent_user_spaces;
}

void PhiCoordinator::SetAllowAgentUserSpaces(bool enabled) {
  // Only the Settings window reaches here — never a verb (see
  // agentSpace.settings.set), so a connected agent cannot widen its own
  // reach.
  if (settings_.allow_agent_user_spaces == enabled) {
    return;
  }
  settings_.allow_agent_user_spaces = enabled;
  LOG(INFO) << "[PhiAgent] agent access to user Spaces "
            << (enabled ? "enabled" : "disabled");
  ScheduleSave(StoreDomain::kSettings);
  PhiSettingsWindow::Refresh();
}

void PhiCoordinator::OpenBrowserSettings() {
  OpenBrowserPage("chrome://settings");
}

void PhiCoordinator::OpenBrowserPage(const std::string& url) {
  if (const SlotId slot = registry_.FirstUserSlot(); slot.valid()) {
    RequestOpen(slot.value, url);
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
  ExecuteControlOnUi("agentSpace.spaces.create", json,
                     ControlCaller::kInternal);
}

std::vector<std::string> PhiCoordinator::URLRuleRows() const {
  std::vector<std::string> rows;
  rows.reserve(spaces_model_.url_rules.size());
  for (const UrlRule& rule : spaces_model_.url_rules) {
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
  spaces_model_.url_rules.push_back({space_id, host, path_prefix, ask});
  ++spaces_model_.url_rules_generation;  // Wire ids regenerate on every write.
  ScheduleSave(StoreDomain::kSpaces);
  PushRoutingTable();
}

void PhiCoordinator::SetURLRulePattern(int index,
                                       const std::string& host,
                                       const std::string& path_prefix) {
  if (index < 0 || index >= static_cast<int>(spaces_model_.url_rules.size()) ||
      host.empty()) {
    return;
  }
  spaces_model_.url_rules[static_cast<size_t>(index)].host = host;
  spaces_model_.url_rules[static_cast<size_t>(index)].path_prefix = path_prefix;
  ++spaces_model_.url_rules_generation;  // Wire ids regenerate on every write.
  ScheduleSave(StoreDomain::kSpaces);
  PushRoutingTable();
}

void PhiCoordinator::SetURLRuleTarget(int index,
                                      const std::string& space_id,
                                      bool ask) {
  if (index < 0 || index >= static_cast<int>(spaces_model_.url_rules.size()) ||
      space_id.empty()) {
    return;
  }
  spaces_model_.url_rules[static_cast<size_t>(index)].space_id = space_id;
  spaces_model_.url_rules[static_cast<size_t>(index)].ask = ask;
  ++spaces_model_.url_rules_generation;  // Wire ids regenerate on every write.
  ScheduleSave(StoreDomain::kSpaces);
  PushRoutingTable();
}

void PhiCoordinator::RemoveURLRule(int index) {
  if (index < 0 || index >= static_cast<int>(spaces_model_.url_rules.size())) {
    return;
  }
  spaces_model_.url_rules.erase(spaces_model_.url_rules.begin() + index);
  ++spaces_model_.url_rules_generation;  // Wire ids regenerate on every write.
  ScheduleSave(StoreDomain::kSpaces);
  PushRoutingTable();
}

void PhiCoordinator::ApplyTheme() {
  palette_ = PhiResolvePalette(PhiThemeById(settings_.theme_id), Appearance(),
                               settings_.saturation);
  RebuildSpacePalettes();
  ScheduleSave(StoreDomain::kSettings);
  for (PhiWindowController* controller : registry_.Controllers()) {
    controller->ApplyPalette();
  }
  PhiSettingsWindow::Refresh();
}

void PhiCoordinator::RebuildSpacePalettes() {
  palette_by_space_.clear();
  for (const Space& space : spaces_model_.spaces) {
    COLORREF accent = 0;
    const bool has_hex = PhiParseColorHex(space.color_hex, &accent);
    if (space.theme_id.empty() && space.saturation < 0 && !has_hex) {
      continue;  // Follows the app-wide theme.
    }
    const int saturation =
        space.saturation < 0 ? settings_.saturation : space.saturation;
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
          PhiResolvePalette(custom, Appearance(), saturation);
      continue;
    }
    const std::string& id =
        space.theme_id.empty() ? settings_.theme_id : space.theme_id;
    palette_by_space_[space.id] =
        PhiResolvePalette(PhiThemeById(id), Appearance(), saturation);
  }
}

void PhiCoordinator::OpenUrlInMainWindow(const std::string& url) {
  if (!framework_ || url.empty()) {
    return;
  }
  // The focused user window; a Kiosk has no Space of its own to prefer, so
  // the page lands wherever the user's browsing already lives — in the
  // browser that slot is SHOWING, never its original.
  PhiWindowController* main = FocusedUserController();
  if (!main) {
    return;
  }
  framework_->OpenTab(ResolveBrowser(main->slot()), url.c_str());
  if (HWND shell = main->shell_hwnd()) {
    ::SetForegroundWindow(shell);
  }
}

std::string PhiCoordinator::KioskTargetSpaceId() const {
  if (PhiWindowController* main = registry_.FirstUserController()) {
    const std::string& active = main->active_space();
    if (!active.empty()) {
      return active;
    }
  }
  // DefaultSpaceId itself falls back to the first Space, so this always
  // names one as long as any exists.
  return DefaultSpaceId();
}

const PhiPalette& PhiCoordinator::PaletteForSlot(int64_t slot) const {
  if (PhiWindowController* window = registry_.Resolve(slot)) {
    auto found = palette_by_space_.find(window->active_space());
    if (found != palette_by_space_.end()) {
      return found->second;
    }
  }
  return palette_;
}

std::string PhiCoordinator::SpaceThemeId(const std::string& space_id) const {
  for (const Space& space : spaces_model_.spaces) {
    if (space.id == space_id) {
      return space.theme_id;
    }
  }
  return std::string();
}

void PhiCoordinator::SetSpaceTheme(const std::string& space_id,
                                   const std::string& theme_id) {
  for (Space& space : spaces_model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    // An empty id clears the override, putting the Space back on the app-wide
    // theme — the Mac's "no entry in spaceThemeIds" state.
    space.theme_id = theme_id.empty() ? std::string() : PhiThemeById(theme_id).id;
    ScheduleSave(StoreDomain::kSpaces);
    RebuildSpacePalettes();
    for (PhiWindowController* controller : registry_.Controllers()) {
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
  return settings_.theme_id;
}

PhiAppearance PhiCoordinator::Appearance() const {
  // Same enumerators in the same order (phi_app_model.h): the model's twin
  // exists only to keep that header off windows.h.
  return static_cast<PhiAppearance>(settings_.appearance);
}

void PhiCoordinator::SetThemeId(const std::string& id) {
  const std::string normalised = PhiThemeById(id).id;
  if (normalised == settings_.theme_id) {
    return;
  }
  settings_.theme_id = normalised;
  ApplyTheme();
}

void PhiCoordinator::SetAppearance(PhiAppearance appearance) {
  if (appearance == Appearance()) {
    return;
  }
  settings_.appearance = static_cast<model::Appearance>(appearance);
  ApplyTheme();
}

void PhiCoordinator::RequestSettings() {
  PhiSettingsWindow::Open(this);
}

void PhiCoordinator::RequestFeedback(int64_t slot,
                                     const PhiFeedbackContext& context) {
  PhiWindowController* controller = registry_.Resolve(slot);
  PhiFeedbackWindow::Open(this, context,
                          controller ? controller->shell_hwnd() : nullptr);
}

void PhiCoordinator::OnShowFeedbackRequested() {
  // Chromium's IDC_FEEDBACK (Help ▸ Report an Issue via the framework, the
  // sad tab's Send feedback, …): the window the user is looking at.
  if (PhiWindowController* controller = FocusedUserController()) {
    controller->OpenFeedback();
  }
}

void PhiCoordinator::OnFeedbackSystemLogs(int request_id, const char* text) {
  PhiFeedbackWindow::OnSystemLogs(request_id, text);
}

bool PhiCoordinator::FeedbackAccount(std::string* email) {
  if (auth_.signed_in()) {
    *email = auth_.user().email;
    return true;
  }
  if (!test_feedback_user_id_.empty()) {
    *email = test_feedback_email_;
    return true;
  }
  return false;
}

void PhiCoordinator::RequestFeedbackSignIn() {
  RequestSignIn();
}

void PhiCoordinator::OpenFeedbackLink(int64_t window_id,
                                      const std::string& url) {
  if (!framework_) {
    return;
  }
  framework_->OpenTab(BrowserId{window_id}, url.c_str());
  FocusWindowForBrowser(window_id);
}

bool PhiCoordinator::FetchFeedbackSystemLogs(int64_t window_id,
                                             int request_id) {
  return framework_ &&
         framework_->FetchFeedbackSystemLogs(window_id, request_id);
}

std::map<std::string, std::string> PhiCoordinator::PhiExtensionVersions(
    int64_t window_id) {
  std::map<std::string, std::string> versions;
  if (!framework_) {
    return versions;
  }
  const char* json = framework_->ListExtensionsJson(window_id);
  std::optional<base::Value> list =
      base::JSONReader::Read(json ? json : "[]", base::JSON_PARSE_RFC);
  if (!list || !list->is_list()) {
    return versions;
  }
  for (const base::Value& row : list->GetList()) {
    const base::DictValue* dict = row.GetIfDict();
    const std::string* id = dict ? dict->FindString("id") : nullptr;
    const std::string* name = dict ? dict->FindString("name") : nullptr;
    const std::string* version =
        dict ? dict->FindString("version") : nullptr;
    if (id && name && version && PhiFeedbackOutbox::IsPhiExtensionId(*id)) {
      versions[*name] = *version;
    }
  }
  return versions;
}

void PhiCoordinator::SyncFeedbackAccount() {
  if (auth_.signed_in()) {
    PhiFeedbackOutbox::Get().SetAccount(auth_.user().sub, auth_.user().email);
    return;
  }
  PhiFeedbackOutbox::Get().SetAccount(test_feedback_user_id_,
                                      test_feedback_email_);
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
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (controller->slot() != SlotId{exclude_slot} &&
        controller->shell_hwnd() == at) {
      return controller->slot().value;
    }
  }
  return 0;
}

void PhiCoordinator::NoteTearOffDrop(POINT screen) {
  tear_off_drop_ = screen;
  tear_off_drop_at_ = ::GetTickCount64();
}

bool PhiCoordinator::TakeTearOffDrop(POINT* screen) {
  if (!tear_off_drop_at_) {
    return false;
  }
  // The Mac's tearOffPlacementTimeout: a tear-off the framework never
  // answered must not place some unrelated window later.
  constexpr ULONGLONG kTearOffPlacementTimeoutMs = 4000;
  const bool fresh =
      ::GetTickCount64() - tear_off_drop_at_ <= kTearOffPlacementTimeoutMs;
  tear_off_drop_at_ = 0;
  if (!fresh) {
    return false;
  }
  *screen = tear_off_drop_;
  return true;
}

bool PhiCoordinator::ConvertStoredRow(SlotId slot,
                                      const std::string& guid,
                                      bool to_bookmark,
                                      const std::string& parent_guid,
                                      int index) {
  const PhiRow* found = tab_store_.Find(guid);
  if (!found) {
    return false;
  }
  const PhiRow row = *found;  // The store mutates below; keep a copy.
  if (to_bookmark ? row.type != PhiRowType::kPinnedTab
                  : row.type != PhiRowType::kBookmark) {
    return false;  // Folders and same-kind rows never convert.
  }
  // The NEW row belongs to the Space it was dropped in (the Mac passes the
  // state's profileId/spaceId) — not to the old row's owner: a profile- or
  // app-scoped pin carries no Space, and a bookmark made under that owner
  // would show in no sidebar at all.
  std::string profile_id, space_id;
  if (!OwnerForSlot(slot, &profile_id, &space_id)) {
    return false;
  }
  // Every live tab bound to the row — either pane — by tag.
  const std::vector<BoundTab> bound = TabsBoundTo(guid);
  // The other kind of row, same panes, same favicons, same arrangement —
  // the Mac's movePinnedTabOut(toBookmark:) / moveBookmarkOut(toPinnedTabs:).
  const std::string new_guid =
      to_bookmark
          ? tab_store_.AddBookmark(profile_id, space_id, parent_guid,
                                   row.title, row.url, /*folder=*/false,
                                   row.favicon_base64, row.favicon_width,
                                   row.favicon_height)
          : tab_store_.AddPinned(profile_id, space_id, row.title, row.url,
                                 row.favicon_base64, row.favicon_width,
                                 row.favicon_height);
  if (new_guid.empty()) {
    return false;
  }
  if (!row.url_b.empty()) {
    tab_store_.SetRowSecondary(new_guid, row.url_b, row.title_b,
                               row.favicon_b_base64, row.favicon_b_width,
                               row.favicon_b_height);
  }
  if (!row.layout.empty()) {
    tab_store_.SetRowLayout(new_guid, row.layout);
  }
  tab_store_.Reorder(new_guid, index >= 0 ? index
                                          : std::numeric_limits<int>::max());
  // Retarget the live tab(s) at the new row — the Mac's updateTabCustomValue
  // — BEFORE the old row goes, so nothing reads as closed in between.
  if (framework_) {
    for (const BoundTab& b : bound) {
      TagTab(b.browser, b.tab.value,
                                TagFor(new_guid, b.pane_b).c_str());
    }
  }
  tab_store_.Remove(guid);
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
  return true;
}

bool PhiCoordinator::RequestDropTabOnWindow(int64_t source_slot,
                                            int tab_a,
                                            int tab_b,
                                            int64_t target_slot) {
  PhiWindowController* source = registry_.Resolve(source_slot);
  PhiWindowController* target = registry_.Resolve(target_slot);
  if (!source || !target || !framework_) {
    return false;
  }
  // Never across the off-the-record boundary.
  if (source->is_incognito() != target->is_incognito()) {
    return false;
  }
  const BrowserId source_browser = ResolveBrowser(SlotId{source_slot});
  const BrowserId target_browser = ResolveBrowser(SlotId{target_slot});
  // By id, one move per member: a split pair moves both, and the framework
  // resolves each index inside the call, after the previous move shifted
  // the strip. A tab no longer in the source is a refused drop.
  bool ok = true;
  for (const TabId tab : {TabId{tab_a}, TabId{tab_b}}) {
    if (!tab.valid()) {
      continue;
    }
    ok = window_model_.FindTab(source_browser, tab) &&
         framework_->MoveTabToWindowById(source_browser, tab, target_browser) &&
         ok;
  }
  if (ok) {
    source->ScheduleRefresh();
    target->ScheduleRefresh();
  }
  return ok;
}

bool PhiCoordinator::AlwaysShowBookmarkBar() const {
  return settings_.always_show_bookmark_bar;
}

void PhiCoordinator::SetAlwaysShowBookmarkBar(bool enabled) {
  if (enabled == settings_.always_show_bookmark_bar) {
    return;
  }
  settings_.always_show_bookmark_bar = enabled;
  ScheduleSave(StoreDomain::kSettings);
  // The bar takes/releases a band of the content rect in every window.
  for (PhiWindowController* controller : registry_.Controllers()) {
    controller->LayoutContents();
  }
  registry_.InvalidateAll();
}

bool PhiCoordinator::BookmarkBarOnNewTab() const {
  return settings_.bookmark_bar_on_new_tab;
}

void PhiCoordinator::SetBookmarkBarOnNewTab(bool enabled) {
  if (enabled == settings_.bookmark_bar_on_new_tab) {
    return;
  }
  settings_.bookmark_bar_on_new_tab = enabled;
  ScheduleSave(StoreDomain::kSettings);
  for (PhiWindowController* controller : registry_.Controllers()) {
    controller->LayoutContents();
  }
  registry_.InvalidateAll();
}

bool PhiCoordinator::AccountInfo(std::string* name,
                                 std::string* email) const {
  if (!auth_.signed_in()) {
    return false;
  }
  if (name) {
    *name = auth_.user().name;
  }
  if (email) {
    *email = auth_.user().email;
  }
  return true;
}

bool PhiCoordinator::AccountSigningIn() const {
  return auth_.signing_in();
}

void PhiCoordinator::RequestSignIn() {
  auth_.BeginSignIn([this](const std::string& url) {
    // The hosted login opens as an ordinary Phi tab — Phi IS the browser
    // RFC 8252 wants the flow to ride in.
    if (const SlotId slot = registry_.FirstUserSlot(); slot.valid()) {
      RequestOpen(slot.value, url);
    }
  });
}

void PhiCoordinator::RequestSignOut() {
  auth_.SignOut();
  PhiSettingsWindow::Refresh();
}

void PhiCoordinator::RequestDefaultBrowser() {
  if (framework_) {
    framework_->RequestDefaultBrowser();
  }
}

PhiLayoutMode PhiCoordinator::LayoutMode() const {
  return settings_.layout_mode;
}

void PhiCoordinator::RequestLayoutMode(PhiLayoutMode mode) {
  if (settings_.layout_mode == mode) {
    return;
  }
  settings_.layout_mode = mode;
  ScheduleSave(StoreDomain::kSettings);
  // Every slot re-lays out: the layout is a client-wide setting, so two
  // windows side by side must never disagree.
  for (PhiWindowController* controller : registry_.Controllers()) {
    controller->ApplyLayoutMode(mode);
  }
}

std::vector<PhiSpaceView> PhiCoordinator::Spaces() const {
  std::vector<PhiSpaceView> out;
  out.reserve(spaces_model_.spaces.size());
  for (const Space& space : spaces_model_.spaces) {
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
    return s ? registry_.Resolve(s->window_id.value) : nullptr;
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
  registry_.InvalidateUserSlots();
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
  PhiWindowController* controller =
      registry_.Resolve(space->window_id.value);
  if (!controller) {
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
  PhiAgentOverlay::Show(controller, watched_agent_task_, label, caption,
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
  registry_.InvalidateUserSlots();
}

void PhiCoordinator::RequestShowTranscript(const std::string& task_id) {
  if (!agent_spaces_.Find(task_id)) {
    return;
  }
  // Own the panel from the first USER window's shell — the panel floats, so
  // any live shell serves as its owner.
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (!controller->is_agent() && controller->shell_hwnd()) {
      PhiTranscriptPanel::Toggle(controller->shell_hwnd(),
                                 PaletteForSlot(controller->slot().value),
                                 task_id, this);
      return;
    }
  }
}

std::vector<std::string> PhiCoordinator::TranscriptLines(
    const std::string& task_id) {
  std::vector<std::string> lines;
  if (PhiAgentSpaces::Space* space = agent_spaces_.Find(task_id)) {
    lines.assign(space->log.begin(), space->log.end());
  }
  return lines;
}

std::string PhiCoordinator::TranscriptAgentName(const std::string& task_id) {
  PhiAgentSpaces::Space* space = agent_spaces_.Find(task_id);
  return space && !space->agent_name.empty() ? space->agent_name : "Agent";
}

bool PhiCoordinator::PushTranscriptMessage(const std::string& task_id,
                                           const std::string& text) {
  const std::string id = agent_spaces_.PushMessage(
      task_id, text, base::Time::Now().InMillisecondsSinceUnixEpoch());
  if (id.empty()) {
    return false;
  }
  // The console shows the user's line inline, like the wire's userMessage.
  agent_spaces_.AppendLog(task_id, "You: " + text);
  return true;
}

void PhiCoordinator::RequestNewWindow(int64_t slot) {
  // Same request the agentSpace.newWindow verb makes: a kUser window whose
  // source is the LIVE browser the slot shows.
  const SlotId source = SlotId{slot};
  const BrowserId browser = ResolveBrowser(source);
  if (!browser.valid()) {
    OpenDefaultShellIfNoUserWindow("New Window from a slot with no browser");
    return;
  }
  PendingCreation pending;
  pending.kind = phi_framework::WindowKind::kUser;
  RequestCreation(phi_framework::WindowKind::kUser, std::string(),
                  /*incognito=*/false, browser, std::move(pending));
}

void PhiCoordinator::RequestSwitchSpace(int64_t slot,
                                        const std::string& space_id) {
  base::DictValue payload;
  payload.Set("windowId", static_cast<double>(slot));
  payload.Set("spaceId", space_id);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  ExecuteControlOnUi("agentSpace.spaces.switch", json,
                     ControlCaller::kInternal);
}

void PhiCoordinator::RequestCreateSpace(int64_t slot, bool new_profile) {
  // Same profile as the slot's current Space unless a new one was asked for.
  std::string profile_id = "Default";
  if (PhiWindowController* controller = registry_.Resolve(slot)) {
    const std::string& active = controller->active_space();
    for (const Space& s : spaces_model_.spaces) {
      if (s.id == active) {
        profile_id = s.profile_id;
        break;
      }
    }
  }
  if (new_profile && framework_) {
    // Profile creation is asynchronous and the basename is only known once
    // OnProfileCreated reports it; the Space (bound to that profile —
    // bindings are immutable, so it must not be created against the wrong
    // one first) follows there.
    PendingProfile pending;
    pending.display_name =
        "Phi Space " + std::to_string(spaces_model_.next_space_id);
    pending.slot = SlotId{slot};
    pending.then = PendingProfile::Then::kOpenSpace;
    RequestProfile(std::move(pending));
    return;
  }
  base::DictValue payload;
  payload.Set("name", "Space " + std::to_string(spaces_model_.next_space_id));
  payload.Set("profileId", profile_id);
  std::string json;
  base::JSONWriter::Write(payload, &json);
  const std::string created = ExecuteControlOnUi(
      "agentSpace.spaces.create", json, ControlCaller::kInternal);
  std::optional<base::DictValue> r =
      base::JSONReader::ReadDict(created, base::JSON_PARSE_RFC);
  if (r) {
    if (const std::string* id = r->FindString("id")) {
      RequestSwitchSpace(slot, *id);
    }
  }
}

void PhiCoordinator::RequestCreateProfileForSpaceForm(int64_t slot,
                                                      const std::string& name) {
  if (!framework_ || name.empty()) {
    return;
  }
  // Same asynchronous flow as "New Space with New Profile" — except the
  // form gets the id (OnProfileCreated) instead of a ready-made Space.
  PendingProfile pending;
  pending.display_name = name;
  pending.slot = SlotId{slot};
  pending.then = PendingProfile::Then::kFillForm;
  RequestProfile(std::move(pending));
}

std::string PhiCoordinator::RequestCreateSpaceWithOptions(
    int64_t slot,
    const PhiNewSpaceSpec& spec) {
  // CreateSpacePanel.create: the Space through the one creation flow, then
  // its theme pinned (setTheme / setOverlaySaturation), then activated in
  // the window the form was in.
  base::DictValue payload;
  if (!spec.name.empty()) {
    payload.Set("name", spec.name);
  }
  payload.Set("profileId",
              spec.profile_id.empty() ? std::string("Default") : spec.profile_id);
  if (!spec.icon.empty()) {
    payload.Set("iconName", spec.icon);
  }
  if (!spec.color_hex.empty()) {
    payload.Set("colorHex", spec.color_hex);
  }
  std::string json;
  base::JSONWriter::Write(payload, &json);
  const std::string created = ExecuteControlOnUi(
      "agentSpace.spaces.create", json, ControlCaller::kInternal);
  std::optional<base::DictValue> r =
      base::JSONReader::ReadDict(created, base::JSON_PARSE_RFC);
  const std::string* id = r ? r->FindString("id") : nullptr;
  if (!id) {
    return std::string();
  }
  const std::string new_id = *id;
  if (!spec.theme_id.empty()) {
    for (Space& space : spaces_model_.spaces) {
      if (space.id == new_id) {
        space.theme_id = PhiThemeById(spec.theme_id).id;
        space.saturation =
            spec.saturation < 0 ? -1 : std::min(100, spec.saturation);
        break;
      }
    }
    ScheduleSave(StoreDomain::kSpaces);
    RebuildSpacePalettes();
    for (PhiWindowController* controller : registry_.Controllers()) {
      controller->ApplyPalette();
    }
    PhiSettingsWindow::Refresh();
  }
  RequestSwitchSpace(slot, new_id);
  return new_id;
}

PhiWindowController* PhiCoordinator::ControllerForSlot(SlotId slot) {
  // Wire windowIds may name a slot, a browser bound to one (what
  // spaces.list reports), or 0 for "the window": one resolver for all.
  return registry_.Resolve(slot.value);
}

BrowserId PhiCoordinator::ResolveBrowser(int64_t slot_or_browser) const {
  const BrowserId shown = ResolveBrowser(SlotId{slot_or_browser});
  return shown.valid() ? shown : BrowserId{slot_or_browser};
}

BrowserId PhiCoordinator::ResolveBrowser(SlotId slot) const {
  PhiWindowController* controller = registry_.Resolve(slot.value);
  return controller ? controller->active_browser_id() : BrowserId();
}

std::string PhiCoordinator::ExecuteControlOnUi(const std::string& type,
                                               const std::string& payload_json,
                                               ControlCaller caller) {
  // Runs on the UI thread — safe to touch registry_ and call framework_.
  base::DictValue out;
  // The phi-browser skill's agent-space lifecycle (create/ping/complete/
  // ownership/log/messages/…) is served first; it owns the taskId-keyed
  // model. `agentSpace.list` deliberately falls THROUGH so both the skill's
  // `tasks` view and the box probes' `windows` view are merged below.
  {
    std::optional<base::DictValue> payload =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    if (caller == ControlCaller::kPipe) {
      // The two runtime gates, BEFORE any ladder sees the verb (lane 5 F3).
      // An agent works inside its own agent Spaces by default; the user's
      // Spaces open up only through the Developer-pane toggle. The internal
      // callers (the client's own UI) are never gated — they ARE the user.
      if (!settings_.allow_agent_user_spaces &&
          IsUserSpaceRequest(type, payload ? &*payload : nullptr)) {
        out.Set("ok", false);
        out.Set("error", "user_space_operations_disabled");
        std::string s;
        base::JSONWriter::Write(out, &s);
        return s;
      }
      // Harness-only verbs: what the box runner needs (a shell HWND, a PNG
      // on disk, the analytics ring, a fake signed-in account) and nothing
      // a shipped agent should have. PhiSettings.json testRig unlocks them.
      const bool harness_only =
          type == "agentSpace.feedback.setAccount" ||
          type == "agentSpace.analytics.list" ||
          type == "agentSpace.window.handle" ||
          (type == "agentSpace.captureWindow" && payload &&
           payload->FindString("outPath"));
      if (harness_only && !settings_.test_rig) {
        out.Set("ok", false);
        out.Set("error", "test_rig_only");
        std::string s;
        base::JSONWriter::Write(out, &s);
        return s;
      }
    }
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
    // The lifecycle phase and the saves still coalescing, so a suite can
    // wait for "running" / 0 instead of sleeping (and never kill the
    // process out from under a write it just asked for).
    out.Set("phase", AppPhaseName(phase_));
    out.Set("pendingSaves", std::popcount(dirty_stores_));
  } else if (type == "agentSpace.list") {
    base::ListValue list;
    for (PhiWindowController* controller : registry_.Controllers()) {
      list.Append(static_cast<double>(controller->slot().value));
    }
    out.Set("windows", std::move(list));
    // The skill reads `tasks`; the box probes read `windows`. Ship both.
    out.Set("tasks", agent_spaces_.TasksJson(::GetTickCount64()));
    // Whether the Agent Transcript console is open (the Mac's View ▸ Agent
    // Transcript). The skill lists tasks at every round bind and runs its
    // session-mirror daemon only while this reads true — with the console
    // closed nobody is watching a mirror, and completion is immediate.
    out.Set("transcriptMirror", PhiTranscriptPanel::IsOpen());
  } else if (type == "agentSpace.newWindow") {
    // A kUser request in the first user window's profile — the framework
    // wants a LIVE browser as the source, so the slot resolves to the
    // browser it is showing, never its (possibly long gone) original. With
    // no user window at all, the default-shell rescue does the same job.
    const SlotId first = registry_.FirstUserSlot();
    if (!first.valid()) {
      OpenDefaultShellIfNoUserWindow("newWindow without a user window");
      out.Set("ok", registry_.HasPending());
    } else {
      PendingCreation pending;
      pending.kind = phi_framework::WindowKind::kUser;
      const RequestToken token = RequestCreation(
          phi_framework::WindowKind::kUser, std::string(),
          /*incognito=*/false, ResolveBrowser(first), std::move(pending));
      out.Set("ok", token.valid());
    }
  } else if (type == "agentSpace.close") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id = 0;
    if (p) {
      window_id = static_cast<int64_t>(p->FindDouble("windowId").value_or(0));
    }
    // Closing a SLOT means closing every Space browser it hosts, not just the
    // browser its id names — same reason the shell window's close box does.
    bool ok = false;
    if (PhiWindowController* controller = registry_.Resolve(window_id)) {
      ok = true;  // Set first: the call below may destroy the controller.
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
      framework_->Navigate(ResolveBrowser(window_id).value, url.c_str());
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.spaces.list") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    PhiWindowController* shell = registry_.Resolve(
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0);
    base::ListValue list;
    int sort_order = 0;
    for (const Space& space : spaces_model_.spaces) {
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
      s.Set("isDefault", space.id == DefaultSpaceId());
      const bool active = shell && shell->active_space() == space.id;
      s.Set("active", active);
      s.Set("isActive", active);
      s.Set("open", shell && shell->HasSpace(space.id));
      // Every user window showing the Space, as CDP windowIds (the Space's
      // browser in each slot) — what the skill pins a binding to
      // (enterContext({kind:'user', window})); empty when it is not open.
      base::ListValue window_ids;
      for (PhiWindowController* other : registry_.Controllers()) {
        if (other->is_agent()) {
          continue;
        }
        const BrowserId browser = other->BrowserIdForSpace(space.id);
        if (browser.valid()) {
          window_ids.Append(static_cast<double>(browser.value));
        }
      }
      s.Set("windowIds", std::move(window_ids));
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
    const std::string id = std::to_string(spaces_model_.next_space_id++);
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
    spaces_model_.spaces.push_back(std::move(space));
    ScheduleSave(StoreDomain::kSpaces);
    RebuildSpacePalettes();  // A wire colorHex tints the new Space.
    if (activate) {
      // Surface it in the user's focused window — explicit opt-in only (the
      // skill's default create never yanks the user's window).
      PhiWindowController* focused = FocusedUserController();
      bool pending = false, cold = false;
      SwitchSlotToSpace(focused ? focused->slot() : SlotId(), id, &pending,
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
    const bool ok = SwitchSlotToSpace(SlotId{slot}, space_id, &pending, &cold);
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
    // Deleting a profile erases its data on disk. The skill contract
    // (references/management.md) promises agents can create and rename
    // profiles but never delete them, so from the pipe this is a test-rig
    // verb only; and even the rig has to say `confirm:true`, the nod the
    // Settings pane asks the user for.
    if (type == "agentSpace.profiles.delete" && caller == ControlCaller::kPipe &&
        !settings_.test_rig) {
      out.Set("ok", false);
      out.Set("error", "test_rig_only");
      std::string s;
      base::JSONWriter::Write(out, &s);
      return s;
    }
    if (type == "agentSpace.profiles.delete" &&
        !(p && p->FindBool("confirm").value_or(false))) {
      out.Set("ok", false);
      out.Set("error", "confirm_required");
      std::string s;
      base::JSONWriter::Write(out, &s);
      return s;
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
    for (const PendingProfile& pending : pending_profiles_) {
      if (ok && pending.display_name == name) {
        ok = false;  // Asked for already; the answer is on its way.
        out.Set("error", "duplicate_name");
      }
    }
    if (ok) {
      // Deferred: the basename exists only once OnProfileCreated names it,
      // and the skill's `const {profileId} = await createProfile(...)`
      // needs it in the reply.
      PendingProfile pending;
      pending.display_name = name;
      pending.then = PendingProfile::Then::kReplyOnly;
      pending.pending_reply_id =
          RegisterPendingReply(/*document=*/false, /*include_html=*/false);
      out.Set("pendingReply", pending.pending_reply_id);
      RequestProfile(std::move(pending));
    } else {
      out.Set("ok", false);
    }
  } else if (type == "agentSpace.window.profile") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    if (const phi_framework::WindowSnapshot* window =
            window_model_.Get(ResolveBrowser(slot))) {
      out.Set("id", window->profile_id);
      out.Set("name", ProfileDisplayName(window->profile_id));
      out.Set("incognito", window->incognito);
    }
    out.Set("ok", out.contains("id"));
  } else if (type == "agentSpace.layout.get") {
    out.Set("mode", PhiLayoutModeName(settings_.layout_mode));
    out.Set("ok", true);
  } else if (type == "agentSpace.layout.set") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    PhiLayoutMode mode = settings_.layout_mode;
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
    out.Set("mode", PhiLayoutModeName(settings_.layout_mode));
    out.Set("sidebarWidth", settings_.sidebar_width);
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
      MirrorPinScope();
      registry_.InvalidateAll();
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
    if (OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
      for (const PhiRow& row : tab_store_.PinnedFor(profile_id, space_id)) {
        base::DictValue entry;
        entry.Set("guid", row.guid);
        entry.Set("title", row.title);
        entry.Set("url", row.url);
        entry.Set("index", row.index);
        entry.Set("favicon", row.favicon_base64);
        entry.Set("faviconWidth", row.favicon_width);
        entry.Set("faviconHeight", row.favicon_height);
        entry.Set("secondaryUrl", row.url_b);
        entry.Set("secondaryTitle", row.title_b);
        entry.Set("layout", row.layout);
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
    PhiWindowController* controller = ControllerForSlot(SlotId{slot});
    std::string profile_id, space_id, url;
    bool known = false;
    if (controller &&
        OwnerForSlot(controller->slot(), &profile_id, &space_id)) {
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
    if (OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
      // No url given: pin what the slot is showing.
      if (url.empty()) {
        if (const phi_framework::WindowSnapshot* window =
                window_model_.Get(ResolveBrowser(slot))) {
          url = window->nav.url;
          if (title.empty()) {
            title = window->nav.title;
          }
        }
      }
      int fw = 0, fh = 0;
      // Only when we are pinning what the slot is SHOWING: the live favicon
      // belongs to the active tab, not to some other url the caller named.
      const std::string favicon = url == LiveUrl(SlotId{slot})
                                      ? CaptureFavicon(SlotId{slot}, &fw, &fh)
                                      : std::string();
      guid = tab_store_.AddPinned(profile_id, space_id, title, url, favicon, fw,
                                  fh);
      // Report what was captured: a caller (or a test) can then tell "no icon
      // yet" apart from "the pin failed".
      out.Set("faviconWidth", fw);
      out.Set("faviconHeight", fh);
      out.Set("liveUrl", LiveUrl(SlotId{slot}));
    }
    if (!guid.empty()) {
      registry_.InvalidateAll();
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
    if (OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
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
    if (OwnerForSlot(SlotId{slot}, &profile_id, &space_id)) {
      if (!folder && url.empty()) {
        if (const phi_framework::WindowSnapshot* window =
                window_model_.Get(ResolveBrowser(slot))) {
          url = window->nav.url;
          if (title.empty()) {
            title = window->nav.title;
          }
        }
      }
      int fw = 0, fh = 0;
      const std::string favicon = (!folder && url == LiveUrl(SlotId{slot}))
                                      ? CaptureFavicon(SlotId{slot}, &fw, &fh)
                                      : std::string();
      guid = tab_store_.AddBookmark(profile_id, space_id, parent, title, url,
                                    folder, favicon, fw, fh);
    }
    if (!guid.empty()) {
      registry_.InvalidateAll();
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
      for (const Space& space : spaces_model_.spaces) {
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
      for (Space& space : spaces_model_.spaces) {
        if (space.id != space_id) {
          continue;
        }
        space.icon = icon;
        ScheduleSave(StoreDomain::kSpaces);
        ok = true;
        registry_.InvalidateAll();
        break;
      }
    }
    out.Set("icon", icon);
    out.Set("ok", ok);
  } else if (type == "agentSpace.theme.get") {
    out.Set("themeId", settings_.theme_id);
    out.Set("appearance", PhiAppearanceName(Appearance()));
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
    out.Set("themeId", settings_.theme_id);
    out.Set("appearance", PhiAppearanceName(Appearance()));
    out.Set("dark", palette_.dark);
    out.Set("ok", ok);
  } else if (type == "agentSpace.settings.get") {
    // Everything the settings window shows, in one read.
    out.Set("section", PhiSettingsWindow::CurrentSection());
    base::ListValue sections;
    // The Mac's panes(), in its order; Developer only in developer mode.
    // (There is no Phi Link pane on Mac — IMChannels exists in its source
    // but is never mounted.)
    for (const char* name : {"account", "general", "profiles", "spaces",
                             "navigation", "ai", "shortcuts"}) {
      sections.Append(name);
    }
    if (settings_.developer_mode) {
      sections.Append("developer");
    }
    out.Set("sections", std::move(sections));
    out.Set("developerMode", settings_.developer_mode);
    out.Set("allowAgentUserSpaces", settings_.allow_agent_user_spaces);
    out.Set("themeId", settings_.theme_id);
    out.Set("appearance", PhiAppearanceName(Appearance()));
    out.Set("saturation", settings_.saturation);
    out.Set("layout", PhiLayoutModeName(settings_.layout_mode));
    out.Set("newTabBehavior", NewTabBehavior() == PhiNewTabBehavior::kOmnibox
                                  ? "omnibox"
                                  : "newTabPage");
    out.Set("alwaysShowFullUrl", settings_.always_show_full_url);
    out.Set("showTabPreviews", settings_.show_tab_previews);
    out.Set("restoreLastSession", settings_.restore_last_session);
    out.Set("analyticsEnabled", settings_.analytics_enabled);
    out.Set("peekViewEnabled", settings_.peek_view_enabled);
    out.Set("autoPeekViewEnabled", settings_.auto_peek_view_enabled);
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
    // The keys that decide what an agent may reach — or what the user has
    // agreed to share — are never the agent's to set: developerMode unlocks
    // the pane that holds the toggle, allowAgentUserSpaces IS the toggle,
    // testRig unlocks the harness verbs, analyticsEnabled is the telemetry
    // consent. Refused whole — a payload that carries one gets none of its
    // keys.
    if (caller == ControlCaller::kPipe && p &&
        (p->contains("developerMode") || p->contains("allowAgentUserSpaces") ||
         p->contains("testRig") || p->contains("analyticsEnabled"))) {
      out.Set("ok", false);
      out.Set("error", "pref_not_agent_settable");
      std::string s;
      base::JSONWriter::Write(out, &s);
      return s;
    }
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
      if (std::optional<bool> previews = p->FindBool("showTabPreviews")) {
        SetShowTabPreviews(*previews);
        ok = true;
      }
      if (std::optional<bool> restore = p->FindBool("restoreLastSession")) {
        SetRestoreLastSession(*restore);
        ok = true;
      }
      // Peek's master first: turning it off drags the sub-switch down with
      // it, so a payload carrying both still ends where it asked to.
      if (std::optional<bool> peek = p->FindBool("peekViewEnabled")) {
        SetPeekViewEnabled(*peek);
        SetAutoPeekViewEnabled(*peek);
        ok = true;
      }
      if (std::optional<bool> auto_peek = p->FindBool("autoPeekViewEnabled")) {
        SetAutoPeekViewEnabled(*auto_peek);
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
        if (*section != "developer" || settings_.developer_mode) {
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
    out.Set("developerMode", settings_.developer_mode);
    out.Set("saturation", settings_.saturation);
    out.Set("alwaysShowFullUrl", settings_.always_show_full_url);
    out.Set("showTabPreviews", settings_.show_tab_previews);
    out.Set("restoreLastSession", settings_.restore_last_session);
    out.Set("peekViewEnabled", settings_.peek_view_enabled);
    out.Set("autoPeekViewEnabled", settings_.auto_peek_view_enabled);
    out.Set("newTabBehavior", NewTabBehavior() == PhiNewTabBehavior::kOmnibox
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
    if (PhiWindowController* shell = registry_.Resolve(
            p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0))
              : 0)) {
      shell->FlushPendingRebuild();  // The rows below must not be stale.
      const RECT content = shell->content_rect();
      base::DictValue rect;
      rect.Set("x", static_cast<int>(content.left));
      rect.Set("y", static_cast<int>(content.top));
      rect.Set("width", static_cast<int>(content.right - content.left));
      rect.Set("height", static_cast<int>(content.bottom - content.top));
      out.Set("content", std::move(rect));
      // Geometry is in PHYSICAL pixels; a caller comparing against design
      // constants has to scale them by this.
      out.Set("dpi", static_cast<int>(shell->dpi()));
      const RECT url = shell->url_field_rect();
      base::DictValue field;
      field.Set("x", static_cast<int>(url.left));
      field.Set("y", static_cast<int>(url.top));
      field.Set("width", static_cast<int>(url.right - url.left));
      field.Set("height", static_cast<int>(url.bottom - url.top));
      out.Set("urlField", std::move(field));
      // Reader View's button, and the two verdicts behind it. An empty rect
      // means the reader is not being offered for the active tab; the two
      // flags say which signal (if either) would have offered it.
      const RECT reader = shell->reader_button_rect();
      base::DictValue reader_rect;
      reader_rect.Set("x", static_cast<int>(reader.left));
      reader_rect.Set("y", static_cast<int>(reader.top));
      reader_rect.Set("width", static_cast<int>(reader.right - reader.left));
      reader_rect.Set("height", static_cast<int>(reader.bottom - reader.top));
      out.Set("reader", std::move(reader_rect));
      out.Set("readerProbe", shell->ActiveTabExtensionOfferable());
      out.Set("readerDistillable", shell->ActiveTabDistillable());
      out.Set("readerActive", shell->ActiveTabShowsReader());
      out.Set("tabsTop", shell->tabs_top());
      out.Set("rowHeight", shell->tab_row_height());
      out.Set("rowGap", shell->tab_row_gap());
      out.Set("titleBar", shell->title_bar_height());
      out.Set("sidebarWidth", shell->sidebar_width());
      out.Set("contentMargin", shell->content_margin());
      const POINT frame = shell->frame_offset();
      out.Set("frameLeft", static_cast<int>(frame.x));
      out.Set("frameTop", static_cast<int>(frame.y));
      out.Set("mode", PhiLayoutModeName(settings_.layout_mode));
      out.Set("rows", shell->LayoutRowsJson());
      {
        // The surface registry's view of this window: origin tab -> kind.
        base::DictValue surfaces;
        for (const auto& [tab, kind] :
             surfaces_.Surfaces(shell->active_browser_id())) {
          surfaces.Set(std::to_string(tab),
                       kind == SidebarSurface::kPeek ? "peek" : "reader");
        }
        out.Set("surfaces", std::move(surfaces));
      }
      out.Set("ok", true);
    } else {
      out.Set("ok", false);
    }
  } else if (type == "agentSpace.feedback.status") {
    // The outbox (every job of the current account with its state) and the
    // form, when open — the harness's view of the feedback pipeline.
    if (std::optional<base::Value> status = base::JSONReader::Read(
            PhiFeedbackOutbox::Get().StatusJson(), base::JSON_PARSE_RFC)) {
      if (base::DictValue* dict = status->GetIfDict()) {
        out.Merge(std::move(*dict));
      }
    }
    if (std::optional<base::Value> form = base::JSONReader::Read(
            PhiFeedbackWindow::StateJson(), base::JSON_PARSE_RFC)) {
      out.Set("form", std::move(*form));
    }
    out.Set("ok", true);
  } else if (type == "agentSpace.feedback.open") {
    // Opens the form for {windowId} (a slot), else the focused window —
    // exactly what Help ▸ Report an Issue does.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    PhiWindowController* controller = nullptr;
    if (p) {
      if (std::optional<double> window_id = p->FindDouble("windowId")) {
        controller = registry_.Resolve(static_cast<int64_t>(*window_id));
      }
    }
    if (!controller) {
      controller = FocusedUserController();
    }
    if (controller) {
      controller->OpenFeedback();
    }
    const HWND form = PhiFeedbackWindow::Handle();
    out.Set("ok", form != nullptr);
    out.Set("hwnd", static_cast<double>(reinterpret_cast<uintptr_t>(form)));
  } else if (type == "agentSpace.feedback.form") {
    // Drives the open form: {description?, url?, attachFiles?:[path],
    // send?:true} — the Send path is the button's own.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    std::string description;
    std::string url;
    std::vector<std::wstring> files;
    bool set_description = false;
    bool set_url = false;
    bool send = false;
    if (p) {
      if (const std::string* value = p->FindString("description")) {
        description = *value;
        set_description = true;
      }
      if (const std::string* value = p->FindString("url")) {
        url = *value;
        set_url = true;
      }
      if (const base::ListValue* list = p->FindList("attachFiles")) {
        for (const base::Value& v : *list) {
          if (v.is_string()) {
            files.push_back(base::UTF8ToWide(v.GetString()));
          }
        }
      }
      send = p->FindBool("send").value_or(false);
    }
    const std::string error = PhiFeedbackWindow::Drive(
        description, set_description, url, set_url, files, send);
    out.Set("ok", error.empty());
    if (!error.empty()) {
      out.Set("error", error);
    }
  } else if (type == "agentSpace.feedback.setAccount") {
    // Harness only: {userId, email} stands in for a signed-in account on a
    // box without one (empty clears). A real session always wins.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    const std::string* user_id = p ? p->FindString("userId") : nullptr;
    const std::string* email = p ? p->FindString("email") : nullptr;
    test_feedback_user_id_ = user_id ? *user_id : std::string();
    test_feedback_email_ = email ? *email : std::string();
    SyncFeedbackAccount();
    out.Set("ok", true);
    out.Set("signedIn", auth_.signed_in());
  } else if (type == "agentSpace.telemetry.status") {
    // What the outbound pipeline is doing. `configured:false` is the normal
    // answer until a collector is named in phi_telemetry.json.
    if (std::optional<base::Value> status = base::JSONReader::Read(
            PhiTelemetry::Get().StatusJson(), base::JSON_PARSE_RFC)) {
      if (base::DictValue* dict = status->GetIfDict()) {
        out.Merge(std::move(*dict));
      }
    }
    out.Set("ok", true);
  } else if (type == "agentSpace.analytics.list") {
    // Everything phi_analytics has delivered this session, oldest first. The
    // only end-to-end observable of the sink while the client has no
    // analytics pipeline: an empty list here means the browser's events are
    // not crossing the boundary.
    base::ListValue events;
    for (const AnalyticsEvent& event : analytics_events_) {
      base::DictValue row;
      row.Set("module", event.module);
      row.Set("event", event.name);
      if (std::optional<base::Value> props = base::JSONReader::Read(
              event.properties_json, base::JSON_PARSE_RFC)) {
        row.Set("properties", std::move(*props));
      }
      events.Append(std::move(row));
    }
    out.Set("events", std::move(events));
    out.Set("ok", true);
  } else if (type == "agentSpace.menu.list") {
    // The shell window's menu bar as the user would see it, dynamic rows
    // included — the automated stand-in for opening each menu by hand.
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    PhiWindowController* controller = ControllerForSlot(SlotId{slot});
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
    PhiWindowController* controller = ControllerForSlot(SlotId{slot});
    out.Set("ok", controller && controller->menu_bar() && !path.empty() &&
                      controller->menu_bar()->InvokePath(path));
  } else if (type == "agentSpace.window.handle") {
    // The slot's shell HWND, so a test/screenshot tool can capture exactly the
    // window it is driving (several slots may exist).
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t slot =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    PhiWindowController* shell = registry_.Resolve(slot);
    if (shell && shell->shell_hwnd()) {
      out.Set("hwnd", static_cast<double>(
                          reinterpret_cast<intptr_t>(shell->shell_hwnd())));
      out.Set("ok", true);
    } else {
      out.Set("ok", false);
    }
  } else if (type == "agentSpace.window.bounds") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    PhiWindowController* shell = registry_.Resolve(window_id);
    int x = 0, y = 0, w = 0, h = 0;
    if (shell && shell->GetBounds(&x, &y, &w, &h)) {
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
    PhiWindowController* shell = registry_.Resolve(window_id);
    bool ok = shell && w > 0 && h > 0 && shell->SetBounds(x, y, w, h);
    out.Set("ok", ok);
  } else if (type == "agentSpace.tabs.list") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    if (framework_ && window_id) {
      std::optional<base::Value> tabs = base::JSONReader::Read(
          framework_->ListTabsJson(ResolveBrowser(window_id).value),
          base::JSON_PARSE_RFC);
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
    TabId opened;
    int index = -1;
    if (framework_ && window_id) {
      const BrowserId browser = ResolveBrowser(window_id);
      opened = framework_->OpenTab(browser, url.c_str());
      // The wire reply is index-shaped; the strip just changed, so pull.
      if (opened.valid() && window_model_.Refresh(framework_, browser)) {
        if (const phi_framework::TabSnapshot* tab =
                window_model_.FindTab(browser, opened)) {
          index = tab->index;
        }
      }
    }
    out.Set("index", index);
    out.Set("tabId", opened.value);
    out.Set("ok", opened.valid());
  } else if (type == "agentSpace.nav.state") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    int64_t window_id =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    if (const phi_framework::WindowSnapshot* window =
            window_id ? window_model_.Get(ResolveBrowser(window_id))
                      : nullptr) {
      out.Set("url", window->nav.url);
      out.Set("title", window->nav.title);
      out.Set("canGoBack", window->nav.can_go_back);
      out.Set("canGoForward", window->nav.can_go_forward);
      out.Set("loading", window->nav.loading);
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
        ok = framework_->GoBack(ResolveBrowser(window_id).value);
      } else if (type == "agentSpace.nav.forward") {
        ok = framework_->GoForward(ResolveBrowser(window_id).value);
      } else if (type == "agentSpace.nav.reload") {
        ok = framework_->Reload(ResolveBrowser(window_id).value, bypass_cache);
      } else {
        ok = framework_->StopLoading(ResolveBrowser(window_id).value);
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
      const BrowserId browser = ResolveBrowser(window_id);
      const phi_framework::TabSnapshot* a =
          window_model_.FindTabAt(browser, index_a);
      const phi_framework::TabSnapshot* b =
          window_model_.FindTabAt(browser, index_b);
      if (window_id && a && b) {
        StoreSplitInternal(SlotId{window_id}, a->id, b->id,
                           p->FindBool("asBookmark").value_or(false),
                           /*insert_index=*/-1, /*bind_bookmark=*/false);
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
    // The wire names a strip index; the model turns it into the id the
    // framework is asked about, so a missing or stale index names nothing
    // (never tab 0).
    const BrowserId browser = ResolveBrowser(window_id);
    const phi_framework::TabSnapshot* tab =
        (framework_ && window_id) ? window_model_.FindTabAt(browser, index)
                                  : nullptr;
    if (tab) {
      const TabId id = tab->id;
      if (type == "agentSpace.tabs.activate") {
        ok = framework_->ActivateTabById(browser.value, id.value);
      } else if (type == "agentSpace.tabs.close") {
        ok = framework_->CloseTabById(browser.value, id.value);
      } else if (type == "agentSpace.tabs.split") {
        // Splits the ACTIVE tab with the tab at `index` (native
        // side-by-side; the sidebar shows one merged row).
        const TabId active = ActiveTabId(browser);
        const char* created =
            active.valid() && active != id
                ? framework_->CreateSplitViewForTabs(browser.value,
                                                     active.value, id.value,
                                                     "vertical")
                : nullptr;
        ok = created && *created;
      } else {
        ok = framework_->UnsplitById(browser, id);
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
    // MoveTab(from, to) meant "final index `to`"; by id that is "after the
    // tab now at `to`" when moving down and "before it" when moving up.
    bool ok = false;
    if (framework_ && window_id && from >= 0 && to >= 0) {
      const BrowserId browser = ResolveBrowser(window_id);
      const phi_framework::TabSnapshot* moving =
          window_model_.FindTabAt(browser, from);
      const phi_framework::TabSnapshot* anchor =
          window_model_.FindTabAt(browser, to);
      // The verb's contract (suite-tabs): a split member is refused rather
      // than dragging its pair along, which is what the anchored move would
      // do. The sidebar's own drags go through SidebarMove and keep the
      // pair semantics.
      if (moving && anchor && moving->split_id.empty()) {
        ok = from == to ||
             (to > from
                  ? framework_->MoveTabAfter(browser.value, moving->id.value,
                                             anchor->id.value, nullptr)
                  : framework_->MoveTabBefore(browser.value, moving->id.value,
                                              anchor->id.value, nullptr));
      }
    }
    out.Set("ok", ok);
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
      if (tab_store_.Reorder(guid, index)) {
        registry_.InvalidateAll();
      }
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
        index >= 0 &&
        std::any_of(spaces_model_.spaces.begin(), spaces_model_.spaces.end(),
                    [&](const Space& sp) { return sp.id == space_id; });
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
    const BrowserId browser = ResolveBrowser(window_id);
    const phi_framework::TabSnapshot* tab =
        (framework_ && window_id) ? window_model_.FindTabAt(browser, index)
                                  : nullptr;
    if (tab) {
      ok = (type == "agentSpace.tabs.group")
               ? framework_->AddTabToNewGroupById(browser, tab->id)
               : framework_->RemoveTabFromGroupById(browser, tab->id);
    }
    out.Set("ok", ok);
  } else if (type == "agentSpace.groups.list") {
    std::optional<base::DictValue> p =
        base::JSONReader::ReadDict(payload_json, base::JSON_PARSE_RFC);
    const int64_t window_id =
        p ? static_cast<int64_t>(p->FindDouble("windowId").value_or(0)) : 0;
    if (framework_ && window_id) {
      if (std::optional<base::Value> groups = base::JSONReader::Read(
              framework_->TabGroupsJson(ResolveBrowser(window_id).value),
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
      const BrowserId browser = ResolveBrowser(window_id);
      if (type == "agentSpace.groups.rename") {
        ok = framework_->RenameTabGroup(browser.value, group_id.c_str(),
                                        text.c_str());
      } else if (type == "agentSpace.groups.color") {
        ok = framework_->SetTabGroupColor(browser.value, group_id.c_str(),
                                          text.c_str());
      } else if (type == "agentSpace.groups.collapse") {
        ok = framework_->SetTabGroupCollapsed(browser.value, group_id.c_str(),
                                              collapsed);
      } else if (type == "agentSpace.groups.ungroup") {
        ok = framework_->UngroupTabGroup(browser.value, group_id.c_str());
      } else {
        ok = framework_->CloseTabGroup(browser.value, group_id.c_str());
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

  if (type.rfind("agentSpace.shadow.", 0) == 0) {
    return HandleShadowVerb(type, payload, now_ms, out);
  }

  // The skill's readerArticle() / saveArticle(): distill the task's tab
  // (targetId, else the window's active tab) without navigating it. The
  // distillation is asynchronous, so the reply is DEFERRED — see
  // RegisterPendingReply.
  if (type == "agentSpace.readerArticle" ||
      type == "agentSpace.readerDocument") {
    const std::string task_id = task_id_of();
    std::string error;
    const BrowserId browser = ResolveScopeBrowser(payload, &error);
    if (!browser.valid() || !framework_) {
      out->Set("ok", false);
      out->Set("error", error.empty() ? "unknown_task" : error);
      return true;
    }
    const std::string* target = payload->FindString("targetId");
    const bool document = type == "agentSpace.readerDocument";
    const int id = RegisterPendingReply(
        document, payload->FindBool("includeHTML").value_or(true));
    // Reader View's own pipeline first — the same site rules, ladder and
    // coverage gate the reader button runs, which is what the Mac answers
    // this verb with. It needs a chrome tab id rather than a CDP target.
    if (reader_.ExtensionAlive()) {
      const TabId tab_id{target && !target->empty()
                             ? framework_->TabIdForCdpTarget(target->c_str())
                             : ActiveTabId(browser).value};
      // Settling walks the page for lazy content first: nobody is watching
      // this tab, so the wait costs the caller time and nothing else.
      if (tab_id.valid() &&
          reader_.StartExtraction(tab_id.value, id, /*settle=*/true)) {
        out->Set("pendingReply", id);
        return true;
      }
    }
    if (!framework_->StartReaderExtraction(
            browser.value, target ? target->c_str() : "", id)) {
      CompletePendingReply(id, "{\"ok\":false,\"error\":\"unknown_target\"}");
    }
    out->Set("pendingReply", id);
    return true;
  }

  if (type == "agentSpace.create") {
    const std::string task_id = task_id_of();
    if (task_id.empty()) {
      out->Set("ok", false);
      out->Set("error", "invalid_payload");
      return true;
    }
    // Re-bind to an existing agent Space if its window is still alive AND
    // still has a tab. A window whose tabs were all closed is a ZOMBIE: on
    // the Mac it is already gone (a tabless window closes), and the skill's
    // heal contract is that re-ensuring the task yields a fresh Space rather
    // than a stranded record — the selftest's "zombie Space heals on
    // re-ensure". The Windows shell keeps a tabless browser alive, so the
    // same contract is kept by closing it here and recreating below.
    if (PhiAgentSpaces::Space* existing = agent_spaces_.Find(task_id)) {
      const bool live = registry_.IsBound(existing->window_id);
      bool zombie = false;
      if (live && framework_) {
        // A window the model has not heard of yet is pulled, not presumed
        // empty — presuming would heal a healthy Space into a fresh one.
        if (!window_model_.Has(existing->window_id)) {
          window_model_.Refresh(framework_, existing->window_id);
        }
        const phi_framework::WindowSnapshot* window =
            window_model_.Get(existing->window_id);
        zombie = !window || window->tabs.empty();
      }
      if (zombie) {
        LOG(INFO) << "[PhiClient] agent space " << task_id
                  << " is tabless — healing with a fresh window";
        CloseAgentSpace(task_id);
      }
      if (!zombie && live) {
        agent_spaces_.Create(task_id, existing->window_id,
                             existing->profile_id, existing->persistent,
                             now_ms);
        out->Set("ok", true);
        out->Set("spaceId", existing->space_id);
        out->Set("windowId", static_cast<double>(existing->window_id.value));
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
    if (registry_.HasPendingTask(task_id)) {
      out->Set("ok", false);
      out->Set("error", "busy");  // Its window is already on its way.
      return true;
    }
    // Ask for the hidden agent browser. It arrives later (contract rule 3)
    // in OnBrowserWindowCreated -> HostAgentWindow, which builds the record
    // and answers the deferred reply with the same shape as the re-bind
    // above; the pipe thread waits on it meanwhile.
    PendingCreation pending;
    pending.task_id = task_id;
    pending.persistent = persistent;
    // The driving agent's name rides the create call; the watch-mode pill
    // shows it so a watching user sees WHO holds the Space, not just "an
    // agent" (the Mac's AgentDriverBadge).
    if (const std::string* agent_name =
            payload ? payload->FindString("agentName") : nullptr) {
      pending.agent_name = *agent_name;
    }
    // The reply slot is handed to the waiter whatever happens: a refused
    // request answers it create_failed inside RequestCreation, and the
    // pipe thread collects that the same way it collects a success.
    pending.pending_reply_id =
        RegisterPendingReply(/*document=*/false, /*include_html=*/false);
    const int reply_id = pending.pending_reply_id;
    RequestCreation(phi_framework::WindowKind::kAgent, profile_id,
                    /*incognito=*/false, BrowserId(), std::move(pending));
    out->Set("pendingReply", reply_id);
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
    PhiTranscriptPanel::RefreshOpen();
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
    PhiTranscriptPanel::RefreshOpen();
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
    if (!id.empty()) {
      // The console shows the user's own lines inline, the Mac transcript's
      // interleaving.
      agent_spaces_.AppendLog(task_id, "You: " + *text);
      PhiTranscriptPanel::RefreshOpen();
    }
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
    framework_->OpenTab(space->window_id, url->c_str());
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
          // Whether an agent may reach the USER's Spaces in this profile —
          // the Developer-pane toggle, app-wide (agent Spaces are always
          // allowed).
          e.Set("agentSpacesAllowed", settings_.allow_agent_user_spaces);
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
    // The cursor glide / input-mirror effects render on the WATCHER's
    // overlay (the Mac AgentSpaceOverlayView); with nobody watching they
    // are accepted no-ops so the skill's mirroring never errors.
    const std::string task_id = task_id_of();
    if (task_id.empty() || !agent_spaces_.Find(task_id)) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    if (type == "agentSpace.cursor") {
      const std::optional<double> x = payload->FindDouble("x");
      const std::optional<double> y = payload->FindDouble("y");
      if (!x || !y) {
        out->Set("ok", false);
        out->Set("error", "invalid_payload");
        return true;
      }
      PhiAgentOverlay::SetAgentCursor(task_id, *x, *y);
    } else {
      const std::string* kind = payload->FindString("kind");
      if (!kind) {
        out->Set("ok", false);
        out->Set("error", "invalid_payload");
        return true;
      }
      const std::optional<double> x = payload->FindDouble("x");
      const std::optional<double> y = payload->FindDouble("y");
      PhiAgentOverlay::PlayAgentEffect(
          task_id, *kind, x.has_value() && y.has_value(),
          x.value_or(0), y.value_or(0),
          payload->FindDouble("w").value_or(0),
          payload->FindDouble("h").value_or(0),
          payload->FindDouble("dy").value_or(1));
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
    if (!space || !space->window_id.valid()) {
      out->Set("ok", false);
      out->Set("error", "unknown_task");
      return true;
    }
    PhiWindowController* shell =
        registry_.Resolve(space->window_id.value);
    if (!shell || !shell->shell_hwnd()) {
      out->Set("ok", false);
      out->Set("error", "no_window");
      return true;
    }
    const std::string* web_path =
        payload ? payload->FindString("webPath") : nullptr;
    if (!PhiCaptureWindowToPng(shell->shell_hwnd(), shell->content_rect(),
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
           std::any_of(spaces_model_.spaces.begin(), spaces_model_.spaces.end(),
                       [&](const Space& s) { return s.id == id; });
  };
  if (type == "agentSpace.urlRules.list") {
    base::ListValue rules;
    const std::vector<std::string> rows = URLRuleRows();
    for (size_t i = 0; i < rows.size(); ++i) {
      const std::vector<std::string> parts = SplitTabRow(rows[i]);
      base::DictValue d;
      d.Set("id",
            model::UrlRuleWireId(spaces_model_.url_rules_generation, i));
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
      size_t parsed = 0;
      if (model::ParseUrlRuleWireId(*id, spaces_model_.url_rules_generation,
                                    &parsed)) {
        index = static_cast<int>(parsed);  // The parser saturates at INT_MAX.
      } else if (id->find('-') != std::string::npos) {
        // A prior generation's id (or one with no index): the list has been
        // rewritten since it was read, so it no longer names anything.
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
  const BrowserId window_id = space->window_id;
  agent_spaces_.Remove(task_id);
  PhiTranscriptPanel::CloseFor(task_id);
  if (watched_agent_task_ == task_id) {
    watched_agent_task_.clear();
  }
  if (PhiWindowController* shell = registry_.Resolve(window_id.value)) {
    shell->RequestClose();  // Fires OnBrowserWindowClosing → cleanup.
  } else if (framework_ && window_id.valid()) {
    framework_->CloseBrowserWindow(window_id.value);
  }
  UpdateSweepTimer();
  registry_.InvalidateUserSlots();  // The agent pip disappears.
}

namespace {

// Epoch milliseconds, for the wire's createdAt.
int64_t EpochMsNow() {
  FILETIME ft = {};
  ::GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000);
}

}  // namespace

PhiCoordinator::ShadowWindow* PhiCoordinator::FindShadowByWindow(
    BrowserId window_id) {
  for (auto& [task_id, shadow] : shadow_windows_) {
    if (shadow.window_id == window_id) {
      return &shadow;
    }
  }
  return nullptr;
}

void PhiCoordinator::CloseShadowWindow(const std::string& task_id,
                                       const char* reason) {
  auto it = shadow_windows_.find(task_id);
  if (it == shadow_windows_.end()) {
    return;
  }
  const BrowserId window_id = it->second.window_id;
  shadow_windows_.erase(it);
  LOG(INFO) << "[PhiClient] shadow window closed (" << reason
            << "): " << task_id;
  if (PhiWindowController* shell = registry_.Resolve(window_id.value)) {
    shell->RequestClose();  // Fires OnBrowserWindowClosing.
  } else if (framework_ && window_id.valid()) {
    framework_->CloseBrowserWindow(window_id.value);
  }
  UpdateSweepTimer();
}

void PhiCoordinator::TouchShadow(ShadowWindow* shadow,
                                 int ttl_seconds,
                                 uint64_t now_ms) {
  if (ttl_seconds <= 0) {
    ttl_seconds = PhiAgentSpaces::kDefaultTtlSeconds;
  }
  ttl_seconds = std::clamp(ttl_seconds, 1, PhiAgentSpaces::kMaxTtlSeconds);
  shadow->keepalive_deadline_ms =
      std::max(shadow->keepalive_deadline_ms,
               now_ms + static_cast<uint64_t>(ttl_seconds) * 1000);
}

bool PhiCoordinator::HandleShadowVerb(const std::string& type,
                                      const base::DictValue* payload,
                                      uint64_t now_ms,
                                      base::DictValue* out) {
  auto fail = [&](const char* code) {
    out->Set("ok", false);
    out->Set("error", code);
    return true;
  };
  const std::string op = type.substr(strlen("agentSpace.shadow."));
  const auto touch = [&](ShadowWindow* shadow, int ttl_seconds) {
    TouchShadow(shadow, ttl_seconds, now_ms);
  };

  if (op == "list") {
    // Every live shadow window. The Mac scopes this to the calling driver's
    // origin + principal; the Windows pipe carries one consented peer, so
    // the whole registry is that caller's.
    base::ListValue rows;
    for (const auto& [task_id, shadow] : shadow_windows_) {
      if (!registry_.IsBound(shadow.window_id)) {
        continue;
      }
      base::DictValue row;
      row.Set("taskId", task_id);
      row.Set("windowId", static_cast<double>(shadow.window_id.value));
      row.Set("profileId", shadow.profile_id);
      row.Set("incognito", shadow.incognito);
      row.Set("createdAt", static_cast<double>(shadow.created_ms));
      rows.Append(std::move(row));
    }
    out->Set("shadows", std::move(rows));
    out->Set("ok", true);
    return true;
  }

  const std::string* task =
      payload ? payload->FindString("taskId") : nullptr;
  if (!task || task->empty()) {
    return fail("invalid_payload");
  }

  if (op == "create") {
    const bool incognito = payload->FindBool("incognito").value_or(false);
    std::string profile_id;
    if (const std::string* p = payload->FindString("profileId")) {
      profile_id = *p;
    } else if (const std::string* pn = payload->FindString("profileName")) {
      profile_id = *pn;
    }
    // Re-binding to a live shadow window of the same taskId is a success,
    // not a conflict, so a driver's next round reattaches. Its incognito-ness
    // is fixed at creation.
    auto existing = shadow_windows_.find(*task);
    if (existing != shadow_windows_.end()) {
      if (registry_.IsBound(existing->second.window_id)) {
        if (existing->second.incognito != incognito) {
          return fail("shadow_incognito_mismatch");
        }
        touch(&existing->second, 0);
        out->Set("ok", true);
        out->Set("windowId",
                 static_cast<double>(existing->second.window_id.value));
        out->Set("incognito", existing->second.incognito);
        return true;
      }
      shadow_windows_.erase(existing);  // Stale record; recreate below.
    }
    if (!framework_) {
      return fail("create_failed");
    }
    if (registry_.HasPendingTask(*task)) {
      return fail("busy");  // Its window is already on its way.
    }
    // Ask for the hidden browser; HostAgentWindow files the record when it
    // arrives and answers the deferred reply ({ok, windowId, incognito}).
    PendingCreation pending;
    pending.task_id = *task;
    pending.pending_reply_id =
        RegisterPendingReply(/*document=*/false, /*include_html=*/false);
    const int reply_id = pending.pending_reply_id;
    RequestCreation(phi_framework::WindowKind::kShadow, profile_id, incognito,
                    BrowserId(), std::move(pending));  // Answers if refused.
    out->Set("pendingReply", reply_id);
    return true;
  }

  auto it = shadow_windows_.find(*task);
  if (it == shadow_windows_.end() ||
      !registry_.IsBound(it->second.window_id)) {
    return fail("unknown_task");
  }
  ShadowWindow& shadow = it->second;
  if (op == "openTab") {
    const std::string* url = payload->FindString("url");
    if (!url || url->empty()) {
      return fail("invalid_payload");
    }
    if (!framework_) {
      return fail("unknown_task");
    }
    framework_->OpenTab(shadow.window_id, url->c_str());
    touch(&shadow, 0);
    out->Set("ok", true);
    return true;
  }
  if (op == "ping") {
    touch(&shadow,
          static_cast<int>(payload->FindDouble("ttlSeconds").value_or(0)));
    out->Set("ok", true);
    return true;
  }
  if (op == "close") {
    CloseShadowWindow(*task, "driver closed");
    out->Set("ok", true);
    return true;
  }
  return false;  // Not a shadow verb after all.
}

void PhiCoordinator::SweepAgentSpaces() {
  const uint64_t now_ms = ::GetTickCount64();
  for (const std::string& task_id : agent_spaces_.Expired(now_ms)) {
    LOG(INFO) << "[PhiClient] agent space expired: " << task_id;
    CloseAgentSpace(task_id);
  }
  // Shadow windows have no user to hold them open: the sweep is their only
  // backstop against an abandoned driver.
  std::vector<std::string> expired_shadows;
  for (const auto& [task_id, shadow] : shadow_windows_) {
    if (shadow.keepalive_deadline_ms && shadow.keepalive_deadline_ms < now_ms) {
      expired_shadows.push_back(task_id);
    }
  }
  for (const std::string& task_id : expired_shadows) {
    CloseShadowWindow(task_id, "keep-alive expired");
  }
  // Creations the framework never announced: it has no failure callback,
  // so the deadline is the only way a waiting verb learns its window (or
  // profile) is not coming.
  for (const PendingCreation& lost : registry_.ExpirePending(now_ms)) {
    LOG(ERROR) << "[PhiClient] window request timed out (kind "
               << static_cast<int>(lost.kind) << ", profile '"
               << lost.profile_id << "', space '" << lost.space_id
               << "', task '" << lost.task_id << "')";
    if (lost.pending_reply_id) {
      CompletePendingReply(lost.pending_reply_id, kCreateTimeoutReply);
    }
  }
  for (auto it = pending_profiles_.begin(); it != pending_profiles_.end();) {
    if (it->deadline_tick && it->deadline_tick <= now_ms) {
      LOG(ERROR) << "[PhiClient] profile creation timed out: '"
                 << it->display_name << "'";
      if (it->pending_reply_id) {
        CompletePendingReply(it->pending_reply_id, kCreateTimeoutReply);
      }
      it = pending_profiles_.erase(it);
    } else {
      ++it;
    }
  }
  UpdateSweepTimer();
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

  if constexpr (!kPhiDevHooks) {
    // No helper, no fallback: the dev store is a --dev-hooks door only.
    out->Set("ok", false);
    out->Set("error", "not_installed");
    return false;
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

bool PhiCoordinator::CredentialGrantCovers(const std::string& grant_key) const {
  // Only a live TIMED grant counts. The persisted "always" rows are display
  // only (LoadCredentialGrants), and the kind is not consulted: a fill is a
  // reveal (CredentialAccessKind), so one exposure, one grant.
  auto timed = credential_timed_grants_.find(grant_key);
  return timed != credential_timed_grants_.end() &&
         ::GetTickCount64() < timed->second.expires_tick;
}

bool PhiCoordinator::PromptCredentialApproval(const std::wstring& detail,
                                              CredentialAccessKind kind,
                                              const std::string& grant_key,
                                              bool once_only) {
  const bool reveal = kind == CredentialAccessKind::kReveal;
  const ULONGLONG now = ::GetTickCount64();
  // Denial memory first: a refused ask stays refused for five minutes, so
  // an agent cannot re-raise the same dialog until the user gives in.
  auto denied = credential_denials_.find(grant_key);
  if (denied != credential_denials_.end()) {
    if (now < denied->second) {
      AppendCredentialAudit(grant_key, kind, "denied (remembered)");
      return false;
    }
    credential_denials_.erase(denied);
  }
  if (!once_only && CredentialGrantCovers(grant_key)) {
    AppendCredentialAudit(grant_key, kind, "allowed (10-minute grant)");
    return true;
  }
  // The exposure banner is the load-bearing line, as on Mac — and it says
  // what is TRUE: a filled page is a page the agent holds a CDP session on.
  const std::wstring exposure =
      reveal ? L"The secret will be GIVEN TO THE AGENT."
             : L"Phi types the secret into the page. Any automation attached "
               L"to that page can read it.";
  const std::wstring text = detail + L"\n\n" + exposure;
  HWND owner = nullptr;
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (controller->shell_hwnd()) {
      owner = controller->shell_hwnd();
      break;
    }
  }
  // No "Always allow": a standing grant plus an agent-chosen page was the
  // exfil path. A once-only ask (cross-origin) offers no timed grant either.
  std::vector<std::wstring> options = {L"Allow once"};
  if (!once_only) {
    options.push_back(L"Allow for 10 minutes");
  }
  options.push_back(L"Deny");
  const int deny_index = static_cast<int>(options.size()) - 1;
  const int timed_index = once_only ? -1 : 1;
  int chosen = -1;
  if (owner) {
    // Deny preselected AND Cancel holding Return: Enter refuses; only a
    // deliberate click on Continue (or a row double-click) approves.
    chosen = PhiPromptChoose(owner, L"Credential request", text.c_str(),
                             options, deny_index, L"Continue", L"Cancel",
                             /*cancel_is_default=*/true);
  } else {
    // No window yet: the MessageBox fallback offers allow-ONCE only — a
    // timed grant must never be given through a degraded dialog.
    PhiModalScope modal;
    chosen = ::MessageBoxW(nullptr, (text + L"\n\nAllow this once?").c_str(),
                           L"Phi — Credential request",
                           MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND |
                               MB_TOPMOST | MB_DEFBUTTON2) == IDYES
                 ? 0
                 : deny_index;
  }
  if (chosen == timed_index) {
    TimedCredentialGrant grant;
    grant.kind = kind;
    grant.expires_tick = now + 10 * 60 * 1000;
    credential_timed_grants_[grant_key] = grant;
    AppendCredentialAudit(grant_key, kind, "allowed (10 minutes)");
    return true;
  }
  if (chosen == 0) {
    AppendCredentialAudit(grant_key, kind,
                          once_only ? "allowed (once, cross-origin)"
                                    : "allowed (once)");
    return true;
  }
  credential_denials_[grant_key] = now + 5 * 60 * 1000;
  AppendCredentialAudit(grant_key, kind, "denied");
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
  consent_.credential_audit.insert(
      consent_.credential_audit.begin(),
      std::string(stamp) + "\t" +
          (kind == CredentialAccessKind::kReveal ? "reveal" : "fill") + "\t" +
          scope + "\t" + decision);
  constexpr size_t kMaxAudit = 50;
  if (consent_.credential_audit.size() > kMaxAudit) {
    consent_.credential_audit.resize(kMaxAudit);
  }
  ScheduleSave(StoreDomain::kConsent);
  PhiSettingsWindow::Refresh();
}

bool PhiCoordinator::HandleCredentialsVerb(const std::string& type,
                                           const base::DictValue* payload,
                                           base::DictValue* out) {
  if constexpr (!kPhiDevHooks) {
    // A release build knows one provider, the vault helper. Without it
    // there is nothing to ask — not the dev store, which only a --dev-hooks
    // build ever loads.
    if (!vault_helper_.running()) {
      if (type == "credentials.status") {
        out->Set("ok", true);
        out->Set("status", "not_installed");
        out->Set("ready", false);
      } else {
        out->Set("ok", false);
        out->Set("error", "not_installed");
      }
      return true;
    }
  }
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
    if constexpr (!kPhiDevHooks) {
      // The helper started but is not answering: unavailable, not the dev
      // store's "logged_out".
      out->Set("ok", true);
      out->Set("status", "unavailable");
      out->Set("ready", false);
      return true;
    }
    out->Set("ok", true);
    out->Set("status", credentials_.present() ? "ready" : "logged_out");
    out->Set("ready", credentials_.ready());
    return true;
  }

  if (type == "credentials.getTotp") {
    // A TOTP code is a SECOND FACTOR handed to the agent, which makes the
    // second factor worth nothing against an agent that already holds the
    // first. Withdrawn from the agent-callable set (docs/architecture-
    // review.md §3.7 item 5); the helper's getTotp stays unused.
    out->Set("ok", false);
    out->Set("error", "totp_not_supported");
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
    // A read hands the secret TO the agent — the reveal exposure. No page
    // is involved, so the grant key is the item scope alone; it never
    // covers a fill (those are keyed by page host).
    if (!PromptCredentialApproval(detail, CredentialAccessKind::kReveal,
                                  scope, /*once_only=*/false)) {
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
    // A cross-origin fill (the item's site is not the page's) ALWAYS
    // prompts, says so in capitals, and is approved once only — never
    // through a timed grant, never into one.
    const bool cross_origin =
        !PhiCredentialVault::HostMatches(page_host, cred_host);
    std::string detail_utf8 =
        "An agent is requesting a credential fill:\n\n" + scope + "  → " +
        field + " on " + page_host;
    if (cross_origin) {
      detail_utf8 += "\n\nCROSS-ORIGIN: " + cred_host + " → " + page_host;
    }
    const std::wstring detail = base::UTF8ToWide(detail_utf8);
    // Grants are per page host: an approval for site A's password on host
    // A never carries to a page the agent navigated to afterwards.
    const std::string grant_key = scope + "|" + page_host;
    if (!PromptCredentialApproval(detail, CredentialAccessKind::kFill,
                                  grant_key, /*once_only=*/cross_origin)) {
      out->Set("ok", false);
      out->Set("error", "user_denied");
      return true;
    }
    // The dialog sat open for a while; the page may have navigated under
    // it. Fill only the host the user approved.
    const std::string host_now = PhiCredentialVault::HostOfUri(
        framework_->TargetUrlForCdpTarget(target_id->c_str()));
    if (host_now != page_host) {
      AppendCredentialAudit(grant_key, CredentialAccessKind::kFill,
                            "refused (page moved after approval)");
      out->Set("ok", false);
      out->Set("error", "origin_changed");
      out->Set("pageHost", host_now);
      return true;
    }
    // The secret goes client vault -> framework -> page DOM. FillTaggedField
    // returns false only when the tab is gone.
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

RequestToken PhiCoordinator::RequestCreation(phi_framework::WindowKind kind,
                                             const std::string& profile_id,
                                             bool incognito,
                                             BrowserId source_window,
                                             PendingCreation pending) {
  phi_framework::CreateRequest request;
  request.kind = kind;
  request.profile_id = profile_id.c_str();
  request.incognito = incognito;
  request.source_window = source_window;
  const RequestToken token =
      framework_ ? framework_->RequestBrowser(request) : RequestToken();
  if (!token.valid()) {
    LOG(ERROR) << "[PhiClient] RequestBrowser refused (kind "
               << static_cast<int>(kind) << ", profile '" << profile_id
               << "')";
    if (pending.pending_reply_id) {
      CompletePendingReply(pending.pending_reply_id,
                           "{\"ok\":false,\"error\":\"create_failed\"}");
    }
    return token;
  }
  pending.kind = kind;
  pending.profile_id = profile_id;
  pending.deadline_tick = ::GetTickCount64() + kCreateTimeoutMs;
  LOG(INFO) << "[PhiClient] RequestBrowser token=" << token.value << " kind="
            << static_cast<int>(kind) << " profile='" << profile_id
            << "' slot=" << pending.slot.value << " space='"
            << pending.space_id << "' task='" << pending.task_id << "'";
  registry_.AddPending(token, std::move(pending));
  UpdateSweepTimer();
  return token;
}

void PhiCoordinator::OnBrowserWindowCreated(
    const phi_framework::WindowCreation& creation) {
  using phi_framework::WindowKind;
  const BrowserId browser = creation.window;
  const HWND hwnd = reinterpret_cast<HWND>(creation.hwnd);
  LOG(INFO) << "[PhiClient] OnBrowserWindowCreated windowId=" << browser.value
            << " kind=" << static_cast<int>(creation.kind)
            << " token=" << creation.request_token.value
            << " opener=" << creation.opener_window.value
            << " restoredFrom=" << creation.restored_from
            << " incognito=" << creation.incognito;
  if (phase_ < AppPhase::kRestoring) {
    Transition(AppPhase::kRestoring, "window");
  }
  // The model learns the browser BEFORE anything is built on it: a
  // controller constructed below reads its rows from the model, and the
  // first push for this window is still a task away.
  window_model_.Refresh(framework_, browser);
  // A requested kind is matched to what RequestCreation filed by TOKEN — the
  // whole point of the contract (lane 1 F5): two cold switches in flight
  // can no longer take each other's browser. A token nothing waits for
  // (expired by the sweep, or unknown) has no instructions to follow, so
  // the window is hosted as the user's.
  WindowKind kind = creation.kind;
  std::optional<PendingCreation> pending;
  if (creation.request_token.valid()) {
    pending = registry_.TakePending(creation.request_token);
    if (!pending) {
      LOG(WARNING) << "[PhiClient] window " << browser.value
                   << " carries token " << creation.request_token.value
                   << " that is not pending; hosting as a user window";
      kind = WindowKind::kUser;
    }
    UpdateSweepTimer();
  } else if (kind == WindowKind::kSpace || kind == WindowKind::kAgent ||
             kind == WindowKind::kShadow) {
    LOG(WARNING) << "[PhiClient] window " << browser.value
                 << " announced as requested kind "
                 << static_cast<int>(kind) << " without a token";
    kind = WindowKind::kUser;
  }
  switch (kind) {
    case WindowKind::kKiosk:
      // Ephemeral, single-tab, no Space and no slot.
      registry_.AddKiosk(browser, std::make_unique<PhiKioskWindow>(
                                      browser.value, hwnd, framework_,
                                      &window_model_, this));
      return;
    case WindowKind::kPeek:
    case WindowKind::kReaderPanel:
      if (HostSurfacePanel(creation)) {
        return;
      }
      break;  // No opener to float over: a plain shell beats a stray window.
    case WindowKind::kAgent:
    case WindowKind::kShadow:
      if (pending->profile_id.empty() && creation.profile_id) {
        pending->profile_id = creation.profile_id;  // "" = first profile.
      }
      HostAgentWindow(browser, hwnd, creation.incognito, *pending);
      return;
    case WindowKind::kSpace:
      if (AttachRequestedSpaceBrowser(browser, hwnd, *pending)) {
        return;
      }
      break;  // Its slot closed meanwhile: host it as a shell of its own.
    case WindowKind::kUser:
      break;
  }
  // LAZY RESTORE: a materialized ghost carries the PREVIOUS session's window
  // id, which is how it is matched back to the Space that owned it — this
  // run's session_id() is freshly allocated and never matches what the
  // client persisted.
  if (creation.restored_from > 0 &&
      AttachRestoredGhost(browser, hwnd, BrowserId{creation.restored_from})) {
    return;
  }
  HostUserShell(browser, hwnd, creation.incognito);
}

bool PhiCoordinator::HostSurfacePanel(
    const phi_framework::WindowCreation& creation) {
  const BrowserId browser = creation.window;
  const HWND hwnd = reinterpret_cast<HWND>(creation.hwnd);
  // The opener is named by BROWSER id; the controller is filed under its
  // slot, and the two only coincide until that slot switches Space.
  PhiWindowController* opener =
      registry_.ControllerForBrowser(creation.opener_window);
  if (!opener) {
    LOG(WARNING) << "[PhiClient] surface " << browser.value
                 << " without an opener window "
                 << creation.opener_window.value;
    return false;
  }
  if (creation.kind == phi_framework::WindowKind::kReaderPanel) {
    // A Reader View surface: the extension's reading page, moved out of its
    // origin window's strip into a browser of its own so it can be hosted
    // over that origin's live page.
    LOG(INFO) << "[PhiClient] reader panel " << browser.value << " over tab "
              << creation.opener_tab.value << " in window "
              << creation.opener_window.value;
    surfaces_.Attach(std::make_unique<PhiReaderPanel>(
        browser.value, hwnd, creation.opener_window.value,
        creation.opener_tab.value, opener, framework_, &window_model_, this));
    return true;
  }
  // A Peek: floats inside its opener window's content card rather than
  // taking a window of its own. One peek per opener: a second replaces the
  // first (the Mac's presentPeek `.replaced`).
  LOG(INFO) << "[PhiClient] peek " << browser.value << " over tab "
            << creation.opener_tab.value << " in window "
            << creation.opener_window.value;
  CloseSurfacesForOrigin(creation.opener_tab, SidebarSurface::kPeek);
  surfaces_.Attach(std::make_unique<PhiPeekPanel>(
      browser.value, hwnd, creation.opener_window.value,
      creation.opener_tab.value, opener, framework_, &window_model_, this));
  return true;
}

void PhiCoordinator::HostAgentWindow(BrowserId browser,
                                     HWND hwnd,
                                     bool incognito,
                                     const PendingCreation& pending) {
  // An agent Space (or a shadow window, which is the same hidden surface
  // without a presence): host the browser OFF-SCREEN in a slot of its own.
  const SlotId slot = SlotId::OpenedBy(browser);
  // Hosted with the agent chrome whether or not the session is off the
  // record: nobody sees it, and the incognito shell's registry opt-out is
  // the agent path's already. The flag lives on the shadow record.
  PhiWindowController* controller = registry_.Add(
      slot,
      std::make_unique<PhiWindowController>(slot, browser, hwnd, framework_,
                                            this, &window_model_,
                                            /*agent=*/true),
      /*agent=*/true, incognito);
  registry_.Bind(browser, slot);
  // Registered BEFORE the first rebuild, so every host lookup the rebuild
  // makes (OwnerForSlot, PaletteForSlot) already finds the slot (lane 1 D12).
  controller->ScheduleRefresh();
  const uint64_t now_ms = ::GetTickCount64();
  base::DictValue reply;
  reply.Set("ok", true);
  reply.Set("windowId", static_cast<double>(browser.value));
  if (pending.kind == phi_framework::WindowKind::kAgent) {
    PhiAgentSpaces::Space& space = agent_spaces_.Create(
        pending.task_id, browser, pending.profile_id, pending.persistent,
        now_ms);
    space.agent_name = pending.agent_name;
    registry_.InvalidateUserSlots();  // A new agent pip appears in the switcher.
    reply.Set("spaceId", space.space_id);
    reply.Set("ownership", "agent");
  } else {
    ShadowWindow& shadow = shadow_windows_[pending.task_id];
    shadow.task_id = pending.task_id;
    shadow.window_id = browser;
    shadow.profile_id = pending.profile_id;
    shadow.incognito = incognito;
    shadow.created_ms = EpochMsNow();
    TouchShadow(&shadow, 0, now_ms);
    LOG(INFO) << "[PhiClient] shadow window created: " << pending.task_id
              << " window=" << browser.value
              << (incognito ? " (incognito)" : "");
    // Echoed on purpose: a driver that asked for incognito checks this flag
    // and refuses to proceed if it is missing (an older build).
    reply.Set("incognito", incognito);
  }
  UpdateSweepTimer();  // The keep-alive sweep, while anything needs it.
  if (pending.pending_reply_id) {
    std::string json;
    base::JSONWriter::Write(reply, &json);
    CompletePendingReply(pending.pending_reply_id, std::move(json));
  }
}

bool PhiCoordinator::AttachRequestedSpaceBrowser(
    BrowserId browser,
    HWND hwnd,
    const PendingCreation& pending) {
  PhiWindowController* target = registry_.Get(pending.slot);
  if (!target) {
    if (pending.slot.valid()) {
      LOG(WARNING) << "[PhiClient] Space browser " << browser.value
                   << " arrived for slot " << pending.slot.value
                   << ", which is gone";
    }
    return false;
  }
  // A Space browser for an existing slot: attach, don't open a new shell.
  target->AttachSpaceBrowser(pending.space_id, browser, hwnd);
  registry_.Bind(browser, pending.slot);
  // "Open Link in Space" / a routed URL into a cold Space / a re-profiled
  // Space's tabs: they open once the Space's browser exists. Bypassing URL
  // routing — the URL may be the navigation a rule just re-homed HERE, and
  // the table rebuilt below races the open, so exempt it outright.
  for (const std::string& url : pending.open_urls) {
    if (framework_) {
      framework_->OpenTabBypassingSpaceRouting(browser.value, url.c_str());
    }
  }
  if (!target->is_agent()) {
    NoteActiveSpace(target->active_space());
  }
  PushSpaceMenu();  // The Space gained a window id.
  return true;
}

bool PhiCoordinator::AttachRestoredGhost(BrowserId browser,
                                         HWND hwnd,
                                         BrowserId restored_from) {
  // The slot that asked for the materialization when one did
  // (SwitchSlotToSpace); the first user slot for a window the cold start
  // rebuilt on its own.
  std::optional<PendingCreation> pending =
      registry_.TakePendingGhost(restored_from);
  UpdateSweepTimer();
  std::string space_id;
  SlotId slot;
  if (pending) {
    space_id = pending->space_id;
    slot = pending->slot;
  } else if (const std::string* space =
                 ghost_spaces_.SpaceForWindowId(restored_from)) {
    space_id = *space;
    slot = registry_.FirstUserSlot();
  }
  PhiWindowController* target = registry_.Get(slot);
  if (space_id.empty() || !target) {
    return false;
  }
  target->AttachSpaceBrowser(space_id, browser, hwnd);
  registry_.Bind(browser, slot);
  ghost_spaces_.ClearParked(space_id);
  NoteActiveSpace(target->active_space());
  PushSpaceMenu();
  return true;
}

void PhiCoordinator::HostUserShell(BrowserId browser, HWND hwnd,
                                   bool incognito) {
  // Option B: the client owns a shell window and reparents Chromium's
  // content in. The shell routes activation back through the framework. An
  // off-the-record browser (IDC_NEW_INCOGNITO_WINDOW) becomes an INCOGNITO
  // shell: fixed-dark, no Spaces/pins/bookmarks, unrouted.
  const SlotId slot = SlotId::OpenedBy(browser);
  PhiWindowController* controller = registry_.Add(
      slot,
      std::make_unique<PhiWindowController>(slot, browser, hwnd, framework_,
                                            this, &window_model_,
                                            /*agent=*/false, incognito),
      /*agent=*/false, incognito);
  registry_.Bind(browser, slot);
  controller->ScheduleRefresh();  // After registration (lane 1 D12).
  if (incognito) {
    return;
  }
  // A user window exists: the cold-start fallback has nothing to rescue.
  if (cold_start_armed_) {
    cold_start_armed_ = false;
    if (msg_hwnd_) {
      ::KillTimer(msg_hwnd_, kColdStartFallbackTimer);
    }
  }
  NoteActiveSpace(controller->active_space());
  PushSpaceMenu();  // The slot's Space gained a window id.
}

// static
LRESULT CALLBACK PhiCoordinator::MouseHookProc(int code,
                                               WPARAM wparam,
                                               LPARAM lparam) {
  PhiCoordinator* self = g_coordinator;
  if (code == HC_ACTION && self && self->surfaces_.HasKind(SidebarSurface::kPeek) &&
      (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN ||
       wparam == WM_MBUTTONDOWN || wparam == WM_NCLBUTTONDOWN ||
       wparam == WM_NCRBUTTONDOWN || wparam == WM_NCMBUTTONDOWN)) {
    const HWND target = reinterpret_cast<MOUSEHOOKSTRUCT*>(lparam)->hwnd;
    // The click is delivered regardless: dismissing is a side effect of
    // clicking elsewhere, not a swallowed click (the Mac lets the click
    // through too).
    self->DismissPeeksForShell(::GetAncestor(target, GA_ROOT), target);
  }
  return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

bool PhiCoordinator::DismissPeeksForShell(HWND shell, HWND inside) {
  if (!shell) {
    return false;
  }
  std::vector<BrowserId> to_dismiss;
  surfaces_.ForEach([&](PhiSurfacePanel& panel) {
    if (panel.kind() != SidebarSurface::kPeek) {
      return;
    }
    const HWND card = panel.hwnd();
    if (!card || ::GetAncestor(card, GA_ROOT) != shell) {
      return;
    }
    if (inside && (inside == card || ::IsChild(card, inside))) {
      return;  // A click INSIDE the card is the card's own business.
    }
    to_dismiss.push_back(BrowserId{panel.window_id()});
  });
  // Closing the peek's browser is the dismissal (the card's own close chip
  // does the same); OnBrowserWindowClosing then detaches the record — so
  // iterate a snapshot, never the registry.
  for (const BrowserId window : to_dismiss) {
    if (framework_) {
      framework_->CloseBrowserWindow(window.value);
    }
  }
  return !to_dismiss.empty();
}

HWND PhiCoordinator::ShellForFocus() const {
  HWND focus = ::GetFocus();
  if (!focus) {
    return nullptr;
  }
  HWND root = ::GetAncestor(focus, GA_ROOT);
  for (PhiWindowController* controller : registry_.Controllers()) {
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
    // Alt pressed and released with nothing in between summons the caption
    // band's hidden menu titles — the keyboard's way to the bar the pointer
    // reveals by hovering. Any chord (Alt+Tab, Alt+Left, Alt+F4, a mnemonic)
    // cleared alt_solo_ on its key-down, so only a bare tap gets here. The
    // key is NOT swallowed: Chromium still sees the release it expects.
    if (wparam == VK_MENU && self->alt_solo_) {
      self->alt_solo_ = false;
      if (HWND shell = self->ShellForFocus()) {
        ::PostMessageW(shell, kMsgPhiToggleMenuReveal, 0, 0);
      }
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
  }
  // Track whether Alt is being held alone, for the reveal gesture above.
  // A repeat (bit 30) is still a solo hold; any other key-down ends it.
  if (wparam == VK_MENU) {
    if (!(lparam & 0x40000000)) {
      self->alt_solo_ = true;
    }
  } else {
    self->alt_solo_ = false;
  }
  const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool alt = (lparam & 0x20000000) != 0;  // The context code bit.
  // Esc dismisses the Peek card floating over the focused shell (the Mac's
  // window-level Escape), swallowed so the page under it does not also see
  // it. With no card open the key goes to the page as usual.
  if (wparam == VK_ESCAPE && !ctrl && !shift && !alt) {
    if (HWND shell = self->ShellForFocus()) {
      if (self->DismissPeeksForShell(shell, nullptr)) {
        return 1;
      }
    }
  }
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
  for (PhiWindowController* controller : registry_.Controllers()) {
    controller->RefreshFieldExtensions();
  }
}

void PhiCoordinator::OnContentFullscreenChanged(int64_t window_id,
                                                bool fullscreen) {
  const BrowserId browser{window_id};
  if (PhiWindowController* shell = registry_.ControllerForBrowser(browser)) {
    shell->OnContentFullscreenChanged(browser, fullscreen);
  }
}

void PhiCoordinator::OnTargetURLChanged(int64_t window_id, const char* url) {
  const BrowserId browser{window_id};
  if (PhiWindowController* shell = registry_.ControllerForBrowser(browser)) {
    shell->OnTargetURLChanged(browser, base::UTF8ToWide(url ? url : ""));
  }
}

void PhiCoordinator::OnOmniboxResults(int64_t window_id, const char* json) {
  const BrowserId browser{window_id};
  if (PhiWindowController* shell = registry_.ControllerForBrowser(browser)) {
    shell->OnOmniboxResults(browser, json ? json : "");
  }
}

void PhiCoordinator::OnImportCompleted(const char* source, bool success) {
  PhiWindowController* focused = FocusedUserController();
  HWND owner = focused ? focused->shell_hwnd() : nullptr;
  if (!success) {
    PhiNotice(owner, L"Import",
              L"The import did not finish. Close Google Chrome and try "
              L"again.");
    return;
  }
  // History/cookies/passwords landed in the profile directly; bookmarks
  // landed in the CHROMIUM model, which the Phi sidebar never shows — copy
  // them into the client store under one folder (the Mac's
  // saveChromiumBookmarksToLocalStore step).
  int copied = 0;
  std::string profile_id, space_id;
  if (focused && framework_ &&
      OwnerForSlot(focused->slot(), &profile_id, &space_id)) {
    std::optional<base::Value> tree = base::JSONReader::Read(
        framework_->AllBookmarksJson(ResolveBrowser(focused->slot()).value),
        base::JSON_PARSE_RFC);
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
  registry_.InvalidateAll();
  PhiSettingsWindow::Refresh();
  std::wstring message = L"Import finished.";
  if (copied > 0) {
    message += L" " + std::to_wstring(copied) +
               L" bookmarks were added to \"Imported from Chrome\".";
  }
  PhiNotice(owner, L"Import", message.c_str());
}

void PhiCoordinator::OnPlaceholderModeChanged(int64_t window_id,
                                              bool entered) {
  // A reader panel holds exactly the reading surface. Losing it means the
  // reader is over — but this fork keeps a tabless browser ALIVE in
  // placeholder mode, so the panel would otherwise stay parented over the
  // origin's page showing a new-tab page instead of revealing the article.
  // Retire it. Deferred: this arrives from inside the tab removal that
  // caused it, and closing a browser from there trips
  // `!observers_.is_notifying_observers()`.
  const BrowserId browser{window_id};
  if (entered && surfaces_.Find(browser) && msg_hwnd_) {
    ::PostMessageW(msg_hwnd_, WM_PHI_READER_RETIRE,
                   static_cast<WPARAM>(window_id), 0);
    return;
  }
  if (PhiWindowController* shell = registry_.ControllerForBrowser(browser)) {
    shell->OnPlaceholderModeChanged(browser, entered);
  }
}

void PhiCoordinator::OnRefreshNewTab(int64_t window_id) {
  // A Space URL rule re-homed a navigation that began on this window's new
  // tab: the tab is a new tab again, so its native page goes back to its
  // clean state (the Mac's resetToInitialState).
  if (PhiWindowController* shell =
          registry_.ControllerForBrowser(BrowserId{window_id})) {
    shell->ResetNativeNtp();
  }
}

void PhiCoordinator::OnWindowChanged(
    const phi_framework::WindowSnapshot& snapshot,
    phi_framework::WindowChangeReason reason) {
  const BrowserId browser = snapshot.window;
  // Stored first: everything below, and every controller rebuild this
  // posts, reads the model rather than the framework.
  window_model_.Update(snapshot);
  if (PhiKioskWindow* kiosk = registry_.Kiosk(browser)) {
    kiosk->RefreshState();
    return;
  }
  if (PhiSurfacePanel* panel = surfaces_.Find(browser)) {
    // A surface's own browser: the only thing that can change in it is the
    // page (a reader's only page change that matters is leaving reader.html).
    panel->RefreshState();
    return;
  }
  if (reason != phi_framework::WindowChangeReason::kFaviconReady) {
    // The surfaces over this window settle now (re-fit, show only over the
    // active tab, close when the origin left the strip) and stale pin tags
    // are folded: rule 1 already puts us after commit, on a fresh task,
    // once per window per flush, so there is nothing to defer for. A
    // favicon landing moves no rows and needs neither.
    ApplyLegacyTagAliases(browser);
    UpdateReaderPanelsForWindow(browser);
  }
  if (PhiWindowController* shell = registry_.ControllerForBrowser(browser)) {
    // Coalesced: the sidebar repaints once per turn.
    shell->ScheduleRefresh();
    // The shell switches Spaces on its own (ShowSpace) without telling the
    // owner; strip activity in the browser on screen is the next best
    // signal for which Space the user is in.
    if (!shell->is_agent() && !shell->is_incognito() &&
        shell->active_browser_id() == browser) {
      NoteActiveSpace(shell->active_space());
      // A user shell is up and showing content: whatever restore path ran
      // (an unfiltered replay, a fresh window, an armed replay whose eager
      // window arrived and disarmed the fallback) has nothing left to
      // report, and no receipt or timer will settle it.
      if (phase_ < AppPhase::kSettled) {
        SettleRestore("window");
      }
    }
  }
}

void PhiCoordinator::OnBrowserWindowClosing(int64_t window_id) {
  LOG(INFO) << "[PhiClient] OnBrowserWindowClosing windowId=" << window_id;
  const BrowserId browser{window_id};
  window_model_.Remove(browser);
  RetireClosingBrowser(browser);
  MaybeEnterClosing();
}

void PhiCoordinator::MaybeEnterClosing() {
  if (phase_ >= AppPhase::kClosing || !window_model_.Browsers().empty() ||
      registry_.HasPending()) {
    return;
  }
  // No browser of any kind is left and none is on its way: Chromium exits
  // once its last Browser goes, and no WM_ENDSESSION accompanies that, so
  // flush here or the coalesced saves die with the timer. (A keep-alive —
  // a download prompt, a background extension — can keep the process up
  // past this point; the flush is then early but never wrong.)
  Transition(AppPhase::kClosing, "lastwindow");
  FlushForShutdown();
}

void PhiCoordinator::RetireClosingBrowser(BrowserId browser) {
  if (registry_.RemoveKiosk(browser)) {
    return;  // A Kiosk owns nothing else — no slot, no Space, no bookkeeping.
  }
  // A surface's own browser closing (its close chip, Esc, the reader's
  // surface tab removed, click-outside) is the whole teardown. Hand the
  // strip back to the origin, the way the Mac reveals the page the overlay
  // was covering, and repaint the row that carried the indicator.
  if (const std::optional<PhiSurfaceRegistry::Origin> gone =
          surfaces_.Detach(browser)) {
    if (framework_) {
      framework_->ActivateTabById(gone->origin_window_id.value,
                                  gone->origin_tab_id.value);
    }
    // Repaint by the origin WINDOW: the origin tab may be hidden in a pin or
    // bookmark cell, which no visible-row lookup would find — and when it
    // was already active, re-activating it raises no tab notification.
    if (PhiWindowController* origin =
            registry_.ControllerForBrowser(gone->origin_window_id)) {
      origin->ScheduleRefresh();
    } else if (PhiWindowController* shown =
                   WindowShowingTab(gone->origin_tab_id)) {
      shown->ScheduleRefresh();
    }
    return;
  }
  // An origin window is going: every surface over it goes with it. They are
  // browsers of their own, so nothing else would close them — and a
  // surviving panel browser keeps the whole process alive after the last
  // window is gone (the Mac's teardown*ForWindowClose, for BOTH kinds).
  std::vector<BrowserId> orphaned;
  for (PhiSurfacePanel* panel : surfaces_.OverWindow(browser)) {
    orphaned.push_back(BrowserId{panel->window_id()});
  }
  for (const BrowserId panel_id : orphaned) {
    surfaces_.Detach(panel_id);
    if (framework_) {
      framework_->CloseBrowserWindow(panel_id.value);
    }
  }
  // A shadow window closing (shadow.close, the keep-alive sweep, a crash)
  // drops its record; it had no presence to take down. An agent Space's
  // hidden window closing (via complete(), the user closing the surfaced
  // window, or a crash) drops its record — the skill learns the task ended
  // from its absence in the list. Both live in a slot of their own.
  const SlotId agent_slot = SlotId::OpenedBy(browser);
  bool hidden_window = false;
  if (ShadowWindow* shadow = FindShadowByWindow(browser)) {
    const std::string task_id = shadow->task_id;  // Copied: erase frees it.
    shadow_windows_.erase(task_id);
    hidden_window = true;
  } else if (PhiAgentSpaces::Space* space = agent_spaces_.FindByWindow(browser)) {
    const std::string task_id = space->task_id;
    agent_spaces_.Remove(task_id);
    PhiTranscriptPanel::CloseFor(task_id);
    if (watched_agent_task_ == task_id) {
      watched_agent_task_.clear();
    }
    hidden_window = true;
  }
  if (hidden_window) {
    // The watch overlay may be bound to this shell: unhook it BEFORE the
    // controller (and its HWND) goes, or it keeps a dangling binding.
    if (PhiWindowController* shell = registry_.Get(agent_slot)) {
      PhiAgentOverlay::HideIfBoundTo(shell);
    }
    registry_.Remove(agent_slot);  // Drops the browser's binding with it.
    UpdateSweepTimer();
    registry_.InvalidateUserSlots();
    return;
  }
  const std::optional<SlotId> slot = registry_.SlotFor(browser);
  if (!slot) {
    return;  // A browser no slot ever hosted (an unrequested stray).
  }
  registry_.Unbind(browser);
  PhiWindowController* shell = registry_.Get(*slot);
  if (!shell) {
    return;
  }
  // The Space on screen as the window goes is what the next cold start
  // should rebuild first — noted before the controller can vanish.
  if (!shell->is_incognito()) {
    NoteActiveSpace(shell->active_space());
  }
  // The slot survives while any Space browser remains.
  if (!shell->ForgetBrowser(browser)) {
    registry_.Remove(*slot);
  }
  PushSpaceMenu();  // A Space lost its window id.
}

bool PhiCoordinator::SwitchSlotToSpace(
    SlotId slot,
    const std::string& space_id,
    bool* pending,
    bool* cold,
    const std::vector<std::string>* reopen_urls) {
  PhiWindowController* shell = ControllerForSlot(slot);
  auto space_it =
      std::find_if(spaces_model_.spaces.begin(), spaces_model_.spaces.end(),
                   [&](const Space& s) { return s.id == space_id; });
  if (!shell || space_it == spaces_model_.spaces.end()) {
    return false;
  }
  if (shell->HasSpace(space_id)) {
    const bool ok = shell->ShowSpace(space_id);
    if (ok) {
      if (!shell->is_agent()) {
        NoteActiveSpace(space_id);
      }
      PushSpaceMenu();  // The visible-Space routing map changed.
    }
    return ok;
  }
  if (!framework_) {
    return false;
  }
  // What OnBrowserWindowCreated does with the browser when it arrives.
  PendingCreation creation;
  creation.slot = shell->slot();
  creation.space_id = space_id;
  if (reopen_urls) {
    creation.open_urls = *reopen_urls;
  }
  if (cold) {
    *cold = true;
  }
  if (pending) {
    *pending = true;
  }
  // Already asked for: a second switch before the browser arrives must not
  // ask for a second browser (the doubled-Space bug of the single-flight
  // globals). Anything new to open rides on the request in flight.
  if (PendingCreation* in_flight =
          registry_.FindPendingSpace(creation.slot, space_id)) {
    in_flight->open_urls.insert(in_flight->open_urls.end(),
                                creation.open_urls.begin(),
                                creation.open_urls.end());
    return true;
  }
  // LAZY RESTORE: this Space may exist only as a PARKED window from the
  // previous session. Rebuild that window rather than spawning a fresh one —
  // the session file still describes the parked one, so a fresh window would
  // stand beside it as a doubled Space. A materialized window is announced
  // with the PREVIOUS session's id rather than a token, so that id keys the
  // pending record — filed BEFORE the call, which may announce inside it.
  if (const PhiGhostSpaces::Remembered* ghost = ghost_spaces_.Parked(space_id)) {
    const std::string profile = ghost->profile;
    const BrowserId window_id = ghost->window_id;
    creation.profile_id = profile;
    creation.deadline_tick = ::GetTickCount64() + kCreateTimeoutMs;
    registry_.AddPendingGhost(window_id, creation);
    UpdateSweepTimer();
    const int outcome = framework_->MaterializeGhostWindow(
        profile.c_str(), static_cast<int>(window_id.value));
    switch (outcome) {
      case 0:  // Materialized: the window arrives via OnBrowserWindowCreated.
        ghost_spaces_.ClearParked(space_id);
        return true;
      case 2:
        // No such ghost — nothing will ever satisfy this. DROP the record,
        // or the Space stays pinned on a materialization that cannot
        // succeed, and fall through to spawning it fresh.
        registry_.TakePendingGhost(window_id);
        ghost_spaces_.ClearParked(space_id);
        break;
      default:
        // Refused FOR NOW (profile still loading, a replay in flight). KEEP
        // the record so the next switch retries; spawning instead would
        // double the Space.
        registry_.TakePendingGhost(window_id);
        UpdateSweepTimer();
        if (pending) {
          *pending = false;
        }
        return false;
    }
  }
  // First visit: ask for this Space's browser IN ITS PROFILE; the request
  // attaches it to the slot when it arrives (a cold profile loads first).
  const RequestToken token = RequestCreation(
      phi_framework::WindowKind::kSpace, space_it->profile_id,
      /*incognito=*/false, BrowserId(), std::move(creation));
  if (!token.valid() && pending) {
    *pending = false;
  }
  return token.valid();
}

BrowserId PhiCoordinator::BrowserForSpace(const std::string& space_id) const {
  if (space_id.empty()) {
    return BrowserId();
  }
  for (PhiWindowController* controller : registry_.Controllers()) {
    const BrowserId browser = controller->BrowserIdForSpace(space_id);
    if (browser.valid()) {
      return browser;
    }
  }
  return BrowserId();
}

BrowserId PhiCoordinator::ResolveScopeBrowser(const base::DictValue* payload,
                                              std::string* error) {
  if (const std::string* task =
          payload ? payload->FindString("taskId") : nullptr) {
    if (PhiAgentSpaces::Space* space = agent_spaces_.Find(*task)) {
      return ResolveBrowser(space->window_id.value);
    }
    // A shadow window is a task too (the skill's kind:'shadow' context), so
    // the tab-management verbs reach its window under the same scope.
    auto shadow = shadow_windows_.find(*task);
    if (shadow != shadow_windows_.end() &&
        registry_.IsBound(shadow->second.window_id)) {
      return ResolveBrowser(shadow->second.window_id.value);
    }
    *error = "unknown_task";
    return BrowserId();
  }
  if (const std::string* space_id =
          payload ? payload->FindString("spaceId") : nullptr) {
    const BrowserId browser = BrowserForSpace(*space_id);
    if (!browser.valid()) {
      *error = "space_not_open";
    }
    return browser;
  }
  *error = "invalid_payload";
  return BrowserId();
}

bool PhiCoordinator::IsUserSpaceRequest(const std::string& type,
                                        const base::DictValue* payload) {
  // The verb families that read or change the user's OWN state. Not here,
  // and so always allowed: the agent-Space lifecycle (create/ping/complete/
  // ownership/…), shadow.*, spaces.list, profiles.list/listProfiles,
  // downloads.*, lazyRestore.*, credentials.* (their own consent), the
  // dialog and reader verbs, and the diagnostics (telemetry.status).
  static constexpr std::string_view kUserSpaceFamilies[] = {
      "agentSpace.spaces.",   "agentSpace.urlRules.", "agentSpace.pinnedTabs.",
      "agentSpace.pinned.",   "agentSpace.bookmarks.", "agentSpace.tabGroups.",
      "agentSpace.splitView.", "agentSpace.tabs.",    "agentSpace.nav.",
      "agentSpace.groups.",   "agentSpace.rows.",     "agentSpace.layout.",
      "agentSpace.theme.",    "agentSpace.settings.", "agentSpace.window.",
      "agentSpace.menu.",     "agentSpace.emoji.",    "agentSpace.feedback.",
  };
  static constexpr std::string_view kUserSpaceVerbs[] = {
      "agentSpace.navigate",        "agentSpace.close",
      "agentSpace.profiles.create", "agentSpace.profiles.rename",
      "agentSpace.profiles.delete",
  };
  if (type == "agentSpace.spaces.list") {
    return false;  // Read-only, and the skill lists at every bind.
  }
  bool user_verb = false;
  for (std::string_view family : kUserSpaceFamilies) {
    if (type.compare(0, family.size(), family) == 0) {
      user_verb = true;
      break;
    }
  }
  for (std::string_view verb : kUserSpaceVerbs) {
    if (type == verb) {
      user_verb = true;
      break;
    }
  }
  if (!user_verb) {
    return false;
  }
  // Scope, in the handlers' own precedence (ResolveScopeBrowser: taskId,
  // then the window/Space): a task — an agent Space or a shadow — is agent
  // territory whatever the verb; an unknown task fails as unknown_task in
  // the handler, which is the better answer than a gate error.
  if (payload && payload->FindString("taskId")) {
    return false;
  }
  // A windowId naming an agent/shadow window (its slot, or a browser it
  // hosts) stays allowed. Anything else — a user slot, a Space id, or no
  // scope at all (the handlers default to the FIRST window, a user's) — is
  // the user's.
  const int64_t window_id =
      payload ? static_cast<int64_t>(payload->FindDouble("windowId")
                                         .value_or(0))
              : 0;
  if (window_id) {
    const BrowserId browser{window_id};
    PhiWindowController* shell = registry_.ControllerForBrowser(browser);
    if (!shell) {
      shell = registry_.Resolve(window_id);
    }
    if (shell && shell->is_agent()) {
      return false;
    }
    if (FindShadowByWindow(browser)) {
      return false;
    }
  }
  return true;
}

PhiWindowController* PhiCoordinator::FocusedUserController() {
  const HWND foreground = ::GetForegroundWindow();
  PhiWindowController* first_user = nullptr;
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (controller->is_agent()) {
      continue;
    }
    if (foreground && controller->shell_hwnd() == foreground) {
      return controller;
    }
    if (!first_user) {
      first_user = controller;
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
    for (PhiWindowController* controller : registry_.Controllers()) {
      if (controller->is_agent()) {
        continue;
      }
      std::string p, s;
      if (OwnerForSlot(controller->slot(), &p, &s) && p == *profile_id) {
        *space_id = s;
        return true;
      }
    }
    for (const Space& space : spaces_model_.spaces) {
      if (space.profile_id == *profile_id) {
        *space_id = space.id;
        return true;
      }
    }
    space_id->clear();
    return true;
  }
  PhiWindowController* focused = FocusedUserController();
  return focused && OwnerForSlot(focused->slot(), profile_id, space_id);
}

bool PhiCoordinator::HandleSkillDialectVerb(const std::string& type,
                                            const base::DictValue* payload,
                                            base::DictValue* out) {
  auto fail = [&](const std::string& code) {
    out->Set("ok", false);
    out->Set("error", code);
    return true;
  };
  // The active tab of `browser` as the skill's {tabId, url, title} row.
  auto active_row = [&](BrowserId browser) {
    base::DictValue row;
    if (const phi_framework::TabSnapshot* tab =
            window_model_.ActiveTab(browser)) {
      row.Set("tabId", tab->id.value);
      row.Set("url", tab->url);
      row.Set("title", tab->title);
    }
    return row;
  };
  auto refresh_all = [&]() { registry_.InvalidateAll(); };

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

  // PhiAgentSpace.getJavaScriptDialogState / handleJavaScriptDialog: the
  // Mac serves these inside its DevTools server; cdp.mjs re-routes them to
  // this channel on Windows (the relay is a byte pipe), same result shape.
  if (type == "agentSpace.getJavaScriptDialogState" ||
      type == "agentSpace.handleJavaScriptDialog") {
    const std::string* target =
        payload ? payload->FindString("targetId") : nullptr;
    if (!target || target->empty() || !framework_) {
      return fail("invalid_payload");
    }
    if (type == "agentSpace.getJavaScriptDialogState") {
      std::optional<base::DictValue> state = base::JSONReader::ReadDict(
          framework_->JavaScriptDialogStateJson(target->c_str()),
          base::JSON_PARSE_RFC);
      if (!state || !state->FindBool("found").value_or(false)) {
        return fail("No target with given id found");
      }
      out->Set("open", state->FindBool("open").value_or(false));
      if (const std::string* dialog_type = state->FindString("type")) {
        out->Set("type", *dialog_type);
      }
      if (const std::string* message = state->FindString("message")) {
        out->Set("message", *message);
      }
      out->Set("ok", true);
      return true;
    }
    const bool accept = payload->FindBool("accept").value_or(false);
    const std::string* prompt_text = payload->FindString("promptText");
    bool handled = false;
    if (!framework_->HandleJavaScriptDialog(
            target->c_str(), accept,
            prompt_text ? prompt_text->c_str() : nullptr, &handled)) {
      return fail("No target with given id found");
    }
    out->Set("handled", handled);
    out->Set("ok", true);
    return true;
  }

  // --- Tab groups, the skill dialect (scope + stable tab ids) --------------
  if (type.rfind("agentSpace.tabGroups.", 0) == 0) {
    const std::string op = type.substr(21);
    std::string error;
    const BrowserId browser = ResolveScopeBrowser(payload, &error);
    if (!browser.valid() || !framework_) {
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
      if (const phi_framework::WindowSnapshot* window =
              window_model_.Get(browser)) {
        for (const phi_framework::GroupSnapshot& g : window->groups) {
          base::DictValue row;
          row.Set("token", g.id);
          row.Set("title", g.title);
          row.Set("color", GroupColorName(g.color));
          row.Set("collapsed", g.collapsed);
          base::ListValue member_ids;
          for (const phi_framework::TabSnapshot& tab : window->tabs) {
            if (tab.group_id == g.id) {
              member_ids.Append(tab.id.value);
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
          framework_->CreateTabGroupForTabs(browser.value, tab_ids_json.c_str());
      if (created.empty()) {
        return fail("create_failed");
      }
      if (const std::string* title =
              payload ? payload->FindString("title") : nullptr) {
        framework_->RenameTabGroup(browser.value, created.c_str(), title->c_str());
      }
      if (const std::string* color =
              payload ? payload->FindString("color") : nullptr) {
        framework_->SetTabGroupColor(browser.value, created.c_str(),
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
        ok &= framework_->RenameTabGroup(browser.value, token->c_str(),
                                         title->c_str());
      }
      if (const std::string* color =
              payload ? payload->FindString("color") : nullptr) {
        ok &= framework_->SetTabGroupColor(browser.value, token->c_str(),
                                           color->c_str());
      }
      if (std::optional<bool> collapsed =
              payload ? payload->FindBool("collapsed") : std::nullopt) {
        ok &= framework_->SetTabGroupCollapsed(browser.value, token->c_str(),
                                               *collapsed);
      }
      out->Set("ok", ok);
      return true;
    }
    if (op == "addTabs") {
      if (!token) {
        return fail("invalid_payload");
      }
      out->Set("ok", framework_->AddTabsToGroup(browser.value, token->c_str(),
                                                tab_ids_json.c_str()));
      return true;
    }
    if (op == "removeTabs") {
      out->Set("ok", framework_->RemoveTabsFromGroupByIds(
                         browser.value, tab_ids_json.c_str()));
      return true;
    }
    if (op == "ungroup" || op == "close") {
      if (!token) {
        return fail("invalid_payload");
      }
      out->Set("ok", op == "ungroup"
                         ? framework_->UngroupTabGroup(browser.value,
                                                       token->c_str())
                         : framework_->CloseTabGroup(browser.value,
                                                     token->c_str()));
      return true;
    }
    return fail("invalid_payload");
  }

  // --- Lazy Space restore, phase 2 (harness surface) ------------------------
  // Deliberately a thin passthrough to the framework: the park/materialize
  // state machine is phase 3, and these exist so the receipt and the three
  // materialize outcomes can be exercised BEFORE any client state depends on
  // them.
  if (type.rfind("agentSpace.lazyRestore.", 0) == 0) {
    const std::string op = type.substr(23);
    if (!framework_) {
      return fail("no_framework");
    }
    if (op == "ghosts") {
      std::optional<base::Value> parked = base::JSONReader::Read(
          framework_->GhostWindowsJson(), base::JSON_PARSE_RFC);
      if (parked) {
        out->Set("parked", std::move(*parked));
      }
      out->Set("receipt", last_restore_receipt_);
      out->Set("ok", true);
      return true;
    }
    if (op == "restore") {
      const std::string* preferred =
          payload ? payload->FindString("profile") : nullptr;
      const bool armed =
          payload ? payload->FindBool("armed").value_or(false) : false;
      std::vector<int> eager;
      if (const base::ListValue* ids =
              payload ? payload->FindList("eagerWindowIds") : nullptr) {
        for (const base::Value& v : *ids) {
          if (v.is_int()) {
            eager.push_back(v.GetInt());
          }
        }
      }
      last_restore_receipt_.clear();
      framework_->RestorePreviousSession(
          preferred ? preferred->c_str() : "", armed,
          eager.empty() ? nullptr : eager.data(),
          static_cast<int>(eager.size()));
      out->Set("ok", true);
      return true;
    }
    if (op == "materialize" || op == "drop") {
      const std::string* profile =
          payload ? payload->FindString("profile") : nullptr;
      const std::optional<int> window_id =
          payload ? payload->FindInt("windowId") : std::nullopt;
      if (!profile || !window_id) {
        return fail("invalid_payload");
      }
      if (op == "drop") {
        out->Set("dropped",
                 framework_->DropGhostWindow(profile->c_str(), *window_id));
        out->Set("ok", true);
        return true;
      }
      const int outcome =
          framework_->MaterializeGhostWindow(profile->c_str(), *window_id);
      // Reported by NAME: "refusedForNow" and "noSuchGhost" mean opposite
      // things to a caller holding a park record (retry vs give up), and a
      // bare number invites collapsing them.
      out->Set("outcome", outcome == 0   ? "materialized"
                          : outcome == 1 ? "refusedForNow"
                                         : "noSuchGhost");
      out->Set("ok", outcome == 0);
      return true;
    }
    return fail("invalid_payload");
  }

  // --- Split view, the skill dialect ----------------------------------------
  if (type.rfind("agentSpace.splitView.", 0) == 0) {
    const std::string op = type.substr(21);
    std::string error;
    const BrowserId browser = ResolveScopeBrowser(payload, &error);
    if (!browser.valid() || !framework_) {
      return fail(error.empty() ? "space_not_open" : error);
    }
    const std::string* split_id = payload ? payload->FindString("splitId")
                                          : nullptr;
    if (op == "list") {
      base::ListValue splits;
      if (const phi_framework::WindowSnapshot* window =
              window_model_.Get(browser)) {
        for (const phi_framework::SplitSnapshot& split : window->splits) {
          base::DictValue row;
          row.Set("splitId", split.id);
          row.Set("layout",
                  split.layout == phi_framework::SplitLayout::kHorizontal
                      ? "horizontal"
                      : "vertical");
          row.Set("ratio", split.ratio);
          row.Set("primaryTabId", split.primary.value);
          row.Set("secondaryTabId", split.secondary.value);
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
          browser.value, primary, secondary, layout ? layout->c_str() : "");
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
                         browser.value, split_id->c_str(),
                         layout ? layout->c_str() : "", ratio));
      return true;
    }
    if (op == "swap") {
      out->Set("ok", framework_->SwapSplitView(browser.value, split_id->c_str()));
      return true;
    }
    if (op == "remove") {
      out->Set("ok",
               framework_->RemoveSplitById(browser.value, split_id->c_str()));
      return true;
    }
    return fail("invalid_payload");
  }

  // --- Downloads (observe and control only — never open) --------------------
  if (type.rfind("agentSpace.downloads.", 0) == 0) {
    const std::string op = type.substr(21);
    std::string error;
    const BrowserId browser = ResolveScopeBrowser(payload, &error);
    if (!browser.valid() || !framework_) {
      return fail(error.empty() ? "space_not_open" : error);
    }
    if (op == "list") {
      std::optional<base::Value> parsed = base::JSONReader::Read(
          framework_->DownloadsJsonForAgent(browser.value),
          base::JSON_PARSE_RFC);
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
          framework_->DownloadsJsonForAgent(browser.value),
          base::JSON_PARSE_RFC);
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
      if (!framework_->DownloadActionByGuid(browser.value, guid->c_str(),
                                            op.c_str())) {
        return fail("unknown_download");
      }
      out->Set("ok", true);
      return true;
    }
    return fail("invalid_payload");
  }

  // --- User-Space tabs and focus (the skill's spaces.* family) --------------
  // The user-Space window a `spaceId` / pinned `windowId` payload names,
  // as the browser id; `space_out` receives the Space (derived from the
  // window when only that is given). A pinned window that does not show the
  // Space is window_not_open — never a cue to fall back elsewhere, the
  // skill's contract for a `{window}` binding.
  const auto resolve_user_window = [&](std::string* space_out,
                                       std::string* error) -> BrowserId {
    const std::string* space_id =
        payload ? payload->FindString("spaceId") : nullptr;
    const BrowserId pinned{
        payload ? static_cast<int64_t>(
                      payload->FindDouble("windowId").value_or(0))
                : 0};
    if (pinned.valid()) {
      for (PhiWindowController* controller : registry_.Controllers()) {
        if (controller->is_agent()) {
          continue;
        }
        const std::string shown = controller->SpaceForBrowser(pinned);
        if (shown.empty()) {
          continue;
        }
        if (space_id && !space_id->empty() && shown != *space_id) {
          break;  // The window exists but shows another Space.
        }
        *space_out = shown;
        return pinned;
      }
      *error = "window_not_open";
      return BrowserId();
    }
    if (!space_id || space_id->empty()) {
      *error = "invalid_payload";
      return BrowserId();
    }
    *space_out = *space_id;
    const BrowserId browser = BrowserForSpace(*space_id);
    if (!browser.valid()) {
      *error = "space_not_open";
    }
    return browser;
  };

  if (type == "agentSpace.spaces.listTabs") {
    std::string space_id, error;
    const BrowserId browser = resolve_user_window(&space_id, &error);
    if (!browser.valid() || !framework_) {
      return fail(error.empty() ? "space_not_open" : error);
    }
    base::ListValue rows;
    if (const phi_framework::WindowSnapshot* window =
            window_model_.Get(browser)) {
      for (const phi_framework::TabSnapshot& tab : window->tabs) {
        base::DictValue row;
        row.Set("tabId", tab.id.value);
        row.Set("index", tab.index);
        row.Set("url", tab.url);
        row.Set("title", tab.title);
        row.Set("active", tab.active);
        // Present only when set: the stored-cell binding tag (TagFor) and
        // the live split, so a probe can assert bindings without pixels.
        if (!tab.tag.empty()) {
          row.Set("tag", tab.tag);
        }
        if (!tab.split_id.empty()) {
          row.Set("splitId", tab.split_id);
        }
        rows.Append(std::move(row));
      }
    }
    out->Set("tabs", std::move(rows));
    out->Set("spaceId", space_id);  // A window-only bind learns its Space.
    out->Set("windowId", static_cast<double>(browser.value));
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.spaces.openTab") {
    const std::string* url = payload ? payload->FindString("url") : nullptr;
    if (!url || url->empty()) {
      return fail("invalid_payload");
    }
    std::string space_id, error;
    const BrowserId browser = resolve_user_window(&space_id, &error);
    if (!browser.valid() || !framework_) {
      return fail(error.empty() ? "space_not_open" : error);
    }
    const bool activate =
        payload->FindBool("activate").value_or(true);
    const TabId previous_active = activate ? TabId() : ActiveTabId(browser);
    // Bypassed like every client-side deliberate open: a URL rule pointing
    // at another Space must not bounce a tab the skill placed HERE.
    framework_->OpenTabBypassingSpaceRouting(browser.value, url->c_str());
    if (previous_active.valid()) {
      // The new tab opened foreground; hand the selection back by id.
      framework_->ActivateTabById(browser.value, previous_active.value);
    }
    out->Set("windowId", static_cast<double>(browser.value));
    out->Set("ok", true);
    return true;
  }

  if (type == "agentSpace.spaces.focus") {
    // A surfaced agent window in the foreground answers as the agent Space.
    const HWND foreground = ::GetForegroundWindow();
    for (PhiWindowController* controller : registry_.Controllers()) {
      if (!controller->is_agent() || !foreground ||
          controller->shell_hwnd() != foreground) {
        continue;
      }
      // An agent window's slot is the browser it opened with (it never
      // switches Space), so the slot number names its window.
      PhiAgentSpaces::Space* agent =
          agent_spaces_.FindByWindow(BrowserId{controller->slot().value});
      if (!agent) {
        break;
      }
      out->Set("spaceId", agent->space_id);
      out->Set("spaceName", "Agent Space");
      out->Set("isAgentSpace", true);
      out->Set("isIncognito", false);
      out->Set("windowId",
               static_cast<double>(ResolveBrowser(controller->slot()).value));
      out->Set("ok", true);
      return true;
    }
    PhiWindowController* controller = FocusedUserController();
    if (!controller) {
      return fail("no_window");
    }
    const std::string space_id = controller->active_space();
    std::string name = space_id;
    for (const Space& space : spaces_model_.spaces) {
      if (space.id == space_id) {
        name = space.name;
        break;
      }
    }
    out->Set("spaceId", space_id);
    out->Set("spaceName", name);
    out->Set("isAgentSpace", false);
    out->Set("isIncognito", controller->is_incognito());
    const BrowserId browser = controller->active_browser_id();
    if (browser.valid()) {
      out->Set("windowId", static_cast<double>(browser.value));
      if (window_model_.ActiveTab(browser)) {
        out->Set("tab", active_row(browser));
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
    for (PhiWindowController* controller : registry_.Controllers()) {
      if (controller->is_agent() || !controller->HasSpace(*space_id)) {
        continue;
      }
      const bool ok = controller->ShowSpace(*space_id);
      if (ok) {
        PushSpaceMenu();
        if (controller->shell_hwnd()) {
          ::SetForegroundWindow(controller->shell_hwnd());
        }
        controller->ScheduleRefresh();
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
        SwitchSlotToSpace(focused->slot(), *space_id, &pending, &cold);
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
    for (Space& space : spaces_model_.spaces) {
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
        ScheduleSave(StoreDomain::kSpaces);
        refresh_all();
      } else {
        ok = false;
      }
    }
    if (const std::string* color = payload->FindString("colorHex")) {
      entry->color_hex = *color;
      ScheduleSave(StoreDomain::kSpaces);
      // The hex drives the Space's tint when no theme is pinned.
      RebuildSpacePalettes();
      for (PhiWindowController* controller : registry_.Controllers()) {
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
      for (const Space& space : spaces_model_.spaces) {
        if (space.id == *space_id) {
          target = &space;
          break;
        }
      }
      if (!target) {
        return fail("unknown_space");
      }
    } else if (!spaces_model_.spaces.empty()) {
      target = &spaces_model_.spaces.front();
      for (const Space& space : spaces_model_.spaces) {
        if (space.id == DefaultSpaceId()) {  // The default role's holder.
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
        if (const std::string* icon = payload->FindString("icon")) {
          // Folders only, as on Mac; an unknown id resolves to "default".
          ok &= row->type == PhiRowType::kBookmarkFolder &&
                SetStoredRowIcon(*guid, PhiResolveFolderIcon(*icon));
        }
        if (const std::string* layout = payload->FindString("layout")) {
          // Split rows only; the divider axis the pair reopens with.
          ok &= (*layout == "vertical" || *layout == "horizontal") &&
                SetStoredRowLayout(*guid, *layout);
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
