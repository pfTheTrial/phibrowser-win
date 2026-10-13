// Static http server for pk-pages/ on 127.0.0.1:8765. Reader View's
// distillability verdict only fires on http(s), so the pk-split cases serve
// their article pages instead of loading them from file:. Launch it outside
// the ssh session's job (pk.mjs `serve` does it through WMI) so it outlives
// the ssh call; pk.mjs `unserve` stops it.
import { createServer } from 'node:http'
import { readFile } from 'node:fs/promises'
import { join, dirname, normalize } from 'node:path'
import { fileURLToPath } from 'node:url'
const ROOT = join(dirname(fileURLToPath(import.meta.url)), 'pk-pages')
createServer(async (req, res) => {
  const rel = normalize(decodeURIComponent(req.url.split('?')[0])).replace(/^[\\/]+/, '')
  try {
    const body = await readFile(join(ROOT, rel || 'left.html'))
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' })
    res.end(body)
  } catch {
    res.writeHead(404); res.end('not found')
  }
}).listen(8765, '127.0.0.1')
