// theme-set.mjs <light|dark|system> [themeId]: switches the appearance.
import { openControl } from '../phi-test.mjs'
const [appearance, themeId] = process.argv.slice(2)
const ctl = await openControl()
try {
  const params = { appearance }
  if (themeId) params.themeId = themeId
  console.log(JSON.stringify(await ctl.send('agentSpace.theme.set', params)))
} finally { ctl.close() }
