// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_shell_layout.h"

#include <algorithm>

namespace phi_client {

// The metric names, unqualified, so the geometry reads as it did.
using shell_metrics::kAddressBarHeight;
using shell_metrics::kAddressBarTop;
using shell_metrics::kBandDividerHeight;
using shell_metrics::kBandGroupGap;
using shell_metrics::kBandTrailingGap;
using shell_metrics::kBookmarkBarHeight;
using shell_metrics::kBookmarkSeparatorHeight;
using shell_metrics::kBottomBarHeight;
using shell_metrics::kBottomSpacer;
using shell_metrics::kCaptionButtonWidth;
using shell_metrics::kChatWidth;
using shell_metrics::kComfortableLeading;
using shell_metrics::kContentMargin;
using shell_metrics::kContentTopGap;
using shell_metrics::kEdgeInset;
using shell_metrics::kFaviconSlot;
using shell_metrics::kFavoritesSpacer;
using shell_metrics::kPinnedDropZoneHeight;
using shell_metrics::kFeedbackIconWidth;
using shell_metrics::kHTabStripLeading;
using shell_metrics::kHTabStripTop;
using shell_metrics::kHeaderHeight;
using shell_metrics::kHeaderSpacer;
using shell_metrics::kNavButton;
using shell_metrics::kNavGroupSpacing;
using shell_metrics::kNewTabRowHeight;
using shell_metrics::kOmniboxHeight;
using shell_metrics::kPillHeight;
using shell_metrics::kPinnedCellHeight;
using shell_metrics::kPinnedGap;
using shell_metrics::kSpaceChip;
using shell_metrics::kSpaceChipGap;
using shell_metrics::kSpaceChipVertical;
using shell_metrics::kSpacesStripHeight;
using shell_metrics::kSpacesStripPad;
using shell_metrics::kSpacesStripTop;
using shell_metrics::kTabRowGap;
using shell_metrics::kTabRowHeight;
using shell_metrics::kTitleBarHeight;
using shell_metrics::kTitleBarHeightTabs;
using shell_metrics::kTopBarField;
using shell_metrics::kTopBarHeight;
using shell_metrics::kTopBarHeightNav;
using shell_metrics::kTopBarLeading;
using shell_metrics::kTopBarSpacing;

const char* PhiLayoutModeName(PhiLayoutMode mode) {
  switch (mode) {
    case PhiLayoutMode::kBalanced:
      return "balanced";
    case PhiLayoutMode::kComfortable:
      return "comfortable";
    case PhiLayoutMode::kPerformance:
      break;
  }
  return "performance";
}

bool PhiLayoutModeFromName(const std::string& name, PhiLayoutMode* out) {
  if (name == "performance") {
    *out = PhiLayoutMode::kPerformance;
  } else if (name == "balanced") {
    *out = PhiLayoutMode::kBalanced;
  } else if (name == "comfortable") {
    *out = PhiLayoutMode::kComfortable;
  } else {
    return false;
  }
  return true;
}

int ShellScaled(int design_units, int dpi) {
  // MulDiv's rounding (to nearest, halves away from zero), so the host test
  // and the box agree on every odd DPI.
  const long long n = static_cast<long long>(design_units) * dpi;
  return static_cast<int>(n >= 0 ? (n + 48) / 96 : (n - 48) / 96);
}

ShellLayout ComputeShellLayout(const ShellLayoutInputs& in) {
  const auto scaled = [&](int design) { return ShellScaled(design, in.dpi); };
  ShellLayout out;
  out.mode = in.mode;
  out.chip_size = kSpaceChip;  // The vertical strip below raises it.
  out.client = {0, 0, in.client_width, in.client_height};
  if (in.content_fullscreen) {
    // Content fullscreen: the hosted child owns every pixel of the shell;
    // every chrome rect stays empty.
    out.content = out.client;
    return out;
  }
  const int w = in.client_width;
  const int h = in.client_height;
  const int sidebar = scaled(in.sidebar_dip);
  // Collapsed: the content card widens to the left margin (which becomes
  // the floating sidebar's hover trigger). The floating panel paints with
  // the expanded layout, which its caller expresses as sidebar_hidden=false.
  const bool hide_sidebar = in.sidebar_hidden;
  const int inset = scaled(kEdgeInset);
  const int btn = scaled(kNavButton);
  const int m = scaled(kContentMargin);

  // The caption band tops every mode: menu titles leading, minimize /
  // maximize / close trailing. The OS caption is gone (WM_NCCALCSIZE), so
  // this row is both the title bar and the menu bar.
  // Comfortable's band carries the tab strip too, so it is taller there.
  const int tb = scaled(in.mode == PhiLayoutMode::kComfortable
                            ? kTitleBarHeightTabs
                            : kTitleBarHeight);
  out.title_bar = {0, 0, w, tb};
  const int cap = scaled(kCaptionButtonWidth);
  out.caption_close = {w - cap, 0, w, tb};
  out.caption_max = {w - 2 * cap, 0, w - cap, tb};
  out.caption_min = {w - 3 * cap, 0, w - 2 * cap, tb};
  // A 24pt control centred in the caption band, its right edge at `right`.
  // The band's trailing cluster is built right-to-left from the caption
  // buttons with this.
  const auto band_control = [&](int right) {
    return SidebarRect{right - btn, (tb - btn) / 2, right, (tb + btn) / 2};
  };
  // Where the menu titles have to stop: the trailing cluster's left edge.
  // Filled in by the modes that build one; the caption buttons otherwise.
  out.band_limit = out.caption_min.left;
  // The seam between the band's last control and the caption buttons, set
  // by each mode once it knows what ends its cluster (search-tabs, in all
  // three). It centres in kBandTrailingGap and rides the cluster's own
  // line, so comfortable's copy follows the strip down with the rest.
  const auto place_band_divider = [&](const SidebarRect& last) {
    const int mid = (last.top + last.bottom) / 2;
    const int half = scaled(kBandDividerHeight) / 2;
    // A 1pt rule, so it thickens with the controls it separates: the band's
    // own bottom edge can stay one physical pixel because it runs the whole
    // window, but a 16pt stub at 200% would read as a scratch.
    const int x = (last.right + out.caption_min.left) / 2;
    out.band_divider = {x, mid - half, x + scaled(1), mid + half};
  };

  // The Spaces strip: 24pt pips from the left (10pt strip padding), one
  // trailing "+" chip. The Mac's second destination (new profile) moved to the
  // Spaces menu.
  const auto place_spaces = [&](int left, int right, int top) {
    out.chip_size = kSpaceChipVertical;  // Only the vertical layouts call this.
    const int pip = scaled(out.chip_size);
    out.spaces_strip = {left, top, right, top + scaled(kSpacesStripHeight)};
    const int chip_top = top + (scaled(kSpacesStripHeight) - pip) / 2;
    out.chips_left = left + scaled(kSpacesStripPad);
    out.chips_top = chip_top;
    out.add_space = {right - scaled(kSpacesStripPad) - pip, chip_top,
                     right - scaled(kSpacesStripPad), chip_top + pip};
    out.chips_limit = out.add_space.left - scaled(kSpaceChipGap);
  };

  // The top bar of the two top-nav layouts: the first 40pt band INSIDE the
  // content card (the Mac's WebContentHeader is the card's first stack item),
  // back / forward / reload leading, the address capsule filling the middle.
  const auto place_top_bar = [&](int left, int right, int top, int leading,
                                 int height) {
    out.top_bar = {left, top, right, top + scaled(height)};
    const int mid = top + scaled(height) / 2;
    int x = left + scaled(leading);
    out.back = {x, mid - btn / 2, x + btn, mid + btn / 2};
    x += btn + scaled(kTopBarSpacing);
    out.forward = {x, mid - btn / 2, x + btn, mid + btn / 2};
    x += btn + scaled(kTopBarSpacing);
    out.reload = {x, mid - btn / 2, x + btn, mid + btn / 2};
    return mid;
  };

  // The bookmark bar takes the content's first band when visible: the strip
  // shows the ground tint and the paint pass lays pills over it.
  const auto carve_bookmark_bar = [&](ShellLayout* l) {
    if (!in.bookmark_bar) {
      return;
    }
    const int bar_h = scaled(kBookmarkBarHeight);
    l->bookmark_bar = {l->content.left, l->content.top, l->content.right,
                       l->content.top + bar_h};
    l->content.top += bar_h;
  };

  if (in.mode == PhiLayoutMode::kComfortable) {
    // Chrome's stack. The tab strip shares the caption band with the window
    // controls — the active tab merges downward into the toolbar — the
    // toolbar runs the full width under it, and the page goes EDGE TO EDGE:
    // no floating card, no margins. The window tint survives only behind
    // the tabs, which is the one band the card never covered anyway.
    //
    // This band has no menu titles: comfortable's menus live behind the
    // overflow button at the toolbar's trailing edge, as Chrome's do.
    out.band_limit = 0;

    // This band's controls sit on the TAB STRIP's centre line, not the
    // band's: the strip starts kHTabStripTop below the top edge and runs to
    // the band's bottom (where the active tab merges into the toolbar), so
    // anything centred on the band reads half that inset high against the
    // tabs it flanks.
    const int strip_mid = (scaled(kHTabStripTop) + tb) / 2;
    // The Phi button leads the band — the corner the Mac's menu bar gives
    // the Phi menu, and where the vertical layouts start their titles. The
    // active Space's compact pip follows it (the Mac clears the traffic
    // lights with 80pt; Windows has none, so the edge inset does).
    out.phi_menu = {inset, strip_mid - btn / 2, inset + btn,
                    strip_mid + btn / 2};
    out.chips_left = out.phi_menu.right + scaled(kTopBarSpacing);
    out.chips_top = strip_mid - scaled(kSpaceChip) / 2;
    // One compact pip.
    out.chips_limit = out.chips_left + scaled(kSpaceChip);
    // search-tabs keeps the trailing spot the vertical layouts park it in,
    // so the button does not move when the layout does — on the strip's
    // line here, the band's there.
    const int search_right = out.caption_min.left - scaled(kBandTrailingGap);
    out.search_tabs = {search_right - btn, strip_mid - btn / 2, search_right,
                       strip_mid + btn / 2};
    place_band_divider(out.search_tabs);
    // The "+" follows the last tab; the strip is full when the next tab
    // would need the room the button is holding.
    const int new_tab_gap = scaled(kTopBarSpacing);
    out.tabs_left = out.chips_limit + scaled(kHTabStripLeading);
    out.tabs_top = scaled(kHTabStripTop);
    out.tabs_limit = out.search_tabs.left - new_tab_gap - btn - new_tab_gap;
    out.horizontal_tabs = true;

    // The toolbar: back / forward / reload leading, the omnibox filling the
    // middle, then extensions, downloads and the overflow menu.
    const int mid =
        place_top_bar(0, w, tb, kComfortableLeading, kTopBarHeight);
    out.content = {0, out.top_bar.bottom, w, h};
    carve_bookmark_bar(&out);
    int r = w - scaled(kTopBarLeading);
    out.overflow = {r - btn, mid - btn / 2, r, mid + btn / 2};
    r = out.overflow.left - scaled(kTopBarSpacing);
    out.downloads = {r - btn, mid - btn / 2, r, mid + btn / 2};
    r = out.downloads.left - scaled(kTopBarSpacing);
    out.extensions = {r - btn, mid - btn / 2, r, mid + btn / 2};
    // Feedback, Chat and the memory chip have no band to sit in here; they
    // are items in the overflow menu instead.
    const int field_h = scaled(kOmniboxHeight);
    out.url_field = {out.reload.right + scaled(kTopBarSpacing),
                     mid - field_h / 2,
                     out.extensions.left - scaled(kTopBarSpacing),
                     mid + field_h / 2};
    const int icon = scaled(kFaviconSlot);
    out.page_icon = {out.url_field.left + scaled(10), mid - icon / 2,
                     out.url_field.left + scaled(10) + icon, mid + icon / 2};
    // Reader View trails INSIDE the pill, where the Mac's
    // WebContentAddressBar keeps it (and where Chrome keeps its own reading
    // icon) — and ONLY when the active tab has an article, the same rule the
    // sidebar's capsule follows.
    if (in.reader_offerable) {
      const int fbtn = field_h - scaled(2);
      const int fr = out.url_field.right - scaled(2);
      out.reader = {fr - fbtn, mid - fbtn / 2, fr, mid + fbtn / 2};
    }
    return out;
  }

  // Vertical layouts: the Mac sidebar stack, top to bottom.
  out.sidebar = {0, 0, sidebar, h};

  if (in.mode == PhiLayoutMode::kBalanced) {
    // Balanced: navigation and the address bar live in the top bar, and
    // search-tabs has left the sidebar header for the caption band — the
    // SAME trailing spot performance parks it in. That empties the header
    // row, so the Spaces strip starts at the sidebar's own inset.
    out.search_tabs =
        band_control(out.caption_min.left - scaled(kBandTrailingGap));
    place_band_divider(out.search_tabs);
    out.band_limit = out.search_tabs.left;
    const int gap = tb + scaled(kContentTopGap);
    if (!hide_sidebar) {
      // The strip starts on the card's own top edge, so the sidebar's Spaces
      // row and the card's address row read as ONE 32pt band across the
      // window rather than two rows that nearly line up.
      place_spaces(0, sidebar, gap);
    }

    const int bar_left = hide_sidebar ? m : sidebar;
    const int mid =
        place_top_bar(bar_left, w - m, gap, kTopBarLeading, kTopBarHeightNav);
    out.content = {bar_left, gap + scaled(kTopBarHeightNav), w - m, h - m};
    carve_bookmark_bar(&out);
    // Chat capsule trailing (balanced shows it in the header, as on Mac).
    // With the AI suite off it is not laid out at all and the address bar
    // takes the room back.
    const int r2 = w - m - scaled(kTopBarLeading);
    if (in.ai_enabled) {
      out.chat = {r2 - scaled(kChatWidth), mid - scaled(kPillHeight) / 2, r2,
                  mid + scaled(kPillHeight) / 2};
    }
    const int field_h = scaled(kTopBarField);
    const int field_right =
        in.ai_enabled ? out.chat.left - scaled(kTopBarSpacing) : r2;
    out.url_field = {out.reload.right + scaled(6), mid - field_h / 2,
                     field_right, mid + field_h / 2};
    // WebContentAddressBar's trailing stack, INSIDE the capsule: the Mac
    // keeps the extensions menu there in balanced exactly as SideAddressBar
    // keeps it in performance — 2pt from the trailing edge, and a 22pt glyph
    // box (its menuButton is a 22pt Lottie in a 24pt hover circle inside the
    // 26pt bar). The Mac's full order is reader, copy link, extensions menu;
    // copy link is the one still to slot in, between the two below.
    const int fmid = (out.url_field.top + out.url_field.bottom) / 2;
    const int fbtn = field_h - scaled(2);
    const int fr = out.url_field.right - scaled(2);
    out.extensions = {fr - fbtn, fmid - fbtn / 2, fr, fmid + fbtn / 2};
    if (in.reader_offerable) {
      const int rr = out.extensions.left - scaled(2);
      out.reader = {rr - fbtn, fmid - fbtn / 2, rr, fmid + fbtn / 2};
    }
    out.tabs_top = out.spaces_strip.bottom + scaled(kHeaderSpacer);
  } else {
    // Performance: back / forward / reload and search-tabs trail the caption
    // band, before the caption buttons — the sidebar has no header row at
    // all now, so the address bar sits directly under the Spaces strip. The
    // trio keeps its 1pt stack spacing and search-tabs follows it, at the
    // same x balanced puts it.
    int r = out.caption_min.left - scaled(kBandTrailingGap);
    out.search_tabs = band_control(r);
    place_band_divider(out.search_tabs);
    r = out.search_tabs.left - scaled(kBandGroupGap);
    out.reload = band_control(r);
    r = out.reload.left - scaled(kNavGroupSpacing);
    out.forward = band_control(r);
    r = out.forward.left - scaled(kNavGroupSpacing);
    out.back = band_control(r);
    out.band_limit = out.back.left;
    if (!hide_sidebar) {
      place_spaces(0, sidebar, tb + scaled(kSpacesStripTop));
      out.url_field = {inset, tb + scaled(kAddressBarTop), sidebar - inset,
                       tb + scaled(kAddressBarTop + kAddressBarHeight)};
      // SideAddressBar's trailing stack INSIDE the capsule: the copy-link
      // button then the extensions-menu button, 24pt each, spacing 2,
      // trailing inset 2 (rightStackView metrics).
      const int fmid = (out.url_field.top + out.url_field.bottom) / 2;
      int fr = out.url_field.right - scaled(2);
      out.extensions = {fr - btn, fmid - btn / 2, fr, fmid + btn / 2};
      fr = out.extensions.left - scaled(2);
      out.copy_link = {fr - btn, fmid - btn / 2, fr, fmid + btn / 2};
      // Reader View leads the stack, and ONLY when the active tab has an
      // article — a reader button on a page with nothing to read is the
      // thing the Mac's "show it only where it has something to offer"
      // change removed.
      if (in.reader_offerable) {
        const int rr = out.copy_link.left - scaled(2);
        out.reader = {rr - btn, fmid - btn / 2, rr, fmid + btn / 2};
      }
      if (in.field_ext_count > 0) {
        // Pinned-extension icons lead the stack — but only while the text
        // keeps the Mac's 84pt minimum (minimumAddressTextWidth); below
        // that they hide wholesale, as on Mac.
        const int strip_w = in.field_ext_count * btn +
                            (in.field_ext_count - 1) * scaled(2);
        const int text_left = out.url_field.left + scaled(12);
        const int stack_left =
            !out.reader.empty() ? out.reader.left : out.copy_link.left;
        const int remaining =
            stack_left - scaled(2) - strip_w - scaled(8) - text_left;
        if (remaining >= scaled(84)) {
          out.field_ext_strip = {stack_left - scaled(2) - strip_w,
                                 fmid - btn / 2, stack_left - scaled(2),
                                 fmid + btn / 2};
        }
      }
    }
    out.content = {hide_sidebar ? m : sidebar, tb + scaled(kContentTopGap),
                   w - m, h - m};
    carve_bookmark_bar(&out);
    out.tabs_top = tb + scaled(kHeaderHeight + kHeaderSpacer);
  }

  // The pinned-tab band sits between the header and the tab list. Empty until
  // favourites exist, exactly as on Mac (which mounts a zero-height container).
  // The pinned band, then the bookmark rows, then "+ New Tab" and the open
  // tabs — the Mac sidebar's order. A non-empty band carries the grid's own
  // 8pt top/bottom insets (PinnedTabLayout.insets).
  // A drag in flight reserves a drop zone in an EMPTY band (the Mac's
  // 100pt drag-time container for pins; one outline row for bookmarks),
  // so a tab can be pinned or bookmarked before the band has a first row.
  const int pinned_rows = in.pinned_row_count;
  const int pinned_height =
      pinned_rows > 0 ? 2 * scaled(kPinnedGap) +
                            pinned_rows * scaled(kPinnedCellHeight) +
                            (pinned_rows - 1) * scaled(kPinnedGap)
      : in.pinned_drop_zone ? scaled(kPinnedDropZoneHeight)
                            : 0;
  out.favorites = {0, out.tabs_top, sidebar, out.tabs_top + pinned_height};
  const int bookmarks_top = out.favorites.bottom +
                            (pinned_height > 0 ? scaled(kFavoritesSpacer) : 0);
  const int bookmark_rows =
      in.bookmark_row_count > 0 ? in.bookmark_row_count
      : in.bookmark_drop_zone   ? 1
                                : 0;
  const int bookmarks_height = bookmark_rows * scaled(kTabRowHeight + kTabRowGap);
  // A non-empty outline is parted from the tab list by the Mac's 16pt
  // separator row (makeAllItems).
  const int separator_h =
      bookmark_rows > 0 ? scaled(kBookmarkSeparatorHeight) : 0;
  const int separator_top = bookmarks_top + bookmarks_height;
  out.bookmark_separator =
      separator_h > 0 ? SidebarRect{inset, separator_top, sidebar - inset,
                                    separator_top + separator_h}
                      : SidebarRect{};
  out.bookmarks =
      bookmark_rows > 0 ? SidebarRect{inset, bookmarks_top, sidebar - inset,
                                      separator_top + separator_h}
                        : SidebarRect{};
  out.new_tab_row = {inset, separator_top + separator_h, sidebar - inset,
                     separator_top + separator_h + scaled(kNewTabRowHeight)};
  out.tabs_top = out.new_tab_row.bottom + scaled(kTabRowGap);

  // The bottom bar: downloads and memory leading (spacing 4), the Feedback
  // and Chat capsules trailing — SidebarBottomBar's row, settings not in it.
  const int bar_h = scaled(kBottomBarHeight);
  const int bar_top = h - scaled(kBottomSpacer) - bar_h;
  out.bottom_bar = {0, bar_top, sidebar, bar_top + bar_h};
  int bx = inset;
  out.downloads = {bx, bar_top, bx + btn, bar_top + bar_h};
  // The browser-memory chip follows downloads only while the AI suite is on;
  // otherwise nothing takes its slot.
  if (in.ai_enabled) {
    bx += btn + scaled(4);
    out.memory = {bx, bar_top, bx + btn, bar_top + bar_h};
  }
  // No extensions button down here in any layout: the Mac's bottom bar holds
  // downloads, memory and the notification card leading, feedback and chat
  // trailing — the extensions menu lives inside whichever address bar the
  // layout has (SideAddressBar or WebContentAddressBar).
  int br = sidebar - inset;
  const int pill_top = bar_top + (bar_h - scaled(kPillHeight)) / 2;
  if (in.mode != PhiLayoutMode::kBalanced) {
    // The Chat capsule fills the whole 24pt row (ChatButton contentHeight).
    // Balanced holds its top-bar chat rect instead: the Mac hides the
    // bottom-bar chat whenever navigation rides at the top
    // (updateChatButtonVisibility's navigationAtTop) — the capsule lives
    // in the header there, and this block must not overwrite it.
    if (in.ai_enabled) {
      out.chat = {br - scaled(kChatWidth), bar_top, br, bar_top + bar_h};
      br = out.chat.left - scaled(4);
    }
  }
  // The narrow sidebar takes ViewThatFits' icon-only feedback variant.
  out.feedback = {br - scaled(kFeedbackIconWidth), pill_top, br,
                  pill_top + scaled(kPillHeight)};

  out.tabs_left = inset;
  out.tabs_limit = out.bottom_bar.top - scaled(kFavoritesSpacer);
  if (hide_sidebar) {
    const SidebarRect zero = {};
    out.favorites = zero;
    out.bookmarks = zero;
    out.bookmark_separator = zero;
    out.new_tab_row = zero;
    out.bottom_bar = zero;
    out.downloads = zero;
    out.memory = zero;
    out.extensions = zero;
    out.feedback = zero;
    out.chat = zero;
    out.tabs_limit = out.tabs_top;  // No tab rows either.
  }
  return out;
}

}  // namespace phi_client
