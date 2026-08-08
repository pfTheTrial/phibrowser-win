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

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/hash/sha1.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"

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

// Reads exactly `n` bytes from the pipe into `out`. False on disconnect.
bool ReadExact(HANDLE pipe, HANDLE ev, size_t n, std::string* out) {
  out->clear();
  char buf[4096];
  while (out->size() < n) {
    DWORD want = static_cast<DWORD>(
        std::min<size_t>(sizeof(buf), n - out->size()));
    DWORD read = 0;
    if (!OvRead(pipe, ev, buf, want, &read)) {
      return false;
    }
    out->append(buf, read);
  }
  return true;
}

// Server -> client WebSocket text frame (unmasked, single frame).
bool WsSendText(HANDLE pipe, HANDLE ev, const std::string& text) {
  std::string frame;
  frame.push_back(static_cast<char>(0x81));  // FIN + text opcode.
  const size_t len = text.size();
  if (len < 126) {
    frame.push_back(static_cast<char>(len));
  } else if (len < 65536) {
    frame.push_back(126);
    frame.push_back(static_cast<char>((len >> 8) & 0xff));
    frame.push_back(static_cast<char>(len & 0xff));
  } else {
    frame.push_back(127);
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((len >> shift) & 0xff));
    }
  }
  frame += text;
  return OvWrite(pipe, ev, frame.data(), frame.size());
}

// Reads one WebSocket message. Returns the opcode (1 text, 8 close, 9 ping,
// 10 pong) and unmasked payload; accumulates continuation frames. False on
// disconnect.
bool WsReadMessage(HANDLE pipe, HANDLE ev, int* opcode, std::string* payload) {
  payload->clear();
  int first_opcode = -1;
  for (;;) {
    std::string hdr;
    if (!ReadExact(pipe, ev, 2, &hdr)) {
      return false;
    }
    const uint8_t b0 = static_cast<uint8_t>(hdr[0]);
    const uint8_t b1 = static_cast<uint8_t>(hdr[1]);
    const bool fin = (b0 & 0x80) != 0;
    const int op = b0 & 0x0f;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7f;
    if (len == 126) {
      std::string ext;
      if (!ReadExact(pipe, ev, 2, &ext)) {
        return false;
      }
      len = (static_cast<uint8_t>(ext[0]) << 8) | static_cast<uint8_t>(ext[1]);
    } else if (len == 127) {
      std::string ext;
      if (!ReadExact(pipe, ev, 8, &ext)) {
        return false;
      }
      len = 0;
      for (int i = 0; i < 8; ++i) {
        len = (len << 8) | static_cast<uint8_t>(ext[i]);
      }
    }
    std::string mask;
    if (masked && !ReadExact(pipe, ev, 4, &mask)) {
      return false;
    }
    std::string data;
    if (len > (16u << 20) || !ReadExact(pipe, ev, len, &data)) {
      return false;  // Oversized or short.
    }
    if (masked) {
      for (size_t i = 0; i < data.size(); ++i) {
        data[i] ^= mask[i % 4];
      }
    }
    if (first_opcode < 0) {
      first_opcode = (op == 0) ? 1 : op;  // Continuation inherits.
    }
    payload->append(data);
    if (fin) {
      *opcode = first_opcode;
      return true;
    }
  }
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
                             PhiControlDelegate* delegate) {
  if (started_) {
    return;
  }
  started_ = true;
  user_data_dir_ = user_data_dir;
  delegate_ = delegate;
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
      ServeControlChannel(pipe, ev, head);
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

void PhiControlServer::ServeControlChannel(HANDLE pipe,
                                           HANDLE ev,
                                           const std::string& head) {
  // WebSocket handshake: echo the accept key. (Header names are
  // case-insensitive; the skill sends this exact casing.)
  const std::string kKeyHeader = "Sec-WebSocket-Key:";
  size_t k = head.find(kKeyHeader);
  if (k == std::string::npos) {
    return;
  }
  size_t vstart = k + kKeyHeader.size();
  while (vstart < head.size() && head[vstart] == ' ') {
    ++vstart;
  }
  size_t vend = head.find("\r\n", vstart);
  std::string key = head.substr(vstart, vend - vstart);
  std::string accept = base::Base64Encode(base::SHA1HashString(
      key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
  std::string resp =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " +
      accept + "\r\n\r\n";
  if (!OvWrite(pipe, ev, resp.data(), resp.size())) {
    return;
  }

  // Message loop: {id, type, payloadJson} -> {id, responseJson|error}.
  for (;;) {
    int opcode = 0;
    std::string msg;
    if (!WsReadMessage(pipe, ev, &opcode, &msg)) {
      break;
    }
    if (opcode == 8) {  // Close.
      break;
    }
    if (opcode != 1) {  // Ping/pong/other: ignore.
      continue;
    }
    std::optional<base::DictValue> parsed = base::JSONReader::ReadDict(msg, base::JSON_PARSE_RFC);
    base::DictValue resp_dict;
    if (parsed) {
      const base::DictValue& req = *parsed;
      std::optional<int> id = req.FindInt("id");
      const std::string* type = req.FindString("type");
      const std::string* payload = req.FindString("payloadJson");
      if (id) {
        resp_dict.Set("id", *id);
      }
      if (type && delegate_) {
        resp_dict.Set("responseJson",
                      delegate_->HandleControlCommand(
                          *type, payload ? *payload : std::string("{}")));
      } else {
        resp_dict.Set("error", "missing type");
      }
    } else {
      resp_dict.Set("error", "bad json");
    }
    std::string out;
    base::JSONWriter::Write(resp_dict, &out);
    if (!WsSendText(pipe, ev, out)) {
      break;
    }
  }
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
  // Chromium's DevTools server rejects requests whose Host is not localhost or
  // an IP ("Host header is specified and is not an IP address or localhost"),
  // so rewrite whatever the pipe client sent to the loopback endpoint we are
  // proxying to. Callers over a named pipe have no meaningful host anyway.
  // Rebuild the head line-by-line: drop any Host header (case-insensitively)
  // and inject the loopback one right after the request line.
  std::string fwd;
  {
    const std::string host_value = "Host: 127.0.0.1:" + std::to_string(port);
    size_t pos = 0;
    bool first_line = true;
    while (pos < head.size()) {
      size_t eol = head.find("\r\n", pos);
      if (eol == std::string::npos) {
        eol = head.size();
      }
      std::string line = head.substr(pos, eol - pos);
      pos = eol + 2;
      std::string lower;
      lower.reserve(line.size());
      for (char c : line) {
        lower.push_back(static_cast<char>(::tolower(c)));
      }
      if (first_line) {
        fwd += line + "\r\n" + host_value + "\r\n";
        first_line = false;
        continue;
      }
      if (lower.rfind("host:", 0) == 0) {
        continue;  // Drop the client's Host.
      }
      fwd += line + "\r\n";
    }
    if (fwd.size() < 2 || fwd.compare(fwd.size() - 4, 4, "\r\n\r\n") != 0) {
      fwd += "\r\n";
    }
  }
  // Forward the (rewritten) request head, then relay both directions.
  ::send(sock, fwd.data(), static_cast<int>(fwd.size()), 0);

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
