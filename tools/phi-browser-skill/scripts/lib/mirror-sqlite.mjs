// Copyright 2026 Phinomenon Inc.
//
// SQLite tail source for the session mirror: Hermes and OpenClaw keep their
// live transcripts as rows in a SQLite database (not an append-only JSONL
// file), so their mirrors poll by monotonic row id instead of byte offset.
// SqliteTail yields the same {index, raw} items as the tailer's JSONL
// TranscriptTail — index is the row's autoincrement id / seq (already
// absolute and ordered, exactly what the session cursor stores), raw is a
// JSON string for the per-agent toEntry — so everything downstream (cursor,
// batching, backfill grace) is shared unchanged.
//
// Reads go through the macOS-bundled /usr/bin/sqlite3 CLI (read-only, JSON
// output): no npm dependencies, and WAL databases allow concurrent readers
// while the agent writes. A missing database throws (the daemon exits, like
// "transcript gone"); a busy/locked read returns no rows and the next poll
// retries.

import { statSync } from 'node:fs'
import { execFileSync } from 'node:child_process'

const SQLITE3 = '/usr/bin/sqlite3'

/** Rows for `sql` as parsed JSON objects ([] on no rows). Throws on error. */
export function queryRows(dbPath, sql) {
  const out = execFileSync(SQLITE3, ['-readonly', '-json', dbPath, sql],
                           { encoding: 'utf8', timeout: 15000 })
  const text = out.trim()
  return text ? JSON.parse(text) : []
}

/** `value` as a single-quoted SQL string literal. */
export function sqlString(value) {
  return `'${String(value).replace(/'/g, "''")}'`
}

/**
 * Incremental reader over an agent's transcript rows. `buildQuery(since)`
 * returns SQL selecting rows with id/seq > `since` in ascending order;
 * `rowToItem(row)` maps one row to {index, raw}. Like the JSONL tail, the
 * first readNew() returns everything (since = -1) and the daemon's cursor
 * skip drops what was already forwarded.
 */
export class SqliteTail {
  constructor(dbPath, buildQuery, rowToItem) {
    this.dbPath = dbPath
    this.buildQuery = buildQuery
    this.rowToItem = rowToItem
    this.since = -1
  }

  /** New rows since the last call: [{index, raw}]. Throws when the db is gone. */
  readNew() {
    statSync(this.dbPath)  // db deleted → throw → daemon exits
    let rows
    try {
      rows = queryRows(this.dbPath, this.buildQuery(this.since))
    } catch { return [] }  // locked/busy: retry next poll
    const out = []
    for (const row of rows) {
      const item = this.rowToItem(row)
      if (!item || !(item.index > this.since)) continue
      this.since = item.index
      out.push(item)
    }
    return out
  }
}
