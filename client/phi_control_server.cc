// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// Raw socket/pipe byte-buffer relay code; pointer arithmetic is inherent.
#pragma allow_unsafe_buffers
#endif

#include <winsock2.h>  // Must precede windows.h (via the header) for Winsock 2.
#include <ws2tcpip.h>

#include "phibrowser-win/client/phi_control_server.h"

#include <string>
#include <vector>

#include "base/logging.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"

#pragma comment(lib, "ws2_32.lib")

namespace phi_client {

namespace {

constexpr int kBufSize = 32 * 1024;
constexpr char kControlPathPrefix[] = "/phi-agent";

void EnsureWinsock() {
  static bool once = [] {
    WSADATA wsa;
    return ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  (void)once;
}

// The pipe is opened FILE_FLAG_OVERLAPPED so read and write can proceed
// CONCURRENTLY from different threads (a synchronous handle serializes I/O,
// which deadlocks a bidirectional relay). Each call supplies a reusable event.

bool OvRead(HANDLE pipe, HANDLE ev, void* buf, DWORD len, DWORD* read) {
  OVERLAPPED ov = {};
  ov.hEvent = ev;
  ::ResetEvent(ev);
  BOOL ok = ::ReadFile(pipe, buf, len, nullptr, &ov);
  if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
    return false;
  }
  return ::GetOverlappedResult(pipe, &ov, read, TRUE) && *read > 0;
}

bool OvWrite(HANDLE pipe, HANDLE ev, const char* data, size_t len) {
  size_t off = 0;
  while (off < len) {
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    ::ResetEvent(ev);
    DWORD wrote = 0;
    BOOL ok = ::WriteFile(pipe, data + off, static_cast<DWORD>(len - off),
                          nullptr, &ov);
    if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
      return false;
    }
    if (!::GetOverlappedResult(pipe, &ov, &wrote, TRUE) || wrote == 0) {
      return false;
    }
    off += wrote;
  }
  return true;
}

// Reads the HTTP request head (through the blank line) from the pipe.
bool ReadHttpHead(HANDLE pipe, HANDLE ev, std::string* head) {
  char buf[4096];
  while (head->find("\r\n\r\n") == std::string::npos) {
    DWORD read = 0;
    if (!OvRead(pipe, ev, buf, sizeof(buf), &read)) {
      return false;
    }
    head->append(buf, read);
    if (head->size() > 1 << 20) {
      return false;  // Runaway head.
    }
  }
  return true;
}

std::string RequestPath(const std::string& head) {
  // "METHOD /path HTTP/1.1\r\n..."
  size_t sp1 = head.find(' ');
  if (sp1 == std::string::npos) {
    return {};
  }
  size_t sp2 = head.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) {
    return {};
  }
  return head.substr(sp1 + 1, sp2 - (sp1 + 1));
}

// Relay thread context: pumps the DevTools TCP socket back into the pipe.
struct SockToPipe {
  SOCKET sock;
  HANDLE pipe;
};

DWORD WINAPI SockToPipeThread(void* arg) {
  auto* ctx = static_cast<SockToPipe*>(arg);
  HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<char> buf(kBufSize);
  for (;;) {
    int n = ::recv(ctx->sock, buf.data(), kBufSize, 0);
    if (n <= 0) {
      break;
    }
    if (!OvWrite(ctx->pipe, ev, buf.data(), static_cast<size_t>(n))) {
      break;
    }
  }
  ::CloseHandle(ev);
  // Unblock the pipe->sock direction blocked on the pipe read.
  ::CancelIoEx(ctx->pipe, nullptr);
  return 0;
}

}  // namespace

PhiControlServer::PhiControlServer() = default;

PhiControlServer::~PhiControlServer() {
  if (listen_thread_) {
    ::CloseHandle(listen_thread_);
  }
}

void PhiControlServer::Start(const std::wstring& user_data_dir,
                             phi_framework::PhiFramework* framework) {
  if (started_) {
    return;
  }
  started_ = true;
  user_data_dir_ = user_data_dir;
  framework_ = framework;
  EnsureWinsock();

  pipe_name_ =
      L"\\\\.\\pipe\\phi-cdp." + std::to_wstring(::GetCurrentProcessId());

  // Advertise the pipe name via the pointer file (Mac discovery contract).
  std::wstring pointer = user_data_dir_ + L"\\CDPAgentSocket";
  HANDLE f = ::CreateFileW(pointer.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    std::string line = base::WideToUTF8(pipe_name_) + "\n";
    DWORD wrote = 0;
    ::WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &wrote,
                nullptr);
    ::CloseHandle(f);
  }

  listen_thread_ = ::CreateThread(
      nullptr, 0,
      [](void* self) -> DWORD {
        static_cast<PhiControlServer*>(self)->ListenLoop();
        return 0;
      },
      this, 0, nullptr);

  LOG(INFO) << "[PhiControl] serving " << base::WideToUTF8(pipe_name_);
}

void PhiControlServer::ListenLoop() {
  for (;;) {
    HANDLE pipe = ::CreateNamedPipeW(
        pipe_name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, kBufSize, kBufSize, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      LOG(ERROR) << "[PhiControl] CreateNamedPipe failed err="
                 << ::GetLastError();
      return;
    }
    // Overlapped connect: wait for a client.
    OVERLAPPED ov = {};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (::ConnectNamedPipe(pipe, &ov)) {
      connected = true;
    } else {
      DWORD e = ::GetLastError();
      if (e == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (e == ERROR_IO_PENDING) {
        ::WaitForSingleObject(ov.hEvent, INFINITE);
        DWORD ignored = 0;
        connected = ::GetOverlappedResult(pipe, &ov, &ignored, FALSE) != 0;
      }
    }
    ::CloseHandle(ov.hEvent);
    if (!connected) {
      ::CloseHandle(pipe);
      continue;
    }
    struct Conn {
      RAW_PTR_EXCLUSION PhiControlServer* self;  // Owns this; outlives the loop.
      HANDLE pipe;
    };
    auto* conn = new Conn{this, pipe};
    HANDLE t = ::CreateThread(
        nullptr, 0,
        [](void* arg) -> DWORD {
          auto* c = static_cast<Conn*>(arg);
          c->self->HandleConnection(c->pipe);
          delete c;
          return 0;
        },
        conn, 0, nullptr);
    if (t) {
      ::CloseHandle(t);
    } else {
      ::CloseHandle(pipe);
      delete conn;
    }
  }
}

void PhiControlServer::HandleConnection(HANDLE pipe) {
  HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::string head;
  if (ReadHttpHead(pipe, ev, &head)) {
    const std::string path = RequestPath(head);
    if (path.rfind(kControlPathPrefix, 0) == 0) {
      // Client control channel — implemented in slice 2. Refuse cleanly.
      static const char kResp[] =
          "HTTP/1.1 501 Not Implemented\r\nContent-Length: 0\r\n\r\n";
      OvWrite(pipe, ev, kResp, sizeof(kResp) - 1);
    } else {
      int port = ResolveDevToolsPort();
      if (port > 0) {
        ProxyToDevTools(pipe, head, port);
      } else {
        static const char kResp[] =
            "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
        OvWrite(pipe, ev, kResp, sizeof(kResp) - 1);
      }
    }
  }
  ::CloseHandle(ev);
  ::FlushFileBuffers(pipe);
  ::DisconnectNamedPipe(pipe);
  ::CloseHandle(pipe);
}

int PhiControlServer::ResolveDevToolsPort() {
  static int cached = 0;
  if (cached > 0) {
    return cached;
  }
  std::wstring file = user_data_dir_ + L"\\DevToolsActivePort";
  // The port appears shortly after the DevTools server starts; wait briefly.
  for (int attempt = 0; attempt < 100; ++attempt) {
    HANDLE h = ::CreateFileW(file.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      char buf[64] = {};
      DWORD read = 0;
      ::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
      ::CloseHandle(h);
      int port = atoi(buf);  // First line is the port.
      if (port > 0) {
        cached = port;
        return port;
      }
    }
    ::Sleep(100);
  }
  return 0;
}

void PhiControlServer::ProxyToDevTools(HANDLE pipe,
                                       const std::string& head,
                                       int port) {
  SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock == INVALID_SOCKET) {
    return;
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::closesocket(sock);
    return;
  }
  // Forward the already-read request head, then relay both directions.
  ::send(sock, head.data(), static_cast<int>(head.size()), 0);

  SockToPipe ctx{sock, pipe};
  HANDLE back = ::CreateThread(nullptr, 0, &SockToPipeThread, &ctx, 0, nullptr);

  HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<char> buf(kBufSize);
  for (;;) {
    DWORD read = 0;
    if (!OvRead(pipe, ev, buf.data(), kBufSize, &read)) {
      break;
    }
    if (::send(sock, buf.data(), static_cast<int>(read), 0) <= 0) {
      break;
    }
  }
  ::CloseHandle(ev);
  // Unblock and join the back-relay.
  ::shutdown(sock, SD_BOTH);
  ::closesocket(sock);
  if (back) {
    ::WaitForSingleObject(back, 2000);
    ::CloseHandle(back);
  }
}

}  // namespace phi_client
