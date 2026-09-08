# `test/lib/` — the shared test library

What every suite used to carry a copy of. Lane 6 §2 counted the copies:
`firstWindow` fifteen times, the PhiUiCap pixel graft five, the restart
four, the "close down to one tab" loop about twenty-one, `waitFor`/
`waitTabs`/`navigateAndSettle` six, the PhiDragCap rig twice, and a second
assertion framework three times.

| module | exports | replaces |
|---|---|---|
| `box.mjs` | `BOX_HOME`, `USER_DATA_DIR`, `boxPath`, `profilePath`, `TEST_DIR` | `C:\Users\jdxin\…` spelled out in seventeen files |
| `wait.mjs` | `sleep`, `waitFor`, `waitTabs`, `waitWindows`, `waitSpaces`, `navigateAndSettle`, `waitForPing` | six hand-rolled poll loops, and the fixed sleeps that had no loop at all |
| `window.mjs` | `firstWindow`, `activeWindow`, `windowLayout`, `windowHandle`, `closeToOneTab`, `normalizeWindow`, `frameInsets` | fifteen `firstWindow`s, ~21 close-down-to-one loops, the inline layout pinning in the pixel suites |
| `app.mjs` | `restartApp`, `killApp`, `powershell` | four copies of the kill + `PhiFxLaunch` + poll-for-ping PowerShell |
| `pixels.mjs` | `capturePixels`, `capturePixel`, `rgb`, `isLight`, `isColoured` | five copies of the PhiUiCap environment-variable graft |
| `dragrig.mjs` | `runSteps`, `park`, `drag`, `clickBox`, `mid` | two copies of the PhiDragCap `drag.txt` → `drag.done` rig |
| `check.mjs` | `check`, `show`, `finish`, `failureCount` | three copies of the rigs' own PASS/FAIL counter |

Rules:

* A module here imports only `node:*` and `../phi-test.mjs`. Nothing here
  imports a suite or a probe.
* `test/probes/` may import from here and from `../phi-test.mjs`, nothing
  else. A probe that grows assertions graduates to a suite.
* Anything that needs a box path asks `box.mjs`, so a second box needs one
  environment variable rather than seventeen edits.

**Deployment moves with the imports.** `../test-box.sh` creates
`phitest/lib/` on the box and uploads every file in this directory before it
uploads a suite; `run-fx-test.sh` does the same for a single suite. Adding a
module here therefore needs no runner edit — but importing a file that is
*not* in `test/lib/` (or `test/phi-test.mjs`) from a suite does, and every
suite then dies on the box with ERR_MODULE_NOT_FOUND.
