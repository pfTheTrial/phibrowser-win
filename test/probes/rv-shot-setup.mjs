// Stages the window for a screenshot of the reader BUTTON and prints where to
// click it. The button only exists in the performance layout's address
// capsule, trailing the copy-link and extensions buttons: from the field's
// right edge, 2pt inset, 24pt extensions, 2pt, 24pt copy-link, 2pt, 24pt
// reader (ComputeLayout). Coordinates come back in the shell's CLIENT space,
// which is what the capture graft's `click,x,y` takes.
import { openControl, sleep } from '../phi-test.mjs'

const url = process.argv[2] || 'https://en.wikipedia.org/wiki/Readability'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]

await ctl.send('agentSpace.window.setBounds', { windowId: w, x: 60, y: 40, width: 1700, height: 1040 })
await sleep(600)
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Performance'] })
await sleep(800)

let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(300); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url })

// Wait for the button to actually be allocated rather than guessing at a
// settle time: the probe reports at document_idle and upstream's
// distillability verdict lands after it, and a shot taken between the two
// catches a capsule with no reader button in it.
let l = null
for (let i = 0; i < 25; i++) {
  await sleep(1000)
  l = await ctl.send('agentSpace.window.layout', { windowId: w })
  if (l.reader && l.reader.width) break
}
console.log('mode', l.mode, 'dpi', l.dpi, 'urlField', JSON.stringify(l.urlField))
console.log('probe', l.readerProbe, 'distillable', l.readerDistillable,
            'reader', JSON.stringify(l.reader))
if (l.reader && l.reader.width) {
  console.log('READER', `${Math.round(l.reader.x + l.reader.width / 2)},${Math.round(l.reader.y + l.reader.height / 2)}`)
} else {
  console.log('READER none')
}
ctl.close()
process.exit(0)
