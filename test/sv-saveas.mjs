// "Save Page As…" raises a Win32 shell dialog. Before the owner fix this
// DCHECK-crashed the browser (base_shell_dialog_win.cc:71 — the owner was
// Chromium's WS_CHILD HWND, not a root window). Clicks the menu item, then
// proves the process is still alive and answering.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
const { menus } = await ctl.send('agentSpace.menu.list', {})
const file = (menus || []).find(m => /^File/i.test(m.title || m.name || ''))
const items = (file && (file.items || file.children)) || []
const save = items.find(i => /Save Page/i.test(i.title || i.name || ''))
console.log('file menu items:', items.map(i => i.title || i.name).join(' | '))
console.log('save item:', JSON.stringify(save))
const title = save && (save.title || save.name)
const r = await ctl.send('agentSpace.menu.click', { path: [file.title || file.name, title] })
console.log('menu.click:', JSON.stringify(r))
await sleep(2500)
const l = await ctl.send('agentSpace.list', {})
console.log('alive after dialog:', JSON.stringify(l.windows))
ctl.close()
