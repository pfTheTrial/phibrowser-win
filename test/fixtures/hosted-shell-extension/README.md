# Isolated hosted-shell extension verification

Run on Windows only after obtaining the main test runner's exclusive handoff:

```powershell
$env:PHI_USER_DATA_DIR = 'C:\phi-fx-test'
node C:\Users\jdxin\phitest\suite-hosted-extension.mjs --report C:\Users\jdxin\phitest\hosted-extension-report.json
```

Upload the suite, `phi-test.mjs`, `lib/wait.mjs`, and this complete directory to
`phitest\fixtures\hosted-shell-extension`. The suite resolves that absolute
Windows fixture path beside itself. Existing deployment scripts do not copy
fixture directories automatically. Do not use a deployment/relaunch wrapper
while another agent owns the box. This suite is intentionally not in suites.txt.

The suite uses the authenticated named-pipe browser CDP target, loads a fresh
unpacked fixture, and opens its driver as a tab. No permissions, background
worker, network requests, or extension storage are used. It refuses an already
installed fixture (by path or name), never reloads existing extensions, and
uninstalls only the ID returned by this run's successful load. Window removal
is restricted to IDs returned by the fixture's own windows.create calls.
Failed cleanup retains the fixture/driver for recovery and fails the report.

Coverage: actual extension windows.create/get, explicit normal/popup bounds,
partial popup bounds, focused:false, minimized (including omitted focus),
maximized, fullscreen, and invalid state/focus/bounds combinations. Requires
an available work area of at least 940 by 640 DIP. Foreground focus is sampled
through chrome.windows; this does not prove absence of sub-sample native HWND
focus changes. Existing tabs/windows are not closed or resized, but normal
test activation changes foreground focus and the driver temporarily adds a tab.

Limits: no action-popup loading/dismissal test, native HWND instrumentation,
permission consent, pre-install confirmation, or post-install notice coverage.
CDP unpacked installation bypasses those install dialogs. This checkout gates
Extensions.loadUnpacked by browser-target session, with no unsafe-extension
debugging switch check in its handler; report capability failures rather than
relaunching or changing policy. CDP loads into the last-used original profile,
so use the agreed isolated test profile and do not run other suites concurrently.

The shared CDP helper discards events and supports serial requests only; this
suite polls state and issues no concurrent CDP requests. Run with an external
timeout if desired; forcibly interrupting a hung browser can prevent cleanup.
