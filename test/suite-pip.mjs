// Native PiP integration: run against the matching framework + client build.
// All content is generated locally; the suite closes only its own tabs.
import fs from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import { pathToFileURL } from 'node:url'
import { test, run, assert, assertEq, sleep, openControl, openCdp, cdpHttp } from './phi-test.mjs'
import { firstWindow } from './lib/window.mjs'
import { waitFor } from './lib/wait.mjs'

async function withPage(body) {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), 'phi-pip-'))
  const marker = path.basename(dir)
  const file = path.join(dir, 'index.html')
  await fs.writeFile(file, `<!doctype html><title>${marker}</title>
    <canvas id="canvas" width="640" height="360"></canvas>
    <video id="video" controls loop width="640" height="360"></video>
    <script>
      const ctx = canvas.getContext('2d');
      setInterval(() => {
        ctx.fillStyle = '#245782'; ctx.fillRect(0, 0, 640, 360);
        ctx.fillStyle = 'white'; ctx.font = '32px sans-serif';
        ctx.fillText('Phi Picture in Picture ' + Date.now(), 20, 180);
      }, 40);
      window.prepareVideo = async () => {
        video.srcObject = canvas.captureStream(25);
        video.muted = true;
        await video.play();
      };
    </script>`)
  const ctl = await openControl()
  let page
  let windowId
  try {
    windowId = await firstWindow(ctl)
    const url = pathToFileURL(file).href
    assertEq((await ctl.send('agentSpace.tabs.new', { windowId, url })).ok, true, 'created fixture tab')
    const target = await waitFor(async () => {
      const targets = await cdpHttp('/json/list')
      return targets.find(t => t.url === url)
    }, { what: 'PiP fixture target' })
    page = await openCdp(new URL(target.webSocketDebuggerUrl).pathname)
    const evaluate = async expression => {
      const result = await page.send('Runtime.evaluate', {
        expression, awaitPromise: true, returnByValue: true, userGesture: true,
      })
      if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails))
      return result.result.value
    }
    await waitFor(async () => await evaluate('typeof prepareVideo === "function"'), { what: 'fixture loaded' })
    await body({ ctl, windowId, evaluate, marker })
  } finally {
    if (page) {
      try { await page.send('Runtime.evaluate', { expression: 'window.pip?.close(); if(document.pictureInPictureElement) document.exitPictureInPicture();' }) } catch {}
      page.close()
    }
    if (windowId !== undefined) {
      const { tabs = [] } = await ctl.send('agentSpace.tabs.list', { windowId })
      for (const tab of tabs.filter(t => t.title.startsWith(marker)).reverse()) {
        await ctl.send('agentSpace.tabs.close', { windowId, index: tab.index })
      }
    }
    ctl.close()
    await fs.rm(dir, { recursive: true, force: true })
  }
}

test('PiP: document opens independently of the native tab shell and can reopen', async () => {
  await withPage(async ({ ctl, evaluate }) => {
    const before = (await ctl.send('agentSpace.list')).windows.length
    for (let i = 0; i < 2; ++i) {
      assert(await evaluate(`(async () => {
        window.pip = await documentPictureInPicture.requestWindow({width: 400, height: 240});
        pip.document.body.textContent = 'Floating Phi document';
        return !pip.closed && pip !== window;
      })()`), 'document PiP is open')
      assertEq((await ctl.send('agentSpace.list')).windows.length, before,
               'document PiP must not create a browser shell')
      assert(await evaluate('!pip.closed && pip.innerWidth > 0'), 'floating document remains alive')
      await evaluate('pip.close()')
      await waitFor(async () => await evaluate('pip.closed'), { what: 'document PiP closed' })
    }
  })
})

test('PiP: video enters and exits a real native overlay', async () => {
  await withPage(async ({ ctl, evaluate }) => {
    const before = (await ctl.send('agentSpace.list')).windows.length
    await evaluate('prepareVideo()')
    assert(await evaluate(`(async () => {
      const pip = await video.requestPictureInPicture();
      return document.pictureInPictureElement === video && pip.width > 0;
    })()`), 'video PiP is open')
    assertEq((await ctl.send('agentSpace.list')).windows.length, before,
             'video overlay must not create a browser shell')
    await evaluate('document.exitPictureInPicture()')
    assert(await evaluate('document.pictureInPictureElement === null'), 'video returned to its tab')
  })
})

test('PiP: playing video pops out on tab switch and returns with its tab', async () => {
  await withPage(async ({ ctl, windowId, evaluate, marker }) => {
    // Record local canvas + oscillator tracks into an ordinary video. A live
    // captureStream alone has no normal media player/audio focus to auto-PiP.
    await evaluate(`(async () => {
      const audio = new AudioContext();
      const oscillator = audio.createOscillator();
      const destination = audio.createMediaStreamDestination();
      oscillator.connect(destination); oscillator.start();
      const stream = canvas.captureStream(25);
      stream.addTrack(destination.stream.getAudioTracks()[0]);
      const recorder = new MediaRecorder(stream, {mimeType: 'video/webm'});
      const chunks = [];
      recorder.ondataavailable = e => chunks.push(e.data);
      const stopped = new Promise(resolve => recorder.onstop = resolve);
      recorder.start();
      await new Promise(resolve => setTimeout(resolve, 1200));
      recorder.stop(); await stopped;
      stream.getTracks().forEach(track => track.stop());
      await audio.close();
      video.src = URL.createObjectURL(new Blob(chunks, {type: 'video/webm'}));
      video.volume = 0.1;
      navigator.mediaSession.setActionHandler('enterpictureinpicture', () => {
        video.requestPictureInPicture().catch(error => window.pipError = String(error));
      });
      await video.play();
    })()`)
    await waitFor(async () => await evaluate('!video.paused && video.currentTime > 0'), { what: 'audible video playing' })
    await sleep(1500)
    assertEq((await ctl.send('agentSpace.tabs.new', {
      windowId, url: 'data:text/html,<title>' + marker + '-away</title>',
    })).ok, true, 'switched away')
    await waitFor(async () => await evaluate('document.pictureInPictureElement === video'),
                  { timeout: 15000, what: 'automatic video PiP (Normal setting required)' })
    const { tabs } = await ctl.send('agentSpace.tabs.list', { windowId })
    const source = tabs.find(tab => tab.title === marker)
    assert(source, 'source tab still exists')
    assertEq((await ctl.send('agentSpace.tabs.activate', { windowId, index: source.index })).ok, true, 'returned to source')
    await waitFor(async () => await evaluate('document.pictureInPictureElement === null'),
                  { what: 'automatic PiP returned to source' })
    await evaluate('video.pause()')
  })
})

run()
