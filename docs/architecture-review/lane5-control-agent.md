# Lane 5 — Control server, verb dispatch, agent trust model

Read-only review of `phibrowser-win/client` (control server, coordinator verb dispatch, agent spaces, drive registry, credentials) plus the consumer (`tools/phi-browser-skill/scripts/lib/cdp.mjs`, `helpers.mjs`, `references/*.md`) and the framework API header. No files were modified. All paths below are under `/home/jx/Phi/phibrowser-win/` unless stated; `PC` = `client/phi_coordinator.cc`, `PCS` = `client/phi_control_server.cc`.

**Correction to the brief.** The code is further along than "no auth (dev/test)". There IS a pipe SDDL (PCS:330-341), a per-connection peer-consent gate keyed by image path (PCS:394-451, PC:600-669), and a typed credential-grant store (PC:7086-7168). The header comment at `client/phi_control_server.h:57-59` ("No peer auth (dev/test); the pipe ACL is the default") is stale. The findings below are about what those mechanisms do *not* cover, and the biggest one is that the consent gate has a second, unguarded door.

---

## 0. Executive summary (what must land before public release)

| # | Finding | Severity |
|---|---|---|
| F1 | **Consent gate bypass:** Chromium's `--remote-debugging-port=0` loopback TCP listener is reachable by any local process, port published in `DevToolsActivePort`; full CDP over every profile with zero consent | Critical |
| F2 | **Standing credential grant + `allowCrossOrigin` = silent password exfil** into any page the agent chooses | Critical |
| F3 | **No user-space permission gate on Windows.** Every management verb (delete Space, delete profile, rewrite URL rules, navigate/close the user's tabs, click menus, change settings incl. download dir) is served to any consented peer; the toggle the skill expects (`user_space_operations_disabled`) does not exist | High |
| F4 | **Consent identity = interpreter path.** "Always Allow" persists `...\node.exe`; every future Node script (and the `name:node.exe` toolhelp fallback matches any exe so named) inherits it. Prompt defaults to Allow; no prompt-rate limit; unbounded connection threads | High |
| F5 | **"Take control" is cooperative for agent Spaces** (`agentSpace.takeover` flips it back, PC:6241-6254) and **client-side verbs never arm the operating mask** (`agentSpace.navigate` on a user window bypasses `PhiUserSpaceDrive`) | High |
| F6 | **No task/connection binding.** `taskId` is a free string; any connection can `complete`/`handoff`/`log`/`userMessage` any task; agent can inject "You: …" lines into the transcript (PC:6359-6385) | High |
| F7 | `credentials.getTotp` serves 2FA codes to the agent (PC:7285-7346) while the skill contract promises `totp_not_supported`; "secret-free fill" is a convention the agent's own process implements (`helpers.mjs` scrubbing), not something Phi enforces — a CDP holder reads the DOM value back | High |
| F8 | **Pipe squatting:** `CreateNamedPipe` without `FILE_FLAG_FIRST_PIPE_INSTANCE`, predictable name `phi-cdp.<pid>`, pointer file written *before* the pipe exists; listener dies permanently on first failure | Medium |
| F9 | Developer-mode setting claims to gate agent access but gates nothing (PC:3572-3578); the agent can flip it via `settings.set` (PC:5457-5460) | Medium |
| F10 | Relay UAF window on slow peer (`SockToPipe ctx` on stack, 2 s join, PCS:680-701); WS handshake/framing laxity; UI-thread blocking on vault helper per `credentials.status` | Medium/Low |

Design target (section 4): in-process CDP transport (kill the TCP port), launch token + peer identity, per-connection capability set with agent-space-only default, single typed verb registry, uniform envelope, credential grants keyed by (kind, item, page-host, peer) with cross-origin never covered by a standing grant.

---

## 1. Transport

### 1.1 Who can connect

- Pipe DACL: `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)` + `PIPE_REJECT_REMOTE_CLIENTS` (PCS:330-341). Owner Rights / SYSTEM / Administrators only; other local users are excluded. Good.
- No explicit integrity label, so the default medium-IL mandatory policy applies: a low-IL or AppContainer process (Chromium renderers/GPU/utility) cannot open the pipe for write. Adequate, but implicit — add an explicit `S:(ML;;NW;;;ME)` so the property survives a future SDDL edit.
- Any medium-IL process running as the user can connect. The only thing between such a process and full browser control is the consent prompt (§1.3). That is acceptable *only if* the consent identity is strong and the prompt is un-spammable; today neither holds (F4).

### 1.2 The second door: the loopback DevTools port (F1 — Critical)

`client/phi_client_main_win.cc:380-389` appends `--remote-debugging-port=0` to the browser process. Chromium then binds `127.0.0.1:<ephemeral>` and writes the port to `<user-data-dir>\DevToolsActivePort`; the control server reads that file (PCS:598-623) and byte-relays to it (PCS:625-702).

Consequences:

- Any local process can read `DevToolsActivePort` (default file ACL) or just port-scan loopback, connect to Chromium directly, and get the *full* DevTools protocol over *every* profile and window — `Target.getTargets`, `Target.attachToTarget` to user tabs, `Storage.getCookies`, `Network.*`, `Runtime.evaluate`, `Page.navigate`, `Browser.setDownloadBehavior`, `Page.captureScreenshot`. The consent gate in `HandleConnection` (PCS:441-451) is never consulted. Chromium's own DevTools server only checks `Host`/`Origin` headers (relevant to web pages, not native processes).
- `cdp.mjs:14-15` asserts "That pipe is Phi's ONLY CDP surface on Windows … the pipe is the door." Nothing in the client or the framework API enforces that.
- The fork's `SetUserReclaimedTabs` refusal (`phi_framework_api.h:663-670`) applies to *drive* commands on reclaimed tabs only; it does not stop reads, and it does nothing for un-reclaimed tabs.

**Fix (must land before release):** stop using a TCP port. Options in ascending cost:
1. `--remote-debugging-pipe` with the client holding the pipe pair. Problem: the client *is* the browser process (the framework binds the delegate in-process, `phi_client_main_win.cc:391-394`), so the fd-3/4 model does not apply directly.
2. Framework-provided in-process transport: a `PhiFramework::OpenDevToolsSession(filter)` that wraps `content::DevToolsAgentHost::CreateForBrowser` + a `DevToolsAgentHostClient`, and the control server pumps frames to it instead of a socket. This also enables per-connection target filtering (§4.5). Recommended.
3. Interim mitigation if (2) slips: keep TCP but have the fork's socket factory accept only connections whose peer PID (`GetExtendedTcpTable` lookup) equals the client's own PID. Weak (races, still a bind on loopback), but closes the door for non-fork attackers.

### 1.3 Peer identity and consent (F4 — High)

- Identity = `GetNamedPipeClientProcessId` → `QueryFullProcessImageNameW` (PCS:399-411). Fallback for an elevated peer: Toolhelp snapshot, `"name:" + exe name` (PCS:412-436).
- Consent key = lowercased image path (PC:620-623). "Always Allow" persists it to `PhiAgentGrants.json` (PC:661-664, 747-767); "Allow Once" caches per session (PC:658-660); "Deny" caches per session (PC:667-668).

Problems:
1. **Path ≠ principal.** The skill runs under Node (`cdp.mjs` is an `.mjs` module; `helpers.mjs:111` calls `connectBrowser`). The grant lands on `C:\Program Files\nodejs\node.exe` (or wherever the agent's runtime lives). Every subsequent Node script — including one dropped by a malicious npm package — is pre-approved. The `name:` fallback is worse: `name:node.exe` matches any binary so named from any directory.
2. **The claim headers are ignored.** `cdp.mjs:228-247` sends `X-Phi-Agent-Pid` and `X-Phi-Agent-Capability`; `helpers.mjs:1136-1174` expects the app to echo a resolved pid/capability on the 101 response. Grep confirms the Windows server never parses or echoes them (no `X-Phi` in `PCS`/`PC`). So the Mac's "task principal" concept has no Windows counterpart; every connection is the same anonymous "consented peer" (the code says so at PC:6735-6738).
3. **Default button is Allow.** `PhiPromptChoose(..., {Allow Once, Always Allow, Deny}, /*default*/0, ...)` (PC:645-647). Enter approves. Same for the credential prompt (PC:7127-7130, default 0 = "Allow once").
4. **Prompt spam / DoS.** `ListenLoop` spawns one thread per connection with `PIPE_UNLIMITED_INSTANCES` and no cap (PCS:337-391). Every un-decided connection from a not-yet-denied path SendMessages a `WM_PHI_AUTHORIZE` (PC:600-610); the UI thread runs the modal loop (`phi_prompt.cc:169-176`), which pumps messages and therefore *dispatches the next cross-thread SendMessage inside the nested loop* → stacked consent dialogs. A local process can open 50 connections and get 50 dialogs.
5. **No revocation surface for session grants**, and the persisted grant file is a default-ACL JSON the same user (i.e., any same-user process) can edit (PC:747-767).
6. **Consent text overpromises scope.** The dialog says "wants to control Phi — tabs, navigation, and page content" (PC:636-638), but the grant also covers deleting profiles, rewriting URL rules, writing PNGs to arbitrary paths (`captureWindow` `outPath`, PC:6479-6529), changing the download directory (PC:5480-5484), clicking any menu item (PC:5697-5722), and toggling developer mode (PC:5457-5460).

### 1.4 Pointer file and pipe lifecycle (F8 — Medium)

- Pipe name is `\\.\pipe\phi-cdp.<pid>` (PCS:302-303) — enumerable. The pointer file is written (PCS:305-315) *before* the listener thread creates the first pipe instance (PCS:317-341). `CreateNamedPipeW` lacks `FILE_FLAG_FIRST_PIPE_INSTANCE`, so a same-user process that pre-creates that name wins; Phi's own create then either coexists (attacker's instances get some connections → they can MITM the skill, feeding it fake page content, or relay to the real pipe so the consent dialog shows the attacker's path but the *skill* thinks it is talking to Phi) or fails outright, in which case `ListenLoop` logs and `return`s (PCS:345-349) — the listener is dead for the life of the process with no retry.
- Pointer file has the default ACL and is never removed on exit; `cdp.mjs:180-189` treats a refused connect as "dead leftover", which is the right client-side behaviour, but the name is trivially guessable so the leftover is not the threat — squatting is.
- **Fix:** random pipe suffix (`phi-cdp.<pid>.<128-bit>`), `FILE_FLAG_FIRST_PIPE_INSTANCE` on the first instance, write the pointer file *after* the first instance exists, restricted ACL on the pointer file, retry-with-backoff in the listener. Put the launch token (§4.1) in the same file.

### 1.5 CDP proxy correctness

- `ProxyToDevTools` runs the back-relay thread on a stack `SockToPipe ctx` (PCS:680-681) and joins with a 2 s timeout (PCS:698-701). If the peer stops reading, `OvWrite` in the back-thread blocks in `GetOverlappedResult(..., TRUE)` (PCS:78) past 2 s, `ProxyToDevTools` returns, `HandleConnection` closes the pipe (PCS:470-472), and the back-thread then dereferences `ctx->pipe` from a dead frame (PCS:272) — UAF/reliability bug. Either heap-allocate `ctx` and let the last thread free it, or `CancelIoEx(pipe)` before the join and wait `INFINITE`.
- `send()` return values are not checked for partial writes (PCS:678, 690); acceptable on blocking sockets but should loop.
- `ReadHttpHead` appends the whole chunk into `head` (PCS:142); any bytes after `\r\n\r\n` (a pipelined first frame) are re-emitted as "header lines" by the rebuild loop (PCS:651-676), corrupting them. The skill never pipelines (`UnixWebSocket` waits for 101), so this is latent.
- `ResolveDevToolsPort` caches in a function-static (PCS:599) — fine for a single browser lifetime.

### 1.6 WebSocket handshake and framing (control channel)

- Handshake: case-sensitive `find("Sec-WebSocket-Key:")` (PCS:480-481); on absence the function returns without writing *any* HTTP response (PCS:482-484) — the peer hangs until its own timeout. No check of `Upgrade`, `Connection`, `Sec-WebSocket-Version`. Should answer `400`.
- Frames: control frames (ping/close) arriving *inside* a fragmented message are appended as data (PCS:246-253); server never answers ping with pong (PCS:522) and never sends a close frame. Unmasked client frames are accepted (spec requires masking) — harmless here. Payload cap 16 MiB per message (PCS:238), head cap 1 MiB (PCS:143) — good.
- Message envelope: `{id, type, payloadJson}` → `{id, responseJson | error}` (PCS:525-546). The response body is a JSON *string* inside JSON (double-encoded) — every reply is serialised twice and parsed twice (PC:4681-4696 build → `JSONWriter`; PCS:536 embeds; `cdp.mjs:673-678` re-parses). Missing `id` → reply has no `id` → the client cannot correlate. `error` and `responseJson` are alternatives, but *application* errors ride inside `responseJson` as `{ok:false,error}` — two error channels (§2.3).

### 1.7 Backpressure and threading

- One request in flight per channel: the serve loop is synchronous (`WsReadMessage` → `HandleControlCommand` → `WsSendText`). A slow verb (credential prompt up to forever, `WaitPendingReply` 40 s at PC:426, `profiles.create` 15 s at PC:394-396) stalls that channel but not others. Fine.
- Pushed events: per-channel `write_lock` + 1 s `OvWriteTimeout` (PCS:576-596). Correct — a wedged peer loses events, does not stall the UI thread.
- `HandleControlCommand` → `SendMessageW(WM_PHI_CONTROL)` (PC:379-389) blocks the I/O thread until the UI thread finishes `ExecuteControlOnUi`. Modal prompts inside (credential, PC:7127; consent, PC:645) run a nested `GetMessageW` loop (`phi_prompt.cc:169-176`), and the `WM_NULL` kick after `Finish` (`phi_prompt.cc:121-129`) correctly handles the "SendMessage from outside the thread" wake-up. No deadlock: the I/O thread never holds a lock the UI thread wants. But **re-entrancy**: while a prompt is up, the nested loop dispatches *other* connections' `WM_PHI_CONTROL`/`WM_PHI_AUTHORIZE` SendMessages, so verb handlers execute in the middle of another verb handler's stack frame (e.g., `agentSpace.spaces.delete` closes windows while `credentials.autofill` is between its origin check at PC:7496-7501 and `FillTaggedField` at PC:7513). Today the harm is bounded (the fill fails cleanly if the tab's tag disappears), but a registry (§4.3) should mark prompting verbs and serialise them.
- `profile_create_pending_` is a `volatile bool` written from both the UI thread (PC:4961) and the I/O thread (PC:396) with no lock; with two connections racing `profiles.create` the second's `busy` check can miss. Minor.
- `vault_helper_.Call` blocks the UI thread up to 30 s (`client/phi_vault_helper.h:41-44, 68-71`) and `credentials.status` calls it on every request (PC:6945-6957, 7260). An agent can freeze the UI with a `credentials.status` loop. Move helper I/O off the UI thread; answer `status` from a cached value.

---

## 2. Dispatch architecture

### 2.1 Routing

`ExecuteControlOnUi` (PC:4664-6011) is a 1,350-line if/else ladder with four pre-filters:

1. `HandleAgentSpaceVerb` (PC:6013-6620) — lifecycle, shadow (→ `HandleShadowVerb` PC:6711), reader, cursor/effect, captureWindow, urlRules.
2. `HandleCredentialsVerb` (PC:7254-7520), gated on prefix `credentials.`.
3. `HandleSkillDialectVerb` (PC:8321-9199) — resolveTabIds, JS dialogs, tabGroups/splitView/downloads/pinnedTabs, spaces.listTabs/openTab/focus/activate/update, bookmark *tree* forms.
4. The remaining ladder — the "box probe"/client-UI dialect, keyed by `windowId`.

Name collisions are disambiguated by **payload shape**, not by name:
- `agentSpace.ping` with `taskId` → lifecycle (PC:6168-6183); without → generic `{ok:true}` (PC:4697).
- `agentSpace.bookmarks.list/add` claim the tree form only when the payload has **no** `windowId` (PC:9059-9070); with one, they fall through to the flat form (PC:5140-5220).
- `agentSpace.spaces.list` and `agentSpace.list` emit *both* dialects' fields in one object (PC:4699-4710, 4770-4800).

Verb inventory: 107 distinct `type == "…"` literals (grep over `PC`), plus 4 shadow ops (`list/create/openTab/ping/close`, PC:6738-6880) and the tabGroups/splitView/downloads/pinnedTabs families that switch on `type.substr(21)` (PC:8402-8760). File:line index of every family:

| Family | Lines |
|---|---|
| lifecycle: create/ping/complete/getOwnership/handoff/takeover/setState/log/markError/readUserMessages/userMessage/openTab/listProfiles/panelSize/cursor/effect/captureWindow | PC:6077-6529 |
| readerArticle/readerDocument (deferred reply) | PC:6040-6075 |
| shadow.* | PC:6711-6880 |
| urlRules.list/add/update/delete | PC:6531-6618 |
| credentials.status/getTotp/get/autofill | PC:7257-7520 |
| resolveTabIds, get/handleJavaScriptDialog | PC:8347-8400 |
| tabGroups.*, splitView.*, downloads.*, pinnedTabs.* | PC:8402-8760 |
| spaces.listTabs/openTab/focus/activate/update | PC:8765-9040 |
| bookmarks tree list/add/addFolder/update/move | PC:9059-9195 |
| list/newWindow/close/navigate | PC:4699-4757 |
| spaces.list/create/switch/rename/delete/setTheme/setIcon/reorder | PC:4759-4891, 5282-5337, 5917-5936 |
| profiles.list/create/rename/delete, window.profile | PC:4892-4983 |
| layout.*, pinned.* (flat), bookmarks.* (flat), emoji.* | PC:4984-5281 |
| theme.*, settings.get/set/open, window.layout | PC:5338-5566 |
| feedback.*, telemetry.status, analytics.list, menu.list/click, window.handle/bounds/setBounds | PC:5567-5769 |
| tabs.list/new/activate/close/split/unsplit/move/group/ungroup, nav.*, rows.reorder, groups.* | PC:5770-6011 |

### 2.2 Parsers

- Payload is parsed once up front for the pre-filters (PC:4671-4672) and then **re-parsed from the raw string in 45 more places** in the ladder (`grep -c 'ReadDict(payload_json'` = 45). Every handler re-does `FindDouble("windowId")` with its own defaulting. No hand-rolled string parsers (all `base::JSONReader`), but four hand-built JSON string literals for errors (PC:385, 455, 471, 6071) that bypass the envelope builder.
- Key-spelling tolerance is ad hoc per verb: `id|profileId`, `name|displayName` (PC:4898-4909, 4931-4935), `profileId|profileName` (PC:6118-6124), `domain` at top level or under `query` (PC:6889-6910).

### 2.3 Response/error shape

Inconsistent in at least five ways:
1. Some replies omit `ok` entirely: `getOwnership` (PC:6215-6224), `listProfiles` (PC:6407-6431), `panelSize` (PC:6432-6438), `tabs.list` (PC:5770-5784), `layout.get`, `settings.get`.
2. Unknown verb → `{"error":"unknown type: …"}` with no `ok:false` (PC:6017).
3. Error codes are mostly snake_case tokens (`unknown_task`, `space_not_open`) but some are English sentences (`"No target with given id found"`, PC:8385, 8398) copied from the Mac DevTools domain.
4. Transport-level `error` (bad json / missing type, PCS:540-543) vs application `{ok:false}` inside `responseJson`.
5. Numeric ids are emitted as `double` (`static_cast<double>(window_id)`, PC:6154 etc.) — fine for JSON, but the client compares them to `Browser.getWindowForTarget` ints; keep, but document.

`cdp.mjs:566-575` papers over (1)-(2) by treating `reply.ok === false` as the only failure signal, so an unknown verb currently *resolves* successfully with `{error:"unknown type…"}` on the `PhiAgentSpace.*` → `agentSpace.*` shim path. That is a real bug: a typo'd verb is a silent success.

### 2.4 Scope resolution

`ResolveScopeBrowser` (PC:8243-8270): `{taskId}` → agent space or shadow window; `{spaceId}` → `BrowserForSpace`; else `invalid_payload`. The client-UI ladder uses `{windowId}` → `ResolveBrowser(slot)` (PC:4652-4658), and the spaces.* dialect additionally accepts `{windowId}` as a *pin* (PC:8728-8756). So there are three scope grammars and no place that says which verbs accept which. Crucially, **none of them checks whether the resolved browser is an agent window or a user window** — `agentSpace.navigate {windowId:<user window>}` (PC:4743-4757), `tabs.close` (PC:5857-5884), `close` (PC:4723-4742), `menu.click` (PC:5697-5722) all act on user windows exactly like agent ones. User window ids are handed out by `spaces.list` → `windowIds` (PC:4791-4800) and by CDP `Browser.getWindowForTarget`.

### 2.5 Re-entrancy with the framework

- `CreateAgentBrowser`/`CreateShadowBrowser` fire `OnBrowserWindowCreated` synchronously (documented `phi_framework_api.h:538-544`, handled via `pending_agent_task_`/`last_agent_window_id_` at PC:6135-6141, 7601-7613). The pattern relies on global scratch state; two nested creates (possible via the nested-modal-loop re-entrancy in §1.7) would cross wires. A registry should serialise create-type verbs.
- `RequestClose` "may erase `it`" (PC:4730-4736) — handled. `OnTabsChanged` re-entrancy is deferred via `WM_PHI_READER_SWEEP` (PC:144-149) — handled.
- `ExecuteControlOnUi` is also called *from the UI itself* (`RequestCreateSpaceWithOptions`, PC:4601) — the verb ladder doubles as the client's internal API. A registry must keep an in-process caller with an "internal / full capability" principal.

---

## 3. Trust model

### 3.1 Agent Spaces vs user Spaces

What exists:
- Agent Space = off-screen `PhiWindowController(agent=true)` (PC:7601-7613); record in `PhiAgentSpaces` with ownership `kAgent|kUser` (`client/phi_agent_spaces.h:37, 97-101`).
- User-space operating mask: browser reports `OnAgentDroveUserSpaceTab/Observed/Stopped` (`phi_framework_api.h:149-167`, PC:2593-2611) → `PhiAgentDriveRegistry` → "Take control" → `SetUserReclaimedTabs` → the *browser* refuses drive commands (PC:213-222; `phi_framework_api.h:663-670`). This is the one hard enforcement point in the whole system, and it is good design.

Gaps:
- **No permission toggle** (F3). `references/management.md:73-80` and `lifecycle.md:113-117` say the entire user-space surface (`spaces.*`, `urlRules.*`, `pinnedTabs.*`, `bookmarks.*`, `profiles.*`, `enterContext({kind:'user'})`, shadow windows) sits behind Settings ▸ Developer ▸ "Allow agents to operate your Spaces" and fails `user_space_operations_disabled` when off, with per-profile `agentSpacesAllowed`. Grep of `PC`/`phi_settings_window.cc` finds neither string; `agentSpacesAllowed` is hard-coded `true` (PC:6423). Profiles are deletable (`agentSpace.profiles.delete` → `DeleteProfileById` → `framework_->DeleteProfile`, PC:4892-4914, 3524-3538) although `management.md:37-38` promises "Profiles can be created and renamed, not deleted (deliberate)". `spaces.delete` cascades with no confirmation (PC:3352+). URL rules — the thing that decides which Space (profile/cookie jar) a navigation lands in — are agent-writable (PC:6554-6618); a rule routing `bank.com` into a Space whose profile the agent controls is a cookie-jar redirection.
- **Developer mode is decorative** (F9). `phi_settings_window.cc:862-867` tells the user that turning developer mode off turns off agent access and the agent password manager; `SetDeveloperMode` only saves a pref (PC:3572-3578), `control_server_.Start` is unconditional (PC:278), `AuthorizeAgentPeerOnUi` never reads it, and `settings.set {developerMode:true}` lets the agent switch it on (PC:5457-5460).
- **Mask only covers CDP drive** (F5). `agentSpace.navigate`, `tabs.new/close/activate`, `spaces.openTab`, `nav.back` go through `framework_->Navigate/NewTab/CloseTab` (PC:4755, 5794, 5873-5882, 8827), not through the DevTools handler that reports `PhiUserSpaceDrive`, so they never arm the mask and are never refused for a reclaimed tab.
- **Agent-space ownership is cooperative** (F5). User "Take control" only flips `ownership` and broadcasts `ownershipChanged` (PC:4425-4434); the agent's CDP session keeps working, and `agentSpace.takeover` (PC:6241-6254) lets the agent flip it straight back with no user involvement. The skill honours it (helpers check `state.task.ownership`), but a non-skill peer need not.
- **Task identity is unbound** (F6). `taskId` is whatever the payload says (PC:6018-6024). There is no map from connection → tasks. Any consented peer can `complete` (PC:6186-6213), `handoff`, `markError`, `log` into, or `captureWindow` any other agent's Space; `agentSpace.userMessage` lets the *agent* enqueue a message that is rendered as `"You: …"` in the transcript (PC:6359-6385) — i.e., the agent can forge user instructions in the user-visible log. The shadow list comment admits the model: "the Windows pipe carries one consented peer, so the whole registry is that caller's" (PC:6735-6738).

### 3.2 Credential flow

Path: `credentials.autofill` → `ParseCredentialQuery` (PC:6889-6910) → `TargetUrlForCdpTarget` for page host (PC:7450-7457) → `ResolveCredentialItem` (helper or DPAPI dev vault, PC:6981-7067) → origin check unless `allowCrossOrigin` (PC:7496-7501) → `PromptCredentialApproval(detail, kFill, scope)` (PC:7505) → `FillTaggedField` (PC:7513-7519). `credentials.get` = same but `kReveal` and the secret is returned (PC:7348-7424).

- **Grant scope = item name (or domain), kind ∈ {fill, reveal}** (PC:7104-7109, key at PC:7368-7369, 7503-7504). Not the page host, not the task, not the peer. `CredentialGrantCovers` (PC:7086-7102) checks only (scope, kind). Standing grants persist to `PhiCredentialGrants.json` (PC:7227-7250); 10-minute grants are in-memory.
- **F2 (Critical):** because the origin check runs *before* and *independently of* the grant check, and `allowCrossOrigin` is an agent-supplied boolean (PC:7434-7436), an agent holding a standing (or 10-minute) *fill* grant for "github.com" can call `credentials.autofill {allowCrossOrigin:true, targetId:<attacker tab>}` and the password is typed into the attacker's DOM with **no prompt** (PC:7104-7109 returns early with "allowed (standing approval)"). The skill's own doc calls this exact mismatch "how a misleading page or injected instruction would exfiltrate a password" (`credentials.md:74-79`). Cross-origin must *never* be covered by a standing grant and must always show the page host in the dialog.
- **Prompt binding.** The prompt names item and page host (PC:7502-7505) but the approval that results is stored without the host. So "Always allow" for a fill on `login.github.com` covers a later fill on `evil.github.com.attacker.example`? No — `HostMatches` rejects that (`phi_credentials.cc:138-157`, parent must contain a dot and be a suffix) — but it *does* cover any `*.github.com` subdomain and, with `allowCrossOrigin`, anything.
- **Repeat prompting.** No denial memory for credentials: each `user_denied` (PC:7387-7391) can be immediately re-requested; combined with default-button-Allow (PC:7127-7130) and `MB_TOPMOST`/`MB_SETFOREGROUND` on the fallback (PC:7134-7137) this is a prompt-fatigue attack. Also the *reveal* "Always allow" is persisted forever and applies to every future consented peer.
- **"Secret-free" is convention (F7).** `FillTaggedField` writes the value into an input via isolated-world script (`phi_framework_api.h:764-775`). The agent holds a CDP session on the same tab and can `Runtime.evaluate('document.querySelector("#password").value')`. The `•••` scrubbing lives in the agent's *own* process (`helpers.mjs`, credentials.md:80-86) — a non-skill peer or a prompt-injected skill just reads it back. The prompt text "The agent never sees it" (PC:7114-7116) is therefore not a guarantee Phi can make. Honest options: (a) treat fill as reveal for grant purposes (kind hierarchy collapses), or (b) make the fill *unreadable* — fill and auto-submit in one browser-side step, or fill into a browser-owned `<input>` shadow that script cannot read (hard, renderer work). At minimum reword the prompt and never let a fill grant be "always".
- **TOTP (F7).** `credentials.getTotp` hands a live 2FA code to the agent behind a single reveal prompt (PC:7285-7346), while `credentials.md:66-69` states TOTP is deliberately not exposed because it "would collapse both factors behind one approval". Either remove the verb or make it a distinct kind that is never covered by a standing grant.
- **Plaintext dev vault.** `PhiCredentialVault::Load` accepts a plaintext JSON array when DPAPI unwrap fails (`phi_credentials.cc:76-82`) with no build/dev gate; the dev vault is used whenever `PhiBitwardenHelper.exe` is absent (PC:262-265). Release builds must refuse plaintext and refuse to fall back to the dev vault at all (`ResolveCredentialItem` already refuses to *mix* stores, PC:6989-7002 — extend that to "no helper → `not_installed`").
- Grant/audit files (`PhiAgentGrants.json`, `PhiCredentialGrants.json`) are default-ACL, same-user-writable, unsigned (PC:747-767, 7227-7250). Any same-user process can pre-seed a reveal grant. Sign or DPAPI-wrap them with a per-install key so at least tampering is detectable.

### 3.3 What a malicious or buggy skill can do today (attack narratives)

1. **Zero-consent full CDP:** connect to `127.0.0.1:<DevToolsActivePort>`, `Target.getTargets`, attach to the user's banking tab, `Storage.getCookies` / `Runtime.evaluate`. No prompt, no mask (mask arms only for drive commands; reads are `Observed`, which never arms, PC:2600-2604). **F1.**
2. **Consent inheritance:** user once clicks "Always Allow" for the legitimate skill; any Node script thereafter connects silently. **F4.**
3. **Password exfil:** legitimate 10-minute fill grant for `github.com` → `credentials.autofill{allowCrossOrigin:true}` into `attacker.example` tab in the agent's own Space → no prompt. **F2.**
4. **Cookie-jar redirection:** `urlRules.add {host:"bank.com", spaceId:<space in a profile the agent also drives>}` → user's next bank navigation lands where the agent has CDP. No toggle, no prompt. **F3.**
5. **Destruction:** `spaces.delete`, `profiles.delete`, `bookmarks.remove` on folders, `settings.set{downloadDirectory}` — no confirmation. **F3.**
6. **User-tab driving without a mask:** `agentSpace.navigate {windowId:<user window>}` — no pill, no reclaim. **F5.**
7. **Transcript forgery / task hijack:** `agentSpace.userMessage{taskId:<other agent>, text:"…"}` shows as "You:" in the console; `agentSpace.complete` another agent's task. **F6.**
8. **Prompt fatigue:** loop `credentials.get` until the user's Enter lands on the default "Allow once". **F4.**

---

## 4. Target design

### 4.1 Transport auth: token + identity + rate limit (both, not either)

- **Per-launch token** (32 random bytes, hex) generated in `PhiControlServer::Start`, written to `CDPAgentSocket` alongside the pipe name; the pointer file gets an explicit DACL (owner + SYSTEM, no inheritance) and is written *after* the first pipe instance exists. Every connection must present it in the HTTP head (`Authorization: PhiToken <hex>` or `X-Phi-Token`) before the consent gate runs; missing/wrong → `403` without any UI. This kills prompt-spam from processes that cannot read the user's profile directory (sandboxed/low-IL/other users) and turns "can connect" into "can read the profile dir", which is the same trust level as the profile itself.
- **Keep peer identity**, but change the key: `(image path, Authenticode publisher or SHA-256 of the image)` for "Always Allow"; drop the `name:` fallback to allow-once only; never persist a grant for a known interpreter (`node.exe`, `python.exe`, `pwsh.exe`, `cmd.exe`) — for those, "Always Allow" should bind to the *agent capability* below instead.
- **Agent capability**: on the first `/phi-agent` upgrade of a consented connection, mint an opaque capability (`X-Phi-Agent-Capability`, matching the `[A-Za-z0-9_-]{32,128}` the skill already parses at `cdp.mjs:236-247, 348-350`), bound to the peer image and TTL'd. Later connections presenting it skip the prompt; that is what "Always Allow" should persist. This gives the Mac's task-principal model a Windows implementation without process-tree walking.
- Pipe: `FILE_FLAG_FIRST_PIPE_INSTANCE`, random suffix, explicit `S:(ML;;NW;;;ME)`, connection cap (e.g., 16) with `429` beyond it, listener retry loop.
- Prompt: default button = Deny; coalesce concurrent requests from the same image into one dialog; after a Deny, refuse silently for N minutes (exists for peers, PC:667-668; add for credentials).

### 4.2 Capability scoping per connection

Each accepted connection gets a `Principal { peer, capability_id, caps: bitset, tasks: set<taskId>, granted_spaces: set<spaceId>, granted_windows: set<windowId> }`, stored in the `ControlChannel` and passed into `HandleControlCommand` (signature change: `HandleControlCommand(const Principal&, type, payload)`).

Default capability set after consent = **`kAgentSpace`** only: create/drive/complete *its own* tasks (taskIds it created on this connection; `agentSpace.create` records `principal.tasks.insert(taskId)`), shadow windows, credentials with prompts, `spaces.list` (read-only), `profiles.list`, `userFocus`. Everything else requires an explicit capability:

| Capability | Verbs | How granted |
|---|---|---|
| `kUserSpaceRead` | `spaces.listTabs`, `bookmarks.list`, `pinnedTabs.list`, `urlRules.list`, `downloads.list` | Settings toggle "Allow agents to read your Spaces" |
| `kUserSpaceWrite` | `spaces.openTab/activate/create/update/rename/reorder/setTheme/setIcon`, `bookmarks.*`, `pinnedTabs.*`, `urlRules.*`, `tabGroups.*`, `splitView.*`, `enterContext({kind:'user'})` | Settings toggle "Allow agents to operate your Spaces" (the one the skill already documents) **and** per-request grant for `urlRules.*` |
| `kDestructive` | `spaces.delete`, `profiles.delete`, `bookmarks.remove` (folder), `groups.close`, `close`, `tabs.close` on user windows | Per-request prompt, never standing |
| `kAppSettings` | `settings.set`, `theme.set`, `layout.set`, `menu.click`, `feedback.*`, `window.setBounds` | Developer mode + toggle; `developerMode` key itself never agent-settable |
| `kHost` | `captureWindow{outPath}`, `window.handle`, `settings.set{downloadDirectory}` | Test-rig only: refuse unless a `--phi-test-rig` switch is present; never in release |
| `kCredentialFill` / `kCredentialReveal` / `kCredentialTotp` | credentials.* | Per-request prompt with grant model in §4.6 |

Scope enforcement lives in one place: `ResolveScope(principal, payload) → ScopedBrowser{browser, kind: agent|shadow|user, task}`, and the registry entry declares which kinds it accepts. A verb declared `scope: agent` given a user `windowId` fails `scope_not_allowed` before the handler runs. This replaces the three grammars in §2.4 and closes F5's client-verb hole (user-window navigation must go through a path that reports `OnAgentDroveUserSpaceTab` — simplest: have the client call a new `framework_->NavigateAsAgent(browser, url, session_tag)` that raises the same report).

Per-connection `tasks` closes F6: lifecycle verbs on a task not in `principal.tasks` fail `not_owner`; `userMessage` is removed from the agent-callable set (it exists for the mirror/tests — give the test rig its own principal). The UI's own calls (`RequestCreateSpaceWithOptions`, PC:4601) use an `Internal` principal with all caps.

### 4.3 Single typed verb registry

```
struct VerbSpec {
  std::string_view name;          // "agentSpace.spaces.delete"
  ScopeKinds scopes;              // {kAgent}, {kUser}, {kAgent|kShadow}, kNone
  Capability required;            // kAgentSpace / kUserSpaceWrite / ...
  const ParamSchema* params;      // typed fields, required/optional, aliases (id|profileId)
  bool prompts;                   // shows UI → serialised, never re-entrant
  bool deferred;                  // pendingReply pattern
  Handler handler;                // (const Principal&, const Scope&, const Params&) -> Result
};
```

- One table (`phi_verbs.cc`), one parse (`Params` built from the schema, aliases resolved there, so `id|profileId`, `name|displayName`, `domain` top-level vs `query.domain` are declared once).
- Dispatcher: parse envelope → look up spec → check capability → resolve scope → validate params → (if `prompts`) take the UI-prompt mutex → call handler → wrap result. The 45 `ReadDict(payload_json)` sites and the payload-shape collision hacks (§2.1) disappear; `bookmarks.list` (flat, `windowId`) and `bookmarks.list` (tree, `spaceId`) become two registered entries with distinct required fields, or one entry with a oneOf schema.
- Registry doubles as the source for a generated `references/verbs.md` and for `agentSpace.capabilities` (a verb the skill can call to learn what this build/principal allows — replaces the "on an older build… throws" prose in the skill).

### 4.4 Uniform envelope

Wire: `{id, type, payload}` (object, not a stringified `payloadJson`) → `{id, ok:true, result:{…}}` | `{id, ok:false, error:{code, message?, details?}}`, events `{event, payload}`. Keep `payloadJson`/`responseJson` accepted for one release behind a version header (`X-Phi-Protocol: 2`), because `cdp.mjs:659-679` is shared with the Mac. Error `code` is a closed enum (`invalid_payload, unknown_verb, scope_not_allowed, not_owner, unknown_task, space_not_open, window_not_open, user_denied, origin_mismatch, capability_required, busy, timeout, internal`). Unknown verb becomes `ok:false` (fixes the silent-success bug in §2.3). Transport-level failures (bad JSON) use the same shape.

### 4.5 CDP proxy restrictions

With the in-process transport from §1.2 option 2 the control server becomes a `DevToolsAgentHostClient` per connection and can filter:
- Rewrite/deny `Target.getTargets`, `Target.attachToTarget`, `Target.setAutoAttach`, `Browser.getWindowForTarget` results to targets whose window is in `principal.granted_windows ∪ (windows of principal.tasks)`. Default: agent + shadow windows of this principal only. `kUserSpaceWrite` + an `enterContext({kind:'user', space})` grant adds that Space's windows (and arms the mask via the existing `PhiUserSpaceDrive` path).
- Deny `Browser.*` global mutators (`setDownloadBehavior` outside the task's profile, `grantPermissions` on user contexts), `Storage.getCookies` on non-granted browser contexts.
- Alternative with less protocol surgery: create each agent task in its **own `BrowserContext`** (`Target.createBrowserContext` semantics, but from the framework: `CreateAgentBrowser(profile, isolated=true)`), so the agent's CDP session physically cannot see user targets unless the user grants a Space. This is the stronger model; the filter above is still needed for the granted-user-Space case.

### 4.6 Credential approval bound to (kind, item, page-host, principal, ttl)

- Grant key = `{kind, item_id, page_host, principal.capability_id}`. `CredentialGrantCovers` requires an exact key match; a `reveal` grant covers `fill` for the same item **and** host only.
- `allowCrossOrigin` always prompts, the prompt says "CROSS-ORIGIN: <item's site> → <page host>", and its approval is once-only (never remembered).
- Standing grants: `fill` may be "10 minutes" or "always"; `reveal` and `totp` are once or 10 minutes only, never "always". Persisted grants are wrapped with DPAPI + HMAC over the file so same-user edits are detected.
- Denial memory: after `user_denied` for a key, auto-deny for 5 minutes without a prompt; prompt default = Deny; one prompt at a time (registry `prompts` mutex).
- Post-prompt re-check: re-read `TargetUrlForCdpTarget` after the user approves and fail `origin_changed` if it moved (cheap TOCTOU hardening on top of the tag-disappears behaviour).
- Reword the fill prompt: "Phi types the secret into the page. Any automation attached to that page can read it." Or ship the auto-submit variant so the agent never has a readable field.
- `credentials.getTotp`: remove, or gate behind a separate "Allow agents to receive 2FA codes" setting that is off by default.
- Release builds: no plaintext vault, no dev-vault fallback when the helper is missing (`status: not_installed`).

### 4.7 Migration plan and cost

| Phase | Work | Est. | Ship gate |
|---|---|---|---|
| P0 (must precede any external build) | Kill the TCP door (§1.2, option 2 or interim 3); launch token in pointer file + restricted ACL; `FILE_FLAG_FIRST_PIPE_INSTANCE` + random name; prompt default Deny + coalescing + connection cap; cross-origin never covered by a standing grant; remove `getTotp` or gate it; refuse plaintext vault in release; real "Allow agents to operate your Spaces" toggle wired to a `kUserSpaceWrite` check in front of the existing ladder (a single `if (!user_space_ops_allowed_ && IsUserSpaceVerb(type)) return user_space_operations_disabled;`); developer-mode key not agent-settable | Client: ~2-3 days. Fork: 2-5 days for the in-process transport (1 day for the interim PID gate) | Blocks release |
| P1 | `Principal` on connection; per-connection task ownership; capability bitset; `ResolveScope` unification; agent-only default with user-Space grant flow; user-window client verbs routed through a mask-reporting framework call | ~1 week client, ~1-2 days fork (`NavigateAsAgent` / report hook) | Blocks release |
| P2 | Typed registry + schema + uniform envelope (protocol v2 with v1 shim); regenerate skill references from the table; fix silent unknown-verb success in `cdp.mjs` shim | ~1-2 weeks; mostly mechanical migration of ~110 handlers, high test coverage already exists (`selftest*.mjs`) | Strongly recommended pre-release; can trail P1 by one milestone |
| P3 | CDP target filtering / per-task browser contexts; credential grant key rewrite + signed grant files; helper I/O off the UI thread; relay/WS correctness fixes (§1.5-1.6) | ~1-2 weeks | Filtering blocks release if P0 used interim option 3; rest can follow |

Total to a defensible release: roughly 4-6 engineer-weeks, of which P0 is the non-negotiable ~1 week.

---

## 5. Ranked findings with evidence

1. **[Critical] Loopback DevTools TCP port bypasses consent entirely.** `client/phi_client_main_win.cc:380-389` (`--remote-debugging-port=0`); `PCS:598-623` reads `DevToolsActivePort`; consent only in `PCS:441-451`; skill's false assumption `cdp.mjs:14-15`. Mask/reclaim does not help: reads never arm (`phi_framework_api.h:158-163`), refusal is drive-only (`:663-670`).
2. **[Critical] Standing fill grant + `allowCrossOrigin` → unprompted cross-origin password fill.** Origin check `PC:7496-7501` skipped by agent-supplied flag `PC:7434-7436`; grant check `PC:7104-7109` keyed on item only (`PC:7086-7102`, key built at `PC:7503-7504`); persisted grants `PC:7227-7250`.
3. **[High] User-space surface ungated; destructive verbs unconfirmed.** No `user_space_operations_disabled` anywhere in `PC`/`phi_settings_window.cc`; `agentSpacesAllowed` hard-coded `PC:6423`; `profiles.delete` `PC:4892-4914` → `PC:3524-3538` (skill promises no deletion, `management.md:37-38`); `spaces.delete` `PC:4875-4891` → `PC:3352`; `urlRules.add/update/delete` `PC:6554-6618`; `settings.set{downloadDirectory}` `PC:5480-5484`; `menu.click` `PC:5697-5722`; `captureWindow{outPath}` arbitrary file write `PC:6479-6529`.
4. **[High] Consent identity is the interpreter's image path; prompt defaults to Allow; unbounded prompt spam.** Key `PC:620-623`; persist `PC:661-664`; `name:` fallback `PCS:412-436`; default index 0 `PC:645-647`; unbounded threads `PCS:337-391`; nested modal dispatch `phi_prompt.cc:169-176` + `PC:600-610`; claim headers never parsed (no `X-Phi` in server/coordinator vs `cdp.mjs:228-247`); grant file default ACL `PC:756-766`.
5. **[High] Ownership/mask are cooperative for agent Spaces and absent for client-verb driving.** `agentSpace.takeover` `PC:6241-6254`; user take-control only flips state `PC:4425-4434`; `agentSpace.navigate` on any `windowId` `PC:4743-4757`; `tabs.close` `PC:5857-5884`; `spaces.openTab` `PC:8804-8837`; none reach `OnAgentDroveUserSpaceTab` `PC:2593-2597`.
6. **[High] No task↔connection binding; transcript forgery.** `task_id_of` `PC:6018-6024`; `complete` `PC:6186-6213`; `userMessage` renders as `"You: "` `PC:6359-6385`; admitted single-principal model `PC:6735-6738`; `HandleControlCommand` signature carries no principal `client/phi_control_server.h:36-37`.
7. **[High] TOTP served; "secret-free" fill unenforced; plaintext vault accepted.** `PC:7285-7346` vs `credentials.md:66-69`; `FillTaggedField` DOM write `phi_framework_api.h:764-775` readable by the agent's own CDP session; scrubbing is agent-side `credentials.md:80-86`; plaintext fallback `client/phi_credentials.cc:76-82`; dev-vault selection `PC:255-265`.
8. **[Medium] Pipe squatting / listener death.** No `FILE_FLAG_FIRST_PIPE_INSTANCE` `PCS:337-341`; predictable name `PCS:302-303`; pointer written first `PCS:305-315`; `return` on failure `PCS:345-349`.
9. **[Medium] Developer mode gates nothing and is agent-settable.** UI claim `phi_settings_window.cc:862-867`; setter `PC:3572-3578`; unconditional server start `PC:278`; verb `PC:5457-5460`.
10. **[Medium] Relay back-thread UAF on slow peer.** Stack `ctx` `PCS:680-681`, 2 s join `PCS:698-701`, dereference `PCS:272`, pipe closed by caller `PCS:470-472`.
11. **[Medium] UI-thread blocking on vault helper per `credentials.status`.** `PC:6945-6957`, `PC:7260`; helper `Call` blocks up to 30 s `client/phi_vault_helper.h:68-71`.
12. **[Low] Unknown verb resolves as success through the skill's `PhiAgentSpace.*` shim.** `PC:6017` emits `{error}` without `ok:false`; `cdp.mjs:566-575` only rejects on `ok === false`.
13. **[Low] Dispatcher hygiene.** 45 re-parses of `payload_json`; 107 string-compared verbs across four layered handlers with payload-shape disambiguation `PC:6168-6171`, `PC:9059-9070`; mixed response shapes `PC:6215-6224, 6407-6431, 5770-5784`; sentence-style error codes `PC:8385, 8398`; hand-built JSON literals `PC:385, 455, 471, 6071`; `volatile` cross-thread flag `PC:396, 4961`.
14. **[Low] WebSocket/HTTP laxity.** No 400 on missing key `PCS:482-484`; case-sensitive header match `PCS:480`; no `Upgrade`/`Version` validation; control frames inside fragments appended as data `PCS:246-253`; no pong `PCS:522`; pipelined bytes after head corrupted in relay `PCS:142, 651-676`.

Positive notes worth keeping: pipe SDDL + `PIPE_REJECT_REMOTE_CLIENTS` (`PCS:330-341`); consent before *both* families (`PCS:394-451`); browser-side reclaim refusal (`PC:213-222`); fill vs reveal kind separation (`PC:7086-7102`); no silent helper→dev-vault fallthrough (`PC:6989-7002`); bounded pushes (`PCS:576-596`); vault helper isolation (`client/phi_vault_helper.h:19-42`); `WM_NULL` wake in the modal loop (`phi_prompt.cc:121-129`).
