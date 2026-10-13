// Web Speech voice list + speak check (the fork's PhiEnumAllVoiceTokens:
// OneCore voices plus every SAPI voice that is not a " Desktop" twin).
// Run on the box from PHI_TEST_DIR: node probes\tts-voices.mjs <page-url> [voice name...]
// Prints the voices the page sees, then speaks "hello" with each named voice
// (userGesture evaluate) and prints the start/end/error events it got.
import { cdpHttp, openCdp, sleep } from '../phi-test.mjs'

const [pageUrl, ...speakNames] = process.argv.slice(2)
const list = await cdpHttp('/json/list')
const target = list.find((t) => t.type === 'page' && t.url.startsWith(pageUrl))
if (!target) { console.log('no target for', pageUrl, list.map((t) => t.url)); process.exit(1) }
const ws = new URL(target.webSocketDebuggerUrl).pathname
const cdp = await openCdp(ws)
const evaluate = async (expression) => {
  const r = await cdp.send('Runtime.evaluate',
                           { expression, awaitPromise: true, returnByValue: true, userGesture: true })
  if (r.exceptionDetails) return 'EXCEPTION ' + JSON.stringify(r.exceptionDetails).slice(0, 300)
  return r.result.value
}
const voices = await evaluate(`new Promise((resolve) => {
  let tries = 0
  const read = () => speechSynthesis.getVoices().map((v) => v.name + '|' + v.lang + '|' + v.localService + '|' + v.default)
  const tick = () => { const v = read(); if (v.length || ++tries > 20) resolve(v); else setTimeout(tick, 250) }
  speechSynthesis.addEventListener('voiceschanged', () => {}, { once: true })
  tick()
})`)
console.log('VOICES', voices.length)
for (const v of voices) console.log('  ' + v)
for (const name of speakNames) {
  const out = await evaluate(`new Promise((resolve) => {
    const voice = speechSynthesis.getVoices().find((v) => v.name === ${JSON.stringify(name)})
    if (!voice) return resolve('no such voice')
    const ev = []; const t0 = performance.now()
    const u = new SpeechSynthesisUtterance('hello')
    u.voice = voice; u.lang = voice.lang
    const stamp = (e) => ev.push(e.type + '@' + Math.round(performance.now() - t0) + (e.error ? ':' + e.error : ''))
    u.onstart = stamp; u.onerror = (e) => { stamp(e); resolve(ev.join(' ')) }
    u.onend = (e) => { stamp(e); resolve(ev.join(' ')) }
    speechSynthesis.cancel(); speechSynthesis.speak(u)
    setTimeout(() => resolve(ev.join(' ') + ' TIMEOUT'), 15000)
  })`)
  console.log('SPEAK', name, '->', out)
  await sleep(500)
}
cdp.close()
process.exit(0)
