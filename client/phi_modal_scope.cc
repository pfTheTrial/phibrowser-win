// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_modal_scope.h"

namespace phi_client {

int& PhiModalDepthRef() {
  static int depth = 0;  // UI thread only.
  return depth;
}

int PhiModalDepth() {
  return PhiModalDepthRef();
}

}  // namespace phi_client
