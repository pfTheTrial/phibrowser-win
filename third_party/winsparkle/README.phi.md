# WinSparkle (vendored)

- Version: 0.9.4
- Source: https://github.com/vslavik/winsparkle/releases/tag/v0.9.4
  (binary release zip; DLL is the project's official x64 build)
- License: MIT-style, see COPYING (expat under COPYING.expat upstream)

Contents kept: `include/` (the C API headers), `x64/WinSparkle.dll`.
The client loads the DLL dynamically (client/phi_updater.cc) from the
directory of phi_client.exe — a deploy without the DLL runs with the
updater disabled, nothing links against an import library.

The signing tool from the same release lives at
installer/tools/winsparkle-tool.exe (EdDSA keygen + update signing on a
Windows machine); installer/make-appcast.sh can sign with a plain
openssl ed25519 key instead.
