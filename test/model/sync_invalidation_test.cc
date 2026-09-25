// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The invalidation stream's SSE parser (docs/user-data-parity.md S5): the
// Mac's PhiSyncInvalidationParser cases.

#include <cstdio>
#include <string>
#include <vector>

#include "phi_sync_invalidation.h"

namespace psync = phi_client::sync;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

}  // namespace

int main() {
  {
    psync::InvalidationParser parser;
    std::vector<psync::InvalidationEvent> events;
    // Split across chunks, CRLF line ends, a BOM, a heartbeat.
    Check(parser.Feed("\xEF\xBB\xBF" "event: ready\r\ndata: {\"v\":1}\r\n", &events) &&
              parser.Feed("\r\n: ping\n", &events) &&
              parser.Feed("event: invalidate\ndata: {\"namespace\":\"chromium:phi\","
                          "\"data_types\":[2000],\"source_client_id\":\"abc\"}\n\n",
                          &events),
          "a well-formed stream parses");
    Check(events.size() == 3 &&
              events[0].kind == psync::InvalidationEvent::Kind::kReady &&
              events[1].kind == psync::InvalidationEvent::Kind::kHeartbeat &&
              events[2].kind == psync::InvalidationEvent::Kind::kInvalidate &&
              events[2].hint.name_space == "chromium:phi" &&
              events[2].hint.data_types == std::vector<int>({2000}) &&
              events[2].hint.source_client_id == "abc" &&
              !events[2].hint.profile_uuid(),
          "ready, heartbeat and a Phi hint");
  }
  {
    psync::InvalidationParser parser;
    std::vector<psync::InvalidationEvent> events;
    Check(parser.Feed("event: invalidate\ndata: {\"namespace\":\"chromium:1a-b\","
                      "\"data_types\":[1,2],\"source_client_id\":\"\"}\n\n",
                      &events) &&
              events.size() == 1 && events[0].hint.profile_uuid() == "1a-b",
          "a Chromium profile hint names its uuid");
  }
  {
    psync::InvalidationParser parser;
    std::vector<psync::InvalidationEvent> events;
    Check(!parser.Feed("event: invalidate\ndata: {\"namespace\":\"chromium:a b\","
                       "\"data_types\":[1]}\n\n",
                       &events),
          "an invalid namespace drops the stream");
    psync::InvalidationParser empty_types;
    Check(!empty_types.Feed("event: invalidate\ndata: {\"namespace\":\"chromium:x\","
                            "\"data_types\":[]}\n\n",
                            &events),
          "a hint without types drops the stream");
    psync::InvalidationParser big;
    Check(!big.Feed(std::string(20 * 1024, 'a'), &events),
          "an oversized line drops the stream");
  }
  if (g_failures) {
    std::printf("sync_invalidation_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("sync_invalidation_test: OK\n");
  return 0;
}
