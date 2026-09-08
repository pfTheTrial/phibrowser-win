# `test/probes/` — the ad-hoc tier

Everything here PRINTS. Nothing here is run by `test-box.sh`: the runner
takes its list from `test/suites.txt`, so a probe can never fail a sweep and
never has to be kept green.

Lane 6 §1 counted them: 64 one-offs, 26 of which ended in an unconditional
`process.exit(0)` and could not fail if they tried. They are still worth
keeping — they are the verb-usage examples and the box-driving recipes for a
feature bring-up (`b6…b16` = August bug numbers, `c1…c4` = agent and
credential checks, `rv-*` = Reader View, `lr-*` = lazy restore, `sv-*` =
sidebar staging) — but they are not tests, and mixing them with the suites
is what made "104 files in test/" look like coverage.

Rules:

* A probe imports only `node:*`, `../phi-test.mjs` and `../lib/*`. Nothing
  else — no sibling probe, no absolute path. (`sv-reader.mjs` used to import
  `file:///C:/Users/jdxin/…`, which meant it ran on exactly one machine;
  it now reads `PHI_SKILL_DIR`.)
* Every file starts with a one-line comment saying what it does.
* **A probe that grows assertions graduates to a suite** — it moves up to
  `test/`, uses `test()/run()` or `lib/check.mjs`, and gets a line in
  `test/suites.txt`. `sv-drag`, `sv-surface` and `sv-splitcell` made that
  trip; they assert, so they stayed out of here.

Running one, on the box:

    cd C:\Users\jdxin\phitest\probes && node diag.mjs

`test-box.sh --probes` uploads this directory alongside the suites when you
need one there; without the flag only the suites and `lib/` go up.
