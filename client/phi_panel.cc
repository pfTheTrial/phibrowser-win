// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_panel.h"

#include <algorithm>
#include <cmath>

namespace phi_client {

// ---------------------------------------------------------------------------
// Pure — compiled on the Linux host too (test/model/panel_test.cc).

PhiCardStackModel::PhiCardStackModel() = default;

PhiCardStackModel::~PhiCardStackModel() = default;

void PhiCardStackModel::Add(uintptr_t id) {
  if (!id || Contains(id)) {
    return;
  }
  order_.push_back(id);
}

void PhiCardStackModel::Remove(uintptr_t id) {
  order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
}

void PhiCardStackModel::Raise(uintptr_t id) {
  if (!id) {
    return;
  }
  Remove(id);
  order_.push_back(id);
}

bool PhiCardStackModel::Contains(uintptr_t id) const {
  return std::find(order_.begin(), order_.end(), id) != order_.end();
}

int PhiCardStackModel::IndexOf(uintptr_t id) const {
  for (size_t i = 0; i < order_.size(); ++i) {
    if (order_[i] == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void PhiCardStackModel::Clear() {
  order_.clear();
}

double PhiLayerShapeDistance(const PhiLayerShape& shape, double px, double py) {
  const double w = static_cast<double>(shape.bounds.width());
  const double h = static_cast<double>(shape.bounds.height());
  if (w <= 0.0 || h <= 0.0) {
    return 1e9;
  }
  // Into the shape's own space, then phi_dwrite's RoundRectDistance — kept
  // identical on purpose (see the header): the ramp a shaped layer's edge
  // gets has to be the ramp PhiFillRoundRectAA would have drawn.
  const double x = px - shape.bounds.left;
  const double y = py - shape.bounds.top;
  const double r =
      std::max(0.0, std::min(static_cast<double>(shape.radius),
                             std::min(w, h) / 2.0));
  const double cx = std::min(std::max(x, r), w - r);
  const double cy = std::min(std::max(y, r), h - r);
  const double dx = x - cx;
  const double dy = y - cy;
  if (dx == 0.0 || dy == 0.0) {
    // On an edge's straight run the distance is to the nearest side; the
    // corner-circle form is only right out past a corner.
    if (x >= 0.0 && x <= w && y >= 0.0 && y <= h) {
      return -std::min(std::min(x, w - x), std::min(y, h - y));
    }
    return std::sqrt(dx * dx + dy * dy) - r;
  }
  return std::sqrt(dx * dx + dy * dy) - r;
}

double PhiLayerShapesCoverage(const std::vector<PhiLayerShape>& shapes,
                              double px,
                              double py) {
  double best = 0.0;
  for (const PhiLayerShape& shape : shapes) {
    // FillCoverage: full a half pixel inside the boundary, nothing a half
    // pixel outside it, one half ON it.
    const double coverage =
        std::clamp(0.5 - PhiLayerShapeDistance(shape, px, py), 0.0, 1.0);
    best = std::max(best, coverage);
    if (best >= 1.0) {
      break;
    }
  }
  return best;
}

bool PhiLayerShapesContain(const std::vector<PhiLayerShape>& shapes,
                           int x,
                           int y) {
  // The pixel's CENTRE, so this and the coverage above agree on the same
  // boundary: a pixel is "in" exactly when it is at least half covered.
  return PhiLayerShapesCoverage(shapes, x + 0.5, y + 0.5) >= 0.5;
}

}  // namespace phi_client

#if defined(_WIN32)

#include <map>

#include "base/no_destructor.h"
#include "phibrowser-win/client/phi_dwrite.h"

namespace phi_client {

namespace {

std::map<HWND, PhiCardStack*>& Stacks() {
  static base::NoDestructor<std::map<HWND, PhiCardStack*>> stacks;
  return *stacks;
}

}  // namespace

// ---------------------------------------------------------------------------
// PhiCardStack.

PhiCardStack::PhiCardStack() = default;

PhiCardStack::~PhiCardStack() = default;

// static
PhiCardStack& PhiCardStack::For(HWND parent) {
  std::map<HWND, PhiCardStack*>& stacks = Stacks();
  auto it = stacks.find(parent);
  if (it == stacks.end()) {
    it = stacks.emplace(parent, new PhiCardStack()).first;
  }
  return *it->second;
}

// static
void PhiCardStack::Forget(HWND parent) {
  std::map<HWND, PhiCardStack*>& stacks = Stacks();
  auto it = stacks.find(parent);
  if (it != stacks.end()) {
    delete it->second;
    stacks.erase(it);
  }
}

void PhiCardStack::Add(HWND window) {
  model_.Add(reinterpret_cast<uintptr_t>(window));
}

void PhiCardStack::Remove(HWND window) {
  model_.Remove(reinterpret_cast<uintptr_t>(window));
}

void PhiCardStack::Raise(HWND window) {
  model_.Raise(reinterpret_cast<uintptr_t>(window));
  Restack();
}

void PhiCardStack::Restack() {
  // Bottom to top: each HWND_TOP puts one window above everything, so the
  // LAST one applied ends up on top — which is what the list means.
  std::vector<uintptr_t> dead;
  for (uintptr_t id : model_.order()) {
    HWND window = reinterpret_cast<HWND>(id);
    if (!::IsWindow(window)) {
      dead.push_back(id);
      continue;
    }
    // A hidden panel keeps its place in the list but does not fight for the
    // z-order: it re-asserts itself when it is shown again.
    if (!::IsWindowVisible(window)) {
      continue;
    }
    ::SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  for (uintptr_t id : dead) {
    model_.Remove(id);
  }
}

// ---------------------------------------------------------------------------
// PhiLayerSurface.

PhiLayerSurface::PhiLayerSurface(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  HDC screen = ::GetDC(nullptr);
  dc_ = ::CreateCompatibleDC(screen);
  if (dc_) {
    bitmap_ =
        ::CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
  }
  if (screen) {
    ::ReleaseDC(nullptr, screen);
  }
  if (!dc_ || !bitmap_ || !bits_) {
    if (bitmap_) {
      ::DeleteObject(bitmap_);
      bitmap_ = nullptr;
    }
    if (dc_) {
      ::DeleteDC(dc_);
      dc_ = nullptr;
    }
    bits_ = nullptr;
    return;
  }
  previous_ = ::SelectObject(dc_, bitmap_);
  width_ = width;
  height_ = height;
}

PhiLayerSurface::~PhiLayerSurface() {
  if (dc_) {
    if (previous_) {
      ::SelectObject(dc_, previous_);
    }
    ::DeleteDC(dc_);
  }
  if (bitmap_) {
    ::DeleteObject(bitmap_);
  }
}

size_t PhiLayerSurface::byte_size() const {
  return static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u;
}

bool PhiLayerSurface::Commit(HWND hwnd, const POINT* screen_origin,
                             BYTE alpha) const {
  if (!valid() || !hwnd || !::IsWindow(hwnd)) {
    return false;
  }
  // GDI+ and GDI both draw through the DC; the bits are not guaranteed to
  // be there until the batch is flushed.
  ::GdiFlush();
  POINT src = {0, 0};
  SIZE size = {width_, height_};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
  POINT origin = screen_origin ? *screen_origin : POINT{0, 0};
  return ::UpdateLayeredWindow(hwnd, nullptr,
                               screen_origin ? &origin : nullptr, &size, dc_,
                               &src, 0, &blend, ULW_ALPHA) != FALSE;
}

// ---------------------------------------------------------------------------
// PhiPanel.

PhiPanel::PhiPanel() = default;

PhiPanel::~PhiPanel() = default;

bool PhiPanel::CreatePanel(const ClassSpec& spec,
                           HWND parent,
                           const RECT& bounds,
                           const Options& options) {
  if (!parent || !::IsWindow(parent)) {
    return false;
  }
  parent_ = parent;
  options_ = options;
  DWORD ex_style = options.ex_style;
  if (options.layered || options.shaped_layer) {
    ex_style |= WS_EX_LAYERED;
  }
  if (options.click_through) {
    ex_style |= WS_EX_TRANSPARENT;
  }
  // WS_CLIPSIBLINGS is the family's, not the caller's: the corner caps are
  // layered siblings ABOVE a panel, and a panel that paints over them
  // squares the content card's rounded corners.
  const DWORD style = WS_CHILD | WS_CLIPSIBLINGS | options.style;
  if (!Create(spec, ex_style, style, bounds, parent)) {
    parent_ = nullptr;
    return false;
  }
  if (options_.in_card_stack) {
    PhiCardStack::For(parent_).Add(hwnd());
  }
  return true;
}

void PhiPanel::SetPanelBounds(const RECT& bounds) {
  if (!alive()) {
    return;
  }
  ::SetWindowPos(hwnd(), nullptr, bounds.left, bounds.top,
                 std::max<LONG>(0, bounds.right - bounds.left),
                 std::max<LONG>(0, bounds.bottom - bounds.top),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void PhiPanel::SetPagePanelBounds(const RECT& bounds, int radius) {
  SetPanelBounds(bounds);
  if (!alive()) {
    return;
  }
  // Extend the other three arcs beyond the panel: only the lower-left
  // corner belongs to the page, below the shell's address row.
  const int width = std::max<LONG>(0, bounds.right - bounds.left);
  const int height = std::max<LONG>(0, bounds.bottom - bounds.top);
  ::SetWindowRgn(hwnd(),
                 bounds.left > 0 && radius > 0
                     ? ::CreateRoundRectRgn(0, -radius * 2,
                                            width + radius * 2, height + 1,
                                            radius * 2, radius * 2)
                     : nullptr,
                 TRUE);
}

void PhiPanel::ShowPanel() {
  if (!alive()) {
    return;
  }
  // SW_SHOWNA: a panel never takes activation from the page.
  ::ShowWindow(hwnd(), SW_SHOWNA);
  RaisePanel();
}

void PhiPanel::HidePanel() {
  if (alive()) {
    ::ShowWindow(hwnd(), SW_HIDE);
  }
}

bool PhiPanel::IsPanelVisible() const {
  return alive() && ::IsWindowVisible(hwnd());
}

void PhiPanel::RaisePanel() {
  if (!alive()) {
    return;
  }
  if (!options_.in_card_stack || !parent_) {
    ::SetWindowPos(hwnd(), HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return;
  }
  PhiCardStack::For(parent_).Raise(hwnd());
}

void PhiPanel::SyncDpiFromParent() {
  if (!alive()) {
    return;
  }
  const UINT next = ::GetDpiForWindow(hwnd());
  if (!next || next == dpi()) {
    return;
  }
  // No suggested rect: a panel is re-fitted by its owner's relayout, never
  // by the DPI message, and the base skips the move when lParam is null.
  ::SendMessageW(hwnd(), WM_DPICHANGED,
                 MAKEWPARAM(static_cast<WORD>(next), static_cast<WORD>(next)),
                 0);
}

void PhiPanel::Paint(HDC, const RECT&) {}

void PhiPanel::LayerShapes(int width,
                           int height,
                           std::vector<PhiLayerShape>* out) const {
  PhiLayerShape whole;
  whole.bounds = {0, 0, width, height};
  whole.radius = 0;
  out->push_back(whole);
}

bool PhiPanel::HitTestShape() const {
  return false;
}

bool PhiPanel::CommitLayer() {
  if (!alive() || !options_.shaped_layer) {
    return false;
  }
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int width = client.right;
  const int height = client.bottom;
  if (width <= 0 || height <= 0) {
    return false;
  }
  std::vector<PhiLayerShape> shapes;
  LayerShapes(width, height, &shapes);
  const RECT dirty = {0, 0, width, height};
  // One rasteriser for every curve the client draws: the layer's alpha
  // comes out of phi_dwrite, never out of a window region.
  return PhiCommitShapedLayer(
      hwnd(), width, height, [&](HDC dc) { Paint(dc, dirty); },
      [&shapes](double px, double py) {
        return PhiLayerShapesCoverage(shapes, px, py);
      });
}

LRESULT PhiPanel::OnMessage(UINT message,
                            WPARAM wparam,
                            LPARAM lparam,
                            bool* handled) {
  if (options_.layered || options_.shaped_layer) {
    switch (message) {
      case WM_PAINT: {
        // A layered window's pixels come from UpdateLayeredWindow, not from
        // the DC — but the invalidate that got us here still has to be
        // consumed or Windows re-posts WM_PAINT forever. Validating and
        // then composing keeps Invalidate() the one spelling a subclass
        // needs, layered or not.
        PAINTSTRUCT ps = {};
        ::BeginPaint(hwnd(), &ps);
        ::EndPaint(hwnd(), &ps);
        if (options_.shaped_layer) {
          CommitLayer();
        } else {
          ComposeLayer();
        }
        *handled = true;
        return 0;
      }
      case WM_NCHITTEST: {
        if (!options_.shaped_layer || !HitTestShape()) {
          break;
        }
        RECT client = {};
        ::GetClientRect(hwnd(), &client);
        std::vector<PhiLayerShape> shapes;
        LayerShapes(client.right, client.bottom, &shapes);
        POINT pt = {static_cast<LONG>(static_cast<short>(LOWORD(lparam))),
                    static_cast<LONG>(static_cast<short>(HIWORD(lparam)))};
        ::ScreenToClient(hwnd(), &pt);
        if (!PhiLayerShapesContain(shapes, static_cast<int>(pt.x),
                                   static_cast<int>(pt.y))) {
          // The air between two chips belongs to whatever is underneath —
          // what the region union used to say by simply not being there.
          *handled = true;
          return HTTRANSPARENT;
        }
        *handled = true;
        return HTCLIENT;
      }
      default:
        break;
    }
  }
  if (message == WM_DPICHANGED_AFTERPARENT) {
    SyncDpiFromParent();
    *handled = true;
    return 0;
  }
  return PhiWindow::OnMessage(message, wparam, lparam, handled);
}

void PhiPanel::OnNcDestroy() {
  if (options_.in_card_stack && parent_) {
    PhiCardStack& stack = PhiCardStack::For(parent_);
    stack.Remove(hwnd());
    if (stack.model().empty()) {
      PhiCardStack::Forget(parent_);  // The last panel over that card left.
    }
  }
  parent_ = nullptr;
  PhiWindow::OnNcDestroy();
}

}  // namespace phi_client

#endif  // defined(_WIN32)
