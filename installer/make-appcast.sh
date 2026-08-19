#!/bin/bash
# Emits the WinSparkle appcast item for one release — the Windows twin of
# the Mac's Sparkle feed (ota.phibrowser.com/win-<ring>/appcast/
# PhiBrowserWinUpdate.xml).
#
# Usage: make-appcast.sh --version X.Y.Z --url https://.../PhiBrowserSetup-X.Y.Z.exe \
#          --setup dist/PhiBrowserSetup-X.Y.Z.exe [--ed25519-key key.pem] [--out appcast.xml]
#
# With --ed25519-key (an openssl ed25519 PRIVATE key: `openssl genpkey
# -algorithm ed25519 -out key.pem`), the enclosure carries
# sparkle:edSignature and the client must ship the matching public key
# (client/phi_updater.cc kEdDsaPublicKey; derive it with
# `openssl pkey -in key.pem -pubout -outform DER | tail -c32 | base64`).
set -euo pipefail

VERSION="" URL="" SETUP="" KEY="" OUT="appcast.xml"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --url) URL="$2"; shift 2 ;;
    --setup) SETUP="$2"; shift 2 ;;
    --ed25519-key) KEY="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$VERSION" && -n "$URL" && -n "$SETUP" ]] || {
  echo "need --version, --url, --setup" >&2; exit 2; }

LENGTH=$(stat -c%s "$SETUP")
SIGNATURE_ATTR=""
if [[ -n "$KEY" ]]; then
  SIGNATURE=$(openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$SETUP" | base64 -w0)
  SIGNATURE_ATTR=" sparkle:edSignature=\"$SIGNATURE\""
fi

cat > "$OUT" <<XML
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Phi Browser for Windows</title>
    <item>
      <title>Version $VERSION</title>
      <pubDate>$(date -R)</pubDate>
      <enclosure url="$URL"
                 sparkle:version="$VERSION"
                 sparkle:os="windows-x64"
                 length="$LENGTH"
                 type="application/octet-stream"$SIGNATURE_ATTR />
    </item>
  </channel>
</rss>
XML
echo "wrote $OUT (length=$LENGTH$([[ -n "$KEY" ]] && echo ', signed'))"
