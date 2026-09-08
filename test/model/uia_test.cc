// Host tests for the pure half of the accessibility provider (phi_uia.h,
// docs/architecture-review.md §3.6 and §4 item 31 / lane3-ui-substrate.md
// §2 "Cross-cutting absences", §6.1, ranked finding 2): the role →
// control-type table, the pattern set each role genuinely has, the
// client → screen rect transform, identity across a hit-list rebuild, the
// point an Invoke clicks at, and the version/event decision table.
//
// These are the parts that decide what a screen reader is TOLD. The COM
// plumbing that carries the answer (IRawElementProviderSimple, the fragment
// root, UiaReturnRawElementProvider) needs a Windows message loop and is
// checked on the box with test/uia-probe.ps1 or Inspect.exe.

#include <cstdio>
#include <string>
#include <vector>

#include "phi_uia.h"
#include "phi_window.h"

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

PhiHit Hit(int left,
           int top,
           int right,
           int bottom,
           int key,
           const wchar_t* name,
           PhiHitRole role) {
  PhiHit hit;
  hit.bounds.left = left;
  hit.bounds.top = top;
  hit.bounds.right = right;
  hit.bounds.bottom = bottom;
  hit.key = key;
  hit.name = name;
  hit.role = role;
  return hit;
}

// THE table. Every role the substrate defines has a row here, so adding an
// enumerator without deciding what it announces fails this test.
void ControlTypes() {
  std::printf("roles: every PhiHitRole maps to one UIA control type\n");
  CHECK(PhiUiaControlType(PhiHitRole::kButton) == 50000);    // Button
  CHECK(PhiUiaControlType(PhiHitRole::kToggle) == 50002);    // CheckBox
  CHECK(PhiUiaControlType(PhiHitRole::kListItem) == 50007);  // ListItem
  CHECK(PhiUiaControlType(PhiHitRole::kTab) == 50019);       // TabItem
  CHECK(PhiUiaControlType(PhiHitRole::kText) == 50020);      // Text
  CHECK(PhiUiaControlType(PhiHitRole::kImage) == 50006);     // Image
  CHECK(PhiUiaControlType(PhiHitRole::kSlider) == 50015);    // Slider
  CHECK(PhiUiaControlType(PhiHitRole::kGroup) == 50026);     // Group
  CHECK(PhiUiaControlType(PhiHitRole::kNone) == 50025);      // Custom

  // The constants the client build static_asserts against the SDK.
  CHECK(PhiUiaControlType(PhiHitRole::kButton) == kPhiUiaButtonControlType);
  CHECK(PhiUiaControlType(PhiHitRole::kGroup) == kPhiUiaGroupControlType);
}

void Patterns() {
  std::printf("patterns: offered only where the hit list carries the state\n");
  CHECK(PhiUiaPatterns(PhiHitRole::kButton) == kPhiUiaInvokePattern);
  CHECK(PhiUiaPatterns(PhiHitRole::kToggle) ==
        (kPhiUiaInvokePattern | kPhiUiaTogglePattern));
  CHECK(PhiUiaPatterns(PhiHitRole::kListItem) ==
        (kPhiUiaInvokePattern | kPhiUiaSelectionItemPattern));
  CHECK(PhiUiaPatterns(PhiHitRole::kTab) ==
        (kPhiUiaInvokePattern | kPhiUiaSelectionItemPattern));

  // Announced, never actioned: a label, a favicon and the box drawn round a
  // group are not things a user can press.
  CHECK(PhiUiaPatterns(PhiHitRole::kText) == kPhiUiaNoPattern);
  CHECK(PhiUiaPatterns(PhiHitRole::kImage) == kPhiUiaNoPattern);
  CHECK(PhiUiaPatterns(PhiHitRole::kGroup) == kPhiUiaNoPattern);
  CHECK(PhiUiaPatterns(PhiHitRole::kNone) == kPhiUiaNoPattern);
  // A slider deliberately gets NO RangeValue: PhiHit carries no minimum,
  // maximum or value, and a range pattern over invented numbers would
  // announce a position the knob is not at.
  CHECK(PhiUiaPatterns(PhiHitRole::kSlider) == kPhiUiaNoPattern);

  // A group is a control element but not a content element, so a screen
  // reader's content walk reads the rows and not the box round them.
  CHECK(!PhiUiaIsContent(PhiHitRole::kGroup));
  CHECK(PhiUiaIsContent(PhiHitRole::kListItem));
  CHECK(PhiUiaIsContent(PhiHitRole::kText));
}

void Rects() {
  std::printf("rects: client to screen, and what counts as offscreen\n");
  PhiRectI row;
  row.left = 8;
  row.top = 40;
  row.right = 288;
  row.bottom = 76;
  // A popover whose client origin is at (1200, 300) on the desktop.
  const PhiRectI screen = PhiUiaToScreen(row, 1200, 300);
  CHECK(screen.left == 1208 && screen.top == 340);
  CHECK(screen.right == 1488 && screen.bottom == 376);
  CHECK(screen.width() == row.width() && screen.height() == row.height());
  // A negative origin (a window straddling the left edge of the desktop)
  // is just as valid.
  const PhiRectI negative = PhiUiaToScreen(row, -20, -10);
  CHECK(negative.left == -12 && negative.top == 30);

  PhiRectI client;
  client.right = 300;
  client.bottom = 400;
  CHECK(!PhiUiaIsOffscreen(row, client));
  // A row scrolled off the top of a popover's viewport: still in the tree
  // so a client can page to it, but reported offscreen.
  PhiRectI above = row;
  above.top = -80;
  above.bottom = -44;
  CHECK(PhiUiaIsOffscreen(above, client));
  // Touching the edge is not overlapping (half-open rects).
  PhiRectI flush = row;
  flush.top = 400;
  flush.bottom = 436;
  CHECK(PhiUiaIsOffscreen(flush, client));
  flush.top = 399;
  flush.bottom = 435;
  CHECK(!PhiUiaIsOffscreen(flush, client));
  // A degenerate hit is nowhere.
  CHECK(PhiUiaIsOffscreen(PhiRectI(), client));
}

void Identity() {
  std::printf("identity: a provider finds its hit again after a rebuild\n");
  std::vector<PhiHit> hits;
  hits.push_back(Hit(0, 0, 200, 30, 10, L"one", PhiHitRole::kListItem));
  hits.push_back(Hit(0, 30, 200, 60, 20, L"two", PhiHitRole::kListItem));
  hits.push_back(Hit(0, 60, 200, 90, 30, L"three", PhiHitRole::kListItem));

  // Nothing moved: the index is trusted and the lookup is O(1).
  CHECK(PhiUiaResolveHit(hits, 1, 20) == 1);
  // The row moved up because the one above it was removed.
  std::vector<PhiHit> after;
  after.push_back(hits[1]);
  after.push_back(hits[2]);
  CHECK(PhiUiaResolveHit(after, 1, 20) == 0);
  CHECK(PhiUiaResolveHit(after, 2, 30) == 1);
  // The row is gone.
  CHECK(PhiUiaResolveHit(after, 0, 10) == -1);
  // An index past the end never reads out of range.
  CHECK(PhiUiaResolveHit(after, 99, 30) == 1);
  CHECK(PhiUiaResolveHit(std::vector<PhiHit>(), 0, 10) == -1);
}

void InvokePoints() {
  std::printf("invoke: the point a synthesised click actually lands on\n");
  std::vector<PhiHit> hits;
  // A downloads-popover row with a "Show in folder" button drawn on top of
  // it — the row is added first, the button second, so the button wins the
  // pointer wherever they overlap (PhiHitAt: last match wins).
  hits.push_back(Hit(0, 0, 300, 40, 100, L"report.pdf", PhiHitRole::kListItem));
  hits.push_back(Hit(200, 8, 292, 32, 101, L"Show in folder",
                     PhiHitRole::kButton));

  int x = 0;
  int y = 0;
  CHECK(PhiUiaInvokePoint(hits, 1, &x, &y));
  CHECK(x == 246 && y == 20);  // The button's own centre.
  CHECK(PhiHitAt(hits, x, y) == 1);

  CHECK(PhiUiaInvokePoint(hits, 0, &x, &y));
  CHECK(PhiHitAt(hits, x, y) == 0);  // Not swallowed by the button.

  // A row whose MIDDLE is covered: the centre resolves to the child, so the
  // scan has to find somewhere else inside the row.
  std::vector<PhiHit> covered;
  covered.push_back(Hit(0, 0, 300, 40, 200, L"row", PhiHitRole::kListItem));
  covered.push_back(Hit(100, 0, 200, 40, 201, L"chip", PhiHitRole::kButton));
  CHECK(PhiHitAt(covered, 150, 20) == 1);
  CHECK(PhiUiaInvokePoint(covered, 0, &x, &y));
  CHECK(PhiHitAt(covered, x, y) == 0);

  // Completely covered: a real click cannot reach it either, so Invoke must
  // fail rather than pretend it did something.
  std::vector<PhiHit> hidden;
  hidden.push_back(Hit(0, 0, 100, 20, 300, L"under", PhiHitRole::kButton));
  hidden.push_back(Hit(0, 0, 100, 20, 301, L"over", PhiHitRole::kButton));
  CHECK(!PhiUiaInvokePoint(hidden, 0, &x, &y));
  CHECK(PhiUiaInvokePoint(hidden, 1, &x, &y));

  // Disabled and out-of-range hits.
  std::vector<PhiHit> disabled;
  disabled.push_back(Hit(0, 0, 100, 20, 400, L"Paste", PhiHitRole::kButton));
  disabled[0].enabled = false;
  CHECK(!PhiUiaInvokePoint(disabled, 0, &x, &y));
  CHECK(!PhiUiaInvokePoint(disabled, 1, &x, &y));
  CHECK(!PhiUiaInvokePoint(disabled, -1, &x, &y));
}

// The event decision table (phi_uia.h): what a hits_ rebuild has to be
// announced as. Getting this wrong is either a screen reader stuck on a
// stale tree or one flooded with structure changes per repaint.
void Events() {
  std::printf("events: the version/event decision table\n");
  std::vector<PhiHit> hits;
  hits.push_back(Hit(0, 0, 200, 30, 10, L"Downloads", PhiHitRole::kListItem));
  hits.push_back(Hit(0, 30, 200, 60, 20, L"Pin", PhiHitRole::kToggle));
  const std::vector<PhiUiaHitState> before = PhiUiaSnapshot(hits);
  CHECK(before.size() == 2);
  CHECK(before[1].role == PhiHitRole::kToggle);

  // A rebuild that produced the same list — the common case for a repaint
  // — raises nothing at all.
  {
    const PhiUiaChanges same = PhiUiaDiff(before, PhiUiaSnapshot(hits));
    CHECK(!same.any());
    CHECK(!PhiUiaShouldRaise(true, same));
  }

  // A name changed in place: one property-changed event, no structure.
  {
    std::vector<PhiHit> renamed = hits;
    renamed[1].name = L"Unpin";
    const PhiUiaChanges change = PhiUiaDiff(before, PhiUiaSnapshot(renamed));
    CHECK(!change.structure);
    CHECK(change.renamed.size() == 1 && change.renamed[0] == 1);
    CHECK(change.reselected.empty() && change.reenabled.empty());
    CHECK(PhiUiaShouldRaise(true, change));
  }

  // Selection moved.
  {
    std::vector<PhiHit> selected = hits;
    selected[0].selected = true;
    const PhiUiaChanges change = PhiUiaDiff(before, PhiUiaSnapshot(selected));
    CHECK(!change.structure);
    CHECK(change.reselected.size() == 1 && change.reselected[0] == 0);
    CHECK(change.renamed.empty());
  }

  // Enablement flipped (a download that finished, a paste with an empty
  // clipboard).
  {
    std::vector<PhiHit> enabled = hits;
    enabled[1].enabled = false;
    const PhiUiaChanges change = PhiUiaDiff(before, PhiUiaSnapshot(enabled));
    CHECK(!change.structure);
    CHECK(change.reenabled.size() == 1 && change.reenabled[0] == 1);
  }

  // More than one property at once.
  {
    std::vector<PhiHit> both = hits;
    both[0].name = L"Downloads (2)";
    both[1].selected = true;
    const PhiUiaChanges change = PhiUiaDiff(before, PhiUiaSnapshot(both));
    CHECK(!change.structure);
    CHECK(change.renamed.size() == 1 && change.reselected.size() == 1);
  }

  // A row appeared: the tree changed shape.
  {
    std::vector<PhiHit> grown = hits;
    grown.push_back(Hit(0, 60, 200, 90, 30, L"New", PhiHitRole::kListItem));
    const PhiUiaChanges change = PhiUiaDiff(before, PhiUiaSnapshot(grown));
    CHECK(change.structure);
    CHECK(change.renamed.empty());
    CHECK(PhiUiaShouldRaise(true, change));
  }

  // A DIFFERENT element at the same position (the keys moved): structure,
  // and the per-element lists are dropped — they would be about elements
  // the client has already thrown away.
  {
    std::vector<PhiHit> reordered;
    reordered.push_back(hits[1]);
    reordered.push_back(hits[0]);
    const PhiUiaChanges change = PhiUiaDiff(before, PhiUiaSnapshot(reordered));
    CHECK(change.structure);
    CHECK(change.renamed.empty());
    CHECK(change.reselected.empty());
    CHECK(change.reenabled.empty());
  }

  // Same key, different role — a row that became a group header.
  {
    std::vector<PhiHit> retyped = hits;
    retyped[0].role = PhiHitRole::kGroup;
    const PhiUiaChanges change = PhiUiaDiff(before, PhiUiaSnapshot(retyped));
    CHECK(change.structure);
  }

  // The gate: nothing is raised into an empty room, however much changed.
  {
    std::vector<PhiHit> grown = hits;
    grown.push_back(Hit(0, 60, 200, 90, 30, L"New", PhiHitRole::kListItem));
    const PhiUiaChanges change = PhiUiaDiff(before, PhiUiaSnapshot(grown));
    CHECK(change.any());
    CHECK(!PhiUiaShouldRaise(false, change));
  }
  CHECK(!PhiUiaShouldRaise(true, PhiUiaChanges()));
  CHECK(!PhiUiaShouldRaise(false, PhiUiaChanges()));

  // An emptied list (a popover that cleared its rows) is a structure change,
  // and so is the first list after an empty one.
  {
    const PhiUiaChanges cleared =
        PhiUiaDiff(before, PhiUiaSnapshot(std::vector<PhiHit>()));
    CHECK(cleared.structure);
    const PhiUiaChanges filled =
        PhiUiaDiff(PhiUiaSnapshot(std::vector<PhiHit>()), before);
    CHECK(filled.structure);
  }
}

// What a screen reader ends up hearing for a real surface: the downloads
// popover's hit list, read through the same functions the provider uses.
void PopoverTree() {
  std::printf("tree: a popover's hit list as an announced tree\n");
  std::vector<PhiHit> hits;
  hits.push_back(Hit(8, 40, 288, 76, 0, L"report.pdf", PhiHitRole::kListItem));
  hits.push_back(Hit(200, 46, 244, 70, 1, L"Open", PhiHitRole::kButton));
  hits.push_back(Hit(244, 46, 288, 70, 2, L"Show in folder",
                     PhiHitRole::kButton));
  hits.push_back(Hit(8, 84, 288, 112, 1000, L"All Downloads",
                     PhiHitRole::kButton));

  // Four children, every one of them named — an unnamed hit is invisible to
  // both the tooltip and the screen reader, and there are none here.
  CHECK(hits.size() == 4);
  for (const PhiHit& hit : hits) {
    CHECK(!hit.name.empty());
    CHECK(PhiUiaControlType(hit.role) != 0);
  }
  CHECK(PhiUiaControlType(hits[0].role) == kPhiUiaListItemControlType);
  CHECK(PhiUiaPatterns(hits[0].role) & kPhiUiaInvokePattern);
  CHECK(PhiUiaPatterns(hits[3].role) & kPhiUiaInvokePattern);

  // Every row can be reached by an Invoke, including the one two buttons
  // are drawn over.
  int x = 0;
  int y = 0;
  for (size_t i = 0; i < hits.size(); ++i) {
    CHECK(PhiUiaInvokePoint(hits, static_cast<int>(i), &x, &y));
    CHECK(PhiHitAt(hits, x, y) == static_cast<int>(i));
  }
}

}  // namespace

int main() {
  ControlTypes();
  Patterns();
  Rects();
  Identity();
  InvokePoints();
  Events();
  PopoverTree();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("uia: all passed\n");
  return 0;
}
