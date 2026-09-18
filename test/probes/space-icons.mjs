// Seeds Spaces wearing a spread of catalogue icons (Fluent twins, type,
// mask-only) so a shot of the strip / Settings › Spaces checks §5.9's draw
// path. Runs ON the box.
import { openControl, sleep } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const windowId = windows[0]
const want = [
  ['Camera', 'phi:phi-icon-camera'], ['Work', 'phi:phi-icon-briefcase'],
  ['Cog', 'phi:phi-icon-cog'], ['R', 'phi:phi-icon-letter-r'],
  ['Three', 'phi:phi-icon-number-3'], ['Help', 'phi:phi-icon-question-mark'],
  ['Cake', 'phi:phi-icon-cake'], ['Beaker', 'phi:phi-icon-beaker'],
]
let { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
for (let i = 0; i < want.length; i++) {
  const [name, icon] = want[i]
  let space = spaces[i]
  if (!space) {
    const made = await ctl.send('agentSpace.spaces.create', { windowId, name })
    await sleep(300)
    spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    space = spaces[i] || spaces.find((s) => s.name === name)
  }
  console.log(name, icon, JSON.stringify(await ctl.send('agentSpace.spaces.update', { windowId, spaceId: space.id, name, iconName: icon })))
}
await ctl.send('agentSpace.spaces.activate', { windowId, spaceId: spaces[0].id }).catch(() => {})
await ctl.send('agentSpace.window.setBounds', { windowId, x: 69, y: 40, width: 1400, height: 900 })
ctl.close()
