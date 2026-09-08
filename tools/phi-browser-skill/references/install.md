# phi-browser skill — setup (Windows)

## 1. Install the skill

Link the skill into your agent's skills folder (Claude Code shown; swap the
destination for another agent — Codex `~/.codex/skills`, Cursor
`~/.cursor/skills`, etc.). PowerShell, from the repository root:

```powershell
New-Item -ItemType Directory -Force "$HOME\.claude\skills" | Out-Null
New-Item -ItemType Junction -Force `
  -Path  "$HOME\.claude\skills\phi-browser" `
  -Target "$PWD\tools\phi-browser-skill"
```

(A junction is used instead of a symlink so it works without Developer Mode /
admin. A plain copy works too if you don't need it to track updates.)

Requires Node >= 22. No npm dependencies.

## 2. How it connects

Phi's client (`phi_client.exe`) owns a **named pipe** and publishes it in a
pointer file, `CDPAgentSocket`, inside the browser's user-data directory:
line 1 is the pipe name, line 2 a **per-launch token**. The skill discovers
the pipe from that file, speaks HTTP/1.1 over it, and multiplexes two
channels the same way macOS does over its Unix socket:

- `/phi-agent` → a WebSocket control channel for the agent-Space lifecycle and
  browser management (`agentSpace.*`, `credentials.*`).
- `/json/version`, `/json/list` → the DevTools discovery documents, and
  `/devtools/<kind>/<id>` → a CDP WebSocket session, both served by the
  client through the browser's in-process DevTools transport — so page
  automation (CDP) rides the same pipe.

That pipe is the only way to reach the protocol: Phi does not open a
loopback DevTools port. Three gates sit in front of it — the pipe's ACL
(owner + SYSTEM + administrators, medium integrity, no remote clients), the
token, which every request must carry as `X-Phi-Token` (a missing or wrong
token is answered `403` before anything else happens), and the user's switch
**Settings ▸ Developer ▸ "Allow agents to control Phi"**. The skill reads the
token from the pointer file and sends it on every request automatically; the
pointer file itself is readable only by the user Phi runs as.

There is no per-program approval on Windows. Phi cannot verify what a pipe
client is (no code-signature identity, and the skill always arrives as
`node.exe`), so it does not pretend to: with the switch on, every local
program holding the token is admitted without a prompt; with it off, the
first connection raises a prompt that offers to turn the switch on (Allow
persists it; Deny refuses connections until the user touches the switch
again). A refused connection reads `403 … agent control is off`.

**Discovery.** The skill searches these user-data directories, first hit wins:

1. `%PHI_USER_DATA_DIR%` — set this to be explicit. The framework/client dev
   build is launched with `--user-data-dir` (e.g. `C:\phi-fx-test`), so point
   the variable at the same path.
2. `%LOCALAPPDATA%\Phinomenon\PhiBrowser\User Data`
3. `%LOCALAPPDATA%\Phi\User Data`
4. `C:\phi-fx-test`

**Launch.** When nothing is running, the skill starts `phi_client.exe` from
`%PHI_CLIENT_EXE%` (or a couple of default install paths) and waits up to 60 s
for the pipe. On a dev/test box the client is usually already running
(launched out of band), so a missing exe is only fatal when discovery also
finds nothing. `PHI_NO_LAUNCH=1` forbids the auto-launch.

## 3. Verify

The skill runs ON the Windows machine (the pipe is local), so run the runner
there. Quickest smoke round (PowerShell):

```powershell
$env:PHI_USER_DATA_DIR = 'C:\phi-fx-test'
node "$HOME\.claude\skills\phi-browser\scripts\runner.mjs" @'
const task = await enterContext({ kind: 'agent', name: 'smoke test' })
cliLog(task)
await openTab('https://example.com')
cliLog(await pageInfo())
EOF
'@
```

(Replace the here-string with your shell's heredoc form as needed.) A hidden
agent Space is created off-screen, `example.com` loads in it, and `pageInfo`
prints the page's URL/title/size.

## 4. Credentials — the agent password vault

`credentials.*` is backed by a local **DPAPI-encrypted** vault,
`PhiCredentials.dat`, in the user-data directory (the Windows stand-in for the
macOS Bitwarden provider — there is no cloud provider on Windows yet).

- `credentialStatus()` reports `ready` when the vault exists, else
  `logged_out`.
- `getCredential(query)` returns an item after the user approves a native
  **allow/deny** prompt. `fillCredential(target, query)` fills a page field
  **without** the secret ever entering the agent: Phi writes the value into
  the tagged field in-process and returns only `{filled:true}`. Both are
  origin-bound — a fill into a page that doesn't belong to the credential's
  site is refused (`origin_mismatch`) unless you pass `{allowCrossOrigin:true}`.
- TOTP is never released (`totp_not_supported`) — 2FA stays with the user.

Seed the vault (until a settings UI exists) by writing a JSON array and letting
the client encrypt it on next load. A **plaintext** `PhiCredentials.dat` is
also accepted as-is for dev/test:

```json
[
  { "type": "login", "name": "Example",
    "domain": "example.com", "uri": "https://example.com",
    "username": "me@example.com", "password": "hunter2" }
]
```

## Troubleshooting

- **CDP endpoint not found**: no pipe pointer was found and `phi_client.exe`
  could not be started. Set `PHI_USER_DATA_DIR` to the running client's
  user-data dir, or `PHI_CLIENT_EXE` to the exe, or start Phi by hand.
- **Launched but never published its pipe**: the client started but 60 s
  passed with no `CDPAgentSocket` — it may be sitting at a prompt, or still
  starting. Check the window and retry.
- **Screenshots are blank**: the agent Space renders off-screen; if a capture
  ever comes back blank, the window was occluded — bring it forward is not yet
  a supported gesture, so retry the capture.
- **`credentials.*` returns `logged_out`**: no `PhiCredentials.dat` in the
  user-data dir. Seed it (section 4).
- **`whole-browser` capture (`screenshotBrowser`) fails**: not wired on
  Windows yet — use `screenshot()` / `annotatedScreenshot()` (CDP), which work.
