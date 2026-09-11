// Host tests for phi_shell_layout (docs/architecture-review.md §3.1
// principle 6): the shell's chrome geometry as a pure value.

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

void Performance() {
  std::printf("performance: sidebar stack, nav trio on the caption band\n");
  const ShellLayout l = ComputeShellLayout(Base(PhiLayoutMode::kPerformance));
  CHECK(!l.horizontal_tabs);
  CHECK((l.title_bar == SidebarRect{0, 0, 1440, 32}));
  CHECK((l.caption_close == SidebarRect{1440 - 46, 0, 1440, 32}));
  CHECK((l.sidebar == SidebarRect{0, 0, 240, 900}));
  CHECK(l.top_bar.empty());
  // The card: flush against the sidebar, 4 under the band, 8 right/bottom.
  CHECK((l.content == SidebarRect{240, 36, 1432, 892}));
  // The nav trio trails the band before search-tabs, which sits one gap
  // before minimize; the menu titles stop at the trio's leading edge.
  CHECK(l.search_tabs.right == l.caption_min.left - 16);
  CHECK(l.reload.right == l.search_tabs.left - 8);
  CHECK(l.forward.right == l.reload.left - 1);
  CHECK(l.back.right == l.forward.left - 1);
  CHECK(l.band_limit == l.back.left);
  CHECK(l.band_divider.left > l.search_tabs.right &&
        l.band_divider.right < l.caption_min.left);
  // The Spaces strip at the sidebar's inset, the address bar under it.
  CHECK((l.spaces_strip == SidebarRect{0, 40, 240, 72}));
  CHECK(l.chip_size == 26);
  CHECK((l.url_field == SidebarRect{8, 32 + 52, 232, 32 + 84}));
  // The capsule's trailing stack: extensions then copy-link, no reader.
  CHECK(l.extensions.right == l.url_field.right - 2);
  CHECK(l.copy_link.right == l.extensions.left - 2);
  CHECK(l.reader.empty());
  CHECK(l.field_ext_strip.empty());
  // No pins, no bookmarks: the "+ New Tab" row sits right at tabs_top.
  CHECK(l.favorites.empty());
  CHECK(l.bookmark_separator.empty());
  CHECK(l.new_tab_row.top == 32 + 88 + 5);
  CHECK(l.tabs_top == l.new_tab_row.bottom + 2);
  CHECK(l.tabs_left == 8);
  CHECK((l.bottom_bar == SidebarRect{0, 900 - 8 - 24, 240, 900 - 8}));
  CHECK(l.tabs_limit == l.bottom_bar.top - 3);
  CHECK(l.downloads.left == 8);
  CHECK(l.memory.empty());  // AI suite off.
  CHECK(l.chat.empty());
  CHECK(l.feedback.right == 232);
}

void PerformanceExtras() {
  std::printf("performance: reader slot, extension strip, bands, bookmark bar\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kPerformance);
  in.reader_offerable = true;
  in.field_ext_count = 2;
  in.pinned_row_count = 2;
  in.bookmark_row_count = 3;
  in.bookmark_bar = true;
  const ShellLayout l = ComputeShellLayout(in);
  CHECK(l.reader.right == l.copy_link.left - 2);
  // With the reader slot up, 240dip leaves the text 74px: the icons hide
  // (the Mac's 84pt minimumAddressTextWidth). Without it they fit.
  CHECK(l.field_ext_strip.empty());
  ShellLayoutInputs no_reader = in;
  no_reader.reader_offerable = false;
  const ShellLayout roomy = ComputeShellLayout(no_reader);
  CHECK(!roomy.field_ext_strip.empty());
  CHECK(roomy.field_ext_strip.right == roomy.copy_link.left - 2);
  CHECK(roomy.field_ext_strip.right - roomy.field_ext_strip.left ==
        2 * 24 + 2);
  // Two pinned rows: 8 + 45 + 8 + 45 + 8.
  CHECK(l.favorites.top == 32 + 93);
  CHECK(l.favorites.bottom - l.favorites.top == 8 + 45 + 8 + 45 + 8);
  // Three bookmark rows after the 3pt spacer, then the 16pt separator.
  CHECK(l.bookmark_separator.top == l.favorites.bottom + 3 + 3 * 38);
  CHECK(l.bookmark_separator.bottom - l.bookmark_separator.top == 16);
  CHECK(l.new_tab_row.top == l.bookmark_separator.bottom);
  // The bookmark bar takes the content's first 32pt band.
  CHECK((l.bookmark_bar == SidebarRect{240, 36, 1432, 68}));
  CHECK(l.content.top == 68);
  // A narrow field hides the extension strip rather than crushing the text.
  no_reader.sidebar_dip = 193;
  const ShellLayout narrow = ComputeShellLayout(no_reader);
  CHECK(narrow.field_ext_strip.empty());
}

void SingleSpace() {
  std::printf("single space: no strip row, no compact pip; bands reclaim it\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kPerformance);
  in.spaces_switch = false;
  ShellLayout l = ComputeShellLayout(in);
  CHECK(l.spaces_strip.empty() && l.add_space.empty());
  CHECK(!l.spaces_switch);
  // The address bar re-pins to the card's top edge (52 - 48 = 4, the
  // content gap) and the header shrinks by the same 48.
  CHECK((l.url_field == SidebarRect{8, 32 + 4, 232, 32 + 36}));
  CHECK(l.url_field.top == l.content.top);
  CHECK(l.new_tab_row.top == 32 + 88 + 5 - 48);
  in = Base(PhiLayoutMode::kBalanced);
  in.spaces_switch = false;
  l = ComputeShellLayout(in);
  CHECK(l.spaces_strip.empty());
  CHECK(l.new_tab_row.top == 36);  // The first row on the card's top edge.
  in.pinned_row_count = 1;
  l = ComputeShellLayout(in);
  CHECK(l.favorites.top == 36 - 8);  // The grid's inset: cells at the edge.
  in = Base(PhiLayoutMode::kComfortable);
  in.spaces_switch = false;
  l = ComputeShellLayout(in);
  CHECK(l.chips_limit == l.chips_left);  // No pip.
  CHECK(l.tabs_left == l.phi_menu.right + 8);
  // The cache key sees the flag.
  ShellLayoutInputs a = Base(PhiLayoutMode::kPerformance);
  ShellLayoutInputs b = a;
  b.spaces_switch = false;
  CHECK(a != b);
}

void Balanced() {
  std::printf("balanced: top bar with the address field, strip on its line\n");
  const ShellLayout l = ComputeShellLayout(Base(PhiLayoutMode::kBalanced));
  CHECK(!l.horizontal_tabs);
  CHECK(l.title_bar.bottom == 32);
  // Search-tabs on the band, the trio on the top bar.
  CHECK(l.search_tabs.right == l.caption_min.left - 16);
  CHECK(l.band_limit == l.search_tabs.left);
  CHECK((l.top_bar == SidebarRect{240, 36, 1432, 68}));
  CHECK(l.back.left == 240 + 16);
  CHECK(l.forward.left == l.back.right + 8);
  CHECK(l.reload.left == l.forward.right + 8);
  // The strip shares the card's top edge.
  CHECK(l.spaces_strip.top == l.top_bar.top);
  CHECK((l.content == SidebarRect{240, 68, 1432, 892}));
  // The field runs from the trio to the bar's trailing pad (no chat).
  CHECK(l.url_field.left == l.reload.right + 6);
  CHECK(l.url_field.right == 1432 - 16);
  CHECK(l.extensions.right == l.url_field.right - 2);
  CHECK(l.copy_link.empty());
  CHECK(l.tabs_top > l.spaces_strip.bottom);
  CHECK(l.chat.empty());
  ShellLayoutInputs ai = Base(PhiLayoutMode::kBalanced);
  ai.ai_enabled = true;
  const ShellLayout with_ai = ComputeShellLayout(ai);
  CHECK(!with_ai.chat.empty());
  CHECK(with_ai.chat.top >= with_ai.top_bar.top);  // Header chat, not bottom.
  CHECK(with_ai.url_field.right == with_ai.chat.left - 8);
  CHECK(!with_ai.memory.empty());
}

void Comfortable() {
  std::printf("comfortable: tab strip on a taller band, page edge to edge\n");
  const ShellLayout l = ComputeShellLayout(Base(PhiLayoutMode::kComfortable));
  CHECK(l.horizontal_tabs);
  CHECK((l.title_bar == SidebarRect{0, 0, 1440, 40}));
  CHECK(l.band_limit == 0);  // No menu titles: the overflow is the way in.
  CHECK(l.sidebar.empty());
  CHECK((l.top_bar == SidebarRect{0, 40, 1440, 80}));
  CHECK((l.content == SidebarRect{0, 80, 1440, 900}));
  CHECK(!l.phi_menu.empty() && l.phi_menu.left == 8);
  CHECK(l.chip_size == 24);
  CHECK(l.chips_left == l.phi_menu.right + 8);
  CHECK(l.chips_limit == l.chips_left + 24);
  CHECK(l.tabs_left == l.chips_limit + 8);
  CHECK(l.tabs_top == 6);
  CHECK(l.tabs_limit == l.search_tabs.left - 8 - 24 - 8);
  CHECK(!l.overflow.empty() && l.overflow.right == 1440 - 16);
  CHECK(l.downloads.right == l.overflow.left - 8);
  CHECK(l.extensions.right == l.downloads.left - 8);
  CHECK(l.url_field.right == l.extensions.left - 8);
  CHECK(!l.page_icon.empty());
  CHECK(l.spaces_strip.empty());
  CHECK(l.new_tab_row.empty());
  CHECK(l.bottom_bar.empty());
  CHECK(l.reader.empty());
  ShellLayoutInputs reading = Base(PhiLayoutMode::kComfortable);
  reading.reader_offerable = true;
  const ShellLayout r = ComputeShellLayout(reading);
  CHECK(!r.reader.empty() && r.reader.right == r.url_field.right - 2);
}

void Collapsed() {
  std::printf("collapsed: the card widens to the margin, the bands empty\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kPerformance);
  in.sidebar_hidden = true;
  const ShellLayout l = ComputeShellLayout(in);
  CHECK((l.content == SidebarRect{8, 36, 1432, 892}));
  CHECK(l.spaces_strip.empty());
  CHECK(l.url_field.empty());
  CHECK(l.favorites.empty());
  CHECK(l.new_tab_row.empty());
  CHECK(l.bottom_bar.empty());
  CHECK(l.downloads.empty() && l.feedback.empty() && l.extensions.empty());
  CHECK(l.tabs_limit == l.tabs_top);  // No rows.
  // Balanced keeps its top bar, pushed to the margin.
  in.mode = PhiLayoutMode::kBalanced;
  const ShellLayout b = ComputeShellLayout(in);
  CHECK(b.top_bar.left == 8);
  CHECK(b.content.left == 8);
  CHECK(b.spaces_strip.empty());
  // The floating panel asks for the expanded layout of a collapsed shell:
  // the very same inputs with sidebar_hidden cleared.
  in.mode = PhiLayoutMode::kPerformance;
  in.sidebar_hidden = false;
  const ShellLayout expanded = ComputeShellLayout(in);
  CHECK(expanded.content.left == 240);
  CHECK(!expanded.url_field.empty());
}

void Fullscreen() {
  std::printf("content fullscreen: the child owns every pixel\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kBalanced);
  in.content_fullscreen = true;
  const ShellLayout l = ComputeShellLayout(in);
  CHECK((l.content == l.client));
  CHECK(l.title_bar.empty() && l.sidebar.empty() && l.top_bar.empty());
  CHECK(l.url_field.empty() && l.caption_close.empty());
}

void Dpi() {
  std::printf("dpi: every rect scales with the monitor, 150%% = 1.5x\n");
  const ShellLayout a = ComputeShellLayout(Base(PhiLayoutMode::kPerformance));
  const ShellLayout b =
      ComputeShellLayout(Base(PhiLayoutMode::kPerformance, 144));
  CHECK(b.title_bar.bottom == 48);
  CHECK(b.sidebar.right == 360);
  CHECK(b.caption_close.right - b.caption_close.left == 69);
  CHECK(b.content.left == 360 && b.content.top == 54);
  CHECK(b.url_field.left == 12 && b.url_field.top == 48 + 78);
  CHECK(b.new_tab_row.top == 48 + 132 + 8);  // 88 → 132, 5 → 8 (7.5 up).
  CHECK(b.bottom_bar.bottom - b.bottom_bar.top == 36);
  CHECK(b.tabs_left == 12);
  // The same inputs at 96 and 144 differ only by the scale.
  CHECK(a.reload.right - a.reload.left == 24);
  CHECK(b.reload.right - b.reload.left == 36);
  const ShellLayout c =
      ComputeShellLayout(Base(PhiLayoutMode::kComfortable, 144));
  CHECK(c.title_bar.bottom == 60);
  CHECK(c.top_bar.bottom == 120);
  CHECK(c.tabs_top == 9);
}

void InputsKey() {
  std::printf("inputs: the cache key sees every field\n");
  const ShellLayoutInputs a = Base(PhiLayoutMode::kPerformance);
  ShellLayoutInputs b = a;
  CHECK(a == b);
  b.reader_offerable = true;
  CHECK(a != b);
  b = a;
  b.client_height += 1;
  CHECK(a != b);
  b = a;
  b.sidebar_hidden = true;
  CHECK(a != b);
  b = a;
  b.bookmark_row_count = 1;
  CHECK(a != b);
}

void DropZones() {
  std::printf("drop zones: empty bands reserve room only while asked\n");
  ShellLayoutInputs in = Base(PhiLayoutMode::kPerformance);
  const ShellLayout none = ComputeShellLayout(in);
  CHECK(none.favorites.empty() && none.bookmarks.empty());
  CHECK(none.bookmark_separator.empty());
  // A drag in flight over an empty profile: the Mac's 100pt drag-time
  // pinned container, then the 3pt spacer, one outline row (38) and the
  // 16pt separator, all before "+ New Tab".
  in.pinned_drop_zone = true;
  in.bookmark_drop_zone = true;
  const ShellLayout zones = ComputeShellLayout(in);
  CHECK(zones.favorites.top == 32 + 93);
  CHECK(zones.favorites.bottom - zones.favorites.top == 100);
  CHECK((zones.bookmarks == SidebarRect{8, zones.favorites.bottom + 3, 232,
                                        zones.favorites.bottom + 3 + 38 + 16}));
  CHECK(zones.bookmark_separator.top == zones.bookmarks.top + 38);
  CHECK(zones.bookmark_separator.bottom == zones.bookmarks.bottom);
  CHECK(zones.new_tab_row.top == zones.bookmarks.bottom);
  // Bands with rows of their own ignore the flags: the outline band spans
  // its rows and the separator.
  in.pinned_row_count = 1;
  in.bookmark_row_count = 2;
  const ShellLayout rows = ComputeShellLayout(in);
  CHECK(rows.favorites.bottom - rows.favorites.top == 8 + 45 + 8);
  CHECK(rows.bookmarks.top == rows.favorites.bottom + 3);
  CHECK(rows.bookmarks.bottom - rows.bookmarks.top == 2 * 38 + 16);
  CHECK(rows.bookmark_separator.bottom == rows.bookmarks.bottom);
  // Collapsed: nothing is reserved.
  in.sidebar_hidden = true;
  const ShellLayout hidden = ComputeShellLayout(in);
  CHECK(hidden.favorites.empty() && hidden.bookmarks.empty());
  // The cache key sees the flags.
  ShellLayoutInputs a = Base(PhiLayoutMode::kPerformance);
  ShellLayoutInputs b = a;
  b.pinned_drop_zone = true;
  CHECK(a != b);
  b = a;
  b.bookmark_drop_zone = true;
  CHECK(a != b);
}

}  // namespace

int main() {
  SingleSpace();
  Scaling();
  Performance();
  PerformanceExtras();
  DropZones();
  Balanced();
  Comfortable();
  Collapsed();
  Fullscreen();
  Dpi();
  InputsKey();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("shell layout: all passed\n");
  return 0;
}
