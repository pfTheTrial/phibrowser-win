// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_TRANSCRIPT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_TRANSCRIPT_H_

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace phi_client {

// The agent transcript store — the Mac's AgentTranscriptStore. One buffer
// per TASK (keyed by taskId, so a persistent Space's re-bind keeps its
// history), each entry typed by what produced it, so the console can draw
// a round separator, the user's own ❯ line, a dim thinking line and an
// error differently. Capped per task and per field exactly as on Mac.
struct PhiTranscriptEntry {
  enum class Kind {
    kAction,     // The skill's terse step line ("openTab example.com").
    kNarration,  // The agent's say()/narrate prose.
    kRound,      // "round started" separator — control-only.
    kStatus,     // "You took control" — control-only.
    kError,      // markError / task failed.
    kUser,       // The user's console line.
    kAssistant,  // Mirrored driving-session prose.
    kThinking,   // Mirrored reasoning, drawn dim.
    kTool,       // Mirrored tool call; Pi settles these in place.
  };
  int64_t seq = 0;      // Global append order across tasks.
  int task_number = 0;  // The pip ordinal at append time ("R1").
  Kind kind = Kind::kAction;
  std::string text;
  std::string detail;
  std::string agent;       // "claude" / "codex" / "pi" / … or empty.
  int64_t ts_ms = 0;       // Epoch ms; a mirror passes the source's time.
  std::string source_id;   // A Pi tool call id, for settling.
  std::string tool_state;  // "pending" | "success" | "error" for kTool.
};

// What the console shows about a task in its filter menu.
struct PhiTranscriptTaskInfo {
  std::string task_id;
  int number = 0;
  bool live = false;  // False once the task ended (buffer retained).
};

class PhiAgentTranscript {
 public:
  static constexpr size_t kMaxEntriesPerTask = 500;
  static constexpr size_t kMaxTerseChars = 300;
  static constexpr size_t kMaxProseChars = 4000;
  static constexpr size_t kMaxDetailChars = 500;
  static constexpr size_t kMaxPiToolDetailChars = 4000;
  // An ended task's buffer outlives it this long (the Mac's continuity
  // window: a task re-created within it resumes its own history) unless
  // the console still shows it — then until the console closes.
  static constexpr int64_t kContinuityMs = 30 * 60 * 1000;

  // The transcript's own clean-up, for agent text shown elsewhere (the
  // handoff prompt): control characters dropped, newlines kept only when
  // `keep_newlines`, trimmed, then cut to `cap` bytes at a UTF-8 boundary
  // with an ellipsis.
  static std::string SanitizeText(const std::string& text,
                                  bool keep_newlines,
                                  size_t cap);

  PhiAgentTranscript();
  ~PhiAgentTranscript();
  PhiAgentTranscript(const PhiAgentTranscript&) = delete;
  PhiAgentTranscript& operator=(const PhiAgentTranscript&) = delete;

  // Appends (sanitised, capped). `entry.seq` is assigned here. A kTool
  // entry whose `source_id` is already pending SETTLES that entry in place
  // (state + detail), keeping its seq and timestamp — the Mac's Pi tool
  // card behaviour. Returns false when the entry was dropped (empty text
  // and no settle).
  bool Append(const std::string& task_id, PhiTranscriptEntry entry);
  // The live tail row a driver reports ("working" / "waiting", the Claude
  // spinner) — never history. Empty clears it.
  void SetActivity(const std::string& task_id, const std::string& text);
  const std::string& Activity(const std::string& task_id) const;

  // The task's buffer, oldest first (empty when unknown).
  std::vector<PhiTranscriptEntry> Entries(const std::string& task_id) const;
  // Every buffered task, live ones first by number, then ended ones.
  std::vector<PhiTranscriptTaskInfo> Tasks() const;
  bool Has(const std::string& task_id) const;

  // The task is (re)bound with this number: a buffer within the continuity
  // window is resumed, an older one is dropped first.
  void NoteTaskLive(const std::string& task_id, int number, int64_t now_ms);
  // The task ended: the buffer is kept for the continuity window (or while
  // the console shows it — the console calls Reap when it closes).
  void NoteTaskEnded(const std::string& task_id, int64_t now_ms);
  // Frees ended buffers past the window. `keep` names the task the console
  // is showing, which stays regardless.
  void Reap(int64_t now_ms, const std::string& keep);

  // The console's Clear: one task's buffer, or every buffer when empty.
  void Clear(const std::string& task_id);

 private:
  struct Buffer {
    int number = 0;
    bool live = false;
    int64_t ended_ms = 0;
    std::deque<PhiTranscriptEntry> entries;
    std::string activity;
  };
  std::map<std::string, Buffer> buffers_;
  int64_t next_seq_ = 1;
};

// The console's line rendering of one entry — shared by the panel and the
// plain-text copy. Kinds carry their own prefix: "❯ " for the user, "⚠ "
// for errors, "── … ──" for rounds; detail follows on an indented line.
std::string PhiRenderTranscriptEntry(const PhiTranscriptEntry& entry);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_TRANSCRIPT_H_
