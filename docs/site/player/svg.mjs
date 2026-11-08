// Writes a demo's replay as one animated SVG (docs/assets/<id>-demo.svg) for places
// that show only an image, such as the README on GitHub: no script runs there, but
// CSS animation does.
//   node docs/site/player/svg.mjs [<id> …]        (default: casa)
// Every frame of the player's replay (framesOf in web/src/ui/demoPlayer.tsx) is drawn
// by the player's own Canvas and RunControl with React's static renderer, from the same
// player-data/<id>.json; the SVG holds that HTML in a <foreignObject> with the
// player's own stylesheet (site/player.css). The Cards, wires, output frames and the
// run control are kept once per distinct drawing, each shown during the frames that
// drew it. Images come from player-data/assets/ as data: URIs.
// Needs what build.mjs needs (web/node_modules, web/src/gen), and its outputs up to date.
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

/** The drawing's size: the canvas as the docs site shows it, and the card around it. */
const W = 1200, CANVAS_H = 700, PAD = 24, HEAD = 64, SAY = 66, FOOT = 26
const H = PAD + HEAD + CANVAS_H + SAY + FOOT + PAD
/** How long the replay holds before it starts and after it ends, in ms. */
const HOLD_START = 1600, HOLD_END = 5200
/** The README cards' colors (docs/assets/docs-card.svg). */
const CARD_BG = '#16282d', CREAM = '#f3ede2', MUTED = '#c3cbc8'

// ---- The player's own components, rendered without a browser --------------------------

const entry = `
import { createElement as h } from 'react'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { Canvas, RunControl, framesOf, IDLE, DONE, provenance } from './demoPlayer'
export { framesOf, IDLE, DONE, provenance }
export function surface(data, frame, base, view) {
  const playing = frame.mode === 'playing'
  return renderToStaticMarkup(h(Canvas, { data, frame, base, compact: false, onSelect: () => undefined, motion: true, view },
    h('div', { className: 'tp-top' }, h(RunControl, { data, frame, playing, onRun: () => undefined, onStop: () => undefined }))))
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
const xhtml = (html) => html.replace(VOID, '<$1$2/>').replace(/&nbsp;/g, '&#160;').replace(/<svg(?=[\s>])(?![^>]*xmlns=)/g, '<svg xmlns="http://www.w3.org/2000/svg"')
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

/** The stacking order the player's CSS gives each kind of element (demoPlayer.css). */
function layerOf(child) {
  if (child.includes('class="cv-wires')) return 0
  if (child.includes('class="tp-top') || child.includes('cv-surface-zoom')) return 4
  return 1
}

/** Images of the output frames as data: URIs (the SVG loads nothing). */
function inlineImages(xml, base) {
  return xml.replace(/src="([^"]+)"/g, (all, url) => {
    if (!url.startsWith(base)) return all
    const file = path.join(DATA, decodeURIComponent(url.slice(base.length)))
    const type = file.endsWith('.png') ? 'image/png' : file.endsWith('.svg') ? 'image/svg+xml' : 'image/jpeg'
    return `src="data:${type};base64,${fs.readFileSync(file).toString('base64')}"`
  })
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

function buildSvg(id) {
  const data = JSON.parse(fs.readFileSync(path.join(DATA, `${id}.json`), 'utf8'))
  if (data.draftOnly) throw new Error(`${id} is a Draft: there is no run to replay`)
  const frames = [{ ...player.IDLE, ms: HOLD_START }, ...player.framesOf(data, 1), { ...player.DONE, ms: HOLD_END }]
  const starts = []
  let total = 0
  for (const f of frames) {
    starts.push(total)
    total += f.ms
  }
  const base = pathToFileURL(path.join(DATA, '/')).href
  const view = { width: W - 2 * PAD, height: CANVAS_H }

  // Each distinct top-level drawing, in an order that keeps every frame's order.
  const pieces = [] // { xml, layer, shown: Set<frame> }
  let shell
  frames.forEach((f, i) => {
    const { open, children, close } = split(xhtml(player.surface(data, f, base, view)))
    shell ??= { open, close }
    let after = -1
    for (const xml of children) {
      let at = pieces.findIndex((p) => p.xml === xml)
      if (at < 0) {
        at = after + 1
        pieces.splice(at, 0, { xml, layer: layerOf(xml), shown: new Set() })
      }
      pieces[at].shown.add(i)
      after = at
    }
  })
  const say = frames.map((f) => (f.mode === 'playing' ? data.steps[f.step].say : f.mode === 'done' ? data.finale : data.intro))
  const sayPieces = [...new Set(say)].map((text) => ({ text, shown: new Set(say.flatMap((s, i) => (s === text ? [i] : []))) }))

  const last = frames.length - 1
  const css = []
  const layer = (shown, name, body, style = '') => {
    const always = shown.size === frames.length
    if (!always) css.push(keyframes(name, shown, starts, total))
    const cls = always ? 'tp-l' : `tp-l ${name}${shown.has(last) ? ' tp-fin' : ''}`
    return `<div class="${cls}" style="${style}">${body}</div>`
  }
  const surface = pieces.map((p, i) => layer(p.shown, `k${i}`, inlineImages(p.xml, base), `z-index:${p.layer}`)).join('')
  const sayLine = sayPieces.map((p, i) => layer(p.shown, `s${i}`, `<p class="tp-say">${esc(p.text)}</p>`)).join('')
  const playerCss = fs.readFileSync(path.join(SITE, 'player.css'), 'utf8')
  const anim = `.tp-l{position:absolute;inset:0;opacity:0;animation:${total}ms step-end infinite both}.tp-l.tp-fin,.tp-l:not([class*=" "]){opacity:1}`
    + pieces.map((p, i) => (p.shown.size < frames.length ? `.k${i}{animation-name:k${i}}` : '')).join('')
    + sayPieces.map((p, i) => (p.shown.size < frames.length ? `.s${i}{animation-name:s${i}}` : '')).join('')
    + '@media (prefers-reduced-motion:reduce){.tp-l{animation:none!important}}'

  const font = `-apple-system, BlinkMacSystemFont, 'Hiragino Sans', 'Noto Sans JP', 'Yu Gothic', sans-serif`
  const title = `${data.title} — Tsugumi のデモ（${data.run ? `${data.run.date} の実行の記録を再生` : '線のつながりから順序を再生'}）`
  const top = PAD + HEAD
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${W} ${H}" width="${W}" height="${H}" role="img" aria-labelledby="t">
<title id="t">${esc(title)}</title>
<style>${playerCss}
.svg-root{position:absolute;inset:0;display:block}
.svg-canvas{position:absolute;left:0;top:0;width:${view.width}px;height:${CANVAS_H}px}
.svg-canvas>.tp-surface{height:100%;border:0.5px solid var(--separator-strong)}
.svg-say{position:absolute;left:0;top:${CANVAS_H + 8}px;width:${view.width}px;height:${SAY - 8}px}
.svg-say .tp-say{color:${CREAM}}
.svg-foot{position:absolute;left:4px;top:${CANVAS_H + SAY}px;font-size:11.5px;color:${MUTED};white-space:nowrap;overflow:hidden;text-overflow:ellipsis;width:${view.width - 8}px}
${css.join('\n')}
${anim}</style>
<defs><clipPath id="card"><rect width="${W}" height="${H}" rx="24"/></clipPath></defs>
<g clip-path="url(#card)">
<rect width="${W}" height="${H}" fill="${CARD_BG}"/>
<polygon points="${W},0 ${W - 200},0 ${W - 80},${HEAD + PAD - 8}" fill="#1d3a3e"/>
<polygon points="${W},0 ${W - 80},${HEAD + PAD - 8} ${W},${HEAD + PAD + 30}" fill="#1a3338"/>
<text x="${PAD + 4}" y="${PAD + 28}" font-family="${font}" font-size="26" font-weight="700" fill="${CREAM}">${esc(data.title)}</text>
<text x="${PAD + 4}" y="${PAD + 54}" font-family="${font}" font-size="15" fill="${MUTED}">${esc(data.description ?? '')}</text>
<foreignObject x="${PAD}" y="${top}" width="${view.width}" height="${H - top - PAD}">
<div xmlns="http://www.w3.org/1999/xhtml" class="tp-root tp-full svg-root">
<div class="svg-canvas">${shell.open}${surface}${shell.close}</div>
<div class="svg-say">${sayLine}</div>
<p class="svg-foot">${esc(player.provenance(data).replace(/Card をクリックすると.*$/, ''))}</p>
</div>
</foreignObject>
</g>
</svg>
`
}

const ids = process.argv.slice(2).length ? process.argv.slice(2) : ['casa']
for (const id of ids) {
  const svg = buildSvg(id)
  const file = path.join(OUT, `${id}-demo.svg`)
  fs.writeFileSync(file, svg)
  console.log(`${path.relative(ROOT, file)} ${(Buffer.byteLength(svg) / 1024).toFixed(0)} KB`)
}
