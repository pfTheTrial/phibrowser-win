// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_

#include <windows.h>

#include <cstdint>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

// The client's unified control socket — the Windows twin of the Mac
// AgentCDPListener. It owns ONE named pipe and speaks HTTP/1.1 over it,
// multiplexing two families by request path (like the Mac UDS):
//
//   /phi-agent...  -> WebSocket, client control served HERE (dispatched to
//                     PhiFramework commands / client state). [slice 2]
//   everything else -> CDP: transparently byte-relayed to Chromium's own
//                     loopback DevTools endpoint (--remote-debugging-port),
//                     so the caller sees one socket for both.
//
// A pointer file (CDPAgentSocket) in the user-data-dir advertises the pipe
// name, matching the Mac discovery contract so the phi-browser-skill client
// works unchanged apart from its transport. No peer auth (dev/test); the pipe
// ACL is the default (current user). Runs on its own Win32 threads with
// blocking I/O; control commands marshal to the browser UI thread.
class PhiControlServer {
 public:
  PhiControlServer();
  ~PhiControlServer();

  PhiControlServer(const PhiControlServer&) = delete;
  PhiControlServer& operator=(const PhiControlServer&) = delete;

  // Starts the listener thread. `user_data_dir` is where the DevToolsActivePort
  // file is read and the CDPAgentSocket pointer file is written. Idempotent.
  void Start(const std::wstring& user_data_dir,
             phi_framework::PhiFramework* framework);

 private:
  void ListenLoop();
  void HandleConnection(HANDLE pipe);
  // Reads Chromium's loopback DevTools port from DevToolsActivePort, waiting
  // for it to appear. Returns 0 on timeout.
  int ResolveDevToolsPort();
  // Byte-relays an already-read HTTP head + the live pipe to a fresh TCP
  // connection on 127.0.0.1:port, in both directions until either closes.
  void ProxyToDevTools(HANDLE pipe, const std::string& head, int port);

  std::wstring user_data_dir_;
  std::wstring pipe_name_;
  // Non-owning; a singleton in chrome.dll, across a module boundary.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  HANDLE listen_thread_ = nullptr;
  volatile bool started_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_
