// Writes the demos' replays as animated SVGs for places that show only an image, such
// as the README on GitHub: no script runs there, but CSS animation does.
//   docs/assets/casa-demo.svg      Cas A, large: the full player with its run control and narration
//   docs/assets/demos/<id>.svg     every recorded demo as a tile: the gallery's compact player
//   node docs/site/player/svg.mjs [<id> …]        (default: all of them)
// Every frame of the player's replay (framesOf in web/src/ui/demoPlayer.tsx) is drawn
// by the player's own Canvas and RunControl with React's static renderer, from the same
// player-data/<id>.json; the SVG holds that HTML in a <foreignObject> with the
// player's own stylesheet (site/player.css). The Cards, wires, output frames and the
// run control are kept once per distinct drawing, each shown during the frames that
// drew it. Images come from player-data/assets/ as data: URIs (a tile's shrunk with
// sips, macOS, as harvest.py does). The stylesheet keeps only the rules the drawing uses.
// Needs what build.mjs needs (web/node_modules, web/src/gen), and its outputs up to date.
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const SITE = path.dirname(HERE)
const ROOT = path.resolve(SITE, '..', '..')
const WEB = path.join(ROOT, 'web')
const DATA = path.join(SITE, 'player-data')
const OUT = path.join(ROOT, 'docs', 'assets')
const require = createRequire(path.join(WEB, 'package.json'))
const esbuild = require('esbuild')

/** The large drawing's size: the canvas as the docs site shows it, and the card around it. */
const W = 1200, CANVAS_H = 700, PAD = 24, HEAD = 64, SAY = 66, FOOT = 26
const H = PAD + HEAD + CANVAS_H + SAY + FOOT + PAD
/** A tile's size (two or three to a row of the README), and the long side of its images in pixels. */
const TILE = { width: 440, height: 264 }, TILE_IMAGE = 200
/** How long a replay holds before it starts and after it ends, in ms. */
const HOLD = { large: [1600, 5200], tile: [900, 3400] }
/** The demo shown large. */
const LARGE = 'casa'
/** The README cards' colors (docs/assets/docs-card.svg). */
const CARD_BG = '#16282d', CREAM = '#f3ede2', MUTED = '#c3cbc8'

// ---- The player's own components, rendered without a browser --------------------------

const entry = `
import { createElement as h } from 'react'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { Canvas, RunControl, framesOf, IDLE, DONE, provenance } from './demoPlayer'
export { framesOf, IDLE, DONE, provenance }
export function surface(data, frame, base, view, compact) {
  const playing = frame.mode === 'playing'
  return renderToStaticMarkup(h(Canvas, { data, frame, base, compact, onSelect: () => undefined, motion: true, view },
    compact ? null : h('div', { className: 'tp-top' }, h(RunControl, { data, frame, playing, onRun: () => undefined, onStop: () => undefined }))))
}
`
const bundle = await esbuild.build({
  stdin: { contents: entry, resolveDir: path.join(WEB, 'src', 'ui'), loader: 'tsx', sourcefile: 'svg-entry.tsx' },
  bundle: true, platform: 'node', format: 'esm', target: 'node22', write: false, jsx: 'automatic',
  nodePaths: [path.join(WEB, 'node_modules')], absWorkingDir: WEB, tsconfig: path.join(WEB, 'tsconfig.json'),
  define: { 'process.env.NODE_ENV': '"production"' }, logLevel: 'warning',
  // The Web UI's settings (model/prefs.ts) are read when its modules load: the browser globals they touch.
  banner: { js: `globalThis.window ??= globalThis; globalThis.document ??= { documentElement: { dataset: {} } };
Object.defineProperty(globalThis, 'localStorage', { value: { getItem: () => null, setItem() {}, removeItem() {} }, configurable: true });` },
})
const tmp = path.join(fs.mkdtempSync(path.join(process.env.TMPDIR ?? '/tmp', 'tsg-svg-')), 'entry.mjs')
fs.writeFileSync(tmp, bundle.outputFiles[0].text)
const player = await import(pathToFileURL(tmp).href)
fs.rmSync(path.dirname(tmp), { recursive: true, force: true })

// ---- HTML to the XML a foreignObject needs, and its top-level elements ---------------------

const VOID = /<(area|base|br|col|embed|hr|img|input|link|meta|source|track|wbr)\b((?:[^>"']|"[^"]*"|'[^']*')*?)\s*\/?>/g
// React writes <svg> without its namespace, which HTML implies and XML does not.
// What an image never shows (labels for assistive technology and tooltips) is left out: the <svg> has its own title.
const xhtml = (html) => html.replace(/ (?:aria-[a-z]+|title|role|data-guide)="[^"]*"/g, '').replace(VOID, '<$1$2/>').replace(/&nbsp;/g, '&#160;').replace(/<svg(?=[\s>])(?![^>]*xmlns=)/g, '<svg xmlns="http://www.w3.org/2000/svg"')
/** Numbers in attributes (sizes, places, paths) to the hundredth of a pixel, or the tenth in a tile: the geometry's doubles are 17 digits. */
const rounded = (xml, digits) => xml.replace(/="[^"]*"/g, (attr) => (attr.startsWith('="data:') ? attr
  : attr.replace(/-?\d+\.\d+/g, (n) => String(Number(Number(n).toFixed(digits))))))
const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')

/** Splits well-formed markup into its root's opening tag, its children (each as markup) and its closing tag. */
function split(xml) {
  const tags = /<\/?[a-zA-Z][^>]*?\/?>|<!--[\s\S]*?-->/g
  const first = tags.exec(xml)
  const open = first[0], children = []
  let depth = 0, start = tags.lastIndex, m
  while ((m = tags.exec(xml))) {
    const t = m[0]
    if (t.startsWith('<!--')) continue
    if (t.startsWith('</')) {
      if (depth === 0) {
        if (m.index > start) children.push(xml.slice(start, m.index))
        return { open, children, close: t }
      }
      depth--
      if (depth === 0) {
        children.push(xml.slice(start, tags.lastIndex))
        start = tags.lastIndex
      }
    } else if (t.endsWith('/>')) {
      if (depth === 0) {
        children.push(xml.slice(start, tags.lastIndex))
        start = tags.lastIndex
      }
    } else {
      if (depth === 0 && m.index > start) {
        children.push(xml.slice(start, m.index))
        start = m.index
      }
      depth++
    }
  }
  throw new Error('unbalanced markup')
}

/** The markup with the inside of each element of one of these classes left out (as `.x > *`), last first. */
function emptied(xml, classes) {
  const opens = [...xml.matchAll(/<(\w+)\b[^>]*?\bclass="([^"]*)"[^>]*>/g)].filter((m) => !m[0].endsWith('/>') && m[2].split(/\s+/).some((c) => classes.includes(c)))
  for (const m of opens.reverse()) {
    const from = m.index + m[0].length
    const tags = new RegExp(`<(/?)${m[1]}\\b[^>]*?(/?)>`, 'g')
    tags.lastIndex = from
    let depth = 1, t
    while (depth && (t = tags.exec(xml))) depth += t[1] ? -1 : t[2] ? 0 : 1
    xml = xml.slice(0, from) + xml.slice(t.index)
  }
  return xml
}

/** What the compact player's rules hide on a small Canvas (demoPlayer.css: .tp-compact, .tp-small, .tp-tiny), left out of a tile:
 *  it is not drawn, and without it most of a Card's states draw the same. The rows keep their sizes. */
function unseen(xml) {
  xml = xml.replace(/<span class="cv-port-label[^"]*"[^>]*>[^<]*<\/span>/g, '')
  if (/class="cv-surface[^"]*\btp-small\b/.test(xml)) xml = emptied(xml, ['cv-param-row', 'cv-summary', 'cv-result', 'cv-stage-row', 'cv-draft-text'])
  if (/class="cv-surface[^"]*\btp-tiny\b/.test(xml)) xml = emptied(xml, ['cv-card-header', 'cv-start', 'cv-routing-icon'])
  return xml
}

/** The stacking order the player's CSS gives each kind of element (demoPlayer.css). */
function layerOf(child) {
  if (child.includes('class="cv-wires')) return 0
  if (child.includes('class="tp-top') || child.includes('cv-surface-zoom')) return 4
  return 1
}

const SHRUNK = fs.mkdtempSync(path.join(process.env.TMPDIR ?? '/tmp', 'tsg-svg-img-'))
process.on('exit', () => fs.rmSync(SHRUNK, { recursive: true, force: true }))

/** An image for a tile: at most TILE_IMAGE pixels on the long side, JPEG unless the PNG stays small. */
function shrunk(file) {
  if (file.endsWith('.svg')) return file
  const out = path.join(SHRUNK, path.basename(file))
  if (!fs.existsSync(out)) {
    execFileSync('sips', ['-Z', String(TILE_IMAGE), file, '--out', out], { stdio: 'ignore' })
    if (out.endsWith('.png') && fs.statSync(out).size > 16_000) {
      execFileSync('sips', ['-s', 'format', 'jpeg', '-s', 'formatOptions', '60', out, '--out', `${out}.jpg`], { stdio: 'ignore' })
      fs.renameSync(`${out}.jpg`, out)
    }
  }
  return out
}

/** Images of the output frames as data: URIs (the SVG loads nothing). */
function inlineImages(xml, base, small) {
  return xml.replace(/src="([^"]+)"/g, (all, url) => {
    if (!url.startsWith(base)) return all
    const file = (small ? shrunk : (f) => f)(path.join(DATA, decodeURIComponent(url.slice(base.length))))
    const bytes = fs.readFileSync(file)
    const type = file.endsWith('.svg') ? 'image/svg+xml' : bytes[0] === 0x89 ? 'image/png' : 'image/jpeg'
    return `src="data:${type};base64,${bytes.toString('base64')}"`
  })
}

/** Each distinct icon (16 × 16, web/src/ui/icons.tsx) once, in <defs>, and <use> where it is drawn. */
function sharedIcons(xml) {
  const ids = new Map()
  const body = xml.replace(/(<svg xmlns="http:\/\/www\.w3\.org\/2000\/svg"[^>]*viewBox="0 0 16 16"[^>]*>)((?:(?!<svg)[\s\S])*?)<\/svg>/g, (_, open, inner) => {
    if (!ids.has(inner)) ids.set(inner, `i${ids.size}`)
    return `${open}<use href="#${ids.get(inner)}"/></svg>`
  })
  return { body, defs: [...ids].map(([inner, id]) => `<g id="${id}">${inner}</g>`).join('') }
}

/** The rules of a minified stylesheet that can match the markup: each class a selector names is in it. */
function usedCss(css, markup) {
  const classes = new Set([...markup.matchAll(/class="([^"]*)"/g)].flatMap((m) => m[1].split(/\s+/)))
  const keep = (selectors) => selectors.split(',').some((sel) => [...sel.matchAll(/\.([\w-]+)/g)].every((m) => classes.has(m[1])))
  let out = '', i = 0
  while (i < css.length) {
    const open = css.indexOf('{', i)
    if (open < 0) break
    const head = css.slice(i, open).trim()
    let depth = 1, j = open + 1
    while (depth && j < css.length) {
      if (css[j] === '{') depth++
      else if (css[j] === '}') depth--
      j++
    }
    const body = css.slice(open + 1, j - 1)
    if (head.startsWith('@media')) {
      const inner = usedCss(body, markup)
      if (inner) out += `${head}{${inner}}`
    } else if (head.startsWith('@')) out += `${head}{${body}}`
    else if (keep(head)) out += `${head}{${body}}`
    i = j
  }
  return out
}

// ---- Timeline: which drawing is on screen when --------------------------------------------

/** Keyframes that show an element in the given frames only (step-end: no fades, as the player). */
function keyframes(name, shown, starts, total) {
  const pct = (ms) => `${((ms / total) * 100).toFixed(3)}%`
  const stops = []
  let last
  starts.forEach((t, i) => {
    const v = shown.has(i) ? 1 : 0
    if (v !== last) stops.push(`${pct(t)}{opacity:${v}}`)
    last = v
  })
  stops.push(`100%{opacity:${last}}`)
  return `@keyframes ${name}{${stops.join('')}}`
}

/** The replay drawn frame by frame, as the distinct drawings with the frames each is in, and the CSS that shows them so. */
function replay(data, view, compact) {
  if (data.draftOnly) throw new Error(`${data.id} is a Draft: there is no run to replay`)
  const [first, end] = HOLD[compact ? 'tile' : 'large']
  // The compact player replays at 0.6 of the full one's pace (demoPlayer.tsx).
  const frames = [{ ...player.IDLE, ms: first }, ...player.framesOf(data, compact ? 0.6 : 1), { ...player.DONE, ms: end }]
  const starts = []
  let total = 0
  for (const f of frames) {
    starts.push(total)
    total += f.ms
  }
  const base = pathToFileURL(path.join(DATA, '/')).href

  // Each distinct top-level drawing, in an order that keeps every frame's order.
  const pieces = [] // { xml, layer, shown: Set<frame> }
  let shell
  frames.forEach((f, i) => {
    const drawn = xhtml(player.surface(data, f, base, view, compact))
    const { open, children, close } = split(rounded(compact ? unseen(drawn) : drawn, compact ? 1 : 2))
    shell ??= { open, close }
    let after = -1
    // A Card's parts apart (its shape, its body, its ports): from one state to the next most of them stay the same.
    const parts = children.flatMap((xml) => {
      if (!xml.startsWith('<div class="cv-card ')) return [xml]
      const card = split(xml)
      return card.children.map((c) => `${card.open}${c}${card.close}`)
    })
    for (const xml of parts) {
      let at = pieces.findIndex((p) => p.xml === xml)
      if (at < 0) {
        at = after + 1
        pieces.splice(at, 0, { xml, layer: layerOf(xml), shown: new Set() })
      }
      pieces[at].shown.add(i)
      after = at
    }
  })

  const last = frames.length - 1
  const css = []
  /** One drawing, shown in its frames (a drawing in the last frame is the one shown without animation). */
  const layer = (shown, name, body, style = '') => {
    if (shown.size === frames.length) return `<div class="tp-l tp-fin" style="${style}">${body}</div>`
    css.push(keyframes(name, shown, starts, total), `.${name}{animation-name:${name}}`)
    return `<div class="tp-l ${name}${shown.has(last) ? ' tp-fin' : ''}" style="${style}">${body}</div>`
  }
  const surface = `${shell.open}${pieces.map((p, i) => layer(p.shown, `k${i}`, inlineImages(p.xml, base, compact), `z-index:${p.layer}`)).join('')}${shell.close}`
  const anim = () => `.tp-l{position:absolute;inset:0;opacity:0;animation:${total}ms step-end infinite both}.tp-l.tp-fin{opacity:1}${css.join('')}`
    + '@media (prefers-reduced-motion:reduce){.tp-l{animation:none!important}}'
  return { frames, surface, layer, anim }
}

const playerCss = fs.readFileSync(path.join(SITE, 'player.css'), 'utf8')
const FONT = `-apple-system, BlinkMacSystemFont, 'Hiragino Sans', 'Noto Sans JP', 'Yu Gothic', sans-serif`
const readDemo = (id) => JSON.parse(fs.readFileSync(path.join(DATA, `${id}.json`), 'utf8'))
const titleOf = (data) => `${data.title} — Tsugumi のデモ（${data.run ? `${data.run.date} の実行の記録を再生` : '線のつながりから順序を再生'}）`

/** The full player with its run control and narration, in the README cards' frame. */
function largeSvg(id) {
  const data = readDemo(id)
  const view = { width: W - 2 * PAD, height: CANVAS_H }
  const r = replay(data, view, false)
  const say = r.frames.map((f) => (f.mode === 'playing' ? data.steps[f.step].say : f.mode === 'done' ? data.finale : data.intro))
  const sayLine = [...new Set(say)].map((text, i) => r.layer(new Set(say.flatMap((s, j) => (s === text ? [j] : []))), `s${i}`, `<p class="tp-say">${esc(text)}</p>`)).join('')
  const body = `<div xmlns="http://www.w3.org/1999/xhtml" class="tp-root tp-full svg-root">
<div class="svg-canvas">${r.surface}</div>
<div class="svg-say">${sayLine}</div>
<p class="svg-foot">${esc(player.provenance(data).replace(/Card をクリックすると.*$/, ''))}</p>
</div>`
  const top = PAD + HEAD
  const icons = sharedIcons(body)
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${W} ${H}" width="${W}" height="${H}" role="img" aria-labelledby="t">
<title id="t">${esc(titleOf(data))}</title>
<style>${usedCss(playerCss, body)}
.svg-root{position:absolute;inset:0;display:block}
.svg-canvas{position:absolute;left:0;top:0;width:${view.width}px;height:${CANVAS_H}px}
.svg-canvas>.tp-surface{height:100%;border:0.5px solid var(--separator-strong)}
.svg-say{position:absolute;left:0;top:${CANVAS_H + 8}px;width:${view.width}px;height:${SAY - 8}px}
.svg-say .tp-say{color:${CREAM}}
.svg-foot{position:absolute;left:4px;top:${CANVAS_H + SAY}px;font-size:11.5px;color:${MUTED};white-space:nowrap;overflow:hidden;text-overflow:ellipsis;width:${view.width - 8}px}
${r.anim()}</style>
<defs><clipPath id="card"><rect width="${W}" height="${H}" rx="24"/></clipPath>${icons.defs}</defs>
<g clip-path="url(#card)">
<rect width="${W}" height="${H}" fill="${CARD_BG}"/>
<polygon points="${W},0 ${W - 200},0 ${W - 80},${HEAD + PAD - 8}" fill="#1d3a3e"/>
<polygon points="${W},0 ${W - 80},${HEAD + PAD - 8} ${W},${HEAD + PAD + 30}" fill="#1a3338"/>
<text x="${PAD + 4}" y="${PAD + 28}" font-family="${FONT}" font-size="26" font-weight="700" fill="${CREAM}">${esc(data.title)}</text>
<text x="${PAD + 4}" y="${PAD + 54}" font-family="${FONT}" font-size="15" fill="${MUTED}">${esc(data.description ?? '')}</text>
<foreignObject x="${PAD}" y="${top}" width="${view.width}" height="${H - top - PAD}">
${icons.body}
</foreignObject>
</g>
</svg>
`
}

/** The gallery's compact player: the whole Canvas fitted to the tile, the run replayed on it. */
function tileSvg(id) {
  const data = readDemo(id)
  const r = replay(data, TILE, true)
  const body = `<div xmlns="http://www.w3.org/1999/xhtml" class="tp-root tp-compact svg-root">${r.surface}</div>`
  const icons = sharedIcons(body)
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${TILE.width} ${TILE.height}" width="${TILE.width}" height="${TILE.height}" role="img" aria-labelledby="t">
<title id="t">${esc(titleOf(data))}</title>
<style>${usedCss(playerCss, body)}
.svg-root{position:absolute;inset:0;display:block}
.svg-root>.tp-surface{border-radius:0}
${r.anim()}</style>
<defs><clipPath id="tile"><rect width="${TILE.width}" height="${TILE.height}" rx="12"/></clipPath>${icons.defs}</defs>
<g clip-path="url(#tile)">
<rect width="${TILE.width}" height="${TILE.height}" fill="#1c1c1e"/>
<foreignObject width="${TILE.width}" height="${TILE.height}">
${icons.body}
</foreignObject>
</g>
</svg>
`
}

// The demos of demos/index.json with a recorded run (player-data/index.json says which).
const shipped = new Set(JSON.parse(fs.readFileSync(path.join(ROOT, 'demos', 'index.json'), 'utf8')).demos.map((d) => d.id))
const recorded = JSON.parse(fs.readFileSync(path.join(DATA, 'index.json'), 'utf8')).demos.filter((d) => d.recorded && shipped.has(d.id)).map((d) => d.id)
const ids = process.argv.slice(2).length ? process.argv.slice(2) : recorded
const write = (file, svg) => {
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, svg)
  return Buffer.byteLength(svg)
}
const kb = (n) => `${(n / 1024).toFixed(0)} KB`
let tiles = 0
for (const id of ids) {
  if (id === LARGE) console.log(`docs/assets/${id}-demo.svg ${kb(write(path.join(OUT, `${id}-demo.svg`), largeSvg(id)))}`)
  const n = write(path.join(OUT, 'demos', `${id}.svg`), tileSvg(id))
  tiles += n
  console.log(`docs/assets/demos/${id}.svg ${kb(n)}`)
}
console.log(`${ids.length} tiles, ${kb(tiles)} in all`)
