// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/synchronization/lock.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

// The identity of a pipe client, resolved at connection accept — the
// Windows analogue of the Mac's process-tree peer identification.
struct PhiAgentPeer {
  DWORD pid = 0;
  // Full image path of the connecting process; empty when the process died
  // or could not be opened (treated as unidentified).
  std::wstring image_path;
};

// Handles /phi-agent control commands. Called on the control server's I/O
// thread; the implementation marshals to the browser UI thread itself.
// `type` is the envelope verb (e.g. "agentSpace.list"); `payload_json` is the
// stringified payload. Returns the stringified JSON response.
class PhiControlDelegate {
 public:
  virtual ~PhiControlDelegate() = default;
  virtual std::string HandleControlCommand(const std::string& type,
                                           const std::string& payload_json) = 0;
  // Called ONCE per accepted connection, on the I/O thread, BEFORE any
  // request is served — control channel and CDP session alike (CDP grants
  // full browser control, so both families sit behind the same consent).
  // Only connections that already presented the launch token reach this;
  // the implementation marshals to the UI thread and may block on a consent
  // dialog. False refuses the connection.
  virtual bool AuthorizeAgentPeer(const PhiAgentPeer& peer) = 0;
};

// The client's unified control socket — the Windows twin of the Mac
// AgentCDPListener. It owns ONE named pipe and speaks HTTP/1.1 over it,
// multiplexing two families by request path (like the Mac UDS):
//
//   /phi-agent...          -> WebSocket, client control served HERE
//                             (dispatched to the delegate).
//   /json, /json/version,  -> the DevTools discovery documents, answered
//   /json/list                from the framework (plain HTTP GET).
//   /devtools/<kind>/<id>  -> WebSocket, a CDP session pumped through the
//                             framework's IN-PROCESS DevTools transport
//                             (PhiFramework::OpenDevToolsSession). There is
//                             no loopback TCP DevTools server; this pipe is
//                             the only door to the protocol.
//
// Trust model (docs/architecture-review.md §3.7, lane5 §4.1) — three gates,
// in order, every connection:
//   1. The pipe's SDDL: owner + SYSTEM + Administrators, medium integrity,
//      remote clients rejected. Other users and sandboxed (low-IL) processes
//      cannot open it at all. The name carries a per-launch random suffix and
//      the first instance is created FILE_FLAG_FIRST_PIPE_INSTANCE, so a
//      squatter cannot pre-create it.
//   2. A per-launch token: 32 random bytes, hex, on line 2 of the pointer
//      file `<user-data-dir>\CDPAgentSocket` (line 1 is the pipe name — the
//      Mac discovery contract). The pointer file has an owner + SYSTEM DACL
//      and is written only after the first pipe instance exists. Every
//      request must carry `X-Phi-Token: <hex>`; a missing or wrong token is
//      answered 403 before anything else happens — no delegate call, no UI —
//      so a process that cannot read the profile directory cannot even make
//      the consent prompt appear.
//   3. Consent: PhiControlDelegate::AuthorizeAgentPeer with the peer's image
//      path, which the coordinator turns into the Allow/Deny dialog and its
//      grant memory.
// Plus a cap of kMaxConnections concurrent connections (429 beyond it, no
// delegate call) so a prompt cannot be spammed.
//
// Runs on its own Win32 threads with blocking overlapped I/O; control
// commands marshal to the browser UI thread. DevTools callbacks arrive on
// the UI thread and are queued for the connection thread to write, so a
// wedged peer never stalls the UI.
class PhiControlServer {
 public:
  PhiControlServer();
  ~PhiControlServer();

  PhiControlServer(const PhiControlServer&) = delete;
  PhiControlServer& operator=(const PhiControlServer&) = delete;

  // Starts the listener thread. `user_data_dir` is where the CDPAgentSocket
  // pointer file is written. `delegate` (owned by the caller, must outlive
  // this) handles /phi-agent control commands and consent. `framework`
  // (owned by the framework, outlives this) carries CDP: every
  // non-/phi-agent request is served through its in-process DevTools
  // transport (PhiFramework::OpenDevToolsSession and friends). Idempotent.
  void Start(const std::wstring& user_data_dir,
             PhiControlDelegate* delegate,
             phi_framework::PhiFramework* framework);

  // Pushes an unsolicited event frame — {"event":<type>,"payloadJson":<json>}
  // — to every live control channel (the Mac's broadcastToTaskDriver). Safe
  // to call from any thread, including the UI thread: each channel's writes
  // are serialized by its own lock and bounded by a short timeout, so one
  // wedged peer drops its event instead of stalling the caller.
  void BroadcastEvent(const std::string& type,
                      const std::string& payload_json);

 private:
  // One accepted pipe plus its read-ahead buffer; defined in the .cc.
  struct Connection;

  // One live /phi-agent channel. Reply writes (the serve loop) and pushed
  // events (any thread) share the pipe, so they share `write_lock`; the
  // broadcast uses its own OVERLAPPED event because one event handle cannot
  // back two concurrent operations.
  struct ControlChannel {
    HANDLE pipe = nullptr;
    HANDLE write_event = nullptr;
    base::Lock write_lock;
  };
  void RegisterChannel(const std::shared_ptr<ControlChannel>& channel);
  void UnregisterChannel(const std::shared_ptr<ControlChannel>& channel);

  void ListenLoop();
  // Writes the two-line pointer file with its restricted DACL. Called from
  // ListenLoop once the first pipe instance exists.
  bool WritePointerFile();
  void HandleConnection(HANDLE pipe);
  // Serves the /phi-agent WebSocket control channel on an accepted
  // connection whose HTTP head has been read and validated. Dispatches each
  // message to delegate_.
  void ServeControlChannel(Connection* conn, const std::string& head);
  // Serves one CDP WebSocket session (/devtools/<kind>/<id>) through the
  // framework's in-process transport until either side closes.
  void ServeDevToolsSession(Connection* conn,
                            const std::string& head,
                            const std::string& target_id);

  std::wstring user_data_dir_;
  std::wstring pipe_name_;
  // Lower-case hex of the per-launch token; empty until Start.
  std::string token_hex_;
  // Non-owning; owned by the caller (the coordinator), outlives this.
  RAW_PTR_EXCLUSION PhiControlDelegate* delegate_ = nullptr;
  // Non-owning; owned by the framework, outlives this.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  HANDLE listen_thread_ = nullptr;
  volatile bool started_ = false;
  // Connections accepted and not yet finished, for the concurrency cap.
  std::atomic<int> active_connections_{0};
  // Live control channels, for BroadcastEvent.
  std::vector<std::shared_ptr<ControlChannel>> channels_;
  mutable base::Lock channels_lock_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_
