// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SURFACE_REGISTRY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SURFACE_REGISTRY_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_sidebar_model.h"

// Attached surfaces — docs/sidebar-architecture.md §4.6. A Peek or a Reader
// View is a browser of its own, hosted in a panel over the content card of
// the window whose tab it is attached to (the ORIGIN). Both kinds share one
// lifecycle, kept here rather than in two parallel maps:
//   origin closes / leaves the window   -> the surface closes
//   origin window closes                -> the surface closes
//   origin's window switches tabs       -> the surface shows only over its tab
//   the card moves                      -> the surface re-fits
//   address bar / Back / Forward on the origin -> the surface closes first
// The sidebar consumes only Surfaces(): origin tab -> kind, for the
// indicator on whichever row owns the origin.

namespace phi_client {

// What every hosted surface panel provides. PhiPeekPanel and PhiReaderPanel
// implement it; the registry and the coordinator never name the concrete
// kind except where a kind-specific gesture demands it.
class PhiSurfacePanel {
 public:
  virtual ~PhiSurfacePanel() = default;

  virtual SidebarSurface kind() const = 0;
  // The panel's own browser window id. Spelt int64_t here because the two
  // panels are implemented outside this model; the registry types them as
  // BrowserId at its own boundary.
  virtual int64_t window_id() const = 0;
  // The browser window and tab the surface is attached to.
  virtual int64_t origin_window_id() const = 0;
  virtual int64_t origin_tab_id() const = 0;
  // The panel's card HWND (what click-outside tests compare against).
  virtual HWND hwnd() const = 0;
  // Whether `window` is the card or something the panel considers part of
  // it — a click there is not a click outside.
  virtual bool ContainsWindow(HWND window) const;
  // Whether a button press at `screen_point`, outside the panel, is the
  // kind that dismisses it. Default: any.
  virtual bool ClickDismisses(POINT screen_point) const;

  // The origin window moved or resized: re-fit to its content card.
  virtual void Reposition() = 0;
  // The origin switched tabs: show only while the origin tab is active.
  virtual void SetVisibleForActiveTab(int64_t active_tab_id) = 0;
  // The hosted page changed.
  virtual void RefreshState() = 0;
  // Above the origin's browser child (the corner caps go above it after).
  virtual void Raise() = 0;
  // Reader only: the surface tab moved in after construction. Default no-op.
  virtual void BindSurfaceTab(int64_t surface_tab_id) {}
  // Peek only: how the peek is about to end, for the usage metric — the
  // closer says why before it closes the browser, and the closing itself
  // is what reports (PhiCoordinator::RetireClosingBrowser). A close nobody
  // announced is the page's own doing. Default no-op.
  virtual void NoteEndReason(const char* reason) {}
};

class PhiSurfaceRegistry {
 public:
  struct Origin {
    SidebarSurface kind = SidebarSurface::kContent;
    BrowserId window_id;  // The panel browser.
    BrowserId origin_window_id;
    TabId origin_tab_id;
  };

  PhiSurfaceRegistry();
  ~PhiSurfaceRegistry();
  PhiSurfaceRegistry(const PhiSurfaceRegistry&) = delete;
  PhiSurfaceRegistry& operator=(const PhiSurfaceRegistry&) = delete;

  // Takes ownership. A second surface of the same kind on the same origin
  // replaces the first in the records (the caller closes the old browser).
  void Attach(std::unique_ptr<PhiSurfacePanel> panel);
  // Forgets the panel whose browser `window_id` is closing; what it was
  // attached to, for the caller to hand the strip back to. Nullopt when
  // `window_id` was no surface.
  std::optional<Origin> Detach(BrowserId window_id);

  PhiSurfacePanel* Find(BrowserId window_id) const;
  // The surface of `kind` (kContent = any) attached to `origin_tab_id`.
  PhiSurfacePanel* ForOrigin(TabId origin_tab_id,
                             SidebarSurface kind = SidebarSurface::kContent) const;
  // Every surface attached to a tab of `origin_window_id`.
  std::vector<PhiSurfacePanel*> OverWindow(BrowserId origin_window_id) const;
  // The sidebar's view: origin tab -> attached kind, for one browser. Keyed
  // by the raw id because it IS the SidebarHost::AttachedSurfaces reply.
  std::map<int, SidebarSurface> Surfaces(BrowserId origin_window_id) const;

  bool empty() const { return panels_.empty(); }
  bool HasKind(SidebarSurface kind) const;
  void ForEach(const std::function<void(PhiSurfacePanel&)>& fn) const;

 private:
  std::map<BrowserId, std::unique_ptr<PhiSurfacePanel>> panels_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SURFACE_REGISTRY_H_
