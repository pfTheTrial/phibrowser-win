// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_

#include <cstdint>
#include <map>
#include <memory>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_control_server.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

// The client's implementation of the framework delegate — the Windows twin of
// the Mac PhiChromiumCoordinator. Registered with the framework before
// ChromeMain; the framework calls it back on the browser UI thread. F2 slice 1
// only observes window lifecycle and logs it; adopting the HWND and drawing
// chrome comes next.
class PhiCoordinator : public phi_framework::PhiFrameworkDelegate {
 public:
  PhiCoordinator();
  ~PhiCoordinator() override;

  PhiCoordinator(const PhiCoordinator&) = delete;
  PhiCoordinator& operator=(const PhiCoordinator&) = delete;

  // phi_framework::PhiFrameworkDelegate:
  void OnFrameworkReady(phi_framework::PhiFramework* framework,
                        const char* user_data_dir_utf8) override;
  void OnBrowserWindowCreated(int64_t window_id, void* hwnd) override;
  void OnBrowserWindowClosing(int64_t window_id) override;

 private:
  // The framework command interface (a singleton in chrome.dll); non-owning,
  // across a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  // The unified control/CDP socket.
  PhiControlServer control_server_;
  // One controller per adopted window, keyed by windowId.
  std::map<int64_t, std::unique_ptr<PhiWindowController>> windows_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_
