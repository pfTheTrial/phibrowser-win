// Host tests for the Space manager's pure helpers (docs/architecture-
// review.md §3.4, phase 4: phi_space_manager.h). Run with test/model/run.sh.
// The header is host-clean, so what is covered here is the default-role
// resolution, its hand-off on delete, the last-Space rule and the reorder
// clamp; the service itself (store, palettes, framework pushes) is over
// base/ and Win32 and is exercised only in the client build.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_space_manager.h"

using namespace phi_client;
using namespace phi_client::model;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

Space MakeSpace(const char* id) {
  Space space;
  space.id = id;
  space.name = std::string("Space ") + id;
  return space;
}

std::vector<Space> Spaces(std::initializer_list<const char*> ids) {
  std::vector<Space> out;
  for (const char* id : ids) {
    out.push_back(MakeSpace(id));
  }
  return out;
}

void TestFind() {
  const std::vector<Space> spaces = Spaces({"1", "7", "abc"});
  CHECK(FindSpaceIn(spaces, "7") == &spaces[1]);
  CHECK(FindSpaceIn(spaces, "abc") == &spaces[2]);
  CHECK(FindSpaceIn(spaces, "2") == nullptr);
  CHECK(FindSpaceIn(spaces, "") == nullptr);
  CHECK(FindSpaceIn({}, "1") == nullptr);
}

// The run-time rule (the coordinator's DefaultSpaceId since the role was
// introduced): the persisted holder while it exists, else the FIRST Space —
// not "1" — and "1" only for an empty list.
void TestDefaultSpaceId() {
  const std::vector<Space> spaces = Spaces({"3", "1", "2"});
  CHECK(DefaultSpaceIdOf(spaces, "2") == "2");
  CHECK(DefaultSpaceIdOf(spaces, "1") == "1");
  CHECK(DefaultSpaceIdOf(spaces, "9") == "3");  // Gone: the first, not "1".
  CHECK(DefaultSpaceIdOf(spaces, "") == "3");
  CHECK(DefaultSpaceIdOf({}, "4") == "default-space");
  CHECK(DefaultSpaceIdOf({}, "") == "default-space");
  // The decoder's rule differs on purpose (it prefers the built-in
  // "default-space" when present); pin the difference so nobody "fixes"
  // one into the other.
  CHECK(ResolveDefaultSpaceId(spaces, "9") == "3");
  const std::vector<Space> with_builtin = Spaces({"3", "default-space"});
  CHECK(ResolveDefaultSpaceId(with_builtin, "9") == "default-space");
  CHECK(DefaultSpaceIdOf(with_builtin, "9") == "3");
}

// The hand-off on delete: the first REMAINING Space in strip order.
void TestDefaultRoleSuccessor() {
  CHECK(DefaultRoleSuccessor(Spaces({"1", "2", "3"}), "1") == "2");
  CHECK(DefaultRoleSuccessor(Spaces({"1", "2", "3"}), "2") == "1");
  CHECK(DefaultRoleSuccessor(Spaces({"1", "2", "3"}), "3") == "1");
  CHECK(DefaultRoleSuccessor(Spaces({"5"}), "5").empty());
  CHECK(DefaultRoleSuccessor({}, "1").empty());
  // A deletee that is not the holder: still "the first other Space".
  CHECK(DefaultRoleSuccessor(Spaces({"4", "5"}), "9") == "4");
}

// Only the LAST Space is undeletable — the default Space itself may go.
void TestCanDelete() {
  CHECK(CanDeleteSpaceIn(Spaces({"1", "2"}), "1"));
  CHECK(CanDeleteSpaceIn(Spaces({"1", "2"}), "2"));
  CHECK(!CanDeleteSpaceIn(Spaces({"1", "2"}), "3"));  // Unknown.
  CHECK(!CanDeleteSpaceIn(Spaces({"1"}), "1"));       // The last one.
  CHECK(!CanDeleteSpaceIn({}, "1"));
}

// Where a dragged chip re-enters the list once it has been taken out: the
// index is clamped to [0, count], so a drop past the end appends and a
// negative one prepends.
void TestReorderClamp() {
  CHECK(ClampSpaceInsertIndex(3, 0) == 0);
  CHECK(ClampSpaceInsertIndex(3, 2) == 2);
  CHECK(ClampSpaceInsertIndex(3, 3) == 3);
  CHECK(ClampSpaceInsertIndex(3, 4) == 3);
  CHECK(ClampSpaceInsertIndex(3, 99) == 3);
  CHECK(ClampSpaceInsertIndex(3, -1) == 0);
  CHECK(ClampSpaceInsertIndex(0, 5) == 0);
}

void TestIncognitoIds() {
  // An Incognito Space is the prefix plus a UUID; the bare rule target and
  // a plain Space are not one.
  CHECK(model::IsIncognitoSpaceId("space.incognito.0f1e"));
  CHECK(!model::IsIncognitoSpaceId("space.incognito"));
  CHECK(!model::IsIncognitoSpaceId("default-space"));
  // The lowest free ordinal: "Incognito 2" comes back once the second
  // closes.
  CHECK(NextIncognitoOrdinal({}) == 1);
  CHECK(NextIncognitoOrdinal({1}) == 2);
  CHECK(NextIncognitoOrdinal({1, 3}) == 2);
  CHECK(NextIncognitoOrdinal({2, 3}) == 1);
}

}  // namespace

int main() {
  TestFind();
  TestDefaultSpaceId();
  TestDefaultRoleSuccessor();
  TestCanDelete();
  TestReorderClamp();
  TestIncognitoIds();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("space_manager_test: all passed\n");
  return EXIT_SUCCESS;
}
