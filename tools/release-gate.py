#!/usr/bin/env python3
# Copyright 2026 The Fyde Innovations Limited. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Release gate for Phi.exe (docs/architecture-review.md §3.9 "Gating").

Asserts, on a BUILT exe and the tree it came from, that the developer-only
hooks are absent and the packaging is consistent. Every check is one the
review found violated at least once; each failure names the finding.

  tools/release-gate.py [--out <dir>] [--source-only]

Exit 0 = all checks pass. Run it from build-client.py's release path and
before cutting an installer. --source-only skips the binary checks.
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLIENT = os.path.join(REPO, "client")
DEFAULT_OUT = "/home/jx/Phi/chromuim/src/out/PhiFxWin"

# Strings that only exist in the exe when PHI_DEV_HOOKS was defined. They
# are the environment-variable NAMES the hooks read, which the compiler
# keeps verbatim; a release exe must not contain them.
DEV_HOOK_STRINGS = [b"PHI_APPCAST_URL", b"PHI_APPCAST_ED25519",
                    b"PHI_VAULT_HELPER"]


def fail(msg):
    print("FAIL: " + msg)
    return 1


def strings_in(path, needles):
    with open(path, "rb") as f:
        data = f.read()
    hits = []
    for n in needles:
        # Both narrow and UTF-16LE spellings: the switch name is narrow, the
        # env names are read through GetEnvironmentVariableA (narrow) but
        # check wide too so a future W-variant cannot slip through.
        wide = n.decode("ascii").encode("utf-16-le")
        if n in data or wide in data:
            hits.append(n.decode("ascii"))
    return hits


def check_binary(out_dir):
    rc = 0
    exe = os.path.join(out_dir, "Phi.exe")
    if not os.path.isfile(exe):
        return fail(f"{exe} missing — build first (or pass --source-only)")
    hits = strings_in(exe, DEV_HOOK_STRINGS)
    if hits:
        rc |= fail("dev hooks compiled in (built with --dev-hooks?): "
                   + ", ".join(hits))
    # The switch NAME is not checked in the binary: Chromium's own switch
    # tables linked into the exe spell it too. The source check above is
    # the gate for the injection (review §4 item 1).
    res = os.path.join(out_dir, "phi_clientkit", "obj", "phi_client.res")
    if os.path.isfile(res):
        with open(res, "rb") as f:
            blob = f.read()
        if "VS_VERSION_INFO".encode("utf-16-le") not in blob:
            rc |= fail("phi_client.res carries no VS_VERSION_INFO "
                       "(review §4 item 42)")
    if rc == 0:
        print("ok: binary")
    return rc


def check_sources():
    rc = 0
    # Installer and rigs must name the exe the build produces.
    installer_dir = os.path.join(REPO, "installer")
    for root, _, files in os.walk(installer_dir):
        for name in files:
            if not name.endswith((".nsi", ".sh", ".ps1")):
                continue
            path = os.path.join(root, name)
            with open(path, errors="replace") as f:
                if "phi_client.exe" in f.read():
                    rc |= fail(f"{os.path.relpath(path, REPO)} references "
                               "phi_client.exe; the build produces Phi.exe "
                               "(review §4 item 8)")
    main_cc = os.path.join(CLIENT, "phi_client_main_win.cc")
    with open(main_cc, errors="replace") as f:
        if re.search(r'"remote-debugging-port"', f.read()):
            rc |= fail("phi_client_main_win.cc still injects "
                       "remote-debugging-port (review §4 item 1)")
    # Every environment override must sit behind kPhiDevHooks.
    for name in ("phi_updater.cc", "phi_coordinator.cc"):
        path = os.path.join(CLIENT, name)
        with open(path, errors="replace") as f:
            src = f.read()
        for env in ("PHI_APPCAST_URL", "PHI_APPCAST_ED25519",
                    "PHI_VAULT_HELPER"):
            idx = src.find(f'"{env}"')
            if idx < 0:
                continue
            window = src[max(0, idx - 1500):idx]
            if "kPhiDevHooks" not in window:
                rc |= fail(f"{name}: {env} is read without a kPhiDevHooks "
                           "gate nearby (review §4 items 3, 4)")
    # Analytics ship opt-in: the model's default must be off (review §3.9).
    model_h = os.path.join(CLIENT, "phi_app_model.h")
    if os.path.isfile(model_h):
        with open(model_h, errors="replace") as f:
            if not re.search(r"analytics_enabled\s*=\s*false", f.read()):
                rc |= fail("phi_app_model.h: analytics_enabled must default "
                           "to false (review §4 item 32)")
    rc_path = os.path.join(CLIENT, "phi_client.rc")
    with open(rc_path, errors="replace") as f:
        if "VS_VERSION_INFO" not in f.read():
            rc |= fail("phi_client.rc has no VS_VERSION_INFO")
    if rc == 0:
        print("ok: sources")
    return rc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=DEFAULT_OUT)
    parser.add_argument("--source-only", action="store_true")
    args = parser.parse_args()
    rc = check_sources()
    if not args.source_only:
        rc |= check_binary(os.path.abspath(args.out))
    print("RELEASE GATE " + ("PASSED" if rc == 0 else "FAILED"))
    sys.exit(rc)


if __name__ == "__main__":
    main()
