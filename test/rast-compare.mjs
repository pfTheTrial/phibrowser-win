// Diffs two directories of PNGs shot by rast-shot.ps1 — the "before" and
// "after" of the Direct2D rasteriser swap in client/phi_dwrite.cc
// (docs/architecture-review.md §3.6, phase 5).
//
//   node rast-compare.mjs <before-dir> <after-dir> [--max N] [--verbose]
//
// Exit 0 when every pair matches within the tolerance, 1 otherwise. The
// tolerance matters: the two rasterisers are NOT expected to be identical.
// A pixel whose centre sits on an arc is weighed differently by each — the
// software field takes 0.5 minus its signed distance, Direct2D integrates
// the covered area — so a curve's antialiased edge moves by up to about 20
// of 255 levels per channel, and nothing else moves at all. What this tool
// is really looking for is the shapes of the failures:
//
//   * differences on STRAIGHT runs, or in a shape's interior — the geometry
//     handed to Direct2D is wrong (an off-by-half in the stroke inset, a
//     radius passed as a diameter);
//   * a whole shape gone, or a hard-edged rectangle where a curve was — the
//     alpha channel is not surviving Direct2D's hand-back to GDI, and the
//     probe in phi_dwrite.cc's ShapeTarget() should have caught it;
//   * dashes at different POSITIONS around a ring — expected and harmless
//     (Direct2D starts its pattern where its geometry's figure starts), but
//     worth eyeballing once;
//   * anything at all outside a curve's edge — not this change.
//
// Run it on the host or on the box; it needs nothing but node.

import { readFileSync, readdirSync, existsSync } from 'node:fs'
import { inflateSync } from 'node:zlib'
import path from 'node:path'

const args = process.argv.slice(2)

function argValue(flag) {
  const i = args.indexOf(flag)
  return i >= 0 ? args[i + 1] : null
}

const maxDelta = Number(argValue('--max') ?? 24)
const verbose = args.includes('--verbose')
// The two positional directories: every flag and, for --max, its value.
const dirs = args.filter((a, i) => !a.startsWith('--') &&
                                   args[i - 1] !== '--max')

if (dirs.length !== 2) {
  console.error('usage: node rast-compare.mjs <before-dir> <after-dir> ' +
                '[--max N] [--verbose]')
  process.exit(2)
}

/** Decodes a non-interlaced 8-bit RGB/RGBA PNG to {width, height, rgba}. */
function decodePng(file) {
  const buf = readFileSync(file)
  if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error(`${file}: not a PNG`)
  let at = 8
  let width = 0
  let height = 0
  let channels = 0
  const idat = []
  while (at < buf.length) {
    const length = buf.readUInt32BE(at)
    const type = buf.toString('ascii', at + 4, at + 8)
    const body = buf.subarray(at + 8, at + 8 + length)
    at += 12 + length
    if (type === 'IHDR') {
      width = body.readUInt32BE(0)
      height = body.readUInt32BE(4)
      const depth = body[8]
      const color = body[9]
      if (depth !== 8 || (color !== 2 && color !== 6)) {
        throw new Error(`${file}: unsupported PNG (depth ${depth}, ` +
                        `colour ${color}); re-shoot with rast-shot.ps1`)
      }
      if (body[12] !== 0) throw new Error(`${file}: interlaced PNG`)
      channels = color === 6 ? 4 : 3
    } else if (type === 'IDAT') {
      idat.push(body)
    } else if (type === 'IEND') {
      break
    }
  }
  const raw = inflateSync(Buffer.concat(idat))
  const stride = width * channels
  const out = Buffer.alloc(width * height * 4)
  let src = 0
  const prior = Buffer.alloc(stride)
  const line = Buffer.alloc(stride)
  for (let y = 0; y < height; ++y) {
    const filter = raw[src++]
    raw.copy(line, 0, src, src + stride)
    src += stride
    for (let i = 0; i < stride; ++i) {
      const a = i >= channels ? line[i - channels] : 0
      const b = prior[i]
      const c = i >= channels ? prior[i - channels] : 0
      let add = 0
      if (filter === 1) add = a
      else if (filter === 2) add = b
      else if (filter === 3) add = (a + b) >> 1
      else if (filter === 4) {
        const p = a + b - c
        const pa = Math.abs(p - a)
        const pb = Math.abs(p - b)
        const pc = Math.abs(p - c)
        add = pa <= pb && pa <= pc ? a : pb <= pc ? b : c
      }
      line[i] = (line[i] + add) & 0xff
    }
    line.copy(prior)
    for (let x = 0; x < width; ++x) {
      const from = x * channels
      const to = (y * width + x) * 4
      out[to] = line[from]
      out[to + 1] = line[from + 1]
      out[to + 2] = line[from + 2]
      out[to + 3] = channels === 4 ? line[from + 3] : 255
    }
  }
  return { width, height, rgba: out }
}

function compare(before, after) {
  if (before.width !== after.width || before.height !== after.height) {
    return { sizeMismatch: true }
  }
  let changed = 0
  let worst = 0
  let box = null
  const histogram = new Map()
  for (let y = 0; y < before.height; ++y) {
    for (let x = 0; x < before.width; ++x) {
      const at = (y * before.width + x) * 4
      let delta = 0
      for (let ch = 0; ch < 4; ++ch) {
        delta = Math.max(delta, Math.abs(before.rgba[at + ch] -
                                         after.rgba[at + ch]))
      }
      if (delta === 0) continue
      ++changed
      worst = Math.max(worst, delta)
      const bucket = delta <= 8 ? '1-8' : delta <= 24 ? '9-24'
                   : delta <= 64 ? '25-64' : '65+'
      histogram.set(bucket, (histogram.get(bucket) ?? 0) + 1)
      box = box
        ? [Math.min(box[0], x), Math.min(box[1], y),
           Math.max(box[2], x), Math.max(box[3], y)]
        : [x, y, x, y]
    }
  }
  const total = before.width * before.height
  return { changed, worst, box, histogram, total }
}

let failures = 0
const [beforeDir, afterDir] = dirs
const names = readdirSync(beforeDir).filter((n) => n.endsWith('.png')).sort()
if (!names.length) {
  console.error(`no PNGs in ${beforeDir}`)
  process.exit(2)
}
for (const name of names) {
  const other = path.join(afterDir, name)
  if (!existsSync(other)) {
    console.log(`${name}: MISSING in ${afterDir}`)
    ++failures
    continue
  }
  const result = compare(decodePng(path.join(beforeDir, name)),
                         decodePng(other))
  if (result.sizeMismatch) {
    console.log(`${name}: size differs — the window state was not restored`)
    ++failures
    continue
  }
  const { changed, worst, box, histogram, total } = result
  if (!changed) {
    console.log(`${name}: identical`)
    continue
  }
  const share = ((changed / total) * 100).toFixed(2)
  const spread = [...histogram.entries()]
    .sort((a, b) => a[0].localeCompare(b[0]))
    .map(([k, v]) => `${k}:${v}`).join(' ')
  const over = worst > maxDelta
  console.log(`${name}: ${changed} px (${share}%) worst ${worst} ` +
              `[${spread}] bbox ${box.join(',')}${over ? '  OVER' : ''}`)
  if (over) ++failures
  if (verbose && changed) {
    console.log('    every changed pixel should sit on the antialiased edge ' +
                'of a curve — check the bbox against a corner, a pill or a ' +
                'ring, not a straight run')
  }
}
if (failures) {
  console.log(`${failures} surface(s) past the tolerance (--max ${maxDelta})`)
  process.exit(1)
}
console.log('all surfaces within tolerance')
