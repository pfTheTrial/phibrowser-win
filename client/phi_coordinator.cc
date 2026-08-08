// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_coordinator.h"

#include <windows.h>

#include <utility>

#include "base/logging.h"

namespace phi_client {

PhiCoordinator::PhiCoordinator() = default;
PhiCoordinator::~PhiCoordinator() = default;

void PhiCoordinator::OnFrameworkReady(phi_framework::PhiFramework* framework) {
  framework_ = framework;
}

void PhiCoordinator::OnBrowserWindowCreated(int64_t window_id, void* hwnd) {
  LOG(INFO) << "[PhiClient] OnBrowserWindowCreated windowId=" << window_id
            << " hwnd=" << hwnd;
  // Option B: the client owns a shell window and reparents Chromium's content
  // in. The shell routes activation back through the framework.
  windows_[window_id] = std::make_unique<PhiWindowController>(
      window_id, static_cast<HWND>(hwnd), framework_);
}

void PhiCoordinator::OnBrowserWindowClosing(int64_t window_id) {
  LOG(INFO) << "[PhiClient] OnBrowserWindowClosing windowId=" << window_id;
  windows_.erase(window_id);
}

}  // namespace phi_client
