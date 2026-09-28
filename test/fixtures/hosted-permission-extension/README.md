# Hosted optional-permission confirmation variant

Upload this entire directory beside the suite at
`phitest/fixtures/hosted-permission-extension`, plus `suite-hosted-permission.mjs`,
`phi-test.mjs`, and `lib/wait.mjs`. Fixture directories are not automatically
copied by the normal suite runner. Run only after exclusive box handoff:

```powershell
node C:\Users\jdxin\phitest\suite-hosted-permission.mjs --user-data-dir C:\phi-fx-test
```

Uses root CDP Extensions.loadUnpacked/uninstall and a driver in an isolated
shell. Refuses an already installed fixture by path/name. Requests only the
optional `tabs` permission from hidden Space A with a user gesture, without
awaiting its Promise; checks A is selected and the request remains pending.
Uninstalls only the ID returned by this run, closing the prompt and driver,
then checks control responsiveness and Space switching. A plain anchor tab
keeps the owned shell alive during uninstall. Cleans up its own shell/Space.
No permission is accepted, no network requests, no native UI automation.

Source path: PermissionsRequestFunction uses PERMISSIONS_PROMPT and the default
ExtensionInstallDialogView. That view observes OnExtensionUninstalled and
closes its widget for the matching ID; ExtensionBrowserWindowHelper closes
ordinary extension pages on unload. The suite infers prompt presentation from
pending request plus owner selection; it does not inspect native dialog pixels
or prove native HWND enabled state after dismissal.

This is **optional-permission confirmation coverage only**. Unpacked loading
bypasses installation consent: neither pre-install confirmation nor post-install
notice is verified. No production edits or automatic launch/policy changes.
