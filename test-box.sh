#!/bin/bash
# Tier 1 — the box integration sweep (docs/architecture-review.md §3.8,
# appendix lane6-tests-tooling.md §5.1).
#
#   ./test-box.sh                       every suite in test/suites.txt, in order
#   ./test-box.sh --suite suite-tabs    one suite (repeatable)
#   ./test-box.sh --isolation reuse     the OLD behaviour: one shared profile
#   ./test-box.sh --network             also run the suites marked !net
#   ./test-box.sh --no-sync             trust what is already on the box
#
# What it owns, so no suite has to: kill, sync, hash-verify, reset the
# profile, seed prefs, launch, wait for a LIVE control pipe, run an ORDERED
# list with a per-suite cap, capture artifacts on failure, and write one JSON
# report. Exit 0 only when every suite passed.
#
# The headline change over run-fx-all.sh is `--isolation`. Four suites have
# failed every sweep since phase 0 purely because they fight over one profile
# (lane 6 §6 item 2): suite-pinned wipes every pin and bookmark, suite-spaces
# deletes every other Space to test the last-space rule, suite-persist leaves
# Spaces behind, and suite-emoji drives Space id "1", which only a profile
# nobody has renamed still has. `fresh` (the default) resets the profile
# between suites; `reuse` reproduces the old sweep for comparison.
#
# NOTE ON QUOTING. Every PowerShell block below is a SINGLE-quoted bash
# string, so a PowerShell `$var` reaches the box as written. Do not switch
# one to double quotes to interpolate a shell variable: bash then eats the
# `$`, PowerShell reads an unbound variable and the run dies somewhere else
# entirely. Interpolate with printf against a `%s` in the single-quoted
# template instead — see ps_run_suite below.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_FILE="${PHI_BOX_ENV:-$REPO/box.env}"
[ -r "$ENV_FILE" ] || { echo "test-box: no box.env at $ENV_FILE" >&2; exit 2; }
# shellcheck source=box.env
. "$ENV_FILE"

SYNC_SCRIPT="${PHI_SYNC_SCRIPT:-/home/jx/Phi/phibrowser-win-build/sync-fx-win.sh}"
SEED_SCRIPT="${PHI_SEED_SCRIPT:-/home/jx/Phi/phibrowser-win-build/seed-fx-prefs.sh}"
SUITES_FILE="$REPO/test/suites.txt"
TESTDIR="$REPO/test"

ISOLATION=fresh
NETWORK=0
DO_SYNC=1
WITH_PROBES=0
REPORT=""
ARTIFACTS=""
declare -a ONLY=()

while [ $# -gt 0 ]; do
  case "$1" in
    --suite) ONLY+=("${2:?--suite needs a name}"); shift 2 ;;
    --isolation) ISOLATION="${2:?--isolation needs fresh|golden|reuse}"; shift 2 ;;
    --network) NETWORK=1; shift ;;
    --no-sync) DO_SYNC=0; shift ;;
    --probes) WITH_PROBES=1; shift ;;
    --report) REPORT="${2:?--report needs a path}"; shift 2 ;;
    --artifacts) ARTIFACTS="${2:?--artifacts needs a directory}"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "test-box: unknown argument: $1" >&2; exit 2 ;;
  esac
done

case "$ISOLATION" in
  fresh|golden|reuse) ;;
  *) echo "test-box: --isolation must be fresh, golden or reuse (got $ISOLATION)" >&2
     exit 2 ;;
esac

STAMP="$(date +%Y%m%d-%H%M%S)"
REPORT="${REPORT:-/tmp/phi-box-$STAMP.json}"
ARTIFACTS="${ARTIFACTS:-/tmp/phi-box-$STAMP-artifacts}"
mkdir -p "$ARTIFACTS" || exit 2

HOST="$PHI_BOX_HOST"
box() { ssh -o BatchMode=yes "$HOST" "$@"; }

# The same box path, spelled the way sftp/scp want it (/C:/Users/...).
sftp_path() { printf '/%s' "$(printf '%s' "$1" | tr '\\' '/')"; }
REMOTE_TEST_URL="$(sftp_path "$PHI_TEST_DIR")/"

die() { echo "ABORT: $*" >&2; exit 2; }

# ---------------------------------------------------------------- the list
read_suites() {
  local line name flags
  while IFS= read -r line; do
    line="${line%%#*}"
    line="$(echo "$line" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
    [ -n "$line" ] || continue
    name="${line%% *}"
    flags="${line#"$name"}"
    if [ "$NETWORK" -eq 0 ] && [ "${flags//[[:space:]]/}" = "!net" ]; then
      SKIPPED+=("$name")
      continue
    fi
    SUITES+=("$name")
  done < "$SUITES_FILE"
}

declare -a SUITES=() SKIPPED=()
if [ "${#ONLY[@]}" -gt 0 ]; then
  SUITES=("${ONLY[@]}")
else
  [ -r "$SUITES_FILE" ] || die "no suite list at $SUITES_FILE"
  read_suites
fi
[ "${#SUITES[@]}" -gt 0 ] || die "no suites to run"

for s in "${SUITES[@]}"; do
  [ -f "$TESTDIR/$s.mjs" ] || die "$SUITES_FILE names $s, but test/$s.mjs does not exist"
done

# --------------------------------------------------------- PowerShell blocks
# Every one of these is single-quoted: PowerShell's $ survives untouched.
ps_kill='
Get-Process Phi -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddSeconds(20)
while (((Get-Process Phi -ErrorAction SilentlyContinue) | Measure-Object).Count -gt 0 -and (Get-Date) -lt $deadline) {
  Get-Process Phi -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 500
}
if (((Get-Process Phi -ErrorAction SilentlyContinue) | Measure-Object).Count -gt 0) {
  Write-Output "STILL-RUNNING"; exit 3
}
Write-Output "KILLED"
'

# Launch, then wait for a pointer file naming a LIVE process. A fixed sleep
# raced a slower cold start and every suite then connected to the previous
# run's dead pipe (ENOENT), which read as nineteen total failures.
ps_launch_tmpl='
Start-Sleep 1
Start-ScheduledTask -TaskName @@TASK@@
$deadline = (Get-Date).AddSeconds(@@SECS@@)
do {
  Start-Sleep -Milliseconds 500
  $ready = $false
  if (Test-Path "@@DIR@@\CDPAgentSocket") {
    $pipe = (Get-Content "@@DIR@@\CDPAgentSocket")[0]
    if ($pipe -match "phi-cdp\.(\d+)\.") {
      $ready = $null -ne (Get-Process -Id $matches[1] -ErrorAction SilentlyContinue)
    }
  }
} until ($ready -or (Get-Date) -gt $deadline)
if (-not $ready) { Write-Output "NO-PIPE"; exit 2 }
Write-Output "LIVE"
'

# Wipe the profile back to first-run.
ps_wipe_tmpl='
$dir = "@@DIR@@"
if (Test-Path $dir) { Remove-Item -Path (Join-Path $dir "*") -Recurse -Force -ErrorAction SilentlyContinue }
else { New-Item -ItemType Directory -Path $dir | Out-Null }
$left = @(Get-ChildItem -Path $dir -Force -ErrorAction SilentlyContinue)
Write-Output ("RESET " + $left.Count)
'

# Restore the profile from the golden snapshot.
ps_golden_tmpl='
$golden = "@@GOLDEN@@"
$dir = "@@DIR@@"
if (-not (Test-Path $golden)) { Write-Output "NO-GOLDEN"; exit 4 }
if (Test-Path $dir) { Remove-Item -Path (Join-Path $dir "*") -Recurse -Force -ErrorAction SilentlyContinue }
else { New-Item -ItemType Directory -Path $dir | Out-Null }
Copy-Item -Path (Join-Path $golden "*") -Destination $dir -Recurse -Force -ErrorAction SilentlyContinue
Write-Output "RESTORED"
'

# Snapshot the settled profile as the golden one.
ps_snapshot_tmpl='
$dir = "@@DIR@@"
$golden = "@@GOLDEN@@"
if (Test-Path $golden) { Remove-Item -Path $golden -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Path $golden | Out-Null
Copy-Item -Path (Join-Path $dir "*") -Destination $golden -Recurse -Force -ErrorAction SilentlyContinue
Write-Output "SNAPSHOT"
'

# Run one suite under a hard cap. A wedged verb fails the suite instead of
# hanging the run.
ps_run_tmpl='
cd "@@TESTDIR@@"
$env:PHI_USER_DATA_DIR = "@@DIR@@"
$env:PHI_TEST_REPORT = "@@TESTDIR@@\report.json"
Remove-Item "$env:PHI_TEST_REPORT" -ErrorAction SilentlyContinue
Remove-Item suite-out.txt, suite-err.txt -ErrorAction SilentlyContinue
$p = Start-Process node -ArgumentList "@@SUITE@@.mjs" -NoNewWindow -PassThru -RedirectStandardOutput suite-out.txt -RedirectStandardError suite-err.txt
if (-not $p.WaitForExit(@@CAPMS@@)) {
  $p.Kill(); Start-Sleep 1
  Write-Output "PHI-SUITE-TIMEOUT"
  if (Test-Path suite-out.txt) { Get-Content suite-out.txt }
  if (Test-Path suite-err.txt) { Get-Content suite-err.txt | Select-Object -First 20 }
  exit 124
}
if (Test-Path suite-out.txt) { Get-Content suite-out.txt }
if (Test-Path suite-err.txt) { Get-Content suite-err.txt | Select-Object -First 20 }
Write-Output ("PHI-SUITE-EXIT " + $p.ExitCode)
exit $p.ExitCode
'

# The client log around a failure.
ps_log_tmpl='
$log = "@@DIR@@\chrome_debug.log"
if (Test-Path $log) {
  Get-Content $log -Tail 600 | Select-String -Pattern "phibrowser-win" | Select-Object -Last 120 | ForEach-Object { $_.Line }
} else { Write-Output "no chrome_debug.log" }
'

# ------------------------------------------------------------------- helpers
# fill <template> [NAME VALUE]... — placeholder substitution that leaves both
# PowerShell's `$` and Windows backslashes exactly as written.
fill() {
  local out="$1"; shift
  while [ $# -gt 1 ]; do
    out="${out//@@$1@@/$2}"
    shift 2
  done
  printf '%s' "$out"
}

kill_app() {
  box "$ps_kill" >/dev/null 2>&1
}

launch_app() {
  local out
  out="$(box "$(fill "$ps_launch_tmpl" TASK "$PHI_LAUNCH_TASK" \
                     SECS "$PHI_LAUNCH_TIMEOUT" DIR "$PHI_USER_DATA_DIR")" \
           2>&1 | tr -d '\r')"
  case "$out" in
    *LIVE*) return 0 ;;
    *) echo "    launch failed: ${out:-no output}" >&2; return 1 ;;
  esac
}

seed_prefs() {
  "$SEED_SCRIPT" "$PHI_USER_DATA_DIR" >/dev/null 2>&1 ||
    { echo "    could not seed test prefs" >&2; return 1; }
}

reset_profile() {
  case "$ISOLATION" in
    fresh)
      box "$(fill "$ps_wipe_tmpl" DIR "$PHI_USER_DATA_DIR")" >/dev/null 2>&1 ||
        return 1 ;;
    golden)
      local out
      out="$(box "$(fill "$ps_golden_tmpl" GOLDEN "$PHI_GOLDEN_DIR" \
                         DIR "$PHI_USER_DATA_DIR")" 2>&1 | tr -d '\r')"
      case "$out" in
        *RESTORED*) ;;
        *NO-GOLDEN*)
          echo "  no golden profile yet — building one from a clean launch"
          box "$(fill "$ps_wipe_tmpl" DIR "$PHI_USER_DATA_DIR")" >/dev/null 2>&1
          seed_prefs || return 1
          launch_app || return 1
          kill_app
          box "$(fill "$ps_snapshot_tmpl" DIR "$PHI_USER_DATA_DIR" \
                       GOLDEN "$PHI_GOLDEN_DIR")" >/dev/null 2>&1 || return 1
          ;;
        *) echo "    profile restore failed: $out" >&2; return 1 ;;
      esac ;;
    reuse) return 0 ;;
  esac
  seed_prefs
}

json_escape() { python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'; }

# ---------------------------------------------------------------- deploy
echo "=== test-box: $HOST, isolation=$ISOLATION, ${#SUITES[@]} suite(s)"
[ "${#SKIPPED[@]}" -gt 0 ] &&
  echo "    skipping (needs --network): ${SKIPPED[*]}"

kill_app

EXE_HASH="(not verified)"
if [ "$DO_SYNC" -eq 1 ]; then
  [ -f "$PHI_OUT_DIR/Phi.exe" ] || die "$PHI_OUT_DIR/Phi.exe missing — build first, or pass --no-sync"
  want="$(md5sum "$PHI_OUT_DIR/Phi.exe" | cut -d' ' -f1)"
  got=""
  for i in 1 2 3 4 5 6; do
    "$SYNC_SCRIPT" >/tmp/fxsync.log 2>&1
    got="$(box '(Get-FileHash -Algorithm MD5 '"$PHI_BOX_DEPLOY"'\Phi.exe).Hash.ToLower()' \
             2>/dev/null | tr -d '\r\n')"
    [ "$want" = "$got" ] && break
    echo "    sync round $i: hash mismatch (want $want, got ${got:-none}), retrying"
    sleep 2
  done
  [ "$want" = "$got" ] || die "deployed Phi.exe never matched the local build"
  EXE_HASH="$want"
  echo "    exe verified: $want"
fi

# Upload the harness, the LIBRARY, the rig and every suite that will run.
# Deployment moves in lockstep with the imports: a suite that imports
# ./lib/foo.mjs is dead on the box unless lib/foo.mjs went up with it, so the
# whole of test/lib/ goes every time rather than a curated list.
echo "    uploading harness + lib + $(( ${#SUITES[@]} )) suite(s)"
box 'New-Item -ItemType Directory -Force -Path '"$PHI_TEST_DIR"'\lib | Out-Null' >/dev/null 2>&1
[ "$WITH_PROBES" -eq 1 ] &&
  box 'New-Item -ItemType Directory -Force -Path '"$PHI_TEST_DIR"'\probes | Out-Null' >/dev/null 2>&1
{
  printf 'put phi-test.mjs\nput uicap.ps1\nput suites.txt\n'
  for f in "$TESTDIR"/lib/*.mjs; do
    printf 'put lib/%s lib/%s\n' "$(basename "$f")" "$(basename "$f")"
  done
  for name in "${SUITES[@]}"; do printf 'put %s.mjs\n' "$name"; done
  if [ "$WITH_PROBES" -eq 1 ]; then
    for f in "$TESTDIR"/probes/*; do
      if [ -d "$f" ]; then
        # A probe's page folder (mc-pages): one level deep is all there is.
        printf -- '-mkdir probes/%s\nput probes/%s/* probes/%s/\n' \
          "$(basename "$f")" "$(basename "$f")" "$(basename "$f")"
      else
        printf 'put probes/%s probes/%s\n' "$(basename "$f")" "$(basename "$f")"
      fi
    done
  fi
} | (cd "$TESTDIR" && sftp -b - -o BatchMode=yes "$HOST:$REMOTE_TEST_URL") \
      >/tmp/phi-sftp.log 2>&1 ||
  die "sftp upload failed — see /tmp/phi-sftp.log"

# ------------------------------------------------------------------- run
if [ "$ISOLATION" = reuse ]; then
  echo "--- one shared profile (the old sweep)"
  seed_prefs || die "could not seed test prefs"
  launch_app || die "client never published a live control pipe"
fi

FAILED=0
RUN_STARTED="$(date -Is)"
: >"$ARTIFACTS/summary.txt"
CASES_JSON="$ARTIFACTS/.cases"
: >"$CASES_JSON"

for suite in "${SUITES[@]}"; do
  echo ""
  echo "=== $suite  $(date +%H:%M:%S)"
  started=$(date +%s)
  status=pass
  if [ "$ISOLATION" != reuse ]; then
    kill_app
    if ! reset_profile; then
      echo "    FAILED to reset the profile"
      status=setup-failed
    elif ! launch_app; then
      status=launch-failed
    fi
  fi

  out=""
  rc=0
  if [ "$status" = pass ]; then
    out="$(box "$(fill "$ps_run_tmpl" TESTDIR "$PHI_TEST_DIR" \
                       DIR "$PHI_USER_DATA_DIR" SUITE "$suite" \
                       CAPMS "$(( PHI_SUITE_TIMEOUT * 1000 ))")" \
             2>&1 | tr -d '\r')"
    rc=$?
    echo "$out"
    case "$out" in
      *PHI-SUITE-TIMEOUT*) status=timeout ;;
      *) [ "$rc" -eq 0 ] || status=fail ;;
    esac
  fi

  elapsed=$(( $(date +%s) - started ))
  if [ "$status" != pass ]; then
    FAILED=1
    echo "    $suite: $status (${elapsed}s) — capturing artifacts"
    mkdir -p "$ARTIFACTS/$suite"
    printf '%s\n' "$out" >"$ARTIFACTS/$suite/stdout.txt"
    box "$(fill "$ps_log_tmpl" DIR "$PHI_USER_DATA_DIR")" \
      >"$ARTIFACTS/$suite/client-log.txt" 2>&1
    box 'cd '"$PHI_TEST_DIR"'; node lib\artifacts.mjs '"$PHI_TEST_DIR"'\artifacts' \
      >/dev/null 2>&1
    scp -q -o BatchMode=yes \
      "$HOST:$(sftp_path "$PHI_TEST_DIR")/artifacts/state.json" \
      "$ARTIFACTS/$suite/state.json" 2>/dev/null || true
  else
    echo "    $suite: pass (${elapsed}s)"
  fi

  # The suite's own machine-readable result, written by run()/finish().
  scp -q -o BatchMode=yes \
    "$HOST:$(sftp_path "$PHI_TEST_DIR")/report.json" \
    "$ARTIFACTS/$suite.report.json" 2>/dev/null || true

  printf '%s\t%s\t%s\n' "$suite" "$status" "$elapsed" >>"$CASES_JSON"
  printf '%-18s %-14s %4ss\n' "$suite" "$status" "$elapsed" >>"$ARTIFACTS/summary.txt"
done

# ---------------------------------------------------------------- report
python3 - "$REPORT" "$ARTIFACTS" "$RUN_STARTED" "$HOST" "$ISOLATION" \
         "$EXE_HASH" <<'PY'
import json, os, sys
report, artifacts, started, host, isolation, exe = sys.argv[1:7]
suites = []
totals = {"pass": 0, "fail": 0}
with open(os.path.join(artifacts, ".cases")) as f:
    for line in f:
        name, status, secs = line.rstrip("\n").split("\t")
        entry = {"suite": name, "status": status, "seconds": int(secs)}
        detail = os.path.join(artifacts, name + ".report.json")
        if os.path.exists(detail):
            try:
                with open(detail, encoding="utf-8") as d:
                    entry["cases"] = json.load(d)
            except Exception as e:
                entry["cases"] = {"error": str(e)}
        cases = entry.get("cases") or {}
        totals["pass"] += cases.get("pass", 0)
        totals["fail"] += cases.get("fail", 1 if status != "pass" else 0)
        suites.append(entry)
payload = {"startedAt": started, "host": host, "isolation": isolation,
           "exeMd5": exe, "artifacts": artifacts,
           "suites": suites,
           "suitesPassed": sum(1 for s in suites if s["status"] == "pass"),
           "suitesFailed": sum(1 for s in suites if s["status"] != "pass"),
           "casesPassed": totals["pass"], "casesFailed": totals["fail"]}
with open(report, "w", encoding="utf-8") as f:
    json.dump(payload, f, indent=1)
    f.write("\n")
print("")
print(f"{payload['suitesPassed']} suite(s) passed, "
      f"{payload['suitesFailed']} failed  |  "
      f"{payload['casesPassed']} case(s) passed, {payload['casesFailed']} failed")
print(f"report:    {report}")
print(f"artifacts: {artifacts}")
PY

rm -f "$CASES_JSON"
if [ "$FAILED" -eq 0 ]; then echo "ALL SUITES PASSED"; else echo "SOME SUITES FAILED"; fi
exit "$FAILED"
