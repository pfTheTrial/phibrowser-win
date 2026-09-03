// Feedback suite: the Mac's "Send Feedback to Phi" form and its outbox
// (UserInterface/Feedback/FeedbackOutbox.swift) on Windows. Drives the real
// form through the control socket — the same code path the Send button
// takes — and reads the outbox's manifests back through the status verb.
//
// Network-free by design: the account service is not reachable from the
// test box's dev account, so a job is expected to reach "failedWaitingRetry"
// (prepared, could not upload) rather than "submitted". What IS asserted is
// everything up to the wire: the form's state, the outbox job, its
// attachment preparation (logs.zip + images + others.zip within the five-slot
// limit) and the backoff bookkeeping.
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
    // The form closes once the job is filed (after the system logs arrive,
    // or the 15 s wait for them lapses).
    status = await waitFor(ctl, s => !s.form.open, 25000)
    assertEq(status.form.open, false, 'form closed after Send')
    const job = status.jobs.find(j => !known.has(j.id))
    assert(job, 'a new outbox job exists')
    assertEq(job.sourceFiles, 1, 'the picked file was copied in')
    assertEq(job.hasSystemLogs, true, 'Chromium system logs were collected')
    assertEq(job.metadata.page.url, 'https://example.com/suite', 'page url')
    assertEq(job.metadata.browser.name, 'Phi Browser', 'browser name')
    assert(job.metadata.extra.os_version.startsWith('Windows'), 'os version')
    assertEq(job.metadata.client_context.trace_id, job.id, 'trace id')
    assertEq(job.metadata.browser.version.length > 0, true, 'client version')

    // The worker prepares the archives before it can fail on the wire.
    status = await waitFor(ctl, s => {
      const j = s.jobs.find(x => x.id === job.id)
      return j && j.prepared > 0 && j.status !== 'preparing'
    }, 60000)
    const prepared = status.jobs.find(x => x.id === job.id)
    assert(prepared, 'job still present (not discarded)')
    const types = prepared.preparedAttachments.map(a => `${a.type}:${a.filename}`)
    assert(types.includes('log:logs.zip'), `logs.zip prepared (${types})`)
    assert(types.includes('other:others.zip'), `others.zip prepared (${types})`)
    assert(prepared.prepared <= 5, 'within the five-attachment limit')
    // No reachable account service on the box: the attempt fails and the
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
