#!/bin/bash
# Builds the PhiSetupUI NSIS plugin (installer/ui/phi_setup_ui.c) as an
# x86-unicode DLL with the Chromium checkout's clang-cl + lld-link and its
# Windows SDK - no Visual Studio, no mingw. Output:
# installer/ui/x86-unicode/PhiSetupUI.dll, which phi-installer.nsi picks up
# through `!addplugindir /x86-unicode`. The DLL is committed so a makensis
# run never needs the toolchain; re-run this after editing the .c file.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC_ROOT=${PHI_SRC_ROOT:-$(cd "$HERE/../../.." && pwd)}
LLVM="$SRC_ROOT/third_party/llvm-build/Release+Asserts/bin"
VS=$(ls -d "$SRC_ROOT"/third_party/depot_tools/win_toolchain/vs_files/*/ | head -1)
[[ -x "$LLVM/clang-cl" && -d "$VS" ]] || {
  echo "clang-cl or the Windows toolchain missing under $SRC_ROOT" >&2; exit 1; }
OUT="$HERE/x86-unicode"
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$LLVM/clang-cl" --target=i386-pc-windows-msvc /winsysroot "$VS" \
  /c /O2 /MT /W4 /WX /GS- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN \
  /D_CRT_SECURE_NO_WARNINGS /wd4100 \
  "$HERE/phi_setup_ui.c" "/Fo$TMP/phi_setup_ui.obj"
"$LLVM/lld-link" /winsysroot:"$VS" /MACHINE:X86 /DLL /NOLOGO /SUBSYSTEM:WINDOWS \
  /OPT:REF /OPT:ICF "/OUT:$OUT/PhiSetupUI.dll" "$TMP/phi_setup_ui.obj" \
  user32.lib gdi32.lib kernel32.lib advapi32.lib dwmapi.lib uxtheme.lib gdiplus.lib
rm -f "$OUT/PhiSetupUI.lib" "$OUT/PhiSetupUI.exp"
python3 - "$OUT/PhiSetupUI.dll" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
m = struct.unpack_from('<H', d, pe + 4)[0]
assert m == 0x14c, hex(m)
print("PhiSetupUI.dll: x86,", len(d), "bytes")
EOF
