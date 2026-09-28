# Picture in Picture

Windows uses the same Chromium PiP controllers, overlay controls, persisted
geometry, and edge parking as the Mac client. In Settings → Advanced,
Auto picture-in-picture offers Off, Normal (default), and Park at edge.
Off disables automatic entry only; the video's context-menu PiP command and
website-initiated video/document PiP remain available.

The native preference remains `autoPictureInPictureMode` in
PhiPreferences.json and syncs using the existing settings descriptor. The
framework reads it on the UI thread through PhiFrameworkDelegate and the
lightweight framework hooks whenever it evaluates automatic entry or parking.
There is no second framework-side preference cache. Changes apply to subsequent
automatic entries; they do not close an existing manual PiP window.

Like Mac, the Phi activation helper combines tab selection with WebContents
visibility, so hiding a Space's hosted browser or minimizing/fully covering it
can trigger auto-PiP. Revealing the source tab exits automatic PiP. Chromium's
media eligibility, origin, site permission, and safety checks still apply;
not every video qualifies. Explicit parking/unparking and geometry persistence
remain owned by the shared Chromium PiP implementation.

Document PiP browsers bypass the Windows client window-created, window-changed,
window-closing, presentation, and fullscreen dispatchers. Chromium owns their
floating windows; adopting one as a user browser would reparent it into a tab
shell and interfere with its independent lifetime.

Validation:

- `test/model/run.sh` includes the native preference-hook regression.
- Chromium's PiP browser tests cover hidden/occluded/restored activation through
  the factory; its parking unit tests now also compile for the Windows client.
- `./test-box.sh --suite suite-pip --isolation reuse` builds no code: run it
  after rebuilding both framework and client. It deploys the build to the test
  box and checks document PiP open/close/reopen, video PiP enter/exit, and
  automatic pop-out/return on tab switching using generated local content.
  The automatic case expects the Normal or Park at edge setting.

A framework rebuild and a matching client rebuild are required for this change
because PhiFrameworkDelegate gains a preference query.
