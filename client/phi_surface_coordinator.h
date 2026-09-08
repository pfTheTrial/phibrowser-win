// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SURFACE_COORDINATOR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SURFACE_COORDINATOR_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_sidebar_model.h"
#include "phibrowser-win/client/phi_surface_registry.h"

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

class PhiShellWindow;
class PhiWindowModel;

// Attached surfaces (docs/architecture-review.md §3.4, lane 1 §1.2 cluster
// 10): the Peek / Reader View panel registry, the Reader View bridge that
// talks to the Mirage extension, the two deferred hops a panel's lifecycle
// needs (expand, retire) and the click-outside dismissal decision.
//
// Invariant (§3.4): a surface is a browser of its own, addressed by
// BrowserId / TabId — this service never holds a controller pointer past a
// call, and every "which window" question goes back through the Host.
class PhiSurfaceCoordinator {
 public:
  // The app's half: which window shows a tab, which slot owns a browser,
  // where a persisted reader style goes, and the two deferred hops (which
  // ride the coordinator's message window) plus the control plane's
  // deferred reply for reader.extract.
  class Host {
   public:
    virtual ~Host() = default;
    // The window whose ON-SCREEN browser holds `tab_id` (by the model, so a
    // tab hidden in a pin or bookmark cell still counts), or null.
    virtual PhiShellWindow* WindowShowingTab(TabId tab_id) = 0;
    // The reader affordance for `tab_id` may have changed — repaint the
    // window showing it, at the next idle.
    virtual void ScheduleRefreshForTab(TabId tab_id) = 0;
    // The slot `browser` belongs to, or an invalid slot.
    virtual SlotId SlotForBrowser(BrowserId browser) const = 0;
    // A reader style axis moved: persist it (the preferences own the file).
    virtual void PersistReaderStyle(const model::ReaderStyle& style) = 0;
    // The verb's deferred reply for reader.extract: its shape, then its
    // answer. Both go through the control dispatcher.
    virtual bool PendingReplyShape(int request_id,
                                   bool* document,
                                   bool* include_html) = 0;
    virtual void CompleteReaderReply(int request_id, std::string json) = 0;
    // The two deferred hops, posted on the coordinator's message window:
    // WM_PHI_READER_EXPAND (a panel's page left reader.html) and
    // WM_PHI_READER_RETIRE (a panel lost its only tab).
    virtual void PostReaderExpand(BrowserId panel) = 0;
    virtual void PostReaderRetire(BrowserId panel) = 0;
  };

  PhiSurfaceCoordinator(const PhiWindowModel& model, Host& host);
  ~PhiSurfaceCoordinator();

  PhiSurfaceCoordinator(const PhiSurfaceCoordinator&) = delete;
  PhiSurfaceCoordinator& operator=(const PhiSurfaceCoordinator&) = delete;

  // The framework arrives with OnFrameworkReady; every operation below is a
  // no-op until it does, exactly as the coordinator's `if (!framework_)`
  // guards were.
  void Init(phi_framework::PhiFramework* framework);

  // The panel registry. Published rather than wrapped: the creation switch
  // (which stays on the coordinator, because it builds the concrete panels)
  // and the sidebar's surface indicator both address it directly — the same
  // documented exception PhiSpaceManager::mutable_model() is.
  PhiSurfaceRegistry& registry() { return surfaces_; }
  const PhiSurfaceRegistry& registry() const { return surfaces_; }
  // Reader View's app half. Holds the live reading style (one store for
  // every reader surface; persisted through the preferences) and the per-tab
  // offer/state the extension reports.
  PhiReaderBridge& reader() { return reader_; }

  // --- The bridge's seams (PhiReaderBridge calls these) --------------------
  // Answers one extension request (PhiFramework::RespondToExtensionMessage).
  void RespondToExtension(const std::string& request_id,
                          const std::string& response_json);
  // App -> extensions. False when the broadcast reached nobody.
  bool BroadcastToExtensions(const std::string& type,
                             const std::string& payload_json);
  PhiShellWindow* WindowShowingTab(TabId tab_id);
  // A reader style axis moved: persist it.
  void OnReaderStyleChanged();
  // Seeds the live style from the loaded settings.
  void SetReaderStyleFromModel(const model::ReaderStyle& style);
  void OnReaderOfferabilityChanged(TabId tab_id);
  // Shapes the framework's article into the verb's reply and answers it.
  void OnReaderArticleExtracted(int request_id, const char* json);

  // --- Reader View panels ---------------------------------------------------
  // The reader extension's surface tab has appeared in the origin's strip:
  // move it into a browser of its own and host that in a panel over the
  // origin's page. False when there is no such tab to move.
  bool PanelizeReaderSurface(TabId origin_tab_id);
  bool HasReaderPanelForOrigin(TabId origin_tab_id) const;
  void CloseReaderPanelForOrigin(TabId origin_tab_id);
  // Closes every surface of `kind` (kContent = all kinds) attached to the
  // tab: the panel browsers close, and OnBrowserWindowClosing forgets them.
  void CloseSurfacesForOrigin(TabId origin_tab_id,
                              SidebarSurface kind = SidebarSurface::kContent);
  // A link inside the reading surface navigated it away from reader.html:
  // hand `url` back to the origin window as a normal tab and drop the panel.
  void ExpandReaderPanelIntoTab(int64_t panel_window_id,
                                const std::string& url);
  // The deferred half of the above (WM_PHI_READER_EXPAND).
  void FinishReaderPanelExpand(BrowserId panel_window);
  // The deferred retire (WM_PHI_READER_RETIRE): a panel that lost its only
  // tab is closed rather than left parked over the origin's page.
  void RetireReaderPanel(BrowserId panel_window);
  // A panel browser entered placeholder mode (it lost its only tab). True
  // when the retire was deferred and the caller must do nothing else.
  bool DeferRetireOnPlaceholder(BrowserId browser, bool entered);
  // A window's tab strip changed: re-fit, re-hide and retire the panels
  // bound to it. Reads the model, so it runs AFTER the snapshot that reports
  // the change has been stored.
  void UpdateReaderPanelsForWindow(BrowserId window_id);

  // --- The sidebar's reader affordance --------------------------------------
  bool IsReaderOfferable(int tab_id, const std::string& url) const;
  bool IsReaderActive(int tab_id, const std::string& url) const;
  void RequestToggleReaderView(int tab_id, const std::string& url);
  // Right-click "Open in Reading Mode": ENTERS rather than toggles.
  void OnOpenReaderView(int64_t window_id, int64_t tab_id);
  // The reader's half of OnExtensionMessage. False leaves the message for
  // the next handler (today: nobody).
  bool HandleExtensionMessage(const std::string& request_id,
                              const std::string& sender_id,
                              const std::string& type,
                              const std::string& payload_json);

  // --- Layout and dismissal -------------------------------------------------
  void RaiseReaderPanels(SlotId slot);
  void NoteContentLayoutChanged(SlotId slot);
  // Whether any Peek card is up (the click-outside hook's cheap guard).
  bool HasPeek() const;
  // Dismisses the Peek card(s) floating over `shell`, skipping any whose
  // card contains `inside` (a click landing in the card itself). Returns
  // true when one was dismissed.
  bool DismissPeeksForShell(HWND shell, HWND inside);

 private:
  RAW_PTR_EXCLUSION const PhiWindowModel& window_model_;
  RAW_PTR_EXCLUSION Host& host_;
  // Non-owning, across a module boundary, so not a raw_ptr. Null until Init.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  // Peek and Reader View panels — browsers of their own, hosted over an
  // origin tab's page, one lifecycle for both kinds.
  PhiSurfaceRegistry surfaces_;
  // Panels whose page left reader.html, with the url to re-open, until the
  // deferred expand runs.
  std::map<BrowserId, std::string> pending_reader_expands_;
  PhiReaderBridge reader_{this};
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SURFACE_COORDINATOR_H_
