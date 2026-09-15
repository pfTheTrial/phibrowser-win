# phibrowser-win

The PhiBrowser Windows client for the framework/client architecture
(chromium branch `phi-framework-win`; design:
`src/windows-framework-design.md`). This repo is symlinked into the
chromium tree as `//phibrowser-win`, mirroring how `phibrowser-mac` and
`phibrowser-linux` sit beside it.

Layout:
- `client/` — sources for `phi_client.exe`, the exe that hosts the browser
  process (calls ChromeMain, never returns; all UI happens in framework →
  client callbacks). `phi_client_main_win.cc` is a deliberate copy of
  chrome/app/chrome_exe_main_win.cc plus the phi divergences — keep it
  diffable against upstream on rebases.
- GN target: `//chrome:phi_client` (defined in chrome/BUILD.gn under
  `is_win && is_phi_browser`; sources referenced through the symlink).

Build (from src, after the toolchain env vars):
```
autoninja -C out/PhiFxWinShip phi_client
```
Children (renderer/GPU/utility) run the stock `chrome.exe` sitting beside
`phi_client.exe` — pulled in automatically via data_deps.

Everything runs on the non-component build (docs/static-build-plan.md):
`out/PhiFxWinShip` (`is_component_build = false`, one fat chrome.dll) is
what `build-fx.sh` / `build-fx-client.sh` / `upload-symbols.sh` default
to, `sync-fx-win.sh` pushes it to the box's `phi-fx-ship` (the "Phi Dev"
desktop shortcut and the PhiFxLaunch / PhiDevLaunch tasks run it), and
`installer/make-installer.sh` packages it. The component build
(`out/PhiFxWin` → `phi-fx-dev`) is retired since 2026-09-14; it still
builds with `PHI_OUT=out/PhiFxWin PHI_REMOTE_DIR=phi-fx-dev` if a
component-only investigation ever needs it. The framework/client API must
stay allocation-safe across the two builds' separate heaps (contract rule
6 in phi_framework_api.h, enforced by `tools/release-gate.py`).

Windows on ARM is the same ship recipe on a second out dir,
`out/PhiFxWinShipArm64` (`target_cpu = "arm64"`; the arm64 libraries are
grafted into the packaged toolchain — docs/static-build-plan.md §10):
`PHI_OUT=out/PhiFxWinShipArm64 ./build-fx.sh chrome phi_client`, then
`PHI_OUT=out/PhiFxWinShipArm64 ./build-fx-client.sh`, sync with
`PHI_REMOTE_DIR=phi-fx-ship-arm64`, and `installer/make-installer.sh
--arch arm64` packages it as `PhiBrowserSetup-<ver>-arm64.exe`. The dev box
is x64, so that tree is only staged there, never run.
