# Isolated hosted popup verification

Upload `suite-hosted-popup.mjs`, `phi-test.mjs`, `lib/wait.mjs`, and this whole
directory to `phitest\fixtures\hosted-popup-extension`. After exclusive test-box
handoff, run `node suite-hosted-popup.mjs --report hosted-popup-report.json` from
phitest, with `PHI_USER_DATA_DIR` set to the agreed test profile. No relaunch or
unsafe-extension flag is performed. Not registered in suites.txt.

The suite loads only this new extension and creates its driver through root CDP
in the default context. Browser.getWindowForTarget and spaces.list map that
driver to its actual shell and existing Space A. Only exit Space B is created;
its model profile label is not assumed to identify the driver's real context.
Popup actions run in the verified driver's existing Browser in A. It
locates the control window afresh by stable shell HWND on every operation.
An English Extensions menu is required. The extension must load into the same
profile as the selected shell; setup fails if the menu does not contain it.
Discovery of the driver URL alone is not readiness: setup waits for the complete
extension document, verifies chrome.runtime.id equals this run's loaded ID, and
requires chrome.action.setPopup. It logs the binding and rechecks identity before
each action configuration; failures include the actual URL and runtime ID.

The driver sets its own action popup URL. The suite dispatches the actual native
fallback through `agentSpace.menu.click`, path `['Extensions', fixtureName]`.
A random-port server bound only to 127.0.0.1 supplies a held image, with a random
run token. A second image beacon proves the popup's window.load event fired.
The positive control requires load completion and persistent target existence;
the delayed case observes the target and held request before switching A to B,
releasing the response, checking target destruction, and monitoring for late
reappearance in B and after returning to A. If cancellation already closed the
held request, release reports that explicitly.

Only the driver is attached through CDP. Popup checks use /json/list discovery:
no popup inspection, Runtime.evaluate, or Target.attachToTarget. No extension
permissions, external network, or storage. Existing extensions are never used;
an already-installed fixture is refused. Cleanup restores the original Space,
deletes only returned Space IDs (and their own tabs), closes only its returned
driver target ID (never an existing shell), and uninstalls only the
extension ID returned by this run. Failed restoration retains resources and
reports their IDs. There are no uploads or remote operations in the suite.

Limits: target lifetime/load assertions do not measure native popup pixels,
HWND foreground/focus, or keyboard/chip input. Space switching uses the control
route. This does not cover install/permission dialogs or explicit loading-popup
dismissal. Driver attachment alone does not inspect the popup. CDP load targets
the last-used original profile. Use an otherwise idle box; do not concurrently
operate the shell. An external hard timeout can interrupt cleanup.
