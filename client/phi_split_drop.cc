// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_split_drop.h"

// GDI+ draws the cards: rounded paths with per-pixel alpha over live page
// content, which plain GDI cannot produce (the agent overlay's sprite layer
// has the same requirement).
#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_gdiplus.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_panel.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

constexpr wchar_t kCardsClass[] = L"PhiSplitDropCards";

// Reference geometry from the Mac's Figma node 5653:25780. The cards scale
// UNIFORMLY inside the page area (one contain-scale for all four), so a wide
// or a tall window cannot stretch a side card back into the top one. Hit
// testing is the edge thirds below and is deliberately independent of this.
constexpr double kHintReferenceWidth = 1290.0;
constexpr double kHintReferenceHeight = 689.0;
constexpr double kSideCardWidth = 160.0;
constexpr double kSideCardHeight = 460.0;
constexpr double kBandCardWidth = 300.0;
constexpr double kBandCardHeight = 120.0;
constexpr double kSideCardInset = 14.0;
constexpr double kBandCardInset = 18.0;
// The card the cursor is resolving to grows (the Mac's activeDropHintScale is
// 1.1; Jixiang asked for half again as large here) and turns OPAQUE — the
// chosen target must read as solid, not as glass over the page.
constexpr double kActiveCardScale = 1.5;
constexpr int kCardCornerRadius = 32;
constexpr int kCardLabelSize = 14;
// The Mac's dashed stroke, in reference points.
constexpr double kCardDashOn = 8.0;
constexpr double kCardDashOff = 10.0;
// Fraction of the page's width / height each edge zone claims.
constexpr double kEdgeZoneFraction = 1.0 / 3.0;
// Once a zone is active it keeps its grip until the pointer leaves it by
// this much, so tremble in a corner cannot alternate two cards.
constexpr int kZoneExitTolerance = 12;
// Chromium's split divider, so the replace cards line up with the real panes.
constexpr int kSplitDividerThickness = 2;

int Width(const RECT& r) {
  return r.right - r.left;
}
int Height(const RECT& r) {
  return r.bottom - r.top;
}

// The four edge-third rects a create-mode drop activates from.
std::array<RECT, 4> EdgeZoneRects(const RECT& area) {
  const int w = static_cast<int>(Width(area) * kEdgeZoneFraction);
  const int h = static_cast<int>(Height(area) * kEdgeZoneFraction);
  return {
      RECT{area.left, area.top, area.left + w, area.bottom},           // left
      RECT{area.right - w, area.top, area.right, area.bottom},         // right
      RECT{area.left, area.top, area.right, area.top + h},             // top
      RECT{area.left, area.bottom - h, area.right, area.bottom},       // bottom
  };
}

size_t ZoneSlot(PhiSplitDropZone zone) {
  switch (zone) {
    case PhiSplitDropZone::kLeft:
      return 0;
    case PhiSplitDropZone::kRight:
      return 1;
    case PhiSplitDropZone::kTop:
      return 2;
    case PhiSplitDropZone::kBottom:
      return 3;
    case PhiSplitDropZone::kNone:
      break;
  }
  return 0;
}

Gdiplus::Color Blend(COLORREF color, double alpha) {
  return Gdiplus::Color(static_cast<BYTE>(std::lround(alpha * 255.0)),
                        GetRValue(color), GetGValue(color), GetBValue(color));
}

// One rounded-rectangle path, the shape every card is drawn from.
void AddRoundedPath(Gdiplus::GraphicsPath* path,
                    const Gdiplus::RectF& r,
                    Gdiplus::REAL radius) {
  const Gdiplus::REAL d = radius * 2;
  if (d <= 0 || r.Width <= d || r.Height <= d) {
    path->AddRectangle(r);
    return;
  }
  path->AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
  path->AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
  path->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
  path->AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
  path->CloseFigure();
}

// The hint cards are a LAYERED, click-through child of the shell over the
// page area: their pixels come from UpdateLayeredWindow (per-pixel alpha
// over live page content), never from a WM_PAINT blit, so the panel runs
// in PhiPanel's `layered` mode and composes in ComposeLayer().
class Cards : public PhiPanel {
 public:
  static Cards* instance;

  explicit Cards(PhiWindowController* controller) : controller_(controller) {
    ClassSpec spec;
    spec.name = kCardsClass;
    spec.cursor = nullptr;
    Options options;
    options.layered = true;
    // The cards are pure output. Hit tests fall through, and the shell
    // keeps the mouse capture the drag runs on.
    options.click_through = true;
    options.ex_style = WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    CreatePanel(spec, controller_->shell_hwnd(), nothing, options);
  }

  ~Cards() override { Destroy(); }

  PhiWindowController* controller() const { return controller_; }

  void Update(const PhiSplitDropHints& hints) {
    const bool same =
        shown_ && hints.area.left == hints_.area.left &&
        hints.area.top == hints_.area.top &&
        hints.area.right == hints_.area.right &&
        hints.area.bottom == hints_.area.bottom &&
        hints.replace == hints_.replace && hints.stacked == hints_.stacked &&
        hints.ratio == hints_.ratio && hints.active == hints_.active;
    hints_ = hints;
    if (same) {
      return;  // Nothing the cards draw has moved.
    }
    SetPanelBounds(hints_.area);
    ShowPanel();
    shown_ = true;
    ComposeLayer();
  }

 protected:
  void ComposeLayer() override { Compose(); }

  // The page area moved under the drag; the cards re-fit to it.
  void OnDpiChanged(UINT) override { Compose(); }

 private:
  // The card rect for each visible zone, in the overlay's own coordinates
  // (the page area's origin is 0,0 here).
  void CardRects(std::array<Gdiplus::RectF, 4>* out,
                 std::array<bool, 4>* visible) const {
    const Gdiplus::REAL w = static_cast<Gdiplus::REAL>(Width(hints_.area));
    const Gdiplus::REAL h = static_cast<Gdiplus::REAL>(Height(hints_.area));
    for (size_t i = 0; i < 4; ++i) {
      (*visible)[i] = false;
      (*out)[i] = Gdiplus::RectF();
    }
    if (w <= 0 || h <= 0) {
      return;
    }
    if (hints_.replace) {
      // The two live panes: the cards ARE the panes, so the user drops on
      // what they can already see.
      RECT area = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
      RECT primary = {}, secondary = {};
      PhiSplitReplacePaneRects(area, hints_.stacked, hints_.ratio, &primary,
                              &secondary);
      const std::array<RECT, 2> panes = {primary, secondary};
      for (size_t i = 0; i < 2; ++i) {
        const int inset = Scaled(static_cast<int>(kSideCardInset));
        (*out)[i] = Gdiplus::RectF(
            static_cast<Gdiplus::REAL>(panes[i].left + inset),
            static_cast<Gdiplus::REAL>(panes[i].top + inset),
            static_cast<Gdiplus::REAL>(
                std::max(0, Width(panes[i]) - inset * 2)),
            static_cast<Gdiplus::REAL>(
                std::max(0, Height(panes[i]) - inset * 2)));
        (*visible)[i] = (*out)[i].Width > 0 && (*out)[i].Height > 0;
      }
      return;
    }
    // Create mode: one contain-scale keeps every card's aspect ratio and the
    // spacing between the four directions.
    const double scale = std::min(w / kHintReferenceWidth,
                                  h / kHintReferenceHeight);
    const auto side = Gdiplus::SizeF(
        static_cast<Gdiplus::REAL>(kSideCardWidth * scale),
        static_cast<Gdiplus::REAL>(kSideCardHeight * scale));
    const auto band = Gdiplus::SizeF(
        static_cast<Gdiplus::REAL>(kBandCardWidth * scale),
        static_cast<Gdiplus::REAL>(kBandCardHeight * scale));
    const auto side_inset = static_cast<Gdiplus::REAL>(kSideCardInset * scale);
    const auto band_inset = static_cast<Gdiplus::REAL>(kBandCardInset * scale);
    (*out)[0] = Gdiplus::RectF(side_inset, (h - side.Height) / 2, side.Width,
                               side.Height);
    (*out)[1] = Gdiplus::RectF(w - side_inset - side.Width,
                               (h - side.Height) / 2, side.Width, side.Height);
    (*out)[2] = Gdiplus::RectF((w - band.Width) / 2, band_inset, band.Width,
                               band.Height);
    (*out)[3] = Gdiplus::RectF((w - band.Width) / 2,
                               h - band_inset - band.Height, band.Width,
                               band.Height);
    for (size_t i = 0; i < 4; ++i) {
      (*visible)[i] = (*out)[i].Width > 0 && (*out)[i].Height > 0;
    }
  }

  const wchar_t* CardLabel(size_t slot) const {
    if (hints_.replace) {
      if (hints_.stacked) {
        return slot == 0 ? PhiStr("browser.splitDropHint.top")
                         : PhiStr("browser.splitDropHint.bottom");
      }
      return slot == 0 ? PhiStr("browser.splitDropHint.leftHalf")
                       : PhiStr("browser.splitDropHint.rightHalf");
    }
    switch (slot) {
      case 0:
        return PhiStr("browser.splitDropHint.leftThird");
      case 1:
        return PhiStr("browser.splitDropHint.rightThird");
      case 2:
        return PhiStr("browser.splitDropHint.topThird");
      default:
        return PhiStr("browser.splitDropHint.bottomThird");
    }
  }

  void Compose() {
    if (!alive() || !PhiEnsureGdiPlus()) {
      return;
    }
    const int width = Width(hints_.area);
    const int height = Height(hints_.area);
    PhiLayerSurface surface(width, height);
    if (!surface.valid()) {
      return;
    }
    {
      Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppPARGB,
                             static_cast<BYTE*>(surface.bits()));
      Gdiplus::Graphics graphics(&bitmap);
      graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
      graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
      std::array<Gdiplus::RectF, 4> rects;
      std::array<bool, 4> visible = {};
      CardRects(&rects, &visible);
      for (size_t i = 0; i < 4; ++i) {
        if (visible[i]) {
          DrawCard(&graphics, rects[i], i,
                   hints_.active != PhiSplitDropZone::kNone &&
                       ZoneSlot(hints_.active) == i);
        }
      }
    }
    surface.Commit(hwnd(), nullptr);
  }

  void DrawCard(Gdiplus::Graphics* graphics,
                const Gdiplus::RectF& base,
                size_t slot,
                bool active) {
    const PhiPalette& palette = controller_->CurrentPalette();
    Gdiplus::RectF rect = base;
    if (active) {
      // Grows around its own centre, so the card the drop will land on reads
      // as the chosen one — then nudged back inside the page area, since at
      // this scale a side card would otherwise run past the edge.
      const Gdiplus::REAL dw =
          rect.Width * static_cast<Gdiplus::REAL>(kActiveCardScale - 1.0);
      const Gdiplus::REAL dh =
          rect.Height * static_cast<Gdiplus::REAL>(kActiveCardScale - 1.0);
      rect = Gdiplus::RectF(rect.X - dw / 2, rect.Y - dh / 2, rect.Width + dw,
                            rect.Height + dh);
      const auto area_w = static_cast<Gdiplus::REAL>(Width(hints_.area));
      const auto area_h = static_cast<Gdiplus::REAL>(Height(hints_.area));
      const Gdiplus::REAL margin = static_cast<Gdiplus::REAL>(Scaled(8));
      rect.Width = std::min(rect.Width, std::max<Gdiplus::REAL>(0, area_w - margin * 2));
      rect.Height = std::min(rect.Height, std::max<Gdiplus::REAL>(0, area_h - margin * 2));
      rect.X = std::max(margin, std::min(rect.X, area_w - margin - rect.Width));
      rect.Y = std::max(margin, std::min(rect.Y, area_h - margin - rect.Height));
    }
    const auto radius = static_cast<Gdiplus::REAL>(
        std::min<double>(Scaled(kCardCornerRadius),
                         std::min(rect.Width, rect.Height) / 2.0));
    Gdiplus::GraphicsPath path;
    AddRoundedPath(&path, rect, radius);
    // The Mac fills the glass card with white at 60%; in dark mode the same
    // wash has to come from the dark side or the label cannot be read. The
    // ACTIVE card is solid: the target the drop will land on hides the page
    // under it instead of letting it bleed through.
    const COLORREF wash = palette.dark ? RGB(0x1C, 0x1C, 0x1E)
                                       : RGB(0xFF, 0xFF, 0xFF);
    Gdiplus::SolidBrush fill(Blend(wash, active ? 1.0 : 0.60));
    graphics->FillPath(&fill, &path);
    if (active) {
      // The Mac's active scrim: a flat ink wash that reads consistently over
      // both the card fill and whatever page is behind it.
      Gdiplus::SolidBrush scrim(
          Blend(palette.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0), 0.06));
      graphics->FillPath(&scrim, &path);
    }
    Gdiplus::Pen pen(Blend(palette.label, 0.35),
                     static_cast<Gdiplus::REAL>(std::max(1, Scaled(1))));
    const Gdiplus::REAL dashes[] = {
        static_cast<Gdiplus::REAL>(kCardDashOn),
        static_cast<Gdiplus::REAL>(kCardDashOff)};
    pen.SetDashPattern(dashes, 2);
    pen.SetDashCap(Gdiplus::DashCapRound);
    graphics->DrawPath(&pen, &path);
    DrawLabel(graphics, rect, CardLabel(slot), palette.label);
  }

  void DrawLabel(Gdiplus::Graphics* graphics,
                 const Gdiplus::RectF& card,
                 const wchar_t* text,
                 COLORREF ink) {
    const auto size = static_cast<Gdiplus::REAL>(Scaled(kCardLabelSize));
    if (card.Height < size * 2 || card.Width < size * 2 || !text) {
      return;  // Too small to say anything legible.
    }
    Gdiplus::FontFamily family(L"Segoe UI");
    if (family.GetLastStatus() != Gdiplus::Ok) {
      return;
    }
    Gdiplus::Font font(&family, size, Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
    const Gdiplus::REAL inset = size;
    const Gdiplus::RectF box(card.X + inset, card.Y + inset,
                             std::max<Gdiplus::REAL>(0, card.Width - inset * 2),
                             std::max<Gdiplus::REAL>(0,
                                                     card.Height - inset * 2));
    Gdiplus::SolidBrush brush(Blend(ink, 1.0));
    graphics->DrawString(text, -1, &font, box, &format, &brush);
  }

  RAW_PTR_EXCLUSION PhiWindowController* controller_ = nullptr;
  bool shown_ = false;
  PhiSplitDropHints hints_;
};

Cards* Cards::instance = nullptr;

}  // namespace

bool PhiSplitZoneIsPrimary(PhiSplitDropZone zone) {
  return zone == PhiSplitDropZone::kLeft || zone == PhiSplitDropZone::kTop;
}

const char* PhiSplitZoneLayout(PhiSplitDropZone zone) {
  return (zone == PhiSplitDropZone::kTop || zone == PhiSplitDropZone::kBottom)
             ? "horizontal"
             : "vertical";
}

PhiSplitDropZone PhiSplitCreateZoneAt(const RECT& area,
                                      POINT pt,
                                      PhiSplitDropZone retained) {
  if (!::PtInRect(&area, pt)) {
    return PhiSplitDropZone::kNone;
  }
  const std::array<RECT, 4> zones = EdgeZoneRects(area);
  if (retained != PhiSplitDropZone::kNone) {
    RECT held = zones[ZoneSlot(retained)];
    ::InflateRect(&held, kZoneExitTolerance, kZoneExitTolerance);
    if (::PtInRect(&held, pt)) {
      return retained;
    }
  }
  // Left and right take the overlapping corners, as on Mac.
  constexpr std::array<PhiSplitDropZone, 4> kOrder = {
      PhiSplitDropZone::kLeft, PhiSplitDropZone::kRight,
      PhiSplitDropZone::kTop, PhiSplitDropZone::kBottom};
  for (size_t i = 0; i < 4; ++i) {
    if (::PtInRect(&zones[i], pt)) {
      return kOrder[i];
    }
  }
  return PhiSplitDropZone::kNone;  // The dead middle band.
}

void PhiSplitReplacePaneRects(const RECT& area,
                              bool stacked,
                              double ratio,
                              RECT* primary,
                              RECT* secondary) {
  const double share = std::min(1.0, std::max(0.0, ratio));
  if (stacked) {
    const int usable = std::max(0, Height(area) - kSplitDividerThickness);
    const int top = static_cast<int>(usable * share);
    *primary = {area.left, area.top, area.right, area.top + top};
    *secondary = {area.left, area.top + top + kSplitDividerThickness,
                  area.right, area.bottom};
    return;
  }
  const int usable = std::max(0, Width(area) - kSplitDividerThickness);
  const int lead = static_cast<int>(usable * share);
  *primary = {area.left, area.top, area.left + lead, area.bottom};
  *secondary = {area.left + lead + kSplitDividerThickness, area.top,
                area.right, area.bottom};
}

PhiSplitDropZone PhiSplitReplaceZoneAt(const RECT& area,
                                       POINT pt,
                                       bool stacked,
                                       double ratio) {
  RECT primary = {}, secondary = {};
  PhiSplitReplacePaneRects(area, stacked, ratio, &primary, &secondary);
  // The divider gap resolves to whichever pane is nearer.
  if (stacked) {
    const int mid = (primary.bottom + secondary.top) / 2;
    return pt.y <= mid ? PhiSplitDropZone::kLeft : PhiSplitDropZone::kRight;
  }
  const int mid = (primary.right + secondary.left) / 2;
  return pt.x <= mid ? PhiSplitDropZone::kLeft : PhiSplitDropZone::kRight;
}

// static
void PhiSplitDropOverlay::Show(PhiWindowController* controller,
                               const PhiSplitDropHints& hints) {
  if (!controller || !controller->shell_hwnd()) {
    return;
  }
  if (Cards::instance && Cards::instance->controller() != controller) {
    Hide();  // The drag moved to another shell window.
  }
  if (!Cards::instance) {
    Cards::instance = new Cards(controller);
  }
  Cards::instance->Update(hints);
}

// static
void PhiSplitDropOverlay::Hide() {
  delete Cards::instance;
  Cards::instance = nullptr;
}

}  // namespace phi_client
