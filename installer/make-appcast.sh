#!/bin/bash
# Emits the WinSparkle appcast item for one release — the Windows twin of
# the Mac's Sparkle feed (ota.phibrowser.com/win-<ring>/appcast/
# PhiBrowserWinUpdate.xml).
#
# Usage: make-appcast.sh --version X.Y.Z --url https://.../PhiBrowserSetup-X.Y.Z.exe \
#          --setup dist/PhiBrowserSetup-X.Y.Z.exe [--arch x64|arm64] \
#          [--arm64-setup dist/PhiBrowserSetup-X.Y.Z-arm64.exe --arm64-url https://.../PhiBrowserSetup-X.Y.Z-arm64.exe] \
#          [--ed25519-key key.pem] [--out appcast.xml]
#
# One item per version, one enclosure per architecture: the enclosure's
# sparkle:os ("windows-x64" / "windows-arm64") is what WinSparkle matches
# against the running Phi's own architecture, so an x64 install on an ARM
# PC keeps getting x64 and the arm64 build only ever gets arm64. --arch
# names the first enclosure's architecture (default x64, or arm64 when the
# --setup file is named *-arm64.exe); --arm64-setup/--arm64-url add the
# arm64 enclosure beside an x64 one. Both are signed with the same key.
#
# With --ed25519-key (an openssl ed25519 PRIVATE key: `openssl genpkey
# -algorithm ed25519 -out key.pem`), the enclosure carries
# sparkle:edSignature and the client must ship the matching public key
# (client/phi_updater.cc kEdDsaPublicKey; derive it with
# `openssl pkey -in key.pem -pubout -outform DER | tail -c32 | base64`).
set -euo pipefail

VERSION="" URL="" SETUP="" ARCH="" ARM64_SETUP="" ARM64_URL="" KEY="" OUT="appcast.xml"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --url) URL="$2"; shift 2 ;;
    --setup) SETUP="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --arm64-setup) ARM64_SETUP="$2"; shift 2 ;;
    --arm64-url) ARM64_URL="$2"; shift 2 ;;
    --ed25519-key) KEY="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$VERSION" && -n "$URL" && -n "$SETUP" ]] || {
  echo "need --version, --url, --setup" >&2; exit 2; }
if [[ -z "$ARCH" ]]; then
  [[ "$SETUP" == *-arm64.exe ]] && ARCH=arm64 || ARCH=x64
fi
[[ "$ARCH" == x64 || "$ARCH" == arm64 ]] || { echo "--arch must be x64 or arm64" >&2; exit 2; }
[[ -z "$ARM64_SETUP" && -z "$ARM64_URL" || -n "$ARM64_SETUP" && -n "$ARM64_URL" ]] || {
  echo "--arm64-setup and --arm64-url go together" >&2; exit 2; }
[[ -n "$ARM64_SETUP" && "$ARCH" == arm64 ]] && {
  echo "--arm64-setup beside an --arch arm64 enclosure: two arm64 enclosures" >&2; exit 2; }

# One <enclosure> element: file, url, architecture.
enclosure() {
  local setup="$1" url="$2" arch="$3" length signature_attr=""
  length=$(stat -c%s "$setup")
  if [[ -n "$KEY" ]]; then
    signature_attr=" sparkle:edSignature=\"$(openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$setup" | base64 -w0)\""
  fi
  cat <<XML
      <enclosure url="$url"
                 sparkle:version="$VERSION"
                 sparkle:os="windows-$arch"
                 length="$length"
                 type="application/octet-stream"$signature_attr />
XML
}

ENCLOSURES=$(enclosure "$SETUP" "$URL" "$ARCH")
SUMMARY="$ARCH:$(stat -c%s "$SETUP")"
if [[ -n "$ARM64_SETUP" ]]; then
  ENCLOSURES+=$'\n'$(enclosure "$ARM64_SETUP" "$ARM64_URL" arm64)
  SUMMARY+=" arm64:$(stat -c%s "$ARM64_SETUP")"
fi

cat > "$OUT" <<XML
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Phi Browser for Windows</title>
    <item>
      <title>Version $VERSION</title>
      <pubDate>$(date -R)</pubDate>
$ENCLOSURES
    </item>
  </channel>
</rss>
XML
echo "wrote $OUT ($SUMMARY$([[ -n "$KEY" ]] && echo ', signed'))"
