// Host tests for phi_shell_layout (docs/architecture-review.md §3.1
// principle 6): the shell's chrome geometry as a pure value — the
// Windows-native two layouts of docs/windows-native-ui.md §4.

#include <cstdio>

#include "phi_shell_layout.h"
#include "phi_backdrop.h"

using namespace phi_client;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

ShellLayoutInputs Base(PhiLayoutMode mode, int dpi = 96) {
  ShellLayoutInputs in;
  in.mode = mode;
  in.client_width = ShellScaled(1440, dpi);
  in.client_height = ShellScaled(900, dpi);
  in.dpi = dpi;
  in.sidebar_dip = 240;
  return in;
}

void Scaling() {
  std::printf("scaling: MulDiv rounding, halves away from zero\n");
  CHECK(ShellScaled(32, 96) == 32);
  CHECK(ShellScaled(32, 144) == 48);
  CHECK(ShellScaled(13, 144) == 20);  // 19.5 rounds up.
  CHECK(ShellScaled(1, 120) == 1);    // 1.25 rounds down.
  CHECK(ShellScaled(-13, 144) == -20);
}

void Names() {
  std::printf("names: performance reads as balanced (impl log D9)\n");
  PhiLayoutMode mode = PhiLayoutMode::kComfortable;
  CHECK(PhiLayoutModeFromName("performance", &mode) &&
        mode == PhiLayoutMode::kBalanced);
  CHECK(PhiLayoutModeFromName("comfortable", &mode) &&
        mode == PhiLayoutMode::kComfortable);
  CHECK(!PhiLayoutModeFromName("sideways", &mode));
  // A stale enum value lays out the vertical layout.
  const ShellLayout l = ComputeShellLayout(Base(PhiLayoutMode::kPerformance));
  CHECK(l.mode == PhiLayoutMode::kBalanced);
  CHECK(!l.horizontal_tabs);
}

void Vertical() {
  std::printf("vertical: sidebar header row beside a flush pane with the "
              "address row\n");
  const ShellLayout l = ComputeShellLayout(Base(PhiLayoutMode::kBalanced));
  CHECK(!l.horizontal_tabs);
  // The address row IS the caption row: 44 high, the caption buttons
  // square at its trailing end.
  CHECK((l.title_bar == SidebarRect{0, 0, 1440, 44}));
  CHECK((l.caption_close == SidebarRect{1440 - 46, 0, 1440, 44}));
  CHECK((l.caption_min == SidebarRect{1440 - 138, 0, 1440 - 92, 44}));
  CHECK((l.sidebar == SidebarRect{0, 0, 240, 900}));
  // The pane: flush top / right / bottom, from the sidebar's edge.
  CHECK((l.top_bar == SidebarRect{240, 0, 1440, 44}));
  CHECK((l.content == SidebarRect{240, 44, 1440, 900}));
  // App mark leading; search and toggle trail the sidebar's own header.
  CHECK((l.app_icon == SidebarRect{8, 6, 40, 38}));
  CHECK((l.sidebar_toggle == SidebarRect{200, 6, 232, 38}));
  CHECK(l.search_tabs.right == l.sidebar_toggle.left - 4);
  CHECK(l.app_icon.right < l.search_tabs.left);
  CHECK(l.back.left == l.top_bar.left + 8);
  CHECK(l.forward.left == l.back.right + 4);
  CHECK(l.reload.left == l.forward.right + 4);
  // Trailing: ⋯ 16 before minimize, the divider between, then extensions
  // 4 apart (no Downloads button: it lives in the sidebar's bottom bar).
  CHECK(l.app_menu.right == l.caption_min.left - 16);
  CHECK(l.band_divider.left > l.app_menu.right &&
        l.band_divider.right < l.caption_min.left);
  CHECK(l.extensions.right == l.app_menu.left - 4);
  // The omnibox: 32 high on the row's centre line, the ordinary group gap
  // before the cluster (no reserved drag run).
  CHECK(l.url_field.left == l.reload.right + 8);
  CHECK(l.url_field.right == l.extensions.left - 8);
  CHECK(l.url_field.top == 6 && l.url_field.bottom == 38);
  // In-field slots: site info leading, the star trailing, no reader.
  CHECK((l.page_icon == SidebarRect{l.url_field.left + 4, 10,
                                    l.url_field.left + 28, 34}));
  // The Mac's hover-only pair trails the field (no star): the site menu
  // last, Copy link before it.
  CHECK((l.site_menu == SidebarRect{l.url_field.right - 28, 10,
                                    l.url_field.right - 4, 34}));
  CHECK(l.copy_link.right == l.site_menu.left - 2);
  CHECK(l.copy_link.top == 10 && l.copy_link.bottom == 34);
  CHECK(l.reader.empty());
  CHECK(l.field_ext_strip.empty());
  // The header row: pips at x 8, 28 dip, "+" trailing.
  CHECK((l.spaces_strip == SidebarRect{0, 44, 240, 88}));
  CHECK(l.chip_size == 28);
  CHECK(l.chips_left == 8 && l.chips_top == 52);
  CHECK((l.add_space == SidebarRect{240 - 8 - 28, 52, 232, 80}));
  CHECK(l.chips_limit == l.add_space.left - 4);
  // No pins, no bookmarks, no section labels: "+ New tab" right under
  // the header.
  CHECK(l.favorites.empty());
  CHECK(l.pinned_label.empty() && l.bookmarks_label.empty());
  CHECK(l.tabs_label.empty());
  CHECK(l.new_tab_row.top == 88);
  // The agent Space's "<Agent>'s tabs" cue is the one label row left.
  ShellLayoutInputs owned = Base(PhiLayoutMode::kBalanced);
  owned.owned_tabs_label = true;
  const ShellLayout o = ComputeShellLayout(owned);
  CHECK((o.tabs_label == SidebarRect{16, 88, 240 - 16, 88 + 28}));
  CHECK(o.new_tab_row.top == 88 + 28);
  CHECK(l.tabs_top == l.new_tab_row.bottom + 2);
  CHECK(l.tabs_left == 8);
  CHECK((l.bottom_bar == SidebarRect{0, 900 - 40, 240, 900}));
  CHECK(l.tabs_limit == l.bottom_bar.top - 4);
  CHECK(l.memory.empty());  // AI suite off.
  CHECK(l.chat.empty());
  CHECK(l.feedback.right == 232);
}

void VerticalExtras() {
  std::printf("vertical: reader slot, extension slots, bands, no bookmark bar\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kBalanced);
  in.reader_offerable = true;
  in.field_ext_count = 2;
  in.pinned_row_count = 2;
  in.bookmark_row_count = 3;
  const ShellLayout l = ComputeShellLayout(in);
  CHECK(l.reader.right == l.copy_link.left - 2);
  // The pinned-extension slots lead the cluster: 28 wide, 2 apart, 4
  // before the extensions button.
  CHECK(!l.field_ext_strip.empty());
  CHECK(l.field_ext_strip.right == l.extensions.left - 4);
  CHECK(l.field_ext_strip.right - l.field_ext_strip.left == 2 * 28 + 2);
  CHECK(l.url_field.right == l.field_ext_strip.left - 8);
  // Two pinned rows right under the header: 8 + 44 + 8 + 44 + 8.
  CHECK(l.pinned_label.empty() && l.favorites.top == 88);
  CHECK(l.favorites.bottom - l.favorites.top == 8 + 44 + 8 + 44 + 8);
  // An 8 dip gap, three bookmark rows, another gap, "+ New tab".
  CHECK(l.bookmarks_label.empty());
  CHECK(l.bookmarks.top == l.favorites.bottom + 8);
  CHECK(l.bookmarks.bottom == l.bookmarks.top + 3 * 38);
  CHECK(l.bookmark_separator.empty());
  CHECK(l.tabs_label.empty());
  CHECK(l.new_tab_row.top == l.bookmarks.bottom + 8);
  // The floating panel: no pin button; the "+" trails the header.
  ShellLayoutInputs floating = in;
  floating.floating_panel = true;
  const ShellLayout f = ComputeShellLayout(floating);
  CHECK(f.add_space.right == 240 - 8);
  // With a single Space the panel keeps the header row, pips and all gone.
  floating.spaces_switch = false;
  const ShellLayout lone = ComputeShellLayout(floating);
  CHECK(!lone.spaces_strip.empty());
  CHECK(lone.add_space.empty() && lone.chips_limit == lone.chips_left);
  CHECK(lone.favorites.top == 88);
  // A narrow window hides the extension slots before crushing the text.
  ShellLayoutInputs narrow = in;
  narrow.client_width = 680;
  const ShellLayout n = ComputeShellLayout(narrow);
  CHECK(n.field_ext_strip.empty());
  CHECK(n.url_field.right == n.extensions.left - 8);
}

void SingleSpace() {
  std::printf("single space: persistent app header, no Space strip\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kBalanced);
  in.spaces_switch = false;
  ShellLayout l = ComputeShellLayout(in);
  CHECK(l.spaces_strip.empty() && l.add_space.empty());
  CHECK(!l.spaces_switch);
  CHECK(l.tabs_label.empty() && l.new_tab_row.top == 44);
  CHECK(!l.app_icon.empty() && !l.search_tabs.empty());
  in.pinned_row_count = 1;
  l = ComputeShellLayout(in);
  CHECK(l.pinned_label.empty() && l.favorites.top == 44);
  CHECK(l.new_tab_row.top == l.favorites.bottom + 8);
  in = Base(PhiLayoutMode::kComfortable);
  in.spaces_switch = false;
  l = ComputeShellLayout(in);
  CHECK(l.chips_limit == l.chips_left);
  CHECK(l.tabs_left == 8 + 8);
  ShellLayoutInputs ai = Base(PhiLayoutMode::kBalanced);
  ai.ai_enabled = true;
  const ShellLayout with_ai = ComputeShellLayout(ai);
  CHECK(!with_ai.chat.empty() && !with_ai.memory.empty());
  CHECK(with_ai.chat.top >= with_ai.bottom_bar.top);
}

void Horizontal() {
  std::printf("horizontal: tab strip on the caption row, toolbar under it, "
              "page edge to edge\n");
  const ShellLayout l = ComputeShellLayout(Base(PhiLayoutMode::kComfortable));
  CHECK(l.horizontal_tabs);
  CHECK((l.title_bar == SidebarRect{0, 0, 1440, 40}));
  CHECK(l.sidebar.empty());
  CHECK((l.top_bar == SidebarRect{0, 40, 1440, 80}));
  CHECK((l.content == SidebarRect{0, 80, 1440, 900}));
  CHECK(l.chip_size == 28);
  CHECK(l.chips_left == 8);
  CHECK(l.chips_limit == l.chips_left + 28);
  CHECK(l.tabs_left == l.chips_limit + 8);
  // The Space chip takes the width the shell measured.
  ShellLayoutInputs chip = Base(PhiLayoutMode::kComfortable);
  chip.space_chip_dip = 132;
  const ShellLayout c = ComputeShellLayout(chip);
  CHECK(c.chips_limit == c.chips_left + 132);
  CHECK(c.tabs_left == c.chips_limit + 8);
  CHECK(l.tabs_top == 6);
  CHECK(l.search_tabs.right == l.caption_min.left - 16);
  CHECK(l.tabs_limit == l.search_tabs.left - 4 - 32 - 4);
  // The toolbar: trio at x 8 on the row's centre line (60), the cluster
  // 8 from the edge, the omnibox between.
  CHECK((l.back == SidebarRect{8, 44, 40, 76}));
  CHECK(l.forward.left == l.back.right + 4);
  CHECK(l.reload.left == l.forward.right + 4);
  CHECK(!l.app_menu.empty() && l.app_menu.right == 1440 - 8);
  CHECK(l.extensions.right == l.app_menu.left - 4);
  CHECK(l.url_field.left == l.reload.right + 8);
  CHECK(l.url_field.right == l.extensions.left - 8);
  CHECK(l.url_field.top == 44 && l.url_field.bottom == 76);
  CHECK(!l.page_icon.empty() && !l.site_menu.empty());
  CHECK(l.sidebar_toggle.empty());
  CHECK(l.spaces_strip.empty());
  CHECK(l.new_tab_row.empty());
  CHECK(l.bottom_bar.empty());
  CHECK(l.reader.empty());
  ShellLayoutInputs reading = Base(PhiLayoutMode::kComfortable);
  reading.reader_offerable = true;
  reading.bookmark_bar = true;
  const ShellLayout r = ComputeShellLayout(reading);
  CHECK(!r.reader.empty() && r.reader.right == r.copy_link.left - 2);
  CHECK(r.copy_link.right == r.site_menu.left - 2);
  CHECK(r.site_menu.right == r.url_field.right - 4);
  // The bookmark bar takes the content's first 28 dip band.
  CHECK((r.bookmark_bar == SidebarRect{0, 80, 1440, 108}));
  CHECK(r.content.top == 108);
}

void SidebarToggle() {
  std::printf("sidebar toggle: trails the sidebar header, stays through a "
              "collapse, absent in horizontal\n");
  const ShellLayout b = ComputeShellLayout(Base(PhiLayoutMode::kBalanced));
  CHECK((b.sidebar_toggle == SidebarRect{200, 6, 232, 38}));
  ShellLayoutInputs collapsed = Base(PhiLayoutMode::kBalanced);
  collapsed.sidebar_hidden = true;
  // Collapsed: the pane starts at x 0 and the toggle leads it there.
  CHECK((ComputeShellLayout(collapsed).sidebar_toggle ==
         SidebarRect{8, 6, 40, 38}));
  CHECK(ComputeShellLayout(Base(PhiLayoutMode::kComfortable))
            .sidebar_toggle.empty());
  ShellLayoutInputs fullscreen = Base(PhiLayoutMode::kBalanced);
  fullscreen.content_fullscreen = true;
  CHECK(ComputeShellLayout(fullscreen).sidebar_toggle.empty());
  // It scales with the row.
  const ShellLayout hi = ComputeShellLayout(Base(PhiLayoutMode::kBalanced, 144));
  CHECK((hi.sidebar_toggle == SidebarRect{300, 9, 348, 57}));
}

void Collapsed() {
  std::printf("collapsed: the pane fills the window, the bands empty\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kBalanced);
  in.sidebar_hidden = true;
  const ShellLayout l = ComputeShellLayout(in);
  CHECK((l.top_bar == SidebarRect{0, 0, 1440, 44}));
  CHECK((l.content == SidebarRect{0, 44, 1440, 900}));
  CHECK(l.spaces_strip.empty());
  CHECK(l.app_icon.empty());
  CHECK(l.search_tabs.right < l.back.left);
  CHECK(!l.url_field.empty());  // The address row is the pane's, not the sidebar's.
  CHECK(l.favorites.empty());
  CHECK(l.new_tab_row.empty());
  CHECK(l.bottom_bar.empty());
  CHECK(l.feedback.empty());
  CHECK(!l.app_menu.empty() && !l.extensions.empty());  // The cluster stays.
  CHECK(l.tabs_limit == l.tabs_top);  // No rows.
  // The floating panel asks for the expanded layout of a collapsed shell:
  // the very same inputs with sidebar_hidden cleared.
  in.sidebar_hidden = false;
  const ShellLayout expanded = ComputeShellLayout(in);
  CHECK(!expanded.new_tab_row.empty());
  CHECK(expanded.content.left == 240);
}

void ContentFullscreen() {
  std::printf("content fullscreen: the child owns every pixel\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kBalanced);
  in.content_fullscreen = true;
  const ShellLayout l = ComputeShellLayout(in);
  CHECK((l.content == SidebarRect{0, 0, 1440, 900}));
  CHECK(l.title_bar.empty() && l.top_bar.empty() && l.sidebar.empty());
  CHECK(l.app_icon.empty() && l.search_tabs.empty());
  CHECK(l.url_field.empty() && l.app_menu.empty());
}

void Dpi() {
  std::printf("dpi: every rect scales with the monitor, 150%% = 1.5x\n");
  const ShellLayout l = ComputeShellLayout(Base(PhiLayoutMode::kBalanced, 144));
  CHECK((l.title_bar == SidebarRect{0, 0, 2160, 66}));
  CHECK((l.caption_close == SidebarRect{2160 - 69, 0, 2160, 66}));
  CHECK((l.sidebar == SidebarRect{0, 0, 360, 1350}));
  CHECK((l.content == SidebarRect{360, 66, 2160, 1350}));
  CHECK(l.url_field.bottom - l.url_field.top == 48);
  CHECK(l.chip_size == 28);  // Design units; the paint scales.
  CHECK(l.chips_left == 12 && l.chips_top == 78);
}

void Backdrop() {
  std::printf("backdrop: translucent ground, readable ink, premultiplied edges\n");
  CHECK(PhiBackdropPixel(0xFFFFFF, 0xFFFFFF) == 0x33333333u);
  CHECK(PhiBackdropPixel(0, 0) == 0x33000000u);
  CHECK(PhiBackdropPixel(0, 0xFFFFFF) == 0xFF000000u);
  CHECK(PhiBackdropPixel(0xFFFFFF, 0) == 0xFFFFFFFFu);
  CHECK(PhiBackdropPixel(0xFF0000, 0xEEEEEE) == 0xFFFF0000u);
  // The split compositor uses straight alpha; recompositing that color
  // must recover the same tint as the sidebar's premultiplied pixels.
  for (bool dark : {false, true}) {
    for (uint32_t ground : {0u, 0xFFFFFFu, 0xE0DCFFu, 0x17312Bu}) {
      const uint32_t premultiplied = PhiBackdropPixel(ground, dark ? 0 : 0xFFFFFF);
      const uint32_t color = PhiBackdropColor(ground, dark);
      CHECK((premultiplied >> 24) == (color >> 24));
      for (int shift : {0, 8, 16}) {
        CHECK(((((color >> shift) & 255) * (color >> 24) + 127) / 255) ==
              ((premultiplied >> shift) & 255));
      }
    }
  }
  // The theme's chroma survives translucency, instead of being multiplied
  // by 20% for a second time. Stronger saturation must remain visible.
  for (uint32_t ground : {0xFFF0F0u, 0xFFCFCFu, 0xE0DCFFu}) {
    const uint32_t tint = PhiBackdropPixel(ground, 0xFFFFFF);
    CHECK((tint >> 24) < 255);
    CHECK(PhiBackdropPixelOver(ground, ground, tint) == tint);
    // A card may match the ground's RGB. Its known interior coverage must
    // still keep it opaque while the outside of the rounded arc stays glass.
    CHECK(PhiBackdropPixelOver(ground, ground, tint, 255) ==
          (0xFF000000u | ground));
    const uint32_t edge = PhiBackdropPixelOver(ground, ground, tint, 128);
    CHECK((edge >> 24) > (tint >> 24) && (edge >> 24) < 255);
    CHECK(static_cast<int>((tint >> 16) & 255) -
              static_cast<int>(tint & 255) ==
          static_cast<int>((ground >> 16) & 255) -
              static_cast<int>(ground & 255));
    CHECK(PhiBackdropPixelOver(0, ground, tint) == 0xFF000000u);
    CHECK(PhiBackdropPixelOver(0xFFFFFF, ground, tint) == 0xFFFFFFFFu);
  }
  for (uint32_t ground : {0u, 0xFFFFFFu, 0xDAE6EFu, 0x252525u}) {
    for (uint32_t value = 0; value < 256; ++value) {
      const uint32_t rgb = value * 0x010101u;
      const uint32_t pixel = PhiBackdropPixel(rgb, ground);
      const int alpha = pixel >> 24;
      CHECK(alpha >= 51 && alpha <= 255);
      for (int shift : {0, 8, 16}) {
        const int channel = (pixel >> shift) & 255;
        CHECK(channel <= alpha);
        // Compositing over the original ground reproduces the GDI image.
        const int restored = channel +
            (((ground >> shift) & 255) * (255 - alpha) + 127) / 255;
        CHECK(restored == static_cast<int>(value));
      }
    }
  }
}

bool Empty(const SidebarRect& r) {
  return r.right <= r.left;
}

void UpdateReminder() {
  std::printf("update: the reminder pill leads search in every layout\n");
  // No update on offer: no pill anywhere.
  CHECK(Empty(ComputeShellLayout(Base(PhiLayoutMode::kBalanced)).update));
  CHECK(Empty(ComputeShellLayout(Base(PhiLayoutMode::kComfortable)).update));

  // Horizontal: left of search on the strip's line, and the tabs stop
  // before it rather than before search.
  ShellLayoutInputs in = Base(PhiLayoutMode::kComfortable);
  const ShellLayout plain = ComputeShellLayout(in);
  in.update_dip = 64;
  const ShellLayout h = ComputeShellLayout(in);
  CHECK(h.update.right == h.search_tabs.left - 4);
  CHECK(h.update.right - h.update.left == 64);
  CHECK(h.update.bottom - h.update.top == 24);
  CHECK(h.update.top + h.update.bottom == h.search_tabs.top + h.search_tabs.bottom);
  CHECK(h.tabs_limit == plain.tabs_limit - (64 + 4));

  // Vertical, sidebar shown: in the header, left of search, clear of the
  // app mark; the address row does not move.
  in = Base(PhiLayoutMode::kBalanced);
  in.update_dip = 64;
  const ShellLayout v = ComputeShellLayout(in);
  CHECK(v.update.right == v.search_tabs.left - 4);
  CHECK(v.update.left >= v.app_icon.right + 4);
  CHECK(v.update.right <= v.sidebar.right);
  CHECK(v.back.left == ComputeShellLayout(Base(PhiLayoutMode::kBalanced)).back.left);

  // A sidebar at the Mac's 225 dip threshold drops the pill.
  in.sidebar_dip = 225;
  CHECK(Empty(ComputeShellLayout(in).update));
  in.sidebar_dip = 226;
  CHECK(!Empty(ComputeShellLayout(in).update));

  // Collapsed: the pill follows search at the address row's lead and the
  // nav trio moves after it.
  in = Base(PhiLayoutMode::kBalanced);
  in.sidebar_hidden = true;
  const ShellLayout collapsed_plain = ComputeShellLayout(in);
  in.update_dip = 64;
  const ShellLayout c = ComputeShellLayout(in);
  CHECK(c.update.left == c.search_tabs.right + 4);
  CHECK(c.back.left == c.update.right + 8);
  CHECK(c.back.left == collapsed_plain.back.left + 4 + 64);

  // HiDPI scales the measured width.
  in = Base(PhiLayoutMode::kComfortable, 144);
  in.update_dip = 64;
  const ShellLayout hi = ComputeShellLayout(in);
  CHECK(hi.update.right - hi.update.left == 96);
  CHECK(hi.update.bottom - hi.update.top == 36);
}

void Inputs() {
  std::printf("inputs: the cache key sees every field\n");
  ShellLayoutInputs a = Base(PhiLayoutMode::kBalanced);
  ShellLayoutInputs b = a;
  CHECK(a == b);
  b.sidebar_hidden = true;
  CHECK(a != b);
  b = a;
  b.field_ext_count = 1;
  CHECK(a != b);
  b = a;
  b.spaces_switch = false;
  CHECK(a != b);
  b = a;
  b.bookmark_bar = true;
  CHECK(a != b);
  b = a;
  b.floating_panel = true;
  CHECK(a != b);
  b = a;
  b.update_dip = 64;
  CHECK(a != b);
}

}  // namespace

int main() {
  Scaling();
  Names();
  Vertical();
  VerticalExtras();
  SingleSpace();
  Horizontal();
  SidebarToggle();
  Collapsed();
  ContentFullscreen();
  Dpi();
  Backdrop();
  UpdateReminder();
  Inputs();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("shell layout: all passed\n");
  return 0;
}
