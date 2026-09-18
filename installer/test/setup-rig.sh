#!/bin/bash
# setup-rig.sh <steps-file> [shot-names...]: runs the elevated PhiSetupCap driver
# (setupcap.ps1: updcap.ps1 steps + wtext) with the given steps, prints its
# log, pulls the named full-screen shots and crops each to the last focused
# window's rect (+ a margin) as shots/<name>.png.
set -uo pipefail
S=${PHI_SETUP_SHOTS:-/tmp/phi-setup-shots}; mkdir -p "$S"
H=jdxin@192.168.100.170
scp -q -o BatchMode=yes "$1" $H:setup/steps.txt
LOG=$(ssh -o BatchMode=yes $H 'Remove-Item C:\Users\jdxin\setup\done -ErrorAction SilentlyContinue; schtasks /Run /TN PhiSetupCap | Out-Null; $d=(Get-Date).AddSeconds(240); while (-not (Test-Path C:\Users\jdxin\setup\done) -and (Get-Date) -lt $d) { Start-Sleep -Milliseconds 500 }; Get-Content C:\Users\jdxin\setup\out.txt' 2>&1 | tr -d '\r')
echo "$LOG" | grep -v "^[0-9]*|[0-9]*|"
RECT=$(echo "$LOG" | grep -o 'rect=[0-9,-]*' | tail -1 | cut -d= -f2)
shift
for n in "$@"; do
  scp -q -o BatchMode=yes $H:setup/shot-$n.png $S/full-$n.png || continue
  if [[ -n "$RECT" ]]; then
    python3 - "$S/full-$n.png" "$S/$n.png" "$RECT" <<'PY'
import sys
from PIL import Image
src, dst, rect = sys.argv[1], sys.argv[2], [int(v) for v in sys.argv[3].split(',')]
im = Image.open(src); l, t, r, b = rect
m = 12
im.crop((max(0, l - m), max(0, t - m), min(im.width, r + m), min(im.height, b + m))).save(dst)
PY
  fi
done
