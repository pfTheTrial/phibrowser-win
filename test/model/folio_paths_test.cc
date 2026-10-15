// Host tests for Folio's pure rules (docs/folio-port.md): the broker's one
// file-name rule, the listing's frontmatter cut, the UTF-8 boundary trim,
// the free-basename bump, the highlights check Undo yields to, and the
// saveable-URL gate. Run with test-host.sh.

#include <cstdio>
#include <set>
#include <string>

#include "phibrowser-win/client/phi_folio_paths.h"

using namespace phi_client;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

void TestBrokerFileName() {
  CHECK(PhiFolioBrokerFileName("Article.md"));
  CHECK(PhiFolioBrokerFileName("Article.mhtml"));
  // A title ending in a period is an ordinary file, not traversal.
  CHECK(PhiFolioBrokerFileName("Worth knowing..md"));
  CHECK(PhiFolioBrokerFileName("Wait... what?.md"));
  CHECK(!PhiFolioBrokerFileName(""));
  CHECK(!PhiFolioBrokerFileName(".md"));
  CHECK(!PhiFolioBrokerFileName(".folio-part-x"));
  CHECK(!PhiFolioBrokerFileName("../x.md"));
  CHECK(!PhiFolioBrokerFileName("a/b.md"));
  CHECK(!PhiFolioBrokerFileName("a\\b.md"));
  CHECK(!PhiFolioBrokerFileName("C:x.md"));
  CHECK(!PhiFolioBrokerFileName("x.md:stream"));
  CHECK(!PhiFolioBrokerFileName(std::string("x\0y.md", 6)));
  CHECK(!PhiFolioBrokerFileName("notes.txt"));
  CHECK(!PhiFolioBrokerFileName("x.md.exe"));
}

void TestFrontmatter() {
  CHECK(PhiFolioFrontmatterPrefix("---\nsource: a\n---\nbody") ==
        "---\nsource: a\n---\n");
  CHECK(PhiFolioFrontmatterPrefix("---\n---\nbody") == "---\n---\n");
  CHECK(PhiFolioFrontmatterPrefix("---\nsource: a\nunterminated").empty());
  CHECK(PhiFolioFrontmatterPrefix("# Title\n---\n").empty());
  CHECK(PhiFolioFrontmatterPrefix("").empty());
}

void TestUtf8Trim() {
  const std::string euro = "\xE2\x82\xAC";  // U+20AC, three bytes.
  CHECK(PhiFolioTrimToUtf8Boundary("abc") == "abc");
  CHECK(PhiFolioTrimToUtf8Boundary("ab" + euro) == "ab" + euro);
  CHECK(PhiFolioTrimToUtf8Boundary("ab" + euro.substr(0, 2)) == "ab");
  CHECK(PhiFolioTrimToUtf8Boundary("ab" + euro.substr(0, 1)) == "ab");
  const std::string emoji = "\xF0\x9F\x98\x80";  // Four bytes.
  CHECK(PhiFolioTrimToUtf8Boundary("x" + emoji.substr(0, 3)) == "x");
  CHECK(PhiFolioTrimToUtf8Boundary("x" + emoji) == "x" + emoji);
  CHECK(PhiFolioTrimToUtf8Boundary("").empty());
}

void TestFreeBasename() {
  std::set<std::string> taken = {"Page", "Page 2"};
  const auto is_taken = [&](const std::string& name) {
    return taken.count(name) != 0;
  };
  CHECK(PhiFolioFirstFreeBasename("Other", is_taken) == "Other");
  CHECK(PhiFolioFirstFreeBasename("Page", is_taken) == "Page 3");
}

void TestHighlights() {
  CHECK(!PhiFolioHasUserHighlights("# A\n\nbody\n"));
  CHECK(PhiFolioHasUserHighlights(
      "# A\n\nbody\n## Highlights\n\n### Highlight 1\n> q\n"));
  // An article's own "Highlights" heading with no entries is not the user's.
  CHECK(!PhiFolioHasUserHighlights("# A\n## Highlights\nThe best bits.\n"));
}

void TestSaveableUrl() {
  CHECK(PhiFolioIsSaveableUrl("https://example.com/"));
  CHECK(PhiFolioIsSaveableUrl("HTTP://example.com/"));
  CHECK(!PhiFolioIsSaveableUrl("chrome://newtab/"));
  CHECK(!PhiFolioIsSaveableUrl("chrome-extension://abc/reader.html"));
  CHECK(!PhiFolioIsSaveableUrl("file:///C:/x.html"));
  CHECK(!PhiFolioIsSaveableUrl(""));
}

}  // namespace

int main() {
  TestBrokerFileName();
  TestFrontmatter();
  TestUtf8Trim();
  TestFreeBasename();
  TestHighlights();
  TestSaveableUrl();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("folio_paths_test: all checks passed\n");
  return 0;
}
