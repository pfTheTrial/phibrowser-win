# Credentials — the user's password manager

Full semantics for `credentialStatus()`, `fillCredential()`,
`runWithCredential()`, and `getCredential()`. Read this before your FIRST
sign-in or secret-touching step of a task.

When a task needs to sign into a site, pull the login from the user's password
manager instead of asking them to paste it: `credentialStatus()` reports
readiness (`ready` / `locked` / `logged_out` / `not_installed`),
`fillCredential(target, domain, {field})` fills a login field directly,
`runWithCredential(domain, command, {env})` runs a command with the secret in
its environment, and `getCredential(domain, {fields})` returns the login.
`domain` is a bare host (`"github.com"`)
or `{domain|id|search}`; a domain query also takes a `username`
(`{domain: 'github.com', username: 'work@co'}`) to pick one of several
accounts on the same site.

The vault serves every standard item type, not just logins: secure notes,
cards, identities, and SSH keys. Domain queries reach logins only (they match
through the login's site URIs); use `{search: 'item name'}` or `{id}` for the
other types — `getCredential({search: 'wifi note'})` returns a secure note's
body as `notes`. Every served item carries `type` (`'login'` / `'note'` /
`'card'` / `'identity'` / `'sshKey'`) and `name`, and its default fields
follow the type: a note serves `notes`, a card serves
`cardholderName`/`brand`/`number`/`expMonth`/`expYear`/`code`, an identity its
name/address/contact fields (including `ssn`/`passportNumber`/
`licenseNumber`), an SSH key `privateKey`/`publicKey`/`fingerprint`. Only
logins can be filled into pages — `fillCredential` on another type fails with
`not_a_login`; other types reach a task through `getCredential` or
`runWithCredential` (e.g. `{env: {SSH_KEY: 'privateKey'}}`).

**Prefer the secret-free helpers — you never see the value.**

- Web form → `fillCredential(target, domain, {field})`. Phi fills the field
  itself — the value goes app → page and never reaches you; all you get back is
  `{filled: true, field, matches}`. `field` is `'password'` (default) or
  `'username'`. A typical login is two calls, then verify by re-observing:

  ```js
  await fillCredential('loc=css:#login', 'github.com', { field: 'username' })
  await fillCredential('loc=css:#password', 'github.com')
  await click('@7') // the Sign in button from observe()
  ```

  In-app fill is a Phi-side capability: on an older build without it,
  `fillCredential` throws with `autofill_not_available`. It will NOT fall back
  to fetching the secret into your context — a fill must never become a reveal.
  If a value genuinely has to enter your context, use `getCredential`. The
  fill lands on the field as it exists when the user approves — if the page
  navigates or re-renders while the approval prompt is up, the call fails
  cleanly (`target_not_found`); re-observe and retry. Password fills require a
  real `type=password` input, username fills a text/email input.
- CLI / API → `runWithCredential(domain, command, {env})`. Runs `command` (an
  argv array, no shell) with credential fields injected as environment
  variables — `{env: {PGPASSWORD: 'password'}}` maps variables to fields
  (username, password, uri, notes, domain, credentialId, plus the
  type-specific card/identity/SSH-key fields such as `number`, `ssn`,
  `privateKey`), or
  `{envAll: true}` injects all present fields as `PHI_CRED_<FIELD>`. Returns `{code, stdout, stderr, timedOut}` with secret
  values scrubbed to `•••` from the captured output:

  ```js
  const r = await runWithCredential('db.internal', ['psql', '-h', 'db.internal', '-U', 'app', '-c', 'select 1'],
                                    { env: { PGPASSWORD: 'password' } })
  ```

  The scrub catches an accidental echo, not a command that transforms the
  secret — only run commands you'd trust with the secret anyway.

Reach for `getCredential` only when the task genuinely needs the value in
your context (composing it into a config file, reading it out to the user) —
never just to fill a form or run a command.

**TOTP/2FA is deliberately not exposed** (`totp_not_supported`): releasing a
live 2FA code to an agent would collapse both factors behind one approval.
When a login hits a 2FA step, `handOff('Enter your 2FA code, then hand
back')` — that step is the user's.

**Fills are origin-bound.** `fillCredential` refuses with `origin_mismatch`
when the current page's host doesn't belong to the credential's site (equal
host or subdomain either way) — that mismatch is exactly how a misleading page
or injected instruction would exfiltrate a password. Don't work around it by
fetching with `getCredential` and filling manually; if the user confirmed the
page legitimately takes that login (an SSO portal), pass
`{allowCrossOrigin: true}`.

**Filled secrets don't ride back into your context.** Page scans report
password-type inputs as `•••` (never their contents — including passwords the
user typed themselves during a handoff), and every secret a fill or run
handled this round is scrubbed from everything the round prints, so a page
readback or error echo can't smuggle the value back to you. Don't try to read
a filled value back; verify a login by its outcome (the post-submit page).

**Ambiguity**: a served credential is always the query's unique match. When
several vault items fit, Phi releases nothing and the call throws `ambiguous`,
listing the candidate usernames — narrow with `{domain, username}` (or
`{id: credentialId}`) and call again, asking the user which account when it
isn't obvious from the task. Phi never picks an account on the user's behalf.

Every secret-touching call (fills and runs included) pops an approve/deny
prompt in Phi that names you and the site; the user may grant a 10-minute
remember for that site. Prompts are typed by exposure — a browser fill, a
command-env injection, or revealing the raw value to you — and a remembered
grant covers only the kind it was approved for (a fill-only grant never
authorizes `getCredential`; a full-access grant covers everything), so a
`user_denied` on getCredential can follow an approved fill: that's the user
declining the ESCALATION, not the task. The prompt also shows a purpose line —
`fillCredential`/`runWithCredential` compose it automatically; pass
`{purpose: '…'}` to `getCredential` so the user sees why you need the value. On denial the call throws `user_denied` — surface that and stop,
don't retry. If status is `locked` or `logged_out`, tell the user to
unlock/sign in from Settings ▸ General ▸ Bitwarden rather than looping. On
`not_found`, retry a non-login item as `{search: 'its name'}` (domain queries
reach logins only), and confirm the name/domain with the user — the item may
be stored under a different one.

Secrets returned by `getCredential` enter your context (transcript, logs), so
request only the fields you need (`{fields: ['username','password']}`) and
never echo the values back. This
surface is behind the same Agent-permissions toggle as browser management
(see `references/management.md`).
