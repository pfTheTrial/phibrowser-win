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
#include "base/values.h"
#include "phibrowser-win/client/phi_verb_registry.h"

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
  // The `X-Phi-Agent-Capability` the request carried (lane 5 §4.1): an
  // opaque id Phi minted for an earlier consented connection, which a
  // later connection — or a helper the agent delegated to — presents to
  // join that principal without a second prompt. Already validated to the
  // wire alphabet ([A-Za-z0-9_-]{32,128}); empty when absent. Whether it
  // still names a live capability is the delegate's call.
  std::string presented_capability;
};

// Handles /phi-agent control commands and the per-connection trust
// bookkeeping. Every method is called on a control server connection
// thread; the implementation marshals to the browser UI thread itself.
//
// `connection_id` is the server's handle for ONE accepted pipe connection
// (never reused within a process). The delegate keys its principal on it
// (docs/architecture-review.md §3.7 item 3): AuthorizeAgentPeer creates the
// principal, HandleControlCommand and the DevTools-session notices act on
// it, OnConnectionClosed retires it.
class PhiControlDelegate {
 public:
  virtual ~PhiControlDelegate() = default;
  // `type` is the envelope verb (e.g. "agentSpace.list"); `payload` is the
  // request's payload, already parsed (v1 parses `payloadJson` for the
  // delegate; v2 carries the object itself). The answer is the TYPED result
  // — success fields, or a code from the closed enum — which the server
  // formats onto whichever protocol the connection speaks. The delegate
  // never sees an envelope and never spells a wire shape.
  virtual PhiVerbResult HandleControlCommand(uint64_t connection_id,
                                             const std::string& type,
                                             base::DictValue payload) = 0;
  // Called ONCE per accepted connection, BEFORE any request is served —
  // control channel and CDP session alike (CDP grants browser control, so
  // both families sit behind the same consent). Only connections that
  // already presented the launch token reach this; the implementation
  // marshals to the UI thread and may block on a consent dialog. False
  // refuses the connection. On success `capability_id` receives the agent
  // capability the connection now holds (minted here, or the one the peer
  // presented), which the server echoes on the 101 as
  // `X-Phi-Agent-Capability`; empty when the delegate issued none.
  virtual bool AuthorizeAgentPeer(uint64_t connection_id,
                                  const PhiAgentPeer& peer,
                                  std::string* capability_id) = 0;
  // The control wire the connection negotiated on its /phi-agent upgrade
  // (1, or 2 when it sent `X-Phi-Protocol: 2`). Called once, right after
  // the 101 and before the first request. A connection that never opens a
  // control channel (a CDP session) is never told.
  virtual void OnControlProtocol(uint64_t connection_id, int protocol) = 0;
  // A CDP session (PhiFramework::OpenDevToolsSession) was opened / closed
  // on the connection. Reported right after the open succeeded and before
  // the 101 goes out, so the delegate can scope the session
  // (SetDevToolsSessionScope) before the peer's first message.
  virtual void OnDevToolsSessionOpened(uint64_t connection_id,
                                       uint64_t session_id) = 0;
  virtual void OnDevToolsSessionClosed(uint64_t connection_id,
                                       uint64_t session_id) = 0;
  // The connection is gone (after its sessions closed). Called only for
  // connections AuthorizeAgentPeer accepted.
  virtual void OnConnectionClosed(uint64_t connection_id) = 0;
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
//      path and the agent capability it presented (if any), which the
//      control dispatcher turns into the Allow/Deny dialog, its grant
//      memory, and a per-connection principal; the capability the
//      connection ends up holding is echoed on the 101.
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
    // The wire the peer asked for on its upgrade (1 unless it sent
    // `X-Phi-Protocol: 2`); a pushed event is shaped for it.
    int protocol = 1;
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
  // message to delegate_ under `connection_id`. `capability_id` (may be
  // empty) is echoed on the 101 as X-Phi-Agent-Capability.
  void ServeControlChannel(Connection* conn,
                           const std::string& head,
                           uint64_t connection_id,
                           const std::string& capability_id);
  // Serves one CDP WebSocket session (/devtools/<kind>/<id>) through the
  // framework's in-process transport until either side closes, reporting
  // the session to delegate_ so it can be scoped to the principal.
  void ServeDevToolsSession(Connection* conn,
                            const std::string& head,
                            const std::string& target_id,
                            uint64_t connection_id,
                            const std::string& capability_id);

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
  // The next connection id (see PhiControlDelegate); 0 is never handed out
  // — the dispatcher uses it for its internal principal.
  std::atomic<uint64_t> next_connection_id_{1};
  // Live control channels, for BroadcastEvent.
  std::vector<std::shared_ptr<ControlChannel>> channels_;
  mutable base::Lock channels_lock_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CONTROL_SERVER_H_
