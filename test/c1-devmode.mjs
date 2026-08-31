import { openControl } from './phi-test.mjs'
const ctl = await openControl()
const r = await ctl.send('agentSpace.settings.set', { developerMode: true })
console.log('devmode set:', r.ok !== false)
const o = await ctl.send('agentSpace.settings.open')
console.log('open:', o.ok)
const s = await ctl.send('agentSpace.settings.set', { section: 'developer' })
console.log('section developer:', s.ok)
ctl.close()
