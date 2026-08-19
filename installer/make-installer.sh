#!/bin/bash
# Builds PhiBrowserSetup-<version>.exe.
#
# The payload is NOT shipped from this machine: the dev box already holds the
# full deployed tree (phi-fx-dev, kept current by sync-fx-win.sh), so the
# payload is staged box-side with robocopy and makensis runs there too (a
# portable NSIS zip, expanded once into ~\nsis). The finished setup exe is
# pulled back into installer/dist/.
#
# Usage: make-installer.sh [--version X.Y.Z] [--host user@host] [--keep-remote]
#   --version    defaults to PHI_WIN_VERSION_STRING in client/phi_version.h
#   --keep-remote  skip pulling the exe back (leave it on the box)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HOST="jdxin@192.168.100.170"
VERSION=""
PULL=yes
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --host) HOST="$2"; shift 2 ;;
    --keep-remote) PULL=no; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "$VERSION" ]]; then
  VERSION=$(grep -oP '(?<=PHI_WIN_VERSION_STRING ")[^"]+' \
    "$HERE/../client/phi_version.h")
fi
echo "version: $VERSION  host: $HOST"

NSIS_VERSION=3.10
NSIS_ZIP="nsis-${NSIS_VERSION}.zip"

# Portable NSIS on the box, expanded once.
if ! ssh "$HOST" "Test-Path C:\\Users\\jdxin\\nsis\\makensis.exe" | grep -q True; then
  echo "installing portable NSIS on the box..."
  if [[ ! -f "/tmp/$NSIS_ZIP" ]]; then
    curl -sL -o "/tmp/$NSIS_ZIP" \
      "https://downloads.sourceforge.net/project/nsis/NSIS%203/${NSIS_VERSION}/${NSIS_ZIP}"
  fi
  scp "/tmp/$NSIS_ZIP" "$HOST:$NSIS_ZIP"
  ssh "$HOST" "Expand-Archive -Force $NSIS_ZIP C:\\Users\\jdxin\\nsis-tmp; \
    if (Test-Path C:\\Users\\jdxin\\nsis) { Remove-Item -Recurse -Force C:\\Users\\jdxin\\nsis }; \
    Move-Item C:\\Users\\jdxin\\nsis-tmp\\nsis-${NSIS_VERSION} C:\\Users\\jdxin\\nsis; \
    Remove-Item -Recurse -Force C:\\Users\\jdxin\\nsis-tmp; echo nsis-ready"
fi

# Stage the payload box-side: the deployed tree's top level (the same globs
# the sync ships) PLUS the resource subdirectories from the initial full
# deploy — the browser is DOA without locales\*.pak. /PURGE keeps the stage
# exact across versions.
echo "staging payload on the box..."
ssh "$HOST" "robocopy C:\\Users\\jdxin\\phi-fx-dev C:\\Users\\jdxin\\phi-stage \
  *.exe *.dll *.pak *.bin *.dat *.manifest /PURGE /NJH /NJS /NDL /NFL | Out-Null; \
  foreach (\$d in 'Dictionaries','locales','MEIPreload','resources') { \
    robocopy C:\\Users\\jdxin\\phi-fx-dev\\\$d C:\\Users\\jdxin\\phi-stage\\\$d \
      /E /PURGE /NJH /NJS /NDL /NFL | Out-Null }; \
  echo staged"

scp "$HERE/phi-installer.nsi" "$HOST:phi-installer.nsi" >/dev/null

echo "running makensis on the box..."
ssh "$HOST" "C:\\Users\\jdxin\\nsis\\makensis.exe /V2 \
  /DPHI_VERSION=$VERSION /DPHI_STAGE=C:\\Users\\jdxin\\phi-stage \
  C:\\Users\\jdxin\\phi-installer.nsi" | tail -3

if [[ "$PULL" == yes ]]; then
  mkdir -p "$HERE/dist"
  echo "pulling PhiBrowserSetup-$VERSION.exe..."
  scp "$HOST:PhiBrowserSetup-$VERSION.exe" "$HERE/dist/" >/dev/null
  ls -la "$HERE/dist/PhiBrowserSetup-$VERSION.exe"
else
  echo "left on box: C:\\Users\\jdxin\\PhiBrowserSetup-$VERSION.exe"
fi
