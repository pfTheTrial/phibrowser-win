#!/bin/bash
# client/phi_client_main_win.cc is a deliberate copy of Chromium's
# chrome/app/chrome_exe_main_win.cc plus a few labelled phi divergences
# (docs/architecture-review.md, lane 7 §5.7). Upstream changes it every few
# milestones, and a silent drift means an upstream fix is quietly dropped.
# This reports the diff so a human can re-apply it; it does not judge which
# hunks are legitimate, because that judgement changes per milestone.
set -uo pipefail
cd "$(dirname "$0")/.."
FORK_SRC="${PHI_FORK_SRC:-../chromuim/src}"
UPSTREAM="$FORK_SRC/chrome/app/chrome_exe_main_win.cc"
MINE=client/phi_client_main_win.cc
if [[ ! -f $UPSTREAM ]]; then
  echo "skip: no upstream at $UPSTREAM (set PHI_FORK_SRC)"
  exit 0
fi
hunks=$(diff -u "$UPSTREAM" "$MINE" | grep -c '^@@')
echo "$hunks divergent hunk(s) against $(basename "$UPSTREAM")"
diff -u "$UPSTREAM" "$MINE" | grep '^@@' | sed 's/^/  /'
echo "Review them against the labelled phi blocks in $MINE."
exit 0
