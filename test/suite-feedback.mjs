// Feedback suite: the Mac's "Send Feedback to Phi" form and its outbox
// (UserInterface/Feedback/FeedbackOutbox.swift) on Windows. Drives the real
// form through the control socket — the same code path the Send button
// takes — and reads the outbox's manifests back through the status verb.
//
// Two outcomes, decided by whether the box has a real Auth0 session
// (agentSpace.account.status signedIn): WITH one the job must reach
// "submitted" — the report really lands on the staging account service
// (verified 2026-09-10, feedback id 19) — and the directory is gone;
// WITHOUT one the stand-in account files the job, everything up to the
// wire is asserted (the form's state, the outbox job, its attachment
// preparation — logs.zip + images + others.zip within the five-slot limit —
// and the backoff bookkeeping) and the upload fails for want of a token.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'
import { writeFileSync, mkdirSync } from 'node:fs'

const SCRATCH = 'C:\\phi-fx-test\\feedback-suite'

async function waitFor(ctl, predicate, timeoutMs = 20000) {
  const started = Date.now()
  for (;;) {
    const status = await ctl.send('agentSpace.feedback.status')
    if (predicate(status)) return status
    if (Date.now() - started > timeoutMs) return status
    await sleep(500)
  }
}

// The box has no Auth0 session: stand a test account in so Send can file
// its job (the upload then fails for want of a token, as asserted below).
// A driven Send while signed OUT is refused rather than prompting — the
// interactive sign-in box is modal and would wedge the control call.
test('feedback: a driven Send while signed out is refused, not prompted',
     async () => {
  const ctl = await openControl()
  try {
    const cleared = await ctl.send('agentSpace.feedback.setAccount', {})
    assertEq(cleared.ok, true, 'account cleared')
    if (cleared.signedIn) return  // A real session: nothing to refuse.
    await ctl.send('agentSpace.feedback.open')
    const r = await ctl.send('agentSpace.feedback.form',
                             { description: 'signed-out probe', send: true })
    assertEq(r.ok, false, 'refused')
    assertEq(r.error, 'no_account', 'because there is no account')
    const status = await ctl.send('agentSpace.feedback.status')
    assertEq(status.form.open, true, 'form still open, nothing filed')
    assertEq(status.form.submitting, false, 'not submitting')
  } finally { ctl.close() }
})

test('feedback: the form opens for the focused window with its tab context',
     async () => {
  const ctl = await openControl()
  try {
    await ctl.send('agentSpace.feedback.setAccount',
                   { userId: 'auth0|suite', email: 'suite@example.com' })
    const opened = await ctl.send('agentSpace.feedback.open')
    assertEq(opened.ok, true, 'form opened')
    const status = await ctl.send('agentSpace.feedback.status')
    assertEq(status.form.open, true, 'form reports open')
    assert(status.form.windowId > 0, 'bound to a browser window')
    assertEq(status.form.canSend, false, 'Send disabled with no description')
    // Reopening rebinds rather than stacking a second window.
    const again = await ctl.send('agentSpace.feedback.open')
    assertEq(again.hwnd, opened.hwnd, 'one app-global window')
  } finally { ctl.close() }
})

test('feedback: Send files a job with logs, images and files prepared',
     async () => {
  mkdirSync(SCRATCH, { recursive: true })
  const text = `${SCRATCH}\\notes.txt`
  writeFileSync(text, 'a note for the report\n'.repeat(100))
  const ctl = await openControl()
  try {
    // A real session uploads for real; the stand-in only files.
    const { signedIn } = await ctl.send('agentSpace.account.status')
    await ctl.send('agentSpace.feedback.open')
    const before = await ctl.send('agentSpace.feedback.status')
    const known = new Set(before.jobs.map(j => j.id))
    const driven = await ctl.send('agentSpace.feedback.form', {
      description: 'suite: the page rendered blank after a reload',
      url: 'https://example.com/suite',
      attachFiles: [text],
    })
    assertEq(driven.ok, true, 'form driven')
    let status = await ctl.send('agentSpace.feedback.status')
    assertEq(status.form.canSend, true, 'Send enabled once described')
    assertEq(status.form.attachments.length, 1, 'the file is listed')
    assertEq(status.form.attachments[0].image, false, 'as a file')

    const sent = await ctl.send('agentSpace.feedback.form', { send: true })
    assertEq(sent.ok, true, 'Send pressed')
    // The form turns into the "Thanks for the report" confirmation once the
    // job is filed (after the system logs arrive, or the 15 s wait for them
    // lapses).
    status = await waitFor(ctl, s => !s.form.open || s.form.sent, 25000)
    assert(!status.form.open || status.form.sent, 'form confirmed the send')
    const job = status.jobs.find(j => !known.has(j.id))
    assert(job, 'a new outbox job exists')
    assertEq(job.sourceFiles, 1, 'the picked file was copied in')
    assertEq(job.hasSystemLogs, true, 'Chromium system logs were collected')
    assertEq(job.metadata.page.url, 'https://example.com/suite', 'page url')
    assertEq(job.metadata.browser.name, 'Phi Browser', 'browser name')
    assert(job.metadata.extra.os_version.startsWith('Windows'), 'os version')
    assertEq(job.metadata.client_context.trace_id, job.id, 'trace id')
    assertEq(job.metadata.browser.version.length > 0, true, 'client version')

    // The worker prepares the archives before it goes on the wire. With a
    // real session the whole job can be through and gone in a few seconds,
    // so a vanished job is accepted there and checked below.
    status = await waitFor(ctl, s => {
      const j = s.jobs.find(x => x.id === job.id)
      return (signedIn && !j) || (j && j.prepared > 0 && j.status !== 'preparing')
    }, 60000)
    const prepared = status.jobs.find(x => x.id === job.id)
    if (prepared) {
      const types = prepared.preparedAttachments.map(a => `${a.type}:${a.filename}`)
      assert(types.includes('log:logs.zip'), `logs.zip prepared (${types})`)
      assert(types.includes('other:others.zip'), `others.zip prepared (${types})`)
      assert(prepared.prepared <= 5, 'within the five-attachment limit')
    } else {
      assert(signedIn, 'job still present (not discarded)')
    }
    if (signedIn) {
      // A real account: presign, upload, submit, and the job directory is
      // removed once the service has the report.
      status = await waitFor(ctl, s => !s.jobs.find(x => x.id === job.id), 90000)
      const left = status.jobs.find(x => x.id === job.id)
      assert(!left, `job submitted and removed (${left && left.status}: ${left && left.lastError})`)
    } else {
      // No token behind the stand-in account: the attempt fails and the
      // job waits on its backoff with the error recorded.
      status = await waitFor(ctl, s => {
        const j = s.jobs.find(x => x.id === job.id)
        return j && j.status === 'failedWaitingRetry'
      }, 90000)
      const failed = status.jobs.find(x => x.id === job.id)
      assertEq(failed.status, 'failedWaitingRetry', 'waiting on backoff')
      assertEq(failed.retryCount, 1, 'one attempt counted')
      assert(failed.nextAttemptAtMs > Date.now() - 1000, 'next attempt scheduled')
      assert(failed.lastError.length > 0, `error recorded: ${failed.lastError}`)
    }
  } finally { ctl.close() }
})

test('feedback: an unreadable file is refused with an alert-free error',
     async () => {
  const ctl = await openControl()
  try {
    await ctl.send('agentSpace.feedback.open')
    const r = await ctl.send('agentSpace.feedback.form', {
      attachFiles: [`${SCRATCH}\\does-not-exist.bin`],
    })
    // The form keeps running (the failure is reported in its state).
    assertEq(r.ok, true, 'driven')
    const status = await ctl.send('agentSpace.feedback.status')
    assertEq(status.form.attachments.length, 0, 'nothing attached')
  } finally { ctl.close() }
})

run()
