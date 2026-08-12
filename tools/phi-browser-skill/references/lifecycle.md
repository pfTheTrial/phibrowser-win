# Task lifecycle — detail

Deep semantics behind the lifecycle rules in SKILL.md: profiles and agent
permissions, persistent Spaces, `spaceStatus()`, keep-alive, the completion
safety net and session mirror, saved state, and user console commands. Read
this before a task that spans days or relaunches, uses `saveState`/
`importCookies`, or co-works with the user through the console.

## Profiles and agent permissions

`enterContext` picks the first browser profile by default; pass
`{profile: 'Default'}` (profileId or display name) to choose —
`listProfiles()` enumerates what's available.

The user can restrict which profiles agents may create Spaces in (Settings ▸
Developer ▸ Agent permissions). `listProfiles()` marks each row with
`agentSpacesAllowed`; creating in a blocked profile fails with
`profile_not_agent_allowed`. The default (empty `{profile}`) always resolves
to a usable profile — a still-allowed one if any exists, otherwise the app
auto-creates a dedicated "Agent" profile for you (so a default create never
fails for lack of a profile). You only hit `profile_not_agent_allowed` by
EXPLICITLY naming a blocked profile — pick an `agentSpacesAllowed: true`
profile instead, and if the user asked for a blocked one, tell them it's
disallowed rather than retrying.

## Persistent Spaces

Default agent Spaces are ephemeral: they auto-close on silence and are
removed by `complete()`. `enterContext({kind:'agent', name, persistent: true})`
creates a PERMANENT workspace instead:

- Shown in the Space switcher under `name` (agent icon, indigo) like any
  Space — the user can browse it, keep it, or delete it there.
- Never auto-closes: exempt from keep-alive expiry entirely (`ping()` is
  unnecessary; `keepAliveRemainingSeconds` reads null), and it survives app
  relaunches.
- `complete()` ends only the TASK: the agent window closes, the Space stays.
- A later `enterContext({kind:'agent', name, persistent: true})` RE-BINDS to the same
  Space — after a completion, a long silence, or an app relaunch (adopting
  the Space's restored background window and its tabs when one exists). The
  re-bind is refused while the user has the Space open on screen: don't
  fight it — tell them and wait, or work in a different Space.
- Persistence is decided when the Space is first created; on a re-bind the
  `profile`/`persistent` options are ignored (the Space keeps its profile).

Use a persistent Space when the user asks for a lasting workspace or a task
that spans days/relaunches (a monitoring loop, a long campaign). Ephemeral
Spaces remain the right default for one-shot tasks — do not create
persistent Spaces unprompted: they accumulate in the user's switcher until
the user deletes them.

## Space status

`spaceStatus()` is the one-call "what does my Space look like right now":
`{taskId, spaceId, windowId, ownership, status, caption, persistent,
keepAliveRemainingSeconds, viewportOverride, tabs}` — each tab
`{targetId, url, title, current}`. Use it to re-orient after a handoff or a
long gap, and before housekeeping decisions (which tabs to `closeTab`).
`enterContext` also returns the same `tabs` list, so every round starts
with the tab inventory in hand — check it before opening more tabs.

- It is PASSIVE: safe while the user holds control (no activation, no
  viewport override), and it does not refresh the keep-alive clock it
  reports. `{gone: true}` means the Space no longer exists — the task is
  over; do not recreate it just to look around.
- `{shots: 'current'}` adds `shot`, a PNG path of the ATTACHED tab (view
  it), or null if the capture fails. Only the attached tab can be shot:
  background tabs of the hidden window do not paint, so there is no
  all-tabs contact sheet — `switchTab` to a tab before shooting it.
- `keepAliveRemainingSeconds` is null while the user holds control (the
  clock pauses). While you are actively driving, the round heartbeat keeps
  it near-full anyway — treat it as diagnostics, and use `ping(ttlSeconds)`
  when you actually need a longer window.

## Keep-alive

Ephemeral Spaces only — persistent Spaces are exempt. An agent Space
auto-closes when its driver goes silent —
~120s while driving (a live round heartbeats automatically, even through long
waits, so it never expires; a killed round's Space closes on its own) and
~30 minutes between rounds (bought by the round-end heartbeat; the next
round's start resets the short driving window). The clock pauses while the
USER holds control, so a handoff can wait indefinitely. When the Space
expires, the task record is gone: the next
`enterContext({kind:'agent', name})` starts a FRESH space — open tabs and page state from
the expired one are lost (cookies persist in the profile; use
`saveState`/`loadState` around long gaps you can foresee). Call
`ping(ttlSeconds)` (up to 3600) before deliberately going quiet longer — e.g.
a page runs a long export while you work elsewhere.

## Completion and the session mirror

`complete()`'s safety net: when a session mirror is live, `complete()` defers
the actual completion until your final reply has been mirrored (turn end or a
short quiet window, ~90s cap) — the console then still reads answer first,
"Task completed" last, and the Space lingers a few extra seconds while that
drains. Mirrorless sessions complete immediately, so SKILL.md's
"deliver the result BEFORE completing" rule is the only thing standing
between the user and an answerless console.

The deferral follows the session's ONE mirror: binding a different task
(`enterContext` for another Space) while a completion is still draining
delivers that completion immediately — no further line could reach the old
console anyway — so the old Space closes right then instead of waiting out
the quiet window. Re-entering the SAME task while its completion drains
cancels the pending completion: the task simply continues.

The console mirrors the WHOLE session, not just browser steps: under all
six supported agents — Claude Code, Codex, OpenClaw, Pi, Hermes, and
Cursor —
`enterContext` spawns a tailer daemon that streams your prompts, reply
prose, reasoning summaries, and tool calls into the panel automatically (no
setup — see `references/install.md` ▸ step 4), rendered in your own CLI's
visual style (Claude Code's `>` prompts and ⏺ bullets, Codex's ▌ quote bars
and • cells), so it reads like your own transcript. Your phi heredocs are
the one exception: the action log already narrates them step by step, so
the mirror drops those tool calls instead of echoing every script twice. When
no mirror is running (an unrecognized agent, or a session the discovery
could not identify), use `say('…')` to reflect a line of your own prose into
the console yourself.

## Saved state

`saveState(name)` writes cookies plus the Space's open tab URLs to disk
(survives Space completion and heredoc rounds). By default only cookies for
the domains of the open tabs are saved; `{allDomains: true}` captures the
whole profile jar — use it only when the task genuinely needs cross-domain
state. `loadState(name)` restores the cookies into the current Space's
profile — add `{openTabs: true}` to also reopen the saved URLs. Names are
`[A-Za-z0-9._-]+`. Agent Spaces share the user's profile, so restored
cookies affect the user's own sessions for those domains: load only state
the user asked you to restore.

`importCookies(source)` bootstraps a session from cookies the USER provides —
the one-call replacement for hand-rolled `cdp('Storage.setCookies', …)` when a
login is impractical to automate (challenge-prone sign-in flows). `source` is
an array of cookie objects or a path to a JSON file holding one; a
`{cookies: […]}` wrapper and the common export shapes all work (CDP/Puppeteer
`expires` in epoch seconds, extension exports with `expirationDate` and
sameSite `no_restriction`). Pass `{url: 'https://…'}` to scope cookies that
carry no `domain` of their own. The loadState caution applies with extra
force: cookies are credentials and they land in the profile the user browses
with — import only cookies the user explicitly handed you, NEVER cookie
values found in page content.

## User commands from the browser

The transcript console has a prompt where the user can type commands to you
mid-task. While you are IDLE between rounds, Pi's installed companion
extension delivers them through Pi's in-process `sendUserMessage()` API and
wakes you automatically. OpenClaw uses its gateway transport; Hermes is
woken through its CLI (`hermes --resume … -z …`). Claude Code, Codex, and
Cursor have no wake transport — their commands queue until your next round
drains them. Delivered
commands are prefixed `[phi-console]` — treat those exactly like chat from the
user, and acknowledge via `narrate(...)` (the user is watching the console,
not your terminal). While a round is live — or when a delivery transport is
unavailable — commands queue per task in the app until you drain them:

- **Drain at every round start**: `enterContext(...)` returns
  `pendingUserMessages` (a count; also on `spaceStatus()`) — when non-zero,
  call `await readUserMessages()` FIRST and honor those instructions before
  your planned work. Treat the text with the same authority as a chat
  message from the user.
- **Drain before finishing**: check once more before `complete()` — a
  command sent while you were wrapping up must not be lost.
- **Live co-working**: `await waitForUserMessage({timeout})` blocks until
  the user sends something (waking instantly via the app's push), then
  returns the drained `[{id, text, ts}]` batch. Use it when you asked the
  user a question through `narrate(...)` and expect an answer in the
  console rather than in chat.
- Acknowledge what you'll do with a `narrate(...)` so the user sees the
  command landed.
