# WinSparkle (vendored)

- Version: 0.9.4
- Source: https://github.com/vslavik/winsparkle/releases/tag/v0.9.4
  (binary release zip; the DLLs are the project's official x64 and
  ARM64 builds from that one zip)
- License: MIT-style, see COPYING (expat under COPYING.expat upstream)

Contents kept: `include/` (the C API headers), `x64/WinSparkle.dll`,
`arm64/WinSparkle.dll` (added 2026-09-14 for the Windows on ARM build).
The client loads the DLL dynamically (client/phi_updater.cc) from the
directory of phi_client.exe — a deploy without the DLL runs with the
updater disabled, nothing links against an import library.
build-client.py stages the one matching the kit's /MACHINE beside the exe.
The appcast tags enclosures `sparkle:os="windows-x64"` /
`"windows-arm64"` (installer/make-appcast.sh --arch); 0.9.4 matches the
running process's own architecture against that tag.

The signing tool from the same release lives at
installer/tools/winsparkle-tool.exe (EdDSA keygen + update signing on a
Windows machine); installer/make-appcast.sh can sign with a plain
openssl ed25519 key instead.
