// Host tests for phi_sidebar_layout (docs/sidebar-architecture.md §8).

#include <cstdio>
#include <vector>

#include "phi_sidebar_layout.h"

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

// 240dip sidebar at 100%: the Mac's default. kEdgeInset 8, kPinnedGap 8,
// kPinnedMinWidth 53, kPinnedCellHeight 45.
PinnedGridMetrics Grid240() {
  PinnedGridMetrics m;
  m.sidebar_dip = 240;
  m.edge_inset_dip = 8;
  m.gap_dip = 8;
  m.min_cell_width_dip = 53;
  m.origin_x = 8;
  m.origin_y = 100;
  m.sidebar_px = 240;
  m.edge_inset_px = 8;
  m.gap_px = 8;
  m.cell_height_px = 45;
  return m;
}

void ColumnRules() {
  std::printf("pinned grid: breakpoints, shrink to the 53dip floor\n");
  CHECK(PinnedGridColumnLimit(240) == 3);   // 232 < 236
  CHECK(PinnedGridColumnLimit(244) == 4);   // 236
  CHECK(PinnedGridColumnLimit(302) == 5);
  CHECK(PinnedGridColumnLimit(360) == 6);
  PinnedGridMetrics m = Grid240();
  CHECK(PinnedGridColumns(m, 1) == 1);  // A lone pin spans the sidebar.
  CHECK(PinnedGridColumns(m, 2) == 2);
  CHECK(PinnedGridColumns(m, 9) == 3);  // Limit.
  m.sidebar_dip = 193;                  // Narrowest: (177-16)/3 = 53 fits.
  CHECK(PinnedGridColumns(m, 9) == 3);
  m.sidebar_dip = 190;                  // (174-16)/3 = 52 < 53 -> 2.
  CHECK(PinnedGridColumns(m, 9) == 2);
}

void GridPlacement() {
  std::printf("pinned grid: stretch, wrap, split cells span two units\n");
  const PinnedGridMetrics m = Grid240();
  PinnedGridPlacement one = LayoutPinnedGrid({1}, m);
  CHECK(one.rows == 1 && one.columns == 1);
  CHECK(((one.cells[0]) == SidebarRect{8, 108, 8 + 224, 108 + 45}));  // Full row.
  PinnedGridPlacement two = LayoutPinnedGrid({1, 1}, m);
  CHECK(two.columns == 2 && two.rows == 1);
  CHECK(two.cells[0].right < two.cells[1].left);
  CHECK(two.cells[1].right == 8 + 224);
  // Three columns: four cells wrap to two rows.
  PinnedGridPlacement four = LayoutPinnedGrid({1, 1, 1, 1}, m);
  CHECK(four.columns == 3 && four.rows == 2);
  CHECK(four.cells[3].top == four.cells[0].top + 45 + 8);
  CHECK(four.cells[3].left == four.cells[0].left);
  // A split cell after two singles does not fit the row: it wraps whole.
  PinnedGridPlacement split = LayoutPinnedGrid({1, 1, 2}, m);
  CHECK(split.rows == 2);
  CHECK(split.cells[2].top == split.cells[0].top + 45 + 8);
  CHECK(split.cells[2].right - split.cells[2].left >
        2 * (split.cells[0].right - split.cells[0].left));
  // Empty.
  CHECK(LayoutPinnedGrid({}, m).rows == 0);
}

void ListBasics() {
  std::printf("list: rows and headers pitch, hidden rows take no room\n");
  ListMetrics m;
  m.left = 8;
  m.right = 232;
  m.top = 300;
  m.row_height = 36;
  m.row_gap = 2;
  m.header_height = 28;
  std::vector<ListItem> items = {{ListItem::Kind::kHeader, false},
                                 {ListItem::Kind::kRow, false},
                                 {ListItem::Kind::kRow, true},
                                 {ListItem::Kind::kRow, false}};
  const ListPlacement p = LayoutList(items, m);
  CHECK(p.rects.size() == 4);
  CHECK(((p.rects[0]) == SidebarRect{8, 300, 232, 328}));
  CHECK(((p.rects[1]) == SidebarRect{8, 330, 232, 366}));
  CHECK(p.rects[2].empty());
  CHECK(((p.rects[3]) == SidebarRect{8, 368, 232, 404}));
  CHECK(p.bottom == 406);
  CHECK(p.content_height == 30 + 38 + 38);
  CHECK(p.scroll_max == 0);  // Not clipping.

  // pad_below opens room after an item, before its gap: the group card's
  // inner insets. The item's own rect does not grow.
  items[0].pad_below = 2;
  items[3].pad_below = 4;
  const ListPlacement q = LayoutList(items, m);
  CHECK(((q.rects[0]) == SidebarRect{8, 300, 232, 328}));
  CHECK(((q.rects[1]) == SidebarRect{8, 332, 232, 368}));
  CHECK(((q.rects[3]) == SidebarRect{8, 370, 232, 406}));
  CHECK(q.bottom == 412);
  CHECK(q.content_height == 32 + 38 + 42);
}

void ListScrollAndClip() {
  std::printf("list: scroll offset, viewport clipping, scroll max\n");
  ListMetrics m;
  m.left = 0;
  m.right = 100;
  m.top = 100;
  m.row_height = 36;
  m.row_gap = 2;
  m.clip = true;
  m.viewport_top = 100;
  m.viewport_bottom = 200;  // Room for ~2.6 rows.
  std::vector<ListItem> items(5, ListItem{ListItem::Kind::kRow, false});
  ListPlacement p = LayoutList(items, m);
  CHECK(p.content_height == 5 * 38);
  CHECK(p.scroll_max == 5 * 38 - 100);
  CHECK(!p.rects[0].empty() && !p.rects[2].empty());  // Partially visible.
  CHECK(p.rects[3].empty() && p.rects[4].empty());
  m.scroll = p.scroll_max;
  p = LayoutList(items, m);
  CHECK(p.rects[0].empty());                       // Scrolled off the top.
  // Content counts the trailing gap, so the last row ends one gap above.
  CHECK(p.rects[4].bottom == m.viewport_bottom - m.row_gap);
  CHECK(p.unclipped[0].top == 100 - p.scroll_max);  // Unclipped stays placed.
}

void StripWidthsRule() {
  std::printf("strip widths: ideal while they fit, resting shrink first\n");
  StripWidths w = SolveStripWidths(3, true, 1000, 160, 200, 60);
  CHECK(w.rest == 160 && w.active == 200);
  w = SolveStripWidths(5, true, 700, 160, 200, 60);  // 5*160+200 > 700.
  CHECK(w.rest == 100 && w.active == 200);
  w = SolveStripWidths(8, true, 700, 160, 200, 60);  // Rest shrinks to 62.
  CHECK(w.rest == 62 && w.active == 200);
  w = SolveStripWidths(20, true, 700, 160, 200, 60);  // Both floored.
  CHECK(w.rest == 60 && w.active == 60);
  w = SolveStripWidths(0, false, 100, 160, 200, 60);
  CHECK(w.active == 0 && w.rest == 160);
}

// The Mac's TabGroupChipView constants at 100%.
StripChipMetrics Chip100() {
  StripChipMetrics m;
  m.leading = 6;
  m.dot = 16;
  m.dot_to_label = 6;
  m.label_slack = 4;
  m.label_right_pad = 6;
  m.count_to_label = 6;
  m.count_h_pad = 6;
  m.count_v_pad = 1;
  m.mosaic = 28;
  m.collapse_gap = 4;
  m.collapse_control = 16;
  m.max_width = 180;
  return m;
}

void StripChipWidthRule() {
  std::printf("strip chip: dot + label + badge|mosaic + chevron, capped\n");
  const StripChipMetrics m = Chip100();
  // Expanded, auto-named: no badge (the name carries the count).
  CHECK(SolveStripChipWidth(50, 7, false, false, m) == 108);
  // Expanded, user-titled: the badge (digits + 2 * 6).
  CHECK(SolveStripChipWidth(50, 7, false, true, m) == 133);
  // Collapsed: the mosaic, titled or not.
  CHECK(SolveStripChipWidth(50, 7, true, false, m) == 142);
  CHECK(SolveStripChipWidth(50, 7, true, true, m) == 142);
  // A long title truncates rather than growing past the cap.
  CHECK(SolveStripChipWidth(300, 7, false, false, m) == 180);
}

void StripChipPlacementRule() {
  std::printf("strip chip: parts placed from both edges, label fills\n");
  const StripChipMetrics m = Chip100();
  {
    const StripChipPlacement p =
        PlaceStripChip({0, 0, 108, 32}, 7, 10, false, false, m);
    CHECK((p.dot == SidebarRect{6, 8, 22, 24}));
    CHECK((p.collapse == SidebarRect{86, 8, 102, 24}));
    CHECK(p.count.empty() && p.mosaic.empty());
    CHECK((p.label == SidebarRect{28, 0, 82, 32}));
  }
  {
    const StripChipPlacement p =
        PlaceStripChip({0, 0, 133, 32}, 7, 10, false, true, m);
    CHECK((p.collapse == SidebarRect{111, 8, 127, 24}));
    CHECK((p.count == SidebarRect{88, 10, 107, 22}));  // 19 x 12
    CHECK((p.label == SidebarRect{28, 0, 82, 32}));
  }
  {
    const StripChipPlacement p =
        PlaceStripChip({0, 0, 142, 32}, 7, 10, true, true, m);
    CHECK(p.count.empty());
    CHECK((p.mosaic == SidebarRect{88, 2, 116, 30}));
    CHECK((p.label == SidebarRect{28, 0, 82, 32}));
  }
  // A chip narrower than its parts never hands the label a negative width.
  {
    const StripChipPlacement p =
        PlaceStripChip({0, 0, 40, 32}, 7, 10, false, false, m);
    CHECK(p.label.left == 28 && p.label.right == 28);
  }
}

void StripMosaicRule() {
  std::printf("strip mosaic: four 10pt cells, favicons then +N\n");
  const std::vector<SidebarRect> cells =
      StripMosaicCells({100, 2, 128, 30}, 10, 2, 3);
  CHECK(cells.size() == 4);
  CHECK((cells[0] == SidebarRect{103, 5, 113, 15}));
  CHECK((cells[1] == SidebarRect{115, 5, 125, 15}));
  CHECK((cells[2] == SidebarRect{103, 17, 113, 27}));
  CHECK((cells[3] == SidebarRect{115, 17, 125, 27}));
  std::vector<StripMosaicSlot> fill = StripMosaicFill(0);
  CHECK(fill.size() == 4 && fill[3].kind == StripMosaicCell::kEmpty);
  fill = StripMosaicFill(2);
  CHECK(fill[0].kind == StripMosaicCell::kFavicon && fill[0].favicon_index == 0);
  CHECK(fill[1].kind == StripMosaicCell::kFavicon && fill[1].favicon_index == 1);
  CHECK(fill[2].kind == StripMosaicCell::kEmpty);
  fill = StripMosaicFill(4);
  CHECK(fill[3].kind == StripMosaicCell::kFavicon && fill[3].favicon_index == 3);
  fill = StripMosaicFill(7);
  CHECK(fill[2].kind == StripMosaicCell::kFavicon);
  CHECK(fill[3].kind == StripMosaicCell::kOverflow && fill[3].overflow == 4);
}

}  // namespace

int main() {
  ColumnRules();
  GridPlacement();
  ListBasics();
  ListScrollAndClip();
  StripWidthsRule();
  StripChipWidthRule();
  StripChipPlacementRule();
  StripMosaicRule();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL PASS\n");
  return 0;
}
