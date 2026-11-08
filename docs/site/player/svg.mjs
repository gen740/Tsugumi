// Writes the demos' replays as animated SVGs for places that show only an image, such
// as the README on GitHub: no script runs there, but CSS animation does.
//   docs/assets/casa-demo.svg      Cas A, large: the full player with its run control and narration
//   docs/assets/demos/<id>.svg     every recorded demo as a tile: the gallery's compact player
//   docs/assets/casa-window.svg    Cas A in a drawing of the Mac app's window, laid out as the README's screenshot
//   docs/assets/casa-{source,timeline,guide}.svg, collatz-subworkflow.svg   the README's other screenshots of the window, moving
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
import { finalsFromRun, flowOf, readJson, readProject, stepsFromRun } from './build.mjs'

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
/** A tile's size (two or three to a row of the README). */
const TILE = { width: 440, height: 264 }
/** How long a replay holds before it starts and after it ends, in ms. */
const HOLD = { large: [1600, 5200], tile: [900, 3400], window: [1800, 5600] }
/** The demo shown large. */
const LARGE = 'casa'
/** The README cards' colors (docs/assets/docs-card.svg). */
const CARD_BG = '#16282d', CREAM = '#f3ede2', MUTED = '#c3cbc8'

// ---- The player's own components, rendered without a browser --------------------------

const entry = `
import { createElement as h } from 'react'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import { Canvas, RunControl, framesOf, IDLE, DONE, provenance } from './demoPlayer'
import { Icon } from './icons'
export { framesOf, IDLE, DONE, provenance }
const control = (data, frame) => h(RunControl, { data, frame, playing: frame.mode === 'playing', onRun: () => undefined, onStop: () => undefined })
/** The canvas, with the run control on it unless \`control\` is false (a tile, or the window, which has it in its title bar). */
export function surface(data, frame, base, view, compact, withControl = !compact) {
  return renderToStaticMarkup(h(Canvas, { data, frame, base, compact, onSelect: () => undefined, motion: true, view },
    withControl ? h('div', { className: 'tp-top' }, control(data, frame)) : null))
}
export const runControl = (data, frame) => renderToStaticMarkup(control(data, frame))
import { nodeData } from './demoPlayer'
import { BoundaryBarView } from './cardNode'
import { WireLayer } from './cardNode'
import { CardGeometry, WireRouter, fitCamera, union } from '../model/canvasKit'
import { pathToScreen } from '../model/canvas'
import { cardState, KIND_ICON } from '../model/session'
const stepsOf = (data) => {
  const m = new Map()
  data.steps.forEach((s, i) => s.cards.forEach((c) => m.set(c, i)))
  return m
}
/** Each Card's state, invocation and icon in a frame, as its node shows them. */
export function cardStates(data, frame) {
  const stepOf = stepsOf(data)
  return data.cards.map((doc) => {
    const n = nodeData(data, stepOf, doc, frame)
    const st = n.state === undefined ? undefined : cardState(n.state)
    return { id: doc.id, icon: doc.symbol ?? KIND_ICON[doc.kind], invocation: Number(n.invocation), state: st }
  })
}
/** A Card in Card's inner Workflow: the player's Canvas with the boundary bars above and below its Cards and their wires,
 *  placed and routed as model/canvas.ts does (computeBoundary, wires), and the camera fitted to all of it. */
export function innerSurface(data, frame, base, view, boundary) {
  const stepOf = stepsOf(data)
  const placed = data.cards.map((doc) => ({ doc, g: new CardGeometry(nodeData(data, stepOf, doc, frame), data.flow) }))
  const rectOf = (p) => ({ x: p.doc.x, y: p.doc.y, ...p.g.size })
  const box = placed.map(rectOf).reduce((u, r) => (u ? union(u, r) : r), null)
  const ins = Object.entries(boundary.inputs ?? {}), outs = Object.entries(boundary.outputs ?? {})
  const width = Math.max(box.width, Math.max(ins.length, outs.length) * 110 + 40)
  const x = box.x + box.width / 2 - width / 2
  const top = { x, y: box.y - 96, width, height: 40 }, bottom = { x, y: box.y + box.height + 56, width, height: 40 }
  const ports = (list, bar, output) => list.map(([name, v], i) => ({ name, point: { x: bar.x + (bar.width * (i + 1)) / (list.length + 1), y: output ? bar.y : bar.y + bar.height }, inner: [v.card, v.port], type: v.type }))
  const bars = { top, bottom, inputs: ports(ins, top, false), outputs: ports(outs, bottom, true) }
  const camera = fitCamera(union(union(box, top), bottom), view, { top: 44, left: 0, bottom: 0, right: 0 }, 1, 54)
  const z = camera.zoom
  const screen = (p) => ({ x: p.x * z + camera.x, y: p.y * z + camera.y })
  const port = (card, name, output) => {
    const p = placed.find((q) => q.doc.id === card)
    const at = p.g.port(name, output)
    return { x: p.doc.x + at.x, y: p.doc.y + at.y }
  }
  const router = new WireRouter(data.flow, [...placed.map(rectOf), top, bottom])
  // The bar's input wires are drawn plain (wires in model/canvas.ts); an output's as the inner Card's sending makes it.
  const out = frame.mode === 'done' ? 'carried' : frame.mode === 'playing' && frame.sub >= 1 ? 'active' : 'normal'
  const draws = [
    ...bars.inputs.map((p) => ({ d: pathToScreen(router.route(p.point, port(p.inner[0], p.inner[1], false)).d, camera), style: 'normal' })),
    ...bars.outputs.map((p) => ({ d: pathToScreen(router.route(port(p.inner[0], p.inner[1], true), p.point).d, camera), style: out })),
  ]
  const bar = (r, list, input) => h(BoundaryBarView, { key: input ? 'in' : 'out', title: input ? 'この Card の入力' : 'この Card の出力', ports: list.map((p) => ({ ...p, inner: [p.inner[0], p.inner[1]] })), input, zoom: z, rect: r,
    style: { position: 'absolute', left: screen(r).x, top: screen(r).y, width: r.width * z, height: r.height * z }, onPointerDown: () => undefined })
  return renderToStaticMarkup(h(Canvas, { data, frame, base, compact: false, onSelect: () => undefined, motion: true, view: { ...view, camera } },
    h(WireLayer, { wires: draws, animate: true, zoom: z, width: view.width, height: view.height }), bar(top, bars.inputs, true), bar(bottom, bars.outputs, false)))
}
export { formatDuration } from '../model/session'
export const icon = (name, size, color) => renderToStaticMarkup(h(Icon, { name, size, style: color ? { color } : undefined }))
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

/** An image at most `side` pixels on its long side (sips, as harvest.py), in its own format: no other compression. */
function shrunk(file, side) {
  if (file.endsWith('.svg') || !side) return file
  const dims = execFileSync('sips', ['-g', 'pixelWidth', '-g', 'pixelHeight', file], { encoding: 'utf8' }).match(/\d+/g).slice(-2).map(Number)
  if (Math.max(...dims) <= side) return file
  const out = path.join(SHRUNK, `${side}-${path.basename(file)}`)
  if (!fs.existsSync(out)) execFileSync('sips', ['-Z', String(side), file, '--out', out], { stdio: 'ignore' })
  return out
}

/** Images of the output frames as data: URIs (the SVG loads nothing), each `sideOf(file)` pixels at most. */
function inlineImages(xml, base, sideOf = () => undefined) {
  return xml.replace(/src="([^"]+)"/g, (all, url) => {
    if (!url.startsWith(base)) return all
    const source = path.join(DATA, decodeURIComponent(url.slice(base.length)))
    const file = shrunk(source, sideOf(source))
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
function replay(data, view, compact, kind = compact ? 'tile' : 'large', only, draw) {
  if (data.draftOnly) throw new Error(`${data.id} is a Draft: there is no run to replay`)
  const [first, end] = HOLD[kind]
  // The compact player replays at 0.6 of the full one's pace (demoPlayer.tsx). `only`: these frames instead (a scene after the run).
  const frames = only ?? [{ ...player.IDLE, ms: first }, ...player.framesOf(data, compact ? 0.6 : 1), { ...player.DONE, ms: end }]
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
    const drawn = xhtml(draw ? draw(f, base) : player.surface(data, f, base, view, compact, kind === 'large'))
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
  // A tile's images at twice the size its output frame has in it (a Retina screen at the tile's own size), no larger.
  // The zoom, from a Card's header (CardGeometry.header: 32 units): a small Canvas has no dot grid to read it from.
  const header = Number(pieces.map((p) => p.xml.match(/class="cv-card-header" style="height:([\d.]+)px/)?.[1]).find(Boolean) ?? 0)
  const zoomed = header / 32
  const sideOf = (file) => {
    if (!compact) return undefined
    const id = Object.keys(data.outputs).find((k) => data.outputs[k].src && file.endsWith(data.outputs[k].src.replace(/^assets\//, '')))
    const f = data.frames[id] ?? {}
    return Math.max(160, Math.ceil(2 * Math.max(f.width ?? 320, f.height ?? 170) * (zoomed || 1)))
  }
  const surface = `${shell.open}${pieces.map((p, i) => layer(p.shown, `k${i}`, inlineImages(p.xml, base, sideOf), `z-index:${p.layer}`)).join('')}${shell.close}`
  const anim = () => `.tp-l{position:absolute;inset:0;opacity:0;animation:${total}ms step-end infinite both}.tp-l.tp-fin{opacity:1}${css.join('')}`
    + '@media (prefers-reduced-motion:reduce){.tp-l{animation:none!important}}'
  /** What a function of the frame draws, each distinct drawing as one layer. */
  const perFrame = (name, draw, style = '') => {
    const drawn = frames.map(draw)
    return [...new Set(drawn)].map((xml, i) => layer(new Set(drawn.flatMap((d, j) => (d === xml ? [j] : []))), `${name}${i}`, xml, style)).join('')
  }
  // The Canvas's zoom, from its dot grid (gridStyle in cardNode.tsx: 20 units apart).
  const zoom = Number(shell.open.match(/background-size:([\d.]+)px/)?.[1] ?? 0) / 20
  /** An animation of its own over the same loop: [ms, declarations] stops, eased by `timing` between them. */
  const custom = (name, stops, timing = 'linear') => {
    const pct = (ms) => `${((ms / total) * 100).toFixed(3)}%`
    css.push(`@keyframes ${name}{${stops.map(([ms, d]) => `${pct(ms)}{${d}}`).join('')}}`, `.${name}{animation:${name} ${total}ms ${timing} infinite both}`)
    return name
  }
  /** Where a Card is drawn on the canvas (screen pixels), found by its title in the last frame. */
  const cardBox = (id) => {
    const xml = pieces.find((p) => p.shown.has(last) && p.xml.startsWith('<div class="cv-card ') && p.xml.includes(`>${id}</span>`))?.xml
    const n = (k) => Number(xml?.match(new RegExp(`^<div[^>]*?[;"]${k}:(-?[\\d.]+)px`))?.[1] ?? 0)
    return { left: n('left'), top: n('top'), width: n('width'), height: n('height') }
  }
  return { frames, starts, total, surface, layer, anim, perFrame, zoom, custom, cardBox }
}

const playerCss = fs.readFileSync(path.join(SITE, 'player.css'), 'utf8')
const FONT = `-apple-system, BlinkMacSystemFont, 'Hiragino Sans', 'Noto Sans JP', 'Yu Gothic', sans-serif`
const readDemo = (id) => JSON.parse(fs.readFileSync(path.join(DATA, `${id}.json`), 'utf8'))
const titleOf = (data) => `${data.title} — Tsugumi のデモ（${data.run ? `${data.run.date} の実行の記録を再生` : '線のつながりから順序を再生'}）`

/** Safari draws the HTML of a <foreignObject> that sits in a positioned box (here: all of it) at its own size when the SVG is
 *  scaled to fit an <img> (WebKit bug 23113): Safari alone gets the scale from the image's width, set by media queries
 *  (the SVG's viewport is the image's box) and applied as a CSS transform. Others scale it with the viewBox. Safari is the
 *  browser that supports `hanging-punctuation`. */
function safariZoom(width) {
  const steps = []
  // About 4 px apart up to the drawing's own width, then coarser (an image is rarely shown larger).
  const fine = Math.min(0.02, Math.max(0.005, 4 / width))
  for (let z = 0.15; z <= 2.0001; z += z < 1.05 ? fine : 0.05) steps.push(`@media (min-width:${(z * width).toFixed(1)}px){:root{--z:${z.toFixed(3)}}}`)
  return `@supports (hanging-punctuation:first) and (font:-apple-system-body){.svg-zoom{transform:scale(var(--z,1))}${steps.join('')}}`
}

/** One SVG for an <img>: its HTML in a <foreignObject> the size of the viewBox, in a box that Safari scales itself. */
function svgDoc({ width, height, title, css, defs = '', html }) {
  const icons = sharedIcons(html)
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-labelledby="t">
<title id="t">${esc(title)}</title>
<style>${css}
.svg-zoom{position:absolute;left:0;top:0;width:${width}px;height:${height}px;transform-origin:0 0}
.svg-zoom .material{backdrop-filter:none;-webkit-backdrop-filter:none}
${safariZoom(width)}</style>
<defs>${defs}${icons.defs}</defs>
<foreignObject width="${width}" height="${height}"><div xmlns="http://www.w3.org/1999/xhtml" class="svg-zoom">${icons.body}</div></foreignObject>
</svg>
`
}

/** The full player with its run control and narration, in the README cards' frame. */
function largeSvg(id) {
  const data = readDemo(id)
  const view = { width: W - 2 * PAD, height: CANVAS_H }
  const r = replay(data, view, false)
  const sayLine = r.perFrame('s', (f) => `<p class="tp-say">${esc(f.mode === 'playing' ? data.steps[f.step].say : f.mode === 'done' ? data.finale : data.intro)}</p>`)
  const top = PAD + HEAD
  const html = `<div class="svg-card">
<svg xmlns="http://www.w3.org/2000/svg" class="svg-poly" width="${W}" height="${H}"><polygon points="${W},0 ${W - 200},0 ${W - 80},${HEAD + PAD - 8}" fill="#1d3a3e"/><polygon points="${W},0 ${W - 80},${HEAD + PAD - 8} ${W},${HEAD + PAD + 30}" fill="#1a3338"/></svg>
<div class="svg-title">${esc(data.title)}</div><div class="svg-desc">${esc(data.description ?? '')}</div>
<div class="tp-root tp-full svg-body" style="left:${PAD}px;top:${top}px;width:${view.width}px;height:${H - top - PAD}px">
<div class="svg-canvas">${r.surface}</div>
<div class="svg-say">${sayLine}</div>
<p class="svg-foot">${esc(player.provenance(data).replace(/Card をクリックすると.*$/, ''))}</p>
</div></div>`
  return svgDoc({
    width: W, height: H, title: titleOf(data), html,
    css: `${usedCss(playerCss, html)}
.svg-card{position:absolute;inset:0;border-radius:24px;overflow:hidden;background:${CARD_BG};font-family:${FONT}}
.svg-poly{position:absolute;left:0;top:0}
.svg-title{position:absolute;left:${PAD + 4}px;top:${PAD - 2}px;font-size:26px;font-weight:700;color:${CREAM}}
.svg-desc{position:absolute;left:${PAD + 4}px;top:${PAD + 38}px;font-size:15px;color:${MUTED}}
.svg-body{position:absolute;display:block}
.svg-canvas{position:absolute;left:0;top:0;width:${view.width}px;height:${CANVAS_H}px}
.svg-canvas>.tp-surface{height:100%;border:0.5px solid var(--separator-strong)}
.svg-say{position:absolute;left:0;top:${CANVAS_H + 8}px;width:${view.width}px;height:${SAY - 8}px}
.svg-say .tp-say{color:${CREAM}}
.svg-foot{position:absolute;left:4px;top:${CANVAS_H + SAY}px;font-size:11.5px;color:${MUTED};white-space:nowrap;overflow:hidden;text-overflow:ellipsis;width:${view.width - 8}px}
${r.anim()}`,
  })
}

/** The gallery's compact player: the whole Canvas fitted to the tile, the run replayed on it. */
function tileSvg(id) {
  const data = readDemo(id)
  const r = replay(data, TILE, true)
  const html = `<div class="tp-root tp-compact svg-tile">${r.surface}</div>`
  return svgDoc({
    width: TILE.width, height: TILE.height, title: titleOf(data), html,
    css: `${usedCss(playerCss, html)}
.svg-tile{position:absolute;inset:0;display:block;border-radius:12px;overflow:hidden;background:#1c1c1e}
.svg-tile>.tp-surface{border-radius:0}
${r.anim()}`,
  })
}

// ---- The Mac app's window -----------------------------------------------------------------

/** The window of the README's screenshots (docs/assets/screenshots, macOS/tools/screenshots/readme.ui): 1440 × 900, the chat column on the right. */
const WIN = { width: 1440, height: 900, bar: 52, chat: 344, caption: 30 }
/** Where the Cas A Canvas has the Cards, output frames and the note: the screenshots' arrangement (Cards in one row, the
 *  spectrum above, the band table and the RGB image below), with the Cards the demo has now (ciao, and the forks begin and share). */
const WINDOW_LAYOUT = {
  cards: { start: [0, 484], begin: [150, 495], load: [230, 484], ciao: [230, 640], bands: [500, 484], share: [770, 495], rgb: [850, 484], spectrum: [1110, 484] },
  displays: { spectrum: [760, 0, 680, 400], bands: [500, 650, 340, 150], rgb: [880, 650, 420, 440] },
  note: [0, 0, 700, 430],
}
/** The shell's colors the player's stylesheet has not (web/src/styles/shell.css, dark). */
const SHELL_VARS = '--indigo:#5e5ce6;--cyan:#64d2ff;--mint:#63e6e2;--teal:#40c8e0;--tertiary:rgba(235,235,245,.32);--tsugumi-cream:#f6ead2;--tsugumi-rufous:#c97d3d'
/** The Web UI's rules for the shell, the guide and its helpers, without comments (usedCss keeps those the drawing uses). */
const webCss = ['styles.css', 'styles/shell.css', 'styles/guide.css'].map((f) => fs.readFileSync(path.join(WEB, 'src', f), 'utf8').replace(/\/\*[\s\S]*?\*\//g, '')).join('\n')
  .replace(/:root(\[data-theme="dark"\])?\s*\{[^}]*\}/g, '')

const chromeIcon = (name, size, color) => xhtml(player.icon(name, size, color))

/** The guide's bird (GuideBird in web/src/ui/guide.tsx): the icon's bird with its wing, the polygons of the icon's perch above the branch. */
function guideBirdDef() {
  const icon = fs.readFileSync(path.join(OUT, 'tsugumi-icon.svg'), 'utf8')
  const group = (id) => split(icon.slice(icon.indexOf(`<g id="${id}">`)))
  const wing = group('perch').children.filter((p) => {
    const ys = (p.match(/points="([^"]*)"/)?.[1] ?? '').trim().split(/\s+/).map((xy) => Number(xy.split(',')[1]))
    return ys.length && Math.min(...ys) < 690
  })
  return `<g id="tsg-bird">${wing.join('')}${group('tsugumi-bird').children.join('')}</g>`
}
const guideBird = () => '<svg xmlns="http://www.w3.org/2000/svg" viewBox="130 270 680 470" class="guide-bird"><g transform="translate(940 0) scale(-1 1)"><use href="#tsg-bird"/></g></svg>'

/** The chat column's head: the guide in its nest (GuideNest), the conversation's title and its buttons (ChatPanel.swift). */
const chatHead = (title, nest = '<span class="guide-nest">' + guideBird() + '</span>') =>
  `<div class="w-chat-head">${nest}${chromeIcon('list.bullet', 14, 'var(--secondary)')}<b class="cv-ellipsis">${esc(title)}</b><span style="flex:1"></span>${chromeIcon('square.and.pencil', 14, 'var(--secondary)')}${chromeIcon('ellipsis.circle', 14, 'var(--secondary)')}</div>`
const CHAT_HELLO = '<div class="w-chat-hello"><p>ツグミです。Workflow の相談・生成・編集も、使い方の質問もどうぞ。</p><p class="secondary">Card を右クリックして参照に追加、$ で Skill を選べます。</p></div>'
const chatInput = (tokens = '0k') => `<div class="w-input"><span class="secondary">メッセージ（Return で送信）</span><span class="w-input-row">${chromeIcon('paperclip', 14, 'var(--secondary)')}${chromeIcon('chevron.down', 11, 'var(--secondary)')}<span>claude-sonnet-5-5</span><span style="flex:1"></span><span class="secondary caption">${tokens}</span>${chromeIcon('arrow.up.circle.fill', 18, 'var(--secondary)')}</span></div>`

/** Cas A with the screenshots' arrangement. */
function casaInWindow() {
  const base = readDemo('casa')
  const L = WINDOW_LAYOUT
  return {
    ...base,
    cards: base.cards.map((c) => ({ ...c, x: L.cards[c.id]?.[0] ?? c.x, y: L.cards[c.id]?.[1] ?? c.y })),
    frames: Object.fromEntries(Object.entries(L.displays).map(([k, [x, y, width, height]]) => [k, { x, y, width, height }])),
    notes: base.notes.slice(0, 1).map((n) => ({ ...n, x: L.note[0], y: L.note[1], width: L.note[2], height: L.note[3] })),
  }
}
const obsidOf = (data) => data.cards.find((c) => c.id === 'load')?.allParams.find((p) => p.name === 'obsid')?.value

/** The app's main window around a replay. `o`: the demo's data, the Canvas's place in the window, and what a scene adds:
 *  `frames` (instead of the run's), a sidebar, the tab's text, a bottom panel, the chat column, and `over(r, place)` drawn over all of it. */
function appWindow(o) {
  const canvas = o.canvas ?? { left: 0, top: WIN.bar, width: WIN.width - WIN.chat, height: WIN.height - WIN.bar }
  const data = o.data
  const r = replay(data, { width: canvas.width, height: canvas.height }, false, 'window', o.frames, o.draw && ((f, base) => o.draw(f, base, { width: canvas.width, height: canvas.height })))
  const place = (b) => ({ left: b.left + canvas.left, top: b.top + canvas.top, width: b.width, height: b.height })
  // The status bar's run state (StatusBar.swift: 実行: <label>), with the run control's icon for it.
  const state = (f) => (f.mode === 'playing' ? ['play.circle.fill', 'var(--orange)', '実行中'] : f.mode === 'done' ? ['checkmark.circle.fill', 'var(--green)', '成功'] : ['circle', 'var(--secondary)', '待機中'])
  const status = r.perFrame('st', (f) => {
    const [name, color, label] = state(f)
    return `<span class="w-item">${chromeIcon(name, 12, color)}<span>実行: ${label}</span></span>`
  })
  const control = r.perFrame('rc', (f) => xhtml(player.runControl(data, f)))
  // The zoom bar and the status bar float over the Canvas's bottom, above a bottom panel when one is open.
  const floor = WIN.height - (o.panelTop ?? canvas.top + canvas.height) + 10
  const left = o.sidebarWidth ?? 0
  const body = `<div xmlns="http://www.w3.org/1999/xhtml" class="tp-root svg-root w-root">
<div class="w-canvas" style="left:${canvas.left}px;top:${canvas.top}px;width:${canvas.width}px;height:${canvas.height}px">${r.surface}</div>
${o.sidebar ?? ''}
<div class="w-bar" style="left:${left}px;width:${WIN.width - WIN.chat - left}px;padding-left:${left ? 12 : 20}px">
  ${left ? '' : '<span class="w-lights"><i style="background:#ff5f57"></i><i style="background:#febc2e"></i><i style="background:#28c840"></i></span>'}
  <span class="w-round">${chromeIcon('sidebar.left', 16)}</span>
  <span class="w-tab">${o.tab ?? esc(data.title)}</span>
  <span class="w-runbox"><span class="w-run">${control}</span><span class="w-item secondary">${chromeIcon('checkmark.seal', 13)}<span>0</span></span><span class="w-sep"></span>${chromeIcon('plus', 14)}</span>
  <span class="w-glass w-right">${chromeIcon('folder', 15)}${chromeIcon('bubble.left.and.bubble.right.fill', 16)}</span>
</div>
<div class="w-zoom w-glass" style="left:${canvas.left + 10}px;bottom:${floor}px">${chromeIcon('minus.magnifyingglass', 14)}<span class="mono-digits">${Math.round(r.zoom * 100)}%</span>${chromeIcon('plus.magnifyingglass', 14)}${chromeIcon('arrow.up.left.and.down.right.magnifyingglass', 14)}${chromeIcon('square.grid.2x2', 14)}${chromeIcon('number', 14, 'var(--accent)')}</div>
<div class="w-status w-glass" style="right:${WIN.chat + 10}px;bottom:${floor}px">
  <span class="w-slot">${status}</span>
  <span class="w-item">${chromeIcon('checkmark.icloud', 13, 'var(--green)')}<span>保存済み</span></span>
  <span class="w-item">${chromeIcon('point.3.connected.trianglepath.dotted', 13, 'var(--secondary)')}<span class="mono-digits">rev 0</span></span>
  <span class="w-item">${chromeIcon('xmark.circle', 12)}<span>0</span>${chromeIcon('exclamationmark.triangle', 12)}<span>0</span></span>
  <span class="w-item w-panels">${['text.alignleft', 'chart.bar.xaxis', 'terminal'].map((n) => `<span class="${o.panel === n ? 'w-on' : ''}">${chromeIcon(n, 14)}</span>`).join('')}</span>
</div>
${o.bottom ? o.bottom(r) : ''}
<div class="w-chat">${o.chat ? o.chat(r) : `${chatHead('新しい会話')}${CHAT_HELLO}${chatInput()}`}</div>
${o.over ? o.over(r, place) : ''}
</div>`
  const H = WIN.height + WIN.caption
  const html = `<div class="w-frame">${body}</div><div class="w-caption">${esc(o.caption)}</div>`
  return svgDoc({
    width: WIN.width, height: H, title: o.title, html, defs: guideBirdDef(),
    css: `${usedCss(playerCss, html)}
${usedCss(webCss, html)}
.w-frame{position:absolute;left:0;top:0;width:${WIN.width}px;height:${WIN.height}px;border-radius:12px;overflow:hidden;box-shadow:inset 0 0 0 1px #4a4a4c}
.svg-root{position:absolute;inset:0;display:block}
.w-root{background:#1e1e1e;font-size:12px;${SHELL_VARS}}
.w-root .icon{vertical-align:middle}
.w-canvas{position:absolute;z-index:0}
.w-canvas>.tp-surface{height:100%;border-radius:0}
.w-bar{position:absolute;top:0;height:${WIN.bar}px;display:flex;align-items:center;gap:12px;padding-right:10px;background:var(--text-bg)}
.w-lights{display:flex;gap:9px;margin-right:6px}.w-lights i{width:12px;height:12px;border-radius:50%}
.w-glass{background:rgba(58,58,60,.72);border:0.5px solid rgba(255,255,255,.1);border-radius:999px;box-shadow:0 1px 3px rgba(0,0,0,.35)}
.w-round{display:inline-flex;align-items:center;justify-content:center;width:36px;height:36px;border-radius:50%;background:rgba(58,58,60,.72);border:0.5px solid rgba(255,255,255,.1)}
.w-tab{width:380px;height:28px;display:flex;align-items:center;gap:6px;padding:0 12px;border-radius:999px;background:rgba(58,58,60,.55);font-weight:600;font-size:12.5px;white-space:nowrap}
.w-runbox{position:relative;margin-left:auto;display:flex;align-items:center;gap:10px;height:34px;padding:0 12px 0 0;border-radius:999px;background:rgba(58,58,60,.72);border:0.5px solid rgba(255,255,255,.1)}
.w-run{position:relative;display:block;width:232px;height:32px}
.w-run .cv-run{background:none!important;border:0!important;box-shadow:none!important;backdrop-filter:none!important}
.w-sep{width:1px;height:16px;background:var(--separator-strong)}
.w-right{display:flex;align-items:center;gap:12px;height:34px;padding:0 12px;margin-left:72px}
.w-item{display:inline-flex;align-items:center;gap:5px;white-space:nowrap}
.w-zoom{position:absolute;display:flex;align-items:center;gap:8px;height:28px;padding:0 10px}
.w-status{position:absolute;display:flex;align-items:center;gap:22px;height:30px;padding:0 14px;font-weight:600}
.w-panels{gap:6px!important;font-weight:400}.w-panels>span{display:inline-flex;padding:2px 5px;border-radius:5px}.w-panels>.w-on{background:var(--accent);color:#fff}
.w-slot{position:relative;display:inline-block;width:92px;height:16px}
.w-chat{position:absolute;right:0;top:0;width:${WIN.chat}px;height:${WIN.height}px;background:#262628;border-left:0.5px solid rgba(255,255,255,.12);overflow:hidden}
.w-chat-head{position:absolute;left:0;right:0;top:48px;display:flex;align-items:center;gap:10px;height:44px;padding:0 14px;font-size:13px}
.w-chat-hello{position:absolute;left:0;right:0;top:92px;padding:10px 14px;border-top:0.5px solid var(--separator)}
.w-chat-hello p{margin:6px 0;font-weight:600;line-height:1.45}.w-chat-hello p.secondary{font-weight:400;font-size:11px}
.w-input{position:absolute;left:10px;right:10px;bottom:10px;height:58px;display:flex;flex-direction:column;justify-content:space-between;padding:8px 12px;border-radius:12px;background:#2f2f31;border:0.5px solid rgba(255,255,255,.12);font-size:13px}
.w-input-row{display:flex;align-items:center;gap:8px}
.w-a{position:absolute}
.w-caption{position:absolute;left:4px;top:${WIN.height + 8}px;font:12px ${FONT};color:#8b949e;white-space:nowrap}
${o.css ?? ''}
${r.anim()}`,
  })
}

/** Cas A run in the window: the README's first screenshot (workflow.png), moving. */
function windowSvg() {
  const data = casaInWindow()
  return appWindow({
    data,
    title: `${data.title} — Tsugumi のアプリでの実行（${data.run.date} の実行の記録を再生）`,
    caption: `${data.run.date} に実際に動かした Cas A の実行の記録（ObsID ${obsidOf(data)}）を、アプリの画面の形で再生しています。窓の枠とチャット欄は静止した絵です。`,
  })
}

// ---- Timeline: which Card ran when, filled in as the run goes --------------------------------

/** The timeline panel (RunTimelineView in web/src/ui/logs.tsx: a row per Card, a bar per invocation split into its stages), from the
 *  recorded invocations. Its right edge follows the replay: real time maps onto each step's frames. */
const STAGE_COLOR = { prepare: 'var(--indigo)', evaluate: 'var(--cyan)', build: 'var(--blue)', execute: 'var(--green)', validate: 'var(--mint)', save: 'var(--teal)' }
const TIMELINE = { top: 697, label: 170, row: 18 }

function timelineSvg() {
  const data = casaInWindow()
  const recorded = JSON.parse(fs.readFileSync(path.join(DATA, 'assets', 'casa.run.json'), 'utf8'))
  const height = WIN.height - TIMELINE.top
  // The panel opens over the Canvas's bottom: the Canvas keeps its place and zoom (as in timeline.png).
  const canvas = { left: 0, top: WIN.bar, width: WIN.width - WIN.chat, height: WIN.height - WIN.bar }
  const total = Math.max(...Object.values(recorded.timeline).map((e) => e.endMs))
  const chartW = canvas.width - TIMELINE.label - 8
  const x = (ms) => (ms / total) * chartW
  const rows = Object.keys(recorded.timeline).sort()
  const bottom = (r) => {
    // Real time at the start and the end of each step's frames, and how far the chart is shown then. Shown by moving a
    // clipping box and moving its content back (transforms only: nothing is laid out again while it plays).
    const shown = [[0, 0], [r.starts[1], 0]]
    data.steps.forEach((st, i) => {
      const at = r.frames.map((f, j) => (f.mode === 'playing' && f.step === i ? j : -1)).filter((j) => j >= 0)
      shown.push([r.starts[at[0]], x(st.startMs)])
      const j = at.at(-1)
      shown.push([r.starts[j] + r.frames[j].ms, x(st.endMs)])
    })
    shown.push([r.total, chartW])
    const reveal = r.custom('tl-reveal', shown.map(([t, w]) => [t, `transform:translateX(${(w - chartW).toFixed(1)}px)`]))
    const back = r.custom('tl-back', shown.map(([t, w]) => [t, `transform:translateX(${(chartW - w).toFixed(1)}px)`]))
    const bars = rows.map((k, i) => {
      const e = recorded.timeline[k]
      const y = i * TIMELINE.row + 3, h = TIMELINE.row - 6
      let t = e.startMs
      const parts = e.stages.map((st) => {
        const x0 = x(t), w = Math.max(st.ms ? 1 : 0, x(t + st.ms) - x0)
        t += st.ms
        return w ? `<rect x="${x0.toFixed(1)}" y="${y}" width="${w.toFixed(1)}" height="${h}" style="fill:${STAGE_COLOR[st.phase] ?? 'var(--gray)'};opacity:0.85"/>` : ''
      }).join('')
      const x0 = x(e.startMs), x1 = Math.max(x0 + 2, x(e.endMs))
      return `<rect x="${x0.toFixed(1)}" y="${y}" width="${(x1 - x0).toFixed(1)}" height="${h}" rx="2" style="fill:var(--green);opacity:0.35"/>${parts}`
    }).join('')
    const stripes = rows.map((_, i) => (i % 2 ? `<rect x="0" y="${i * TIMELINE.row}" width="${chartW}" height="${TIMELINE.row}" class="timeline-stripe"/>` : '')).join('')
    const date = recorded.run.date.split('-').map(Number)
    return `<div class="w-timeline" style="top:${TIMELINE.top}px;width:${canvas.width}px;height:${height}px">
<div class="w-tl-bar">${chromeIcon('chart.bar.xaxis', 14)}<b>タイムライン</b><span class="secondary" style="margin-left:24px">実行</span><span class="w-tl-pick">${date[0]}年${date[1]}月${date[2]}日 · 成功 ${chromeIcon('chevron.up.chevron.down', 10)}</span><span style="flex:1"></span>${chromeIcon('xmark', 12, 'var(--secondary)')}</div>
<div class="timeline"><div class="timeline-head"><span class="caption2 secondary mono-digits" style="width:${TIMELINE.label}px;text-align:right">${player.formatDuration(0)}</span><span class="spacer" style="flex:1"></span><span class="caption2 tertiary">ドラッグで範囲を拡大・横スクロールで移動</span><span class="w-tl-all">全体</span><span class="caption2 secondary mono-digits">${player.formatDuration(total / 1000)}</span></div>
<div class="w-tl-rows"><div class="timeline-labels" style="width:${TIMELINE.label}px">${rows.map((k) => `<div class="timeline-label caption" style="height:${TIMELINE.row}px">${k}</div>`).join('')}</div>
<div class="w-tl-chart" style="width:${chartW}px;height:${rows.length * TIMELINE.row}px"><svg xmlns="http://www.w3.org/2000/svg" width="${chartW}" height="${rows.length * TIMELINE.row}">${stripes}</svg>
<div class="w-tl-clip ${reveal}" style="height:${rows.length * TIMELINE.row}px;width:${chartW}px"><svg xmlns="http://www.w3.org/2000/svg" class="${back}" width="${chartW}" height="${rows.length * TIMELINE.row}">${bars}</svg></div></div></div></div>
</div>`
  }
  return appWindow({
    data, canvas, bottom, panel: 'chart.bar.xaxis', panelTop: TIMELINE.top,
    title: `${data.title} — タイムライン（${data.run.date} の実行の記録）`,
    caption: `タイムラインの棒は ${data.run.date} の Cas A の実行（ObsID ${obsidOf(data)}）で記録した各 Card の開始・終了と段階の時間です。窓の枠とチャット欄は静止した絵です。`,
    css: `.w-timeline{position:absolute;left:0;background:var(--window-bg);border-top:0.5px solid var(--separator-strong);display:flex;flex-direction:column}
.w-tl-bar{display:flex;align-items:center;gap:8px;height:32px;padding:0 12px;flex:none;border-bottom:0.5px solid var(--separator)}
.w-tl-pick{display:inline-flex;align-items:center;gap:8px;margin-left:8px;padding:3px 10px;border-radius:6px;background:var(--control-bg);font-weight:500}
.w-tl-all{padding:1px 8px;border-radius:5px;background:var(--control-bg);font-size:11px;color:var(--tertiary)}
.w-tl-rows{display:flex}.w-tl-chart{position:relative}.w-tl-chart>svg{position:absolute;left:0;top:0}
.w-tl-clip{position:absolute;left:0;top:0;overflow:hidden}
.timeline-stripe{fill:color-mix(in srgb,var(--secondary) 7%,transparent)}
.timeline{padding:0}`,
  })
}

// ---- The guide: Tsugumi flies to the Card it explains ----------------------------------------

/** What Tsugumi answered in the README's screenshot (docs/assets/screenshots/guide.png: Cas A, `$demo spectrum の Card が何を
 *  示しているか、画面で指しながら教えて`), written out from it: the bubble by the Card and the answer in the chat. */
const GUIDE = {
  ask: '$demo spectrum の Card が何を示しているか、画面で指しながら教えて',
  say: 'spectrum は光子のエネルギー分布（スペクトル）を描きます。赤・緑・青の帯域と元素の輝線が一目でわかります。',
  choices: ['パラメータを見せて', '表示の見方を教えて', 'ありがとう、もう大丈夫'],
  answer: [
    `<h4>処理</h4><ol><li><code>provenance.json</code> に書かれたイベントファイルを開きます。</li><li><b>bands</b> と同じ範囲（中心から 2 ピクセル刻み × 360 ピクセルの正方形）の光子だけを選びます。</li><li>エネルギー（eV）のヒストグラムを作ります。</li><li>縦軸を対数にして描きます。</li></ol>`,
    `<h4>グラフの見方</h4><ul><li>黒い階段状の線が光子数です。横軸は keV です。</li><li>赤・緑・青の帯は、<b>bands</b> の 3 帯域です。<ul><li>赤は 600–1650 eV です。</li><li>緑は 1650–2250 eV です。</li><li>青は 2250–7500 eV です。</li></ul></li><li>左下の凡例に、色ごとの光子数が出ます。</li><li>点線と元素名は、残骸の輝線の位置です。<ul><li>Mg は 1.35 keV です。</li><li>Si は 1.86 keV です。</li><li>S は 2.45 keV です。</li><li>Ar は 3.12 keV です。</li><li>Ca は 3.90 keV です。</li><li>Fe は 6.68 keV です。</li></ul></li><li>どの元素がどの色に入るかがわかります。Mg は赤、Si は緑、S 以降は青です。</li><li>タイトルには ObsID と露光時間が出ます。</li></ul>`,
    `<h4>パラメータ</h4><ul><li><code>min_ev</code> は 300 で、描く範囲の下限です。</li><li><code>max_ev</code> は 8000 で、描く範囲の上限です。</li><li><code>bin_ev</code> は 20 で、1 ビンの幅です。</li><li>この 3 つは見た目を変えるだけで、帯域の境界は変わりません。帯域の境界は <b>bands</b> のパラメータです。</li></ul>`,
    `<p>直近の実行は成功しているので、画面右上にグラフが出ているはずです。次は、パラメータの変更か表示の見方のどちらかをご案内できます。</p>`,
  ],
}
/** The scene's moments in ms: asked, the flight, the bubble, each part of the answer in the chat. */
const GUIDE_T = { total: 16000, ask: 1200, fly: 3000, land: 3900, parts: [4300, 5100, 5900, 6700] }

function guideSvg() {
  const data = casaInWindow()
  const T = GUIDE_T
  const choices = `<div class="row gap6 wrap">${GUIDE.choices.map((c) => `<span class="guide-choice">${esc(c)}</span>`).join('')}</div>`
  /** Shown from `from` to `to` (ms), in steps as the app's views appear. */
  const during = (r, name, from, to = T.total) => r.custom(name, [[0, `opacity:${from === 0 ? 1 : 0}`], ...(from ? [[from, 'opacity:1']] : []), ...(to < T.total ? [[to, 'opacity:0']] : []), [T.total, `opacity:${to < T.total ? 0 : 1}`]], 'step-end')
  const chat = (r) => {
    const thinking = '<p class="secondary w-thinking">考えています…</p>'
    const asked = `<div class="w-ask">${esc(GUIDE.ask)}</div>`
    const title = GUIDE.ask.slice(0, 22) + '…'
    const column = (head, body, tokens = '0k') => `${head}<div class="w-msgs">${body}</div>${chatInput(tokens)}`
    const nest = (cls) => `<span class="guide-nest${cls}">${guideBird()}</span>`
    const views = [
      [0, T.ask, `${chatHead('新しい会話')}${CHAT_HELLO}${chatInput()}`],
      [T.ask, T.fly, column(chatHead(title, nest(' thinking')), asked + thinking, '1k')],
      [T.fly, T.parts[0], column(chatHead(title, nest(' away')), asked + thinking, '1k')],
      ...GUIDE.answer.map((_, i) => [T.parts[i], T.parts[i + 1] ?? T.total,
        column(chatHead(title, nest(' away')), `${asked}<div class="w-answer">${GUIDE.answer.slice(0, i + 1).join('')}</div>${i === GUIDE.answer.length - 1 ? choices : ''}`, i === GUIDE.answer.length - 1 ? '22k' : '18k')]),
    ]
    return views.map(([from, to, html], i) => `<div class="w-view ${during(r, `cv${i}`, from, to)}">${html}</div>`).join('')
  }
  const over = (r, place) => {
    const b = place(r.cardBox('spectrum'))
    // The perch (perchBox in guide.tsx): beside the Card's top right, as large as the Canvas's zoom (perchScale).
    const k = Math.max(0.45, Math.min(1, r.zoom)), bird = { w: 90 * k, h: 64 * k }
    const right = b.left + b.width + 320 < WIN.width
    const px = right ? b.left + b.width + 18 : b.left - 18, py = b.top + 10
    const perch = { left: px - bird.w / 2, top: py - bird.h }
    // Home (homeBox): the nest in the chat's head, the bird 90 × 64 standing on it.
    const nest = { cx: WIN.width - WIN.chat + 14 + 16, bottom: 48 + 22 + 12 }
    const home = { left: nest.cx - 45, top: nest.bottom - 4 - 64 }
    const fly = r.custom('g-fly', [
      [0, `opacity:0;transform:translate(${home.left - perch.left}px,${home.top - perch.top}px) scale(${1 / k})`],
      [T.fly, `opacity:1;transform:translate(${home.left - perch.left}px,${home.top - perch.top}px) scale(${1 / k});animation-timing-function:ease-in-out`],
      [T.land, 'opacity:1;transform:none'], [T.total, 'opacity:1;transform:none']], 'step-end')
    // The bubble (GuideFlyer): its right edge 30 px past the bird's feet, 12 px below them (the answer is in the chat: the bird faces it).
    const feetX = perch.left + bird.w / 2, feetY = perch.top + bird.h
    const bubble = { left: Math.max(8, feetX - 300 + 30), top: feetY + 12 }
    const pop = r.custom('g-pop', [[0, 'opacity:0;transform:scale(0.92)'], [T.land, 'opacity:0;transform:scale(0.92);animation-timing-function:ease-out'], [T.land + 200, 'opacity:1;transform:none'], [T.total, 'opacity:1;transform:none']], 'step-end')
    return `<div class="w-a ${during(r, 'g-ring', T.land)}" style="left:${b.left}px;top:${b.top}px;width:${b.width}px;height:${b.height}px"><div class="w-ring"></div></div>
<div class="w-a w-fly ${fly}" style="left:${perch.left}px;top:${perch.top}px;width:${bird.w}px;height:${bird.h}px"><div class="guide-flying-bird">${guideBird()}</div></div>
<div class="w-a ${pop}" style="left:${bubble.left}px;top:${bubble.top}px;transform-origin:100% 0">
<div class="guide-speech material"><div class="row gap6"><span class="flex1">${esc(GUIDE.say)}</span>${chromeIcon('xmark', 12, 'var(--secondary)')}</div>
<span class="guide-more">詳しくはチャットに書きました →</span>
<div class="col gap6">${choices}</div>
<div class="guide-reply-field"><span class="w-field secondary">ツグミに返事</span>${chromeIcon('arrow.up.circle.fill', 16, 'var(--secondary)')}</div></div></div>`
  }
  return appWindow({
    data, chat, over,
    frames: [{ ...player.DONE, ms: T.total }],
    title: `${data.title} — ツグミが spectrum の Card を案内する`,
    caption: `吹き出しとチャットの文は README の画像（guide.png、2026-11-04 に Claude が実際に答えたもの）を書き写したもの。飛び方と置き場所は Web UI のガイド（guide.tsx）に合わせています。Canvas は ${data.run.date} の実行（ObsID ${obsidOf(data)}）。`,
    css: `.w-view{position:absolute;inset:0}
.w-msgs{position:absolute;left:0;right:0;top:92px;bottom:80px;display:flex;flex-direction:column;justify-content:flex-end;gap:10px;padding:0 14px 8px;overflow:hidden;font-size:13px;line-height:1.5;border-top:0.5px solid var(--separator)}
.w-ask{align-self:flex-end;max-width:85%;padding:6px 10px;border-radius:10px;background:rgba(255,255,255,.09)}
.w-thinking{margin:0}
.w-answer h4{margin:6px 0 2px;font-size:13px;font-weight:700;color:var(--label)}
.w-answer ol,.w-answer ul{margin:0;padding-left:20px;list-style:revert}.w-answer ul ul{margin:0}
.w-answer p{margin:6px 0}
.w-answer code{font-family:var(--mono);font-size:12px;background:rgba(255,255,255,.08);padding:0 3px;border-radius:3px}
.w-ring{position:absolute;inset:0;outline:2.5px solid var(--tsugumi-rufous);outline-offset:4px;border-radius:8px;animation:guide-ring 1.6s ease-in-out infinite}
.w-fly{transform-origin:0 0;z-index:30}
.w-fly .guide-flying-bird{position:absolute;inset:0;width:100%;height:100%;transition:none;filter:none}
.guide-speech{background:color-mix(in srgb,var(--window-bg) 92%,transparent)}
.guide-choice{display:inline-block}
.w-field{flex:1;padding:3px 7px;border-radius:6px;background:var(--text-bg);border:0.5px solid var(--separator-strong);font-size:12.5px}
.row{display:flex;align-items:center}.col{display:flex;flex-direction:column}.gap6{gap:6px}.wrap{flex-wrap:wrap}.flex1{flex:1;min-width:0}`,
  })
}

// ---- Card settings: reading a Card's code ---------------------------------------------------

/** The code editor's standard colors in dark (EditorTheme.standard in macOS/Sources/App/Settings/AppPreferences.swift). */
const SYNTAX = { comment: '#30d158', string: '#ff453a', number: '#bf5af2', keyword: '#ff375f', function: '#0a84ff', type: '#40c8e0', constant: '#ff9f0a', property: '#64d2ff' }
const PY_KEYWORDS = new Set(['and', 'as', 'assert', 'class', 'def', 'elif', 'else', 'except', 'False', 'for', 'from', 'if', 'import', 'in', 'is', 'lambda', 'None', 'not', 'or', 'pass', 'raise', 'return', 'True', 'try', 'while', 'with', 'yield'])

/** Python in the editor's colors, line by line. The app colors with tree-sitter (TreeSitterHighlighter.swift); these
 *  rules are its regular expressions (CodeEditor.swift) with calls, CONSTANTS, Types and .properties told apart. */
function highlighted(code) {
  const token = /("""[\s\S]*?"""|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')|(#[^\n]*)|\b(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\b|(\.)?\b([A-Za-z_]\w*)\b(\s*\()?/g
  const span = (kind, text) => text.split('\n').map((t) => (t ? `<span style="color:${SYNTAX[kind]}">${esc(t)}</span>` : '')).join('\n')
  let out = '', at = 0, m
  while ((m = token.exec(code))) {
    out += esc(code.slice(at, m.index))
    at = token.lastIndex
    const [all, str, comment, num, dot, word, call] = m
    if (str) out += span('string', str)
    else if (comment) out += span('comment', comment)
    else if (num) out += span('number', num)
    else {
      const kind = PY_KEYWORDS.has(word) ? 'keyword' : call ? 'function' : /^[A-Z][A-Z0-9_]+$/.test(word) ? 'constant' : /^[A-Z]/.test(word) ? 'type' : dot ? 'property' : undefined
      out += (dot ?? '') + (kind ? span(kind, word) : esc(word)) + esc(call ?? '')
    }
    if (!all) token.lastIndex++
  }
  return (out + esc(code.slice(at))).split('\n')
}

/** The scene's moments in ms: the window opens over the Canvas, the code scrolls down to plt.show(). */
const SOURCE_T = { total: 13000, open: 1400, opened: 1750, scroll: 4600, scrolled: 6400 }
const SETTINGS = { left: 120, top: 70, width: 1200, height: 760, side: 160, files: 360, line: 16, header: 52 }

function sourceSvg() {
  const data = casaInWindow()
  const T = SOURCE_T, Z = SETTINGS
  const dir = path.join(ROOT, 'demos', 'casa', 'definitions', 'spectrum')
  const code = fs.readFileSync(path.join(dir, 'src', 'main.py'), 'utf8').replace(/\n$/, '')
  const lines = highlighted(code)
  // The call that puts the figure in the Card's display (the docstring names it too: the last one).
  const show = code.split('\n').findLastIndex((l) => l.trim() === 'plt.show()')
  const view = Z.height - Z.header - 26 - 48
  const visible = Math.floor(view / Z.line)
  const scrollTo = Math.max(0, Math.min(lines.length - visible, show - Math.floor(visible / 2))) * Z.line
  const files = [['folder.fill', 'src', 0, 'var(--accent)'], ['chevron.left.forwardslash.chevron.right', 'main.py', 1, '', true],
    ...fs.readdirSync(dir).filter((f) => !f.startsWith('_card') && !f.startsWith('_meta') && f !== 'src').sort().map((f) => [f.endsWith('.nix') ? 'snowflake' : 'tablecells', f, 0])]
  const over = (r, place) => {
    const b = place(r.cardBox('spectrum'))
    const open = r.custom('s-open', [
      [0, `opacity:0;transform:translate(${b.left - Z.left}px,${b.top - Z.top}px) scale(${(b.width / Z.width).toFixed(3)})`],
      [T.open, `opacity:0;transform:translate(${b.left - Z.left}px,${b.top - Z.top}px) scale(${(b.width / Z.width).toFixed(3)});animation-timing-function:ease-out`],
      [T.opened, 'opacity:1;transform:none'], [T.total - 400, 'opacity:1;transform:none;animation-timing-function:ease-in'], [T.total, 'opacity:0;transform:none']])
    const scroll = r.custom('s-scroll', [[0, 'transform:none'], [T.scroll, 'transform:none;animation-timing-function:ease-in-out'], [T.scrolled, `transform:translateY(-${scrollTo}px)`], [T.total, `transform:translateY(-${scrollTo}px)`]])
    const mark = r.custom('s-mark', [[0, 'opacity:0'], [T.scrolled + 200, 'opacity:0'], [T.scrolled + 500, 'opacity:1'], [T.total, 'opacity:1']])
    const ring = r.custom('s-ring', [[0, 'opacity:0'], [T.open - 600, 'opacity:0'], [T.open - 550, 'opacity:1'], [T.total - 400, 'opacity:1'], [T.total - 350, 'opacity:0'], [T.total, 'opacity:0']], 'step-end')
    const categories = [['general', '一般', 'slider.horizontal.3'], ['nix', 'Nix 定義', 'snowflake'], ['source', 'ソース', 'doc.text'], ['ports', '入出力', 'arrow.left.arrow.right'],
      ['executions', '実行履歴', 'clock.arrow.circlepath'], ['artifacts', '成果物', 'shippingbox'], ['context', '追加コンテキスト', 'text.bubble']]
    return `<div class="w-a ${ring}" style="left:${b.left - 4}px;top:${b.top - 4}px;width:${b.width + 8}px;height:${b.height + 8}px;border:2px solid var(--accent);border-radius:10px"></div>
<div class="w-a s-win ${open}" style="left:${Z.left}px;top:${Z.top}px;width:${Z.width}px;height:${Z.height}px">
<div class="s-title"><span class="w-lights"><i style="background:#ff5f57"></i><i style="background:#febc2e"></i><i style="background:#28c840"></i></span><b>spectrum — ${esc(data.title)}</b></div>
<div class="s-side">${categories.map(([k, t, icon]) => `<div class="s-cat${k === 'source' ? ' on' : ''}">${chromeIcon(icon, 15)}<span>${t}</span></div>`).join('')}</div>
<div class="s-files">${files.map(([icon, name, depth, color, on]) => `<div class="s-file${on ? ' on' : ''}" style="padding-left:${12 + depth * 18}px">${depth ? '' : chromeIcon('chevron.down', 9, 'var(--secondary)')}${chromeIcon(icon, 13, color || 'var(--secondary)')}<span>${esc(name)}</span></div>`).join('')}
<div class="s-gen secondary">生成</div>${['_inputs.fbs', '_params.fbs'].map((f) => `<div class="s-file" style="padding-left:12px">${chromeIcon('doc', 13, 'var(--secondary)')}<span>${f}</span><span class="s-ro">RO</span></div>`).join('')}
<div class="s-plus secondary">${chromeIcon('plus', 12)}${chromeIcon('minus', 12)}</div></div>
<div class="s-code">
<div class="s-head">${chromeIcon('chevron.left.forwardslash.chevron.right', 13)}<b>main.py</b><span class="secondary caption">spectrum › src</span><span style="flex:1"></span><span class="s-btn">整形</span></div>
<div class="s-view" style="height:${view}px"><div class="s-lines ${scroll}">
<div class="s-mark ${mark}" style="top:${show * Z.line}px"></div>
${lines.map((l, i) => `<div class="s-line"><span class="s-no">${i + 1}</span><span class="s-text">${l || ' '}</span></div>`).join('')}
</div></div>
<div class="s-foot"><span class="s-btn">再読込</span><span class="s-btn off">適用</span><span class="s-btn">閉じる</span></div></div>
</div>`
  }
  return appWindow({
    data, over,
    frames: [{ ...player.DONE, ms: T.total }],
    title: `${data.title} — spectrum の Card 設定でソースを読む`,
    caption: `ソースは demos/casa の spectrum の main.py そのもの。設定の窓と色分けはアプリ（CardSettingsEditor・CodeEditor）に合わせた絵です。値を変えてその Card だけ再実行する動きは、記録がないので入れていません。`,
    css: `.s-win{transform-origin:0 0;z-index:40;border-radius:12px;overflow:hidden;background:#1e1e1e;box-shadow:0 22px 70px rgba(0,0,0,.55),0 0 0 1px #4a4a4c;font-size:13px}
.s-title{position:absolute;left:0;right:0;top:0;height:${Z.header}px;display:flex;align-items:center;gap:14px;padding:0 20px;background:#2a2a2c;font-size:13px}
.s-side{position:absolute;left:0;top:${Z.header}px;bottom:0;width:${Z.side}px;padding:8px;background:#2a2a2c;border-right:0.5px solid var(--separator)}
.s-cat{display:flex;align-items:center;gap:10px;height:32px;padding:0 10px;border-radius:7px}.s-cat.on{background:rgba(255,255,255,.1)}
.s-files{position:absolute;left:${Z.side}px;top:${Z.header}px;bottom:0;width:${Z.files}px;padding:8px 6px;border-right:0.5px solid var(--separator)}
.s-file{display:flex;align-items:center;gap:7px;height:22px;border-radius:5px;padding-right:8px}.s-file.on{background:var(--accent);color:#fff}.s-file.on .icon{color:#fff!important}
.s-gen{font-size:11px;font-weight:600;margin:14px 6px 4px}.s-ro{margin-left:auto;font-size:9px;padding:0 4px;border-radius:3px;background:rgba(255,255,255,.12);color:var(--secondary)}
.s-plus{position:absolute;left:10px;bottom:10px;display:flex;gap:12px}
.s-code{position:absolute;left:${Z.side + Z.files}px;right:0;top:${Z.header}px;bottom:0;background:var(--text-bg)}
.s-head{display:flex;align-items:center;gap:8px;height:26px;padding:0 12px;border-bottom:0.5px solid var(--separator)}
.s-btn{display:inline-flex;padding:2px 10px;border-radius:6px;background:rgba(255,255,255,.12);font-size:12px}.s-btn.off{opacity:.45}
.s-view{position:relative;overflow:hidden}
.s-lines{position:relative;font:12.5px/${Z.line}px var(--mono)}
.s-line{display:flex;height:${Z.line}px;white-space:pre}
.s-no{flex:none;width:44px;padding-right:12px;text-align:right;color:var(--tertiary);background:rgba(255,255,255,.03)}
.s-text{padding-left:12px;color:var(--label)}
.s-mark{position:absolute;left:44px;right:0;height:${Z.line}px;background:rgba(10,132,255,.32)}
.s-foot{position:absolute;left:0;right:0;bottom:0;height:48px;display:flex;justify-content:flex-end;align-items:center;gap:10px;padding:0 14px;border-top:0.5px solid var(--separator);background:#232325}`,
  })
}

// ---- Card in Card: inside collatz-cpp ----------------------------------------------------------

/** A Card in Card's inner Workflow as player data: its Cards, wires and layout (read as build.mjs reads a demo), and its
 *  part of the demo's recorded run (the timeline's `<card>/<inner>` entries). */
function innerDemo(demo, card) {
  const root = path.join(ROOT, 'demos', demo)
  const ext = readJson(path.join(root, 'definitions', card, '_link.json')).external
  const inner = path.join(root, 'externals', ext)
  const portsOf = readJson(path.join(DATA, 'assets', 'ports.json'), {})
  const { doc, wires, cards } = readProject(inner, portsOf[demo], `${card}/`)
  const layout = readJson(path.join(inner, 'layout.json'), {})
  for (const c of cards) Object.assign(c, layout.cards?.[c.id] ?? { x: 0, y: 0 })
  const recorded = readJson(path.join(DATA, 'assets', `${demo}.run.json`))
  const timeline = Object.fromEntries(Object.entries(recorded.timeline).filter(([k]) => k.startsWith(`${card}/`)).map(([k, v]) => [k.slice(card.length + 1), v]))
  const steps = stepsFromRun(cards, timeline)
  const outer = readDemo(demo)
  return {
    id: `${demo}-${card}`, title: outer.title, flow: flowOf(layout, cards, wires), draftOnly: false, cards, wires, notes: [], frames: {},
    // The run control shows the whole run's time at its end, as the app does inside a Card in Card.
    run: { ...outer.run, totalMs: outer.run.totalMs }, steps: steps.map((s) => ({ ...s, say: '' })), final: finalsFromRun(cards, timeline), outputs: {},
    boundary: Object.fromEntries(Object.entries(doc.boundary ?? {}).map(([side, ports]) => [side, Object.fromEntries(Object.entries(ports).map(([name, v]) => [name, { ...v,
      type: (side === 'inputs' ? outer.cards.find((c) => c.id === card)?.inputs : outer.cards.find((c) => c.id === card)?.outputs)?.find((p) => p.name === name)?.typeName }]))])),
    outer, card,
  }
}

const SIDEBAR = 250

function subworkflowSvg() {
  const data = innerDemo('collatz', 'collatz-cpp')
  const subgraphs = data.outer.cards.filter((c) => c.kind === 'subgraph').map((c) => c.id)
  // The inner loop ran 112 times in 2.3 s: replayed at about half the player's pace, so the counts can be read.
  const frames = [{ ...player.IDLE, ms: 1800 }, ...player.framesOf(data, 2.4), { ...player.DONE, ms: 5600 }]
  const canvas = { left: SIDEBAR, top: WIN.bar, width: WIN.width - WIN.chat - SIDEBAR, height: WIN.height - WIN.bar }
  const sidebar = (r) => {
    const list = r.perFrame('sb', (f) => player.cardStates(data, f).map((c) => `<div class="b-row">${chromeIcon(c.icon, 13, 'var(--secondary)')}<span class="flex1">${esc(c.id)}</span>${c.state ? `${chromeIcon(c.state.icon, 13, `var(--${c.state.color})`)}<span class="mono-digits secondary">#${c.invocation}</span>` : ''}</div>`).join(''))
    return `<div class="b-side" style="width:${SIDEBAR}px">
<span class="w-lights" style="position:absolute;left:20px;top:20px"><i style="background:#ff5f57"></i><i style="background:#febc2e"></i><i style="background:#28c840"></i></span>
<div class="b-head">Workflow 階層</div>
<div class="b-row b-root">${chromeIcon('chevron.down', 10, 'var(--secondary)')}${chromeIcon('square.stack.3d.up', 14, 'var(--secondary)')}<b>${esc(data.title)}</b></div>
${subgraphs.map((id) => `<div class="b-row b-child${id === data.card ? ' on' : ''}">${chromeIcon('square.stack.3d.up', 14, id === data.card ? 'var(--accent)' : 'var(--secondary)')}<span class="flex1">${esc(id)}</span>${id === data.card ? '<span class="caption secondary">表示中</span>' : ''}</div>`).join('')}
<div class="b-head" style="margin-top:18px">Card を追加</div><div class="b-search secondary">検索</div>
<div class="b-head" style="margin-top:18px">表示中の Card</div>
<div class="b-list">${list}</div></div>`
  }
  return appWindow({
    data, canvas, frames,
    draw: (f, base, view) => player.innerSurface(data, f, base, view, data.boundary),
    sidebarWidth: SIDEBAR,
    over: (r) => sidebar(r),
    tab: `${chromeIcon('chevron.left', 11)}<span>${esc(data.title)}</span>${chromeIcon('chevron.right', 10, 'var(--secondary)')}<span style="color:var(--accent);font-weight:500">${esc(data.card)}</span>`,
    title: `${data.title} — Card in Card（${data.card}）の中の Workflow`,
    caption: `${data.card} の中の Card・線・境界は demos/collatz のもの、各 Card の回数と順序は ${data.run.date} の実行の記録（全体 ${player.formatDuration(data.run.totalMs / 1000)}）。中の collect の出力は記録がないので出していません。`,
    css: `.b-side{position:absolute;left:0;top:0;bottom:0;padding:56px 10px 10px;background:#2a2a2c;border-right:0.5px solid var(--separator);font-size:13px;z-index:5}
.b-head{font-size:11px;font-weight:600;color:var(--secondary);margin:6px 4px}
.b-row{display:flex;align-items:center;gap:8px;height:28px;padding:0 8px;border-radius:6px}
.b-child{padding-left:28px}.b-child.on{font-weight:600}
.b-search{margin:4px;padding:3px 8px;border-radius:6px;background:rgba(255,255,255,.08)}
.b-list{position:relative;height:240px}.b-list>.tp-l{inset:0}
.flex1{flex:1;min-width:0}`,
  })
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
  if (id === LARGE) {
    console.log(`docs/assets/${id}-demo.svg ${kb(write(path.join(OUT, `${id}-demo.svg`), largeSvg(id)))}`)
    console.log(`docs/assets/${id}-window.svg ${kb(write(path.join(OUT, `${id}-window.svg`), windowSvg()))}`)
    console.log(`docs/assets/${id}-timeline.svg ${kb(write(path.join(OUT, `${id}-timeline.svg`), timelineSvg()))}`)
    console.log(`docs/assets/${id}-guide.svg ${kb(write(path.join(OUT, `${id}-guide.svg`), guideSvg()))}`)
    console.log(`docs/assets/${id}-source.svg ${kb(write(path.join(OUT, `${id}-source.svg`), sourceSvg()))}`)
    console.log(`docs/assets/collatz-subworkflow.svg ${kb(write(path.join(OUT, 'collatz-subworkflow.svg'), subworkflowSvg()))}`)
  }
  const n = write(path.join(OUT, 'demos', `${id}.svg`), tileSvg(id))
  tiles += n
  console.log(`docs/assets/demos/${id}.svg ${kb(n)}`)
}
console.log(`${ids.length} tiles, ${kb(tiles)} in all`)
