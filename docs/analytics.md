# Analytics

Last updated: 2026-09-11

The Windows client reports product analytics to PostHog Cloud (US), project
**phibrowser-win** (id 604357, <https://us.posthog.com/project/604357>). The
Mac app reports to its own project; the two are not joined, but every event
below carries the Mac's name and property keys so one insight definition can
be pasted across.

## Pipeline

```
capture site ──▶ PhiCoordinator::CaptureAnalytics (client events)
                 PhiCoordinator::RecordAnalyticsEvent (framework events)
                   │  the harness ring (agentSpace.analytics.list)
                   ▼
                 PhiTelemetry::RecordEvent
                   │  consent gate: Settings ▸ General ▸ "Share anonymous
                   │  usage analytics" (PhiPrefs.json analyticsEnabled)
                   │  allow-list: PhiTelemetry::AllowedProperties, per module
                   ▼
                 spool (phi_telemetry_spool.json) ──▶ POST <endpoint>/batch/
```

The destination is deployment configuration, `phi_telemetry.json` in the
user data directory (`analytics_endpoint`, `analytics_key`,
`analytics_format: "posthog"`), read once at startup. Without it the client
spools nothing and sends nothing. Uploads go in batches of up to 64, after
30 s idle; a failed batch is retried with backoff, a 4xx is dropped.

Every event carries, stamped at send time (`PhiTelemetry::SenderProperties`):
`$lib` = `phibrowser-win`, `$lib_version` / `$app_version` (the client
version), `$app_build`, `$app_namespace` (`com.phibrowser.win`, with the
channel spelled in for canary/debug builds, `com.phibrowser.canary.win`),
`$os` = `Windows`, `$os_version`, `$device_type` = `Desktop`, plus
`distinct_id` and `phi_module`.

## Identity

`distinct_id` is a random GUID generated once per user data directory
(`phi_telemetry_id`). Nothing ties it to an account; there is no `identify`
on Windows yet.

## Privacy contract

Properties never carry site-level or user-content data — no URLs, page
titles, search terms, Space names, profile names, agent process paths. The
allow-list in `client/phi_telemetry.cc` is the enforcement: a property not
fenced there for its module is dropped at egress and counted
(`scrubbedProperties` in `agentSpace.telemetry.status`). Adding a property
is two edits, the capture site and the fence.

## Events

Client-originated events are built in `client/phi_client_analytics.cc`; the
module is the allow-list row.

| Event | Module | Properties | Trigger |
| --- | --- | --- | --- |
| `Application Installed` | `app` | `version`, `build` | First launch over a user data directory (no `phi_last_version` file) |
| `Application Updated` | `app` | `version`, `build`, `previous_version` | A launch whose version differs from the one recorded at the previous launch |
| `Application Opened` | `app` | `from_background` (false at launch, true on a return to the foreground), `version`, `layout_mode` (`performance` / `balanced` / `comfortable`), `ai_enabled` (always false today), `is_guest_mode` (always false today) | Launch (`PhiCoordinator::LoadStores`, once the settings and the consent are in), and every `WM_ACTIVATEAPP` that brings Phi to the foreground |
| `Application Backgrounded` | `app` | — | `WM_ACTIVATEAPP` taking the foreground away from Phi (the process-wide edge, not per window) |
| `user_defaults_snapshot` | `settings` | `new_tab_behavior` (`ntp` / `omnibox`), `layout_mode`, `app_language`, `appearance` (`system` / `light` / `dark`), `is_phi_default_browser`, `peek_view_enabled`, `always_show_full_url`, `show_tab_previews`, `show_open_tab_indicators`, `dim_unloaded_tab_icons`, `restore_last_session_enabled`, `developer_mode_enabled`, `cdp_agent_access_enabled` (Developer ▸ "Allow agents to control Phi"), `agent_user_spaces_enabled`, `always_show_bookmark_bar` | Launch, right after `Application Opened`. The Mac's keys where the setting exists on Windows; the last two are Windows-only |
| `space_created` | `spaces` | `total_spaces`, `non_default_profile` | The user's own create form commits (`agentSpace.spaces.create` from an internal caller); an agent's create is not counted |
| `space_switched` | `spaces` | `total_spaces` | The user switches Space in a shell window — a pip, the Space menu, Ctrl+N (`PhiShellWindow::SwitchToSpace`); not restores, not agent-driven switches |
| `agent_task_started` | `agent` | `origin` (always `phi_agent`), `persistent`, `agent_name` | An agent Space's window is up (`HostAgentWindow`), and a re-bind to an existing task |
| `agent_task_completed` | `agent` | `origin`, `persistent`, `agent_name`, `success` | `agentSpace.complete` — the completion path only; a Space the user closes under a task is not a completion |
| `agent_user_space_command` | `agent` | `command` (the verb name), `agent_name` | A pipe caller's verb passes the capability gate into the user's own Spaces: one aimed at a user window, or an app-wide one needing `kUserSpaceRead`/`kUserSpaceWrite`. Never for the test-rig host |
| *Chromium-originated events* | `memory_saver`, `renderer_health` | see the registry | Captured in the browser core through `phi_analytics::Capture()`; inventoried in `chrome/browser/phinomenon/analytics/README.md` in the fork |

`agent_name` is the Mac's `AgentDriverBadge.telemetryName`: a known brand's
product label (`Claude Code`, `Codex`, `Pi`, …), `other` for a name the
badge resolver gives no mark to, `unknown` for none. The raw name is never
sent.

## Not ported yet

The Mac's AI sidebar, Kiosk, import, onboarding (`oobe_*`), first-time
action, credential-access and feature-entry events have no Windows
counterpart yet, either because the feature does not exist in this client
or because its capture site has not been added. Add the row here in the
same change that adds the capture site.

## Verifying on the box

```
node C:\Users\jdxin\phitest\tele-status.mjs     # endpoint, sent, lastStatus
```

`agentSpace.analytics.list` over the control pipe returns everything
captured this session (properties included, pre-scrub); PostHog's Activity
view for project 604357 shows what arrived. `/batch/` answers 200 even for
a wrong key, so the Activity view is the proof, not the status.
