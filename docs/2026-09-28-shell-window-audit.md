# Windows shell, popup, and focus audit

Date: 2026-09-28. Findings below record the original source review. The follow-up
implementation status is tracked separately so original evidence is preserved.

## Follow-up implementation

Changes span both the Chromium fork and Windows client. The paired Chromium
commit is `b060f7e72e`; the client implementation and verification artifacts are
committed with this report. The framework and client must be rebuilt together
because their interface changed.

- 1–2: native shell show/activation, bounds/readback, minimize/maximize/restore,
  and fullscreen routing; background popups request inactive presentation.
  Extension creation applies validated outer bounds and initial window state.
- 3: slide completion and NTP synchronization preserve newer control focus;
  background shells cannot reclaim Win32/Aura focus.
- 4: native file pickers wait for their owning Space to be presented, re-resolve
  the shell after moves, and cancel safely when the owner/listener disappears.
- 5: pending extension popups are cancellable and recheck presentation before
  showing; Space changes close existing/pending popups.
- 6: actionable install dialogs activate their owning Space; stale informational
  notices are suppressed instead of appearing over another Space.
  Runtime testing below still finds an optional-permission owner-presentation
  failure, so this finding is not fully resolved.
- 7: Chromium activation now selects the owning Space and presents its shell,
  including the activation path used by beforeunload dialogs.
- 8: extension-origin device choosers have a toolbar-independent bubble;
  chooser presentation is gated on the owning Space. A lifetime-safe native
  presentation watcher also dismisses a chooser during outgoing animations,
  when Chromium's child remains temporarily visible.
- 9: content fullscreen exits on leaving its Space; browser fullscreen belongs
  to the shell, honors the requested monitor, synchronizes Space controllers,
  and preserves pre-fullscreen placement for readback/restoration.
- 10–12: queued accelerators validate their originating Space/browser/tab;
  Find targets a focused Peek; Close closes a known empty shell.
- 13: the extensions panel remains present underneath its context menu. Native
  ownership, completion tokens, owner destruction, and explicit cancellation
  keep asynchronous menu lifetime separate from the panel's lifetime.

Regression coverage added: native hosted-window presentation properties,
deferred file-picker ownership/cancellation, pending extension-popup
cancellation, and a platform-independent shortcut queue test.

Verification so far:

- Full host-model suite passed with
  `PHI_FORK_SRC=/home/jx/Phi/chromuim/src bash test/model/run.sh` (exit 0),
  including the new shortcut tests and 10,461 sync-vector cases.
- Individual changed Windows production objects and new test objects compiled.
- The first combined build was interrupted by a siso spawn-helper segmentation
  fault, not a compiler diagnostic. The lower-parallelism full retry passed
  (367 steps); the final incremental build and selected regression-test object
  build also passed (24 steps). Logs: `/tmp/phi-shell-build-retry.log` and
  `/tmp/phi-shell-build-final.log`.
- After adding the chooser presentation watcher, the framework rebuilt and
  relinked successfully (6 steps); log: `/tmp/phi-shell-build-verified.log`.
- The client rebuilt and linked successfully against the refreshed framework
  kit (152 compiled files). Output: `out/PhiFxWinShip/Phi.exe`; log:
  `/tmp/phi-shell-client-build.log`.
- Windows deployment/testing was subsequently explicitly approved and performed;
  see the runtime results below. The earlier safety-review block was resolved by
  that approval.
- Compiling the native regression-test objects is not equivalent to running
  them. The per-finding runtime checks below remain the acceptance checklist.
- Include a focused device chooser during keyboard/programmatic animated Space
  switching in runtime acceptance: the new presentation watcher covers the
  interval before outgoing WebContents becomes hidden.
- Changes are committed locally at the user's request; nothing has been pushed.
- Final whitespace checks passed in both repositories.

## Windows deployment and runtime verification

Deployed to `jdxin@192.168.100.170`, directory
`C:\Users\jdxin\phi-fx-ship`, using the existing `PhiFxLaunch` task and
`C:\phi-fx-test` profile. Remote SHA-256 values match the local artifacts:

- `Phi.exe`: `3fed222b7f8c1443f0a1bbd0ff157231f45d2e63042b885b4dd214a08eb3ac77`
- `chrome.dll`: `0275ed2e278d0cc6ad0fc5584aac4697893208e5e1bdfb50530174847e1d7453`

Runtime results:

| Suite | Result |
| --- | --- |
| Core control, navigation, new-window/close, CDP | 7 passed |
| Native shell window API | 4 passed |
| Tabs and page-created tabs | 8 passed |
| Spaces | 8 passed, 2 failed (see below) |
| Chromium Browser-domain shell geometry/state and hidden-Space beforeunload | 3 passed on complete repeat |
| Actual extension windows.create/get, focus, initial states, invalid options, cleanup | 10 passed |
| Native Extensions action popup: positive load, Space-exit cancellation, no late resurrection, cleanup | 4 passed |
| Hidden-Space optional-permission confirmation | Failed twice: originating Space not revealed |

The targeted shell tests compare Chromium geometry with independently queried
native shell bounds, including partial updates and minimized/maximized/fullscreen
restoration. The beforeunload test closes a page in hidden Space A while B is
selected, verifies A is selected, cancels the actual dialog, and verifies the
original page survives. Extension tests load an isolated unpacked fixture,
sample API focus during inactive creation, and remove only their own resources.
They do not prove absence of sub-sample native HWND focus changes.

An initial combined targeted run passed geometry/state but timed out discovering
the third test's new target **before beforeunload was armed**. Logs showed
DevTools UI-task discovery timeouts. After a scoped test-process restart, the
isolated beforeunload case passed; a later complete three-case repeat also
passed. The original transient is retained as evidence, not silently discarded.

### Remaining failure: default-Space deletion can shut down the browser

The Space suite passes ordinary switching, tab isolation, close-after-switch,
rename, and deletion. Its rapid default-role handoff/deletion case causes the
last physical Browser to close while replacement creation is still pending.
The process exits; the following reorder case consequently cannot connect.

The log shows outgoing default closure, a replacement request for cold
`Profile 4`, last remaining Browser closure, then profile-load rejection during
shutdown. Source inspection finds that `RetreatFromSpace` closes the outgoing
Browser immediately, although `RequestBrowser` posts asynchronous creation;
pending client requests do not hold Chromium alive. This path predates the
current patches, but no baseline binary reproduction has been performed, so
regression attribution is not definitive. No production fix was made during
this deployment/verification request. Do not mask the failure with test sleeps.

Suggested fix: retain the last Browser/slot until its replacement successfully
attaches, including failure/timeout handling; a process keep-alive alone does
not preserve the slot when `ForgetBrowser` empties it.

### Remaining failure: optional-permission confirmation leaves its owner hidden

An isolated extension driver in Space A requests the optional `tabs` permission
after switching its shell to B. Runtime diagnostics confirm the expected
extension identity and active user gesture when requesting. Ten seconds later
the permission result remains pending, the driver is hidden, and B—not A—is
still the shell's active Space. This reproduced twice. Uninstalling only the
fixture cancels its pending UI and cleanup succeeds.

This exercises the permission-confirmation variant of `ExtensionInstallDialogView`.
It establishes failed originating-Space presentation, but the test does not
inspect the native dialog HWND and therefore cannot prove where the prompt was
actually parented. Native-parent selection in
`ChromeExtensionFunctionDetails::GetNativeWindowForUI` precedes the new dialog
gate and warrants investigation. Pre-install confirmation and post-install
notices remain unverified; unpacked fixture loading bypasses those flows.

The popup probe's first attempts had fixture-profile setup problems. The final
bounded probe uses the known working driver's original Space and focuses its
actual Browser before dispatching the native Extensions menu action. All four
cases pass: a positive-control popup fully loads and stays alive, switching
Spaces closes it, a second popup blocked on a loopback image is cancelled on
Space exit, and releasing that response does not resurrect it (including after
switching back). The test never attaches DevTools to the popup itself. It
restores the original Space and removes its own fixture, driver, and Space B.

Still unverified: pre-install confirmation, post-install notices, retained
extension-panel context-menu interaction, device-chooser native interaction,
file-picker native interaction, multi-monitor mixed-DPI fullscreen, and real
keyboard/Peek interaction. The corresponding native regression objects compiled
previously, but those test executables have not been run on this box.

Local logs: `/tmp/phi-shell-win-core.log`,
`/tmp/phi-shell-win-regression.log`, `/tmp/phi-shell-win-crash.log`,
`/tmp/phi-shell-win-targeted.log`, `/tmp/phi-shell-win-beforeunload.log`,
`/tmp/phi-shell-win-targeted-repeat.log`, `/tmp/phi-shell-win-extension.log`.
Popup/permission logs: `/tmp/phi-shell-win-popup.log`,
`/tmp/phi-shell-win-popup-retry.log`, `/tmp/phi-shell-win-permission.log`, and
`/tmp/phi-shell-win-popup-permission-final.log`. The successful final popup run is
`/tmp/phi-shell-win-popup-bounded.log`.

## Original rebase verification (before implementation)

- Fetched `phi/phi-r152` at `c203e9e43196bee9b2abada05d32182a403b4293`.
- Rebased Chromium branch `feat/phi-r152-framework-win` from `c454c9f373` to
  `4314a39d76`. Backup: `backup/phi-r152-framework-win-before-rebase-20260928`.
- Windows client: `4eba877`; no implementation changes in this audit.
- Resolved the session-restore conflict by retaining upstream's optional ghost
  window set and the Windows cold-start hook.
- Updated the Windows `ToggleChatSidebar` stub to accept upstream's new
  `window_id` argument. The existing no-op behavior is unchanged.
- Windows x64 object builds passed for `phi_bridge_wrapper_stub.cc` and
  `chrome/browser/sessions/session_restore.cc`, including generated dependencies.
  The underlying build exited 0 and reported `Build Succeeded: 228 steps`.
  `build-fx.sh` incorrectly reported failure because its success-message match
  expects `The build has finished successfully`. Log:
  `/tmp/fx-build-PhiFxWinShip.log`.
- `git diff --check` passed. Range-diff shows only the conflict adaptation and
  bridge signature adjustment beyond the original Windows commit. No full link,
  deployment, or GUI tests were performed. Nothing was pushed.

## Architecture and scope

Windows reparents each Space's Chromium HWND into a native shell as `WS_CHILD`
(`client/phi_browser_host.cc:405`). Its Views/Aura window tree remains present.
Mac instead presents content in a plain native shell with hidden backing windows.
Consequently, Mac's missing-Views-widget and hidden-compositor failures cannot
simply be assumed to reproduce on Windows.

The review covers the cases in Mac's `docs/hosted-shell-window-fixes-and-testing.md`
and the newly fetched background-window, chooser, and Peek-find fixes. Many new
upstream changes are guarded by `IS_MAC_PHI`; rebasing does not enable them on
Windows. Chromium paths below are relative to `chromuim/src`; `client/` paths
are relative to this repository. Line numbers refer to the revisions above.

## Findings

### 1. New shells activate before inactive-show intent is applied

**High confidence; focus defect.** `client/phi_shell_window.cc:1139` uses
`SW_SHOW` for every ordinary shell. The creation callback runs inside browser
creation, before `chrome.windows.create({focused:false})` reaches Chromium's
`ShowInactive()` path (`chrome/browser/extensions/api/tabs/tabs_api.cc:1353`).
That later call addresses the child widget, after the shell has already activated.

The related background-page popup protection added at
`chrome/browser/ui/browser.cc:1933` is Mac-only. A popup from an inactive Space
has neither that general presentation check nor a shell creation contract carrying
inactive-show intent. Upstream's separate actor-task/fullscreen exceptions do not
repair the unconditional shell show.

**Verify:** create a normal extension window with `focused:false`; then trigger
an allowed popup from a background Space. The original foreground window and
keyboard target must remain unchanged. Repeat `focused:true` as a control.

### 2. Chromium window APIs operate on the child rather than the shell

**High confidence; geometry and activation defect.**
`chrome/browser/ui/views/frame/browser_view.cc:1774` changes the browser widget's
bounds; `:2305` reads that widget's bounds. Extension window update/readback uses
these paths. The Win32 coordinate conversion in `HWNDMessageHandler` corrects
child coordinates, but does not retarget the operation to the shell.

`BrowserView::Activate()` at `:1798` likewise activates the child.
`ui/views/win/hwnd_message_handler.cc:903` calls `SetForegroundWindow` on that
HWND. The framework itself documents why this does not activate a child window
(`phi_framework_impl.cc:1236`). No callback selects the owning Space. Window
state operations through the same browser widget also need shell routing.

**Verify:** extension `windows.update` bounds, partial updates, focus,
minimize/maximize/restore, and readback; include an inactive Space and another
foreground shell. The existing `test/suite-window.mjs` tests the different
`agentSpace.window.*` API and does not establish extension API correctness.

### 3. Deferred Space-switch completion can override newer focus

**High confidence; race.** `client/phi_shell_window.cc:3930` calls
`FocusActiveBrowser()` when a slide finishes. `client/phi_browser_host.cc:670`
sets Win32 focus and Aura activation without checking whether this shell is still
foreground or whether a newer user action focused an address field. Native NTP
focus takes a separate path; that does not guard the normal page path.

**Verify:** start a Space switch, then focus another shell or the address field
before the slide lands. Completion must not reclaim keyboard input.

### 4. Late file pickers can block the wrong Space

**High confidence; missing presentation gate.**
`ui/shell_dialogs/select_file_dialog_win.cc:254` walks the owner to `GA_ROOT`,
then immediately starts/posts the native dialog. Correct shell parenting already
fixes the former child-owner DCHECK, but there is no active-Space check, deferred
request, or re-resolution of logical ownership at presentation time.

**Verify:** with download save prompts enabled, start a download whose headers
are delayed in A, switch to B, then release the response. B must remain usable;
the prompt should wait for A. Also exercise owner closure and movement between
shells before a queued dialog presents.

### 5. An extension action popup can finish loading after its Space is left

**High confidence; missing cancellation/gate.**
`chrome/browser/ui/views/extensions/extension_popup.cc:170` ignores dismissal
while the popup is not visible. Its load callback (`:249`) later calls
`ShowBubble()` (`:357`), which shows the widget and focuses extension contents
without checking shell presentation. Shell clicks explicitly dismiss visible
popups (`client/phi_shell_window.cc:4222`), but cannot cancel this pending show
through that visibility-guarded path. Space changes do not change the source
Browser's active tab, so the tab-selection observer is not a sufficient guard.

**Verify:** open a slow-loading extension popup in A and switch to B before load
completes, using both a Space chip and a keyboard shortcut. No popup from A
should appear or take focus in B.

Leaving an extension popup open while switching to another application is an
explicit upstream behavior (`set_close_on_deactivate(false)`), not by itself a
bug. Actual native visibility/stacking after a Space switch still needs GUI testing.

### 6. Extension install UI lacks Space presentation checks

**High confidence for the missing checks; visible symptoms need runtime testing.**
The post-install notice has a Windows-specific nonmodal fallback:
`chrome/browser/ui/extensions/extension_dialog_utils_views.cc:114` calls
`phi_framework/phi_extension_actions.cc:224`. It creates an anchored bubble with
a close button by default, avoiding the original Mac stuck-modal notice failure.
However, its `widget->Show()` is unconditional. The coordinator's extension-button
rectangle lookup (`client/phi_coordinator.cc:4375`) finds the shell for any resident
Browser without checking that Browser is presented.

The pre-install **Add extension?** confirmation is separate:
`chrome/browser/ui/views/extensions/extension_install_dialog_view.cc:81` only
checks active tab within its Browser; it does not select that Browser's Space
before showing a browser-modal dialog.

**Verify:** switch A to B while installation is pending. Exercise both the
permission confirmation and the completed-install notice. Neither should
interrupt an unrelated Space; confirmation and dismissal must recover input.

### 7. Leave-site dialogs do not reveal the Space requiring confirmation

**High confidence for routing; exact dialog placement needs runtime testing.**
`client/phi_shell_window.cc:3954` closes all resident Browsers when a shell closes.
A hidden Space may therefore require before-unload confirmation.
`chrome/browser/ui/views/chrome_javascript_app_modal_view_factory_views.cc:101`
activates the requesting contents, but `Browser::ActivateContents()`
(`chrome/browser/ui/browser.cc:1956`) only selects the tab and calls child-widget
activation. It does not ask the shell to present the owning Space.

Do not describe this as universally missing modal blocking: Win32's
`HWNDMessageHandler::InitModalType()` walks the owner/parent hierarchy and disables
the shell. The missing behavior is presenting the correct page/Space and ensuring
the dialog is visible in that context.

**Verify:** make an edited page in A require before-unload confirmation, switch
to B, and close the shell. A's confirmation must be visible with A's page. Test
Stay, Leave, and multiple resident Spaces requiring confirmation.

### 8. Device choosers still assume a conventional browser toolbar/active tab

**High confidence for the extension-origin early return; presentation needs GUI testing.**
`chrome/browser/ui/views/permissions/chooser_bubble_ui.cc:249` requires an
extensions toolbar and returns without showing a chooser if it is absent.
Windows disables the normal toolbar/tab-strip presentation in
`window_feature_controller/window_feature_controller.cc:81`; `ToolbarView` uses
location mode (`toolbar_view.cc:194`) and returns before creating the extensions
container (`:355`). The custom extension action popup fallback does not cover
this chooser path. This affects eligible extension pages requesting device access
through this shared chooser implementation.

The ordinary web-page chooser checks active tab within the Browser (`:325`),
then shows its widget (`:347`) without checking active Space. A native chooser
from a still-active tab in a hidden Browser is therefore not explicitly gated.
The newly fetched Mac anchor override does not compile on Windows; however,
Windows' existing fallback anchors to the embedded page's origin, so the Mac
sidebar-overlap symptom is not established here.

**Verify:** USB/HID/serial chooser flows supported by the test environment, from
a normal page and an eligible extension page. Repeat while switching Spaces.

### 9. Content fullscreen is not scoped to the presented Space or target monitor

**High confidence; state/routing gap.**
`client/phi_browser_host.cc:336` accepts fullscreen events from any Browser owned
by the shell. `ShowSpace()` (`:691`) neither exits outgoing content fullscreen
nor clears the shell's content-fullscreen state. The next Space can inherit
fullscreen layout from a different Browser; a late event from a hidden Browser
can also affect the shell. This is not evidence of Mac's exact lifted-view overlay
bug: Windows hides/reparents entire child windows.

The fullscreen event carries only `(window_id, bool)`
(`phi_framework_impl.cc:4778`), and the shell always chooses its current monitor.
The requested display ID is not carried through this route.

**Verify:** fullscreen video in A, then switch to B; exit or close A afterward.
Also use Window Management permission and `requestFullscreen({screen: target})`
with two monitors. Track DOM fullscreen and shell layout separately.

### 10. Posted shortcuts can execute in a different Space

**High confidence; missing command attribution.**
`client/phi_keyboard_accelerators.cc:176` posts a `WM_COMMAND` containing only
the command ID. `client/phi_app_menu.cc:649` resolves the currently presented
Browser when the command is handled. There is no originating session ID or
generation. A Space change between posting and execution can redirect a command;
the Mac replay fix explicitly binds commands to their originating session.

**Verify:** deterministically queue a close/new-tab command, change presentation
before dispatch, and release the message. An unrelated destination Space must
not be modified. Include delayed cold-Space attachment.

### 11. Ctrl+F in a Windows Peek targets the underlying Space

**High confidence; routing defect.** Peek is a child panel of the shell containing
its own Browser (`client/phi_peek_panel.cc:213`). The global keyboard hook resolves
the shell through `ShellForFocus()` (`client/phi_coordinator.cc:4340`). Find then
uses `MenuActiveBrowser()` and the generic command route
(`client/phi_app_menu.cc:649`, `:736`), which targets the Space Browser rather
than the focused Peek Browser.

The newly fetched Mac find fix is `IS_MAC_PHI`-only and addresses a different
tab-hosting arrangement; enabling its guard is not a Windows fix.

**Verify:** open a Peek with text absent from the underlying page, focus it, and
press Ctrl+F. Search, match navigation, and the find bar must belong to the Peek.

### 12. Ctrl+W on an empty placeholder does not close the shell

**High confidence behavior; Mac parity gap.**
`client/phi_app_menu.cc:786` closes only a valid active tab and otherwise returns
handled. There is no empty-placeholder fallback to close the shell as in the Mac
fix. Validate the intended Windows product behavior before changing this policy.

**Verify:** close the last real tab, then press Ctrl+W again on the placeholder.

### 13. Extension context menus dismiss the Extensions panel

**Confirmed deliberate behavior; Mac parity gap, not a proven stacking failure.**
`client/phi_extensions_popover.cc:282` calls `Dismiss()` before opening an
extension's context menu. Mac's acceptance case explicitly keeps the Extensions
popover open with the menu above it. Windows avoids this overlap by closing its
panel. Matching the Mac interaction requires retaining the panel and verifying
menu ownership, stacking, and focus rather than claiming the stacking fix exists.

## Additional verification needed, not established defects

- Shell deactivation currently hides the floating sidebar but does not explicitly
  mirror deactivation to Aura (`client/phi_shell_window.cc:6880`), while activation
  is explicitly forwarded. Test Browser active-state bookkeeping, extension key
  bindings, and popup dismissal across multiple shells and Alt-Tab; other native
  focus notifications may compensate, so absence of a symmetric API alone is not
  proof of broken behavior.
- Test extension accelerators with the native address/NTP field focused. The shell
  keyboard hook handles its own command set; it does not implement Mac's general
  first-pass extension accelerator bridge. Also exercise Keyboard Lock.
- FedCM, embedded permissions, Windows Hello/passkeys, client-certificate details,
  Live Caption return-to-tab, permission bubbles, and ordinary find-bar focus need
  native runtime coverage. Retained Views widgets rule out assuming Mac's exact
  null-widget crash; they do not prove every dialog is correct.

## Existing protections and limitations

Windows already corrects native file-picker ownership, forwards basic focus into
the browser child, anchors custom extension action/installation bubbles, and
closes visible action popups on shell clicks. Its hidden normal toolbar also avoids
assuming Mac's exact permission-chip/extension-toolbar animation wait.

The native find bar remains inside a live Views hierarchy. No direct evidence was
found for the Mac hidden-compositor stall on an ordinary presented Windows page.
The targeted compilation above validates rebase compatibility only, not any of
the UI scenarios. Ad-hoc scripts under `test/probes/` are explicitly not regression
tests (`test/probes/README.md`); the cases above need assertion-based Windows GUI
coverage and a freshly rebuilt framework/client.

Outside this popup/focus audit, two upstream feature differences remain visible:
the Windows chat-toggle bridge is still a no-op, and its cold-start restore bridge
supplies only eager IDs, leaving the new ghost set unset. Thus the new Mac chat
targeting and unmapped-window restore behavior are not automatically ported by
this rebase. These are not additional reproduced popup failures.
