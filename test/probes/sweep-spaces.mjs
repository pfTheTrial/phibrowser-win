// One-off maintenance: list (and with --delete, remove) accumulated
// test-created Spaces. Keeps the active space and anything not matching the
// junk name patterns. Usage: node sweep-spaces.mjs [--delete]
import { openControl } from '../phi-test.mjs'

const JUNK = /^(Work|Research|Play|Second|Renamed.*|Work Space|Menu Space.*|Temp.*|Test.*|Seq [AB]|Shared [AB]|Closable|GapCheck|Persisted \d+)$/

const doDelete = process.argv.includes('--delete')
const ctl = await openControl()
try {
  const { windows } = await ctl.send('agentSpace.list')
  const windowId = windows[0]
  const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
  const counts = new Map()
  for (const s of spaces) counts.set(s.name, (counts.get(s.name) || 0) + 1)
  const act = spaces.find((s) => s.active)
  console.log(`total spaces: ${spaces.length}; active: ${JSON.stringify(act && act.name)}`)
  for (const [name, n] of [...counts.entries()].sort((a, b) => b[1] - a[1])) {
    const junk = JUNK.test(name) ? 'JUNK' : 'keep'
    console.log(`${String(n).padStart(4)}  ${junk}  ${JSON.stringify(name)}`)
  }
  if (!doDelete) process.exit(0)

  let removed = 0, failed = 0
  for (const s of spaces) {
    if (s.active || !JUNK.test(s.name)) continue
    const r = await ctl.send('agentSpace.spaces.delete', { spaceId: s.id })
    if (r.ok) removed++
    else { failed++; if (failed <= 5) console.log(`refused: ${s.name}: ${r.error || ''}`) }
  }
  const after = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
  console.log(`removed ${removed}, refused ${failed}, remaining ${after.length}`)
} finally { ctl.close() }
