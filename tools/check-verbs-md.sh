#!/bin/sh
# Fails when tools/phi-browser-skill/references/verbs.md has drifted from the
# C++ verb registry (client/phi_verb_registry.cc). Regenerates the document
# into memory and diffs it against the checked-in one; the fix is always
# `tools/gen-verbs-md.py`.
#
# Run it from CI, from the release gate, or by hand after touching the
# registry table.
exec python3 "$(dirname "$0")/gen-verbs-md.py" --check
