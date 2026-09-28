// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cassert>
#include <iostream>

#include "chrome/browser/phinomenon/phi_framework/phi_framework_hooks.h"

namespace {
bool enabled = true;
bool parked = false;
void ReadPreferences(bool* out_enabled, bool* out_parked) {
  *out_enabled = enabled;
  *out_parked = parked;
}
}  // namespace

int main() {
  using namespace phi_framework;
  bool actual_enabled = false;
  bool actual_parked = true;
  // No client: Normal, including when callers reuse their output variables.
  InvokeAutoPictureInPicturePreferences(&actual_enabled, &actual_parked);
  assert(actual_enabled && !actual_parked);

  SetAutoPictureInPicturePreferencesHook(&ReadPreferences);
  for (const auto mode : {0, 1, 2, 0, 2, 1}) {
    enabled = mode != 0;
    parked = mode == 2;
    InvokeAutoPictureInPicturePreferences(&actual_enabled, &actual_parked);
    assert(actual_enabled == enabled);
    assert(actual_parked == parked);
  }

  SetAutoPictureInPicturePreferencesHook(nullptr);
  actual_enabled = false;
  actual_parked = true;
  InvokeAutoPictureInPicturePreferences(&actual_enabled, &actual_parked);
  assert(actual_enabled && !actual_parked);
  std::cout << "auto PiP preference hooks: PASS\n";
}
