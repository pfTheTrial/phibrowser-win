// Host tests for phi_shell_layout (docs/architecture-review.md §3.1
// principle 6): the shell's chrome geometry as a pure value — the
// Windows-native two layouts of docs/windows-native-ui.md §4.

#include <cstdio>

#include "phi_shell_layout.h"

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
  // Leading: toggle, search (32 buttons, 4 apart), a gap, the nav trio.
  CHECK((l.sidebar_toggle == SidebarRect{248, 6, 280, 38}));
  CHECK(l.search_tabs.left == l.sidebar_toggle.right + 4);
  CHECK(l.back.left == l.search_tabs.right + 8);
  CHECK(l.forward.left == l.back.right + 4);
  CHECK(l.reload.left == l.forward.right + 4);
  // Trailing: ⋯ 16 before minimize, the divider between, then downloads
  // and extensions 4 apart.
  CHECK(l.app_menu.right == l.caption_min.left - 16);
  CHECK(l.band_divider.left > l.app_menu.right &&
        l.band_divider.right < l.caption_min.left);
  CHECK(l.downloads.right == l.app_menu.left - 4);
  CHECK(l.extensions.right == l.downloads.left - 4);
  // The omnibox: 32 high on the row's centre line, a 32 drag gutter
  // before the cluster.
  CHECK(l.url_field.left == l.reload.right + 8);
  CHECK(l.url_field.right == l.extensions.left - 32);
  CHECK(l.url_field.top == 6 && l.url_field.bottom == 38);
  CHECK((l.drag_gutter == SidebarRect{l.url_field.right, 0, l.extensions.left, 44}));
  // In-field slots: site info leading, the star trailing, no reader.
  CHECK((l.page_icon == SidebarRect{l.url_field.left + 4, 10,
                                    l.url_field.left + 28, 34}));
  CHECK(l.star.right == l.url_field.right - 4);
  CHECK(l.reader.empty());
  CHECK(l.field_ext_strip.empty());
  // The header row: pips at x 8, 28 dip, "+" trailing.
  CHECK((l.spaces_strip == SidebarRect{0, 0, 240, 44}));
  CHECK(l.chip_size == 28);
  CHECK(l.chips_left == 8 && l.chips_top == 8);
  CHECK((l.add_space == SidebarRect{240 - 8 - 28, 8, 232, 36}));
  CHECK(l.chips_limit == l.add_space.left - 4);
  // No pins, no bookmarks: the Tabs label under the header, "+ New tab"
  // under it.
  CHECK(l.favorites.empty());
  CHECK(l.pinned_label.empty() && l.bookmarks_label.empty());
  CHECK((l.tabs_label == SidebarRect{16, 44, 240 - 16, 44 + 28}));
  CHECK(l.new_tab_row.top == 44 + 28);
  CHECK(l.pin.empty());
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
  CHECK(l.reader.right == l.star.left - 2);
  // The pinned-extension slots lead the cluster: 28 wide, 2 apart, 4
  // before the extensions button.
  CHECK(!l.field_ext_strip.empty());
  CHECK(l.field_ext_strip.right == l.extensions.left - 4);
  CHECK(l.field_ext_strip.right - l.field_ext_strip.left == 2 * 28 + 2);
  CHECK(l.url_field.right == l.field_ext_strip.left - 32);
  // The Pinned label, then two pinned rows: 8 + 44 + 8 + 44 + 8.
  CHECK(l.pinned_label.top == 44 && l.favorites.top == 44 + 28);
  CHECK(l.favorites.bottom - l.favorites.top == 8 + 44 + 8 + 44 + 8);
  // The Bookmarks label, three rows, then the Tabs label.
  CHECK(l.bookmarks_label.top == l.favorites.bottom);
  CHECK(l.bookmarks.top == l.bookmarks_label.bottom);
  CHECK(l.bookmarks.bottom == l.bookmarks.top + 3 * 38);
  CHECK(l.bookmark_separator.empty());
  CHECK(l.tabs_label.top == l.bookmarks.bottom);
  CHECK(l.new_tab_row.top == l.tabs_label.bottom);
  // The floating panel: the pin trails the header, the "+" moves in.
  ShellLayoutInputs floating = in;
  floating.floating_panel = true;
  const ShellLayout f = ComputeShellLayout(floating);
  CHECK((f.pin == SidebarRect{240 - 8 - 32, 6, 232, 38}));
  CHECK(f.add_space.right == f.pin.left - 4);
  // With a single Space the panel keeps the row for the pin alone.
  floating.spaces_switch = false;
  const ShellLayout lone = ComputeShellLayout(floating);
  CHECK(!lone.spaces_strip.empty() && !lone.pin.empty());
  CHECK(lone.add_space.empty() && lone.chips_limit == lone.chips_left);
  CHECK(lone.pinned_label.top == 44);
  // A narrow window hides the extension slots before crushing the text.
  ShellLayoutInputs narrow = in;
  narrow.client_width = 900;
  const ShellLayout n = ComputeShellLayout(narrow);
  CHECK(n.field_ext_strip.empty());
  CHECK(n.url_field.right == n.extensions.left - 32);
}

void SingleSpace() {
  std::printf("single space: no header row; the bands start at the top\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kBalanced);
  in.spaces_switch = false;
  ShellLayout l = ComputeShellLayout(in);
  CHECK(l.spaces_strip.empty() && l.add_space.empty());
  CHECK(!l.spaces_switch);
  CHECK(l.tabs_label.top == 0 && l.new_tab_row.top == 28);
  in.pinned_row_count = 1;
  l = ComputeShellLayout(in);
  CHECK(l.pinned_label.top == 0 && l.favorites.top == 28);
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
  CHECK(l.downloads.right == l.app_menu.left - 4);
  CHECK(l.extensions.right == l.downloads.left - 4);
  CHECK(l.url_field.left == l.reload.right + 8);
  CHECK(l.url_field.right == l.extensions.left - 8);
  CHECK(l.url_field.top == 44 && l.url_field.bottom == 76);
  CHECK(!l.page_icon.empty() && !l.star.empty());
  CHECK(l.drag_gutter.empty());
  CHECK(l.sidebar_toggle.empty());
  CHECK(l.spaces_strip.empty());
  CHECK(l.new_tab_row.empty());
  CHECK(l.bottom_bar.empty());
  CHECK(l.reader.empty());
  ShellLayoutInputs reading = Base(PhiLayoutMode::kComfortable);
  reading.reader_offerable = true;
  reading.bookmark_bar = true;
  const ShellLayout r = ComputeShellLayout(reading);
  CHECK(!r.reader.empty() && r.reader.right == r.star.left - 2);
  // The bookmark bar takes the content's first 28 dip band.
  CHECK((r.bookmark_bar == SidebarRect{0, 80, 1440, 108}));
  CHECK(r.content.top == 108);
}

void SidebarToggle() {
  std::printf("sidebar toggle: leads the address row, stays through a "
              "collapse, absent in horizontal\n");
  const ShellLayout b = ComputeShellLayout(Base(PhiLayoutMode::kBalanced));
  CHECK((b.sidebar_toggle == SidebarRect{248, 6, 280, 38}));
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
  CHECK((hi.sidebar_toggle == SidebarRect{372, 9, 420, 57}));
}

void Collapsed() {
  std::printf("collapsed: the pane fills the window, the bands empty\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kBalanced);
  in.sidebar_hidden = true;
  const ShellLayout l = ComputeShellLayout(in);
  CHECK((l.top_bar == SidebarRect{0, 0, 1440, 44}));
  CHECK((l.content == SidebarRect{0, 44, 1440, 900}));
  CHECK(l.spaces_strip.empty());
  CHECK(!l.url_field.empty());  // The address row is the pane's, not the sidebar's.
  CHECK(l.favorites.empty());
  CHECK(l.new_tab_row.empty());
  CHECK(l.bottom_bar.empty());
  CHECK(l.feedback.empty());
  CHECK(!l.downloads.empty() && !l.extensions.empty());  // The cluster stays.
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
  CHECK(l.chips_left == 12 && l.chips_top == 12);
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
  Inputs();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("shell layout: all passed\n");
  return 0;
}
