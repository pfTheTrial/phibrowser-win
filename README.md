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
autoninja -C out/PhiFxWin phi_client
```
Children (renderer/GPU/utility) run the stock `chrome.exe` sitting beside
`phi_client.exe` — pulled in automatically via data_deps.

Two framework builds exist (docs/static-build-plan.md): `out/PhiFxWin`
is the component build for day-to-day iteration; `out/PhiFxWinShip`
(`is_component_build = false`, one fat chrome.dll) is what evaluation
installers and releases are cut from — `PHI_OUT=out/PhiFxWinShip` for
`build-fx.sh` / `build-fx-client.sh` / `upload-symbols.sh`,
`PHI_REMOTE_DIR=phi-fx-ship` for `sync-fx-win.sh`,
`installer/make-installer.sh --from phi-fx-ship`. The client is built the
same way against either; the framework/client API must stay
allocation-safe across the two builds' separate heaps (contract rule 6 in
phi_framework_api.h, enforced by `tools/release-gate.py`).
