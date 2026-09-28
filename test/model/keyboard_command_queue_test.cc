// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cassert>
#include <iostream>

#include "phibrowser-win/client/phi_keyboard_accelerators.h"

int main() {
  using namespace phi_client;
  using Queue = PhiKeyboardCommandQueue;
  const Queue::Target a{"a", BrowserId{10}, TabId{100}};
  const Queue::Target b{"b", BrowserId{20}, TabId{200}};
  Queue queue;
  // Queue close/new-tab, then switch before dispatch: neither is replayed
  // into B, nor resurrected when A is presented again.
  const auto close = queue.Enqueue({1, a, {}, 0});
  const auto new_tab = queue.Enqueue({2, a, {}, 0});
  assert(!queue.Take(close, b));
  assert(!queue.Take(new_tab, b));
  assert(!queue.Take(close, a));
  assert(!queue.Take(new_tab, a));

  // Matching presentation executes once and preserves the focused Peek.
  const auto find = queue.Enqueue({3, a, BrowserId{30}, 123});
  const auto command = queue.Take(find, a);
  assert(command && command->id == 3 && command->find_browser == BrowserId{30});
  assert(command->focus == 123);
  assert(!queue.Take(find, a));

  // Lazy attachment and browser replacement must not inherit old shortcuts.
  const Queue::Target cold{"cold", {}, {}};
  const auto cold_command = queue.Enqueue({2, cold, {}, 0});
  assert(!queue.Take(cold_command, {"cold", BrowserId{40}, TabId{400}}));
  const auto replaced = queue.Enqueue({1, a, {}, 0});
  assert(!queue.Take(replaced, {"a", BrowserId{11}, TabId{100}}));
  const auto selected = queue.Enqueue({1, a, {}, 0});
  assert(!queue.Take(selected, {"a", BrowserId{10}, TabId{101}}));

  // Failed posts and destroyed shells release commands. New instances cannot
  // consume a message left in the Windows queue for a reused HWND.
  const auto failed = queue.Enqueue({1, a, {}, 0});
  queue.Erase(failed);
  assert(!queue.Take(failed, a));
  const auto destroyed = queue.Enqueue({1, a, {}, 0});
  queue.Clear();
  assert(!queue.Take(destroyed, a));
  Queue replacement;
  const auto fresh = replacement.Enqueue({2, a, {}, 0});
  assert(fresh != destroyed);
  assert(!replacement.Take(destroyed, a));
  assert(replacement.Take(fresh, a));
  std::cout << "keyboard command queue tests passed\n";
}
