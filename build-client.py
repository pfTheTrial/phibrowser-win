#!/usr/bin/env python3
# Copyright 2026 The Fyde Innovations Limited. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Builds Phi.exe from this repo against a PREBUILT framework.

The framework build compiles no client code; it exports its recipe —
compile / rc / link commands plus every chromium-side input — as
out/phi_clientkit/kit.json (written by the fork's phi_sdk_package.py,
either into a local out dir or into a distributable SDK zip). This script
replays that recipe against client/*.cc: identical clang-cl flags (so the
objects are ABI-interchangeable with the framework's), the framework's own
objects and import libs on the link line, the stub scaffold object swapped
out for the real client ones. No gn, no ninja — a client edit rebuilds in
seconds, and no Chromium checkout is needed when building from an SDK.

Modes:
  --sdk <dir>   build inside an unzipped framework SDK (see README-SDK.md);
                the winsysroot comes from --winsysroot or $PHI_WINSYSROOT
                if the SDK doesn't already have one placed
  --out <dir>   build inside a local framework out dir (the default,
                /home/jx/Phi/chromuim/src/out/PhiFxWin)

The exe lands at <out>/Phi.exe; client resources (emoji catalog,
icon) are copied to <out>/resources/.
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys

REPO_DIR = os.path.dirname(os.path.abspath(__file__))
CLIENT_DIR = os.path.join(REPO_DIR, "client")
DEFAULT_OUT = "/home/jx/Phi/chromuim/src/out/PhiFxWin"
KIT = "phi_clientkit/kit.json"
OBJ_DIR = "phi_clientkit/obj"
RESOURCES = ("emoji-catalog.json", "phi.ico", "strings.json")


def stem_of(path):
    return os.path.splitext(os.path.basename(path))[0]


def load_kit(out_dir):
    kit_path = os.path.join(out_dir, KIT)
    if not os.path.exists(kit_path):
        sys.exit(f"{kit_path} missing — run the fork's phi_sdk_package.py "
                 "(or unzip a framework SDK) first")
    return json.load(open(kit_path))


def prepare_tree(out_dir, winsysroot):
    """The recipe's paths are relative to an out dir two levels below the
    source root (out/PhiFxWin): make sure the client repo and the
    winsysroot are reachable from there."""
    src_root = os.path.normpath(os.path.join(out_dir, "..", ".."))
    # Zip extraction drops execute bits — heal the bundled tools.
    for tool in ("third_party/llvm-build/Release+Asserts/bin/clang-cl",
                 "third_party/llvm-build/Release+Asserts/bin/lld-link",
                 "build/toolchain/win/rc/linux64/rc"):
        path = os.path.join(src_root, tool)
        if os.path.isfile(path) and not os.access(path, os.X_OK):
            os.chmod(path, 0o755)
    link = os.path.join(src_root, "phibrowser-win")
    if not os.path.exists(link):
        os.symlink(REPO_DIR, link)
        print(f"linked {link} -> {REPO_DIR}")
    m = re.search(r"/winsysroot:?(\S+)", json.load(
        open(os.path.join(out_dir, KIT)))["compile_template"])
    sysroot = os.path.normpath(os.path.join(out_dir, m.group(1)))
    if not os.path.isdir(sysroot):
        winsysroot = winsysroot or os.environ.get("PHI_WINSYSROOT")
        if not winsysroot or not os.path.isdir(winsysroot):
            sys.exit(f"winsysroot missing at {sysroot} — pass --winsysroot "
                     "(or set $PHI_WINSYSROOT) to your MSVC/Windows-SDK "
                     "vs_files/<hash> directory")
        os.makedirs(os.path.dirname(sysroot), exist_ok=True)
        os.symlink(os.path.abspath(winsysroot), sysroot)
        print(f"linked {sysroot} -> {winsysroot}")


def retarget(template, old_source, old_out, source, out):
    if old_source not in template or old_out not in template:
        sys.exit(f"cannot retarget recipe command onto {source}")
    return template.replace(old_source, source).replace(old_out, out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", help="unzipped framework SDK root")
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help="local framework build directory")
    parser.add_argument("--winsysroot",
                        help="MSVC/Windows-SDK vs_files/<hash> directory")
    parser.add_argument("-f", "--force", action="store_true",
                        help="recompile everything")
    parser.add_argument("--dev-hooks", action="store_true",
                        help="define PHI_DEV_HOOKS: compile in the "
                             "developer-only hooks (env overrides for the "
                             "appcast and vault helper, plaintext dev vault)."
                             " Never for a release build.")
    parser.add_argument("--only", action="append", default=[],
                        metavar="STEM",
                        help="compile only these client sources (stem, e.g. "
                             "phi_coordinator); repeatable")
    parser.add_argument("--no-link", action="store_true",
                        help="stop after compiling (syntax/type check)")
    args = parser.parse_args()
    out_dir = os.path.abspath(
        os.path.join(args.sdk, "out/PhiFxWin") if args.sdk else args.out)

    kit = load_kit(out_dir)
    prepare_tree(out_dir, args.winsysroot)
    os.makedirs(os.path.join(out_dir, OBJ_DIR), exist_ok=True)

    sources = sorted(
        os.path.join(CLIENT_DIR, f) for f in os.listdir(CLIENT_DIR)
        if f.endswith(".cc"))
    if args.only:
        wanted = {stem_of(s) for s in args.only}
        unknown = wanted - {stem_of(s) for s in sources}
        if unknown:
            sys.exit(f"--only: no such client source: {sorted(unknown)}")
        sources = [s for s in sources if stem_of(s) in wanted]
    compile_template = kit["compile_template"]
    if args.dev_hooks:
        compile_template = compile_template.replace(
            " /c ", " /DPHI_DEV_HOOKS=1 /c ", 1)
    headers_mtime = max(
        (os.path.getmtime(os.path.join(CLIENT_DIR, f))
         for f in os.listdir(CLIENT_DIR) if f.endswith(".h")), default=0)

    # Compile what's stale. Sources are addressed src-root-relative, exactly
    # as the recipe's own compile was.
    jobs = []
    client_objs = []
    for source in sources:
        stem = stem_of(source)
        obj = f"{OBJ_DIR}/{stem}.obj"
        client_objs.append(obj)
        cmd = retarget(compile_template, kit["template_source"],
                       kit["template_obj"],
                       f"../../phibrowser-win/client/{stem}.cc", obj)
        obj_path = os.path.join(out_dir, obj)
        if not args.force and os.path.exists(obj_path) and \
                os.path.getmtime(obj_path) >= max(
                    os.path.getmtime(source), headers_mtime):
            continue
        jobs.append((os.path.basename(source), cmd))

    res = f"{OBJ_DIR}/phi_client.res"
    client_objs.append(res)
    res_path = os.path.join(out_dir, res)
    rc_source = os.path.join(CLIENT_DIR, "phi_client.rc")
    if args.force or not os.path.exists(res_path) or \
            os.path.getmtime(res_path) < os.path.getmtime(rc_source):
        jobs.append(("phi_client.rc", retarget(
            kit["rc_template"], kit["rc_template_source"],
            kit["rc_template_out"],
            "../../phibrowser-win/client/phi_client.rc", res)))

    failed = False
    if jobs:
        print(f"compiling {len(jobs)} file(s)...")
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=os.cpu_count()) as pool:
            def run(job):
                name, cmd = job
                proc = subprocess.run(cmd, shell=True, cwd=out_dir,
                                      capture_output=True, text=True,
                                      errors="replace")
                return name, proc
            for name, proc in pool.map(run, jobs):
                output = "\n".join(
                    line for line in (proc.stdout + proc.stderr).splitlines()
                    if not line.startswith("Note: including file:")
                    and line.strip())
                if proc.returncode != 0:
                    failed = True
                    print(f"FAILED {name}\n{output}")
                elif output:
                    print(f"{name}: {output}")
    if failed:
        sys.exit(1)
    if args.no_link or args.only:
        print("compile OK (no link)")
        return

    # Link: the recipe's inputs with the scaffold stub swapped for the real
    # client objects, and the output renamed to Phi.exe.
    # The scaffold links Chromium's version resource; Phi.exe carries its
    # own VERSIONINFO (phi_client.rc), and two resources of the same id are
    # a link error, so the Chromium one is dropped here.
    inputs = [i for i in kit["link_inputs"]
              if i not in kit["scaffold_objs"]
              and not i.endswith("chrome_exe_version.res")] + client_objs
    rsp_path = os.path.join(out_dir, "phi_clientkit/Phi.exe.rsp")
    with open(rsp_path, "w") as f:
        f.write("\n".join(inputs) + "\n")
        f.write(kit["libs"] + " " + kit["solibs"] + " " +
                kit["ldflags"] + "\n")
    link = kit["link_command"].replace(kit["scaffold_exe"], "Phi.exe")
    link = re.sub(r'"@[^"]*"', f'"@{rsp_path}"', link)
    print("linking Phi.exe...")
    proc = subprocess.run(link, shell=True, cwd=out_dir,
                          capture_output=True, text=True, errors="replace")
    if proc.returncode != 0:
        print(proc.stdout + proc.stderr)
        sys.exit(1)

    resources_dir = os.path.join(out_dir, "resources")
    os.makedirs(resources_dir, exist_ok=True)
    for name in RESOURCES:
        shutil.copy2(os.path.join(CLIENT_DIR, "resources", name),
                     resources_dir)
    # Stage the vendored WinSparkle.dll beside the client: the sync ships
    # every top-level DLL, and the updater loads it from the exe's own dir.
    winsparkle = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "third_party", "winsparkle", "x64",
                              "WinSparkle.dll")
    staged = os.path.join(out_dir, "WinSparkle.dll")
    if os.path.exists(winsparkle) and (
            not os.path.exists(staged) or
            os.path.getmtime(staged) < os.path.getmtime(winsparkle)):
        shutil.copy2(winsparkle, staged)
        print("staged WinSparkle.dll")

    print("CLIENT BUILD OK: " + os.path.join(out_dir, "Phi.exe"))


if __name__ == "__main__":
    main()
