# Challenges and consent banners — detail

Deep semantics behind SKILL.md's two page-gate rules: Cloudflare challenges
(hand off on first sight) and cookie-consent banners (auto-dismissed by
`goto`/`openTab`). Read this when a challenge appears or a banner survives
the automatic pass.

## Cloudflare challenges

A `goto`/`openTab` that lands on "Just a moment…", or an `observe()` that
returns a near-empty page whose one iframe is `crossOrigin: true` from
`challenges.cloudflare.com`, is a Cloudflare challenge. Confirm with
`detectChallenge()` → `null` or `{vendor, kind, url, title}` where `kind` is
`interstitial` (full-page gate), `turnstile` (widget embedded in a normal
page, e.g. a login form), or `blocked` (a hard block/error page).

A challenge is the USER's step from the moment it appears: hand off the
FIRST time you see one. Do not try to pass it as the agent — no waiting it
out, no reloading or re-navigating, and NEVER an attempt to solve it (no
clicking the checkbox, no `js()` into the widget: it lives in a cross-origin
iframe and scores exactly the kind of input automation produces).

```js
const ch = await detectChallenge()
if (ch && ch.kind !== 'blocked') {
  await handOff('Cloudflare wants a human check on example.com — ' +
                'complete the verification, then click "Hand back".')
  cliLog({ handedOff: true, challenge: ch })
  return
}
```

Then end the round, start the hand-back watcher (see SKILL.md ▸ "Hand-back
watcher"), and tell the user in chat. When the watcher fires, re-check
`detectChallenge()` and re-observe before continuing — passing the challenge
reloads onto the real page, so refs from before it are gone. Expect repeats:
clearance can be per-path, so a later navigation on the same site may
challenge again — each new challenge gets the same handoff, never an
agent-side retry.

`kind: 'blocked'` has nothing for the user to click either: report it and
ask how to proceed instead of handing off, and do not retry the navigation.

## Cookie-consent banners

`goto()` and `openTab()` automatically run a **static rule set** that dismisses
the common cookie/GDPR banners before returning — a per-CMP accept-all selector
table (OneTrust, Didomi, Cookiebot, Quantcast, Usercentrics, TrustArc, Osano,
Iubenda, …), then per-CMP **close** controls for notice-only banners that ship
no accept control at all (the CCPA OneTrust variant: "Cookie Settings" + ✕
only), matched against the top document and same-origin frames. It is
deterministic: no observe, no screenshot, no model turn. Because banners are
usually injected a beat after load on a first visit, the pass polls briefly for
one to surface — clicking the instant a matching control appears, waiting ~1.2s
when nothing consent-like is present yet, and extending (to ~3s) once a banner
is spotted still rendering — so most of the time it is already gone by the time
you look. Opt out per call with `{acceptCookies: false}` (e.g. to test the
banner yourself); tune the wait with `{acceptCookies: {waitMs: 8000}}`.

When a banner is still up — an unlisted CMP, a late injection, or one that needs
the text pass — call `acceptCookies()` yourself. It re-runs the selector tiers
**plus** guarded text heuristics: a visible control whose exact label is an
accept phrase (several languages) inside a consent-looking container — never a
Reject/Manage/Settings control — and, failing that, an explicit Close/✕-labeled
control in the same kind of container. It returns:

- `{clicked: true, rule, text}` — done; re-observe and continue.
- `{clicked: false, reason: 'cross-origin-frame', frameSrc}` — the CMP is in a
  cross-origin iframe page JS can't reach (e.g. Sourcepoint). Fall back to
  `annotatedScreenshot()` + `click(x, y)` on the accept button.
- `{clicked: false, reason: 'none', pending}` — nothing clicked; `pending: true`
  means a consent-looking box is present but no accept control matched, so
  observe and click it yourself.

Why accept rather than dismiss: the banner usually intercepts pointer events for
the whole page, so a later `click`/`fillInput` lands on the overlay; dismissing
without choosing tends to re-prompt on every navigation; and accepting persists
consent + session cookies into the shared profile, so later navigations and
rounds start warm instead of cold — fewer repeated gates and friendlier bot
scoring. Close controls are therefore tried only AFTER both accept tiers found
nothing — the case of notice-only banners, where closing IS the intended
dismissal (and the vendor persists it, e.g. OneTrust's OptanonAlertBoxClosed).

Distinguish a routine cookie notice (let the rules accept it and move on) from a
genuinely consequential choice — a login, a paywall, a purchase, or an
account-level privacy setting. Don't click those through on the user's behalf;
hand off or ask. A plain "we use cookies" notice is not one of them.
