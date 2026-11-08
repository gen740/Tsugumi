// Writes the demos' replays as animated SVGs for places that show only an image, such
// as the README on GitHub: no script runs there, but CSS animation does.
//   docs/assets/casa-demo.svg      Cas A, large: the full player with its run control and narration
//   docs/assets/demos/<id>.svg     every recorded demo as a tile: the gallery's compact player
//   docs/assets/casa-window.svg    Cas A in a drawing of the Mac app's window, laid out as the README's screenshot
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
function replay(data, view, compact, kind = compact ? 'tile' : 'large') {
  if (data.draftOnly) throw new Error(`${data.id} is a Draft: there is no run to replay`)
  const [first, end] = HOLD[kind]
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
    const drawn = xhtml(player.surface(data, f, base, view, compact, kind === 'large'))
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
  /** What a function of the frame draws, each distinct drawing as one layer. */
  const perFrame = (name, draw, style = '') => {
    const drawn = frames.map(draw)
    return [...new Set(drawn)].map((xml, i) => layer(new Set(drawn.flatMap((d, j) => (d === xml ? [j] : []))), `${name}${i}`, xml, style)).join('')
  }
  // The Canvas's zoom, from its dot grid (gridStyle in cardNode.tsx: 20 units apart).
  const zoom = Number(shell.open.match(/background-size:([\d.]+)px/)?.[1] ?? 0) / 20
  return { frames, surface, layer, anim, perFrame, zoom }
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
  const sayLine = r.perFrame('s', (f) => `<p class="tp-say">${esc(f.mode === 'playing' ? data.steps[f.step].say : f.mode === 'done' ? data.finale : data.intro)}</p>`)
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

// ---- The Mac app's window -----------------------------------------------------------------

/** The window of docs/assets/screenshots/workflow.png (macOS/tools/screenshots/readme.ui): 1440 × 900, the chat column on the right. */
const WIN = { width: 1440, height: 900, bar: 52, chat: 344, caption: 30 }
/** Where its Canvas has the Cards, output frames and the note: the screenshot's arrangement (Cards in one row, the spectrum
 *  above, the band table and the RGB image below), with the Cards the demo has now (ciao, and the forks begin and share). */
const WINDOW_LAYOUT = {
  cards: { start: [0, 484], begin: [150, 495], load: [230, 484], ciao: [230, 640], bands: [500, 484], share: [770, 495], rgb: [850, 484], spectrum: [1110, 484] },
  displays: { spectrum: [760, 0, 680, 400], bands: [500, 650, 340, 150], rgb: [880, 650, 420, 440] },
  note: [0, 0, 700, 430],
}

const chromeIcon = (name, size, color) => xhtml(player.icon(name, size, color))

function windowSvg(id) {
  const base = readDemo(id)
  const L = WINDOW_LAYOUT
  const data = {
    ...base,
    cards: base.cards.map((c) => ({ ...c, x: L.cards[c.id]?.[0] ?? c.x, y: L.cards[c.id]?.[1] ?? c.y })),
    frames: Object.fromEntries(Object.entries(L.displays).map(([k, [x, y, width, height]]) => [k, { x, y, width, height }])),
    notes: base.notes.slice(0, 1).map((n) => ({ ...n, x: L.note[0], y: L.note[1], width: L.note[2], height: L.note[3] })),
  }
  const canvas = { width: WIN.width - WIN.chat, height: WIN.height - WIN.bar }
  const r = replay(data, canvas, false, 'window')
  const run = data.run
  // The status bar's run state (StatusBar.swift: 実行: <label>), with the run control's icon for it.
  const state = (f) => (f.mode === 'playing' ? ['play.circle.fill', 'var(--orange)', '実行中'] : f.mode === 'done' ? ['checkmark.circle.fill', 'var(--green)', '成功'] : ['circle', 'var(--secondary)', '待機中'])
  const status = r.perFrame('st', (f) => {
    const [name, color, label] = state(f)
    return `<span class="w-item">${chromeIcon(name, 12, color)}<span>実行: ${label}</span></span>`
  })
  const control = r.perFrame('rc', (f) => xhtml(player.runControl(data, f)))
  const bird = fs.readFileSync(path.join(OUT, 'tsugumi-icon.svg'), 'utf8').replace(/<title>[^<]*<\/title>/, '').replace(/width="1024" height="1024"/, 'width="26" height="26"')
  const body = `<div xmlns="http://www.w3.org/1999/xhtml" class="tp-root svg-root w-root">
<div class="w-canvas">${r.surface}</div>
<div class="w-bar">
  <span class="w-lights"><i style="background:#ff5f57"></i><i style="background:#febc2e"></i><i style="background:#28c840"></i></span>
  <span class="w-round">${chromeIcon('sidebar.left', 16)}</span>
  <span class="w-tab">${esc(data.title)}</span>
  <span class="w-runbox"><span class="w-run">${control}</span><span class="w-item secondary">${chromeIcon('checkmark.seal', 13)}<span>0</span></span><span class="w-sep"></span>${chromeIcon('plus', 14)}</span>
  <span class="w-glass w-right">${chromeIcon('folder', 15)}${chromeIcon('bubble.left.and.bubble.right.fill', 16)}</span>
</div>
<div class="w-zoom w-glass">${chromeIcon('minus.magnifyingglass', 14)}<span class="mono-digits">${Math.round(r.zoom * 100)}%</span>${chromeIcon('plus.magnifyingglass', 14)}${chromeIcon('arrow.up.left.and.down.right.magnifyingglass', 14)}${chromeIcon('square.grid.2x2', 14)}${chromeIcon('number', 14, 'var(--accent)')}</div>
<div class="w-status w-glass">
  <span class="w-slot">${status}</span>
  <span class="w-item">${chromeIcon('checkmark.icloud', 13, 'var(--green)')}<span>保存済み</span></span>
  <span class="w-item">${chromeIcon('point.3.connected.trianglepath.dotted', 13, 'var(--secondary)')}<span class="mono-digits">rev 0</span></span>
  <span class="w-item">${chromeIcon('xmark.circle', 12)}<span>0</span>${chromeIcon('exclamationmark.triangle', 12)}<span>0</span></span>
  <span class="w-item">${chromeIcon('text.alignleft', 14)}${chromeIcon('chart.bar.xaxis', 14)}${chromeIcon('terminal', 14)}</span>
</div>
<div class="w-chat">
  <div class="w-chat-head">${bird}${chromeIcon('list.bullet', 14, 'var(--secondary)')}<b>新しい会話</b><span style="flex:1"></span>${chromeIcon('square.and.pencil', 14, 'var(--secondary)')}${chromeIcon('ellipsis.circle', 14, 'var(--secondary)')}</div>
  <div class="w-chat-hello"><p>ツグミです。Workflow の相談・生成・編集も、使い方の質問もどうぞ。</p><p class="secondary">Card を右クリックして参照に追加、$ で Skill を選べます。</p></div>
  <div class="w-input"><span class="secondary">メッセージ（Return で送信）</span><span class="w-input-row">${chromeIcon('paperclip', 14, 'var(--secondary)')}${chromeIcon('chevron.down', 11, 'var(--secondary)')}<span>claude-sonnet-5-5</span><span style="flex:1"></span>${chromeIcon('arrow.up.circle.fill', 18, 'var(--secondary)')}</span></div>
</div>
</div>`
  const icons = sharedIcons(body)
  const caption = `${run.date} に実際に動かした Cas A の実行の記録（ObsID ${base.cards.find((c) => c.id === 'load').allParams.find((p) => p.name === 'obsid').value}）を、アプリの画面の形で再生しています。窓の枠とチャット欄は静止した絵です。`
  const H = WIN.height + WIN.caption
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${WIN.width} ${H}" width="${WIN.width}" height="${H}" role="img" aria-labelledby="t">
<title id="t">${esc(`${data.title} — Tsugumi のアプリでの実行（${run.date} の実行の記録を再生）`)}</title>
<style>${usedCss(playerCss, body)}
.svg-root{position:absolute;inset:0;display:block}
.w-root{background:#1e1e1e;font-size:12px}
.w-root .icon{vertical-align:middle}
.w-canvas{position:absolute;left:0;top:${WIN.bar}px;width:${canvas.width}px;height:${canvas.height}px}
.w-canvas>.tp-surface{height:100%;border-radius:0}
.w-bar{position:absolute;left:0;top:0;width:${canvas.width}px;height:${WIN.bar}px;display:flex;align-items:center;gap:12px;padding:0 10px 0 20px;background:var(--text-bg)}
.w-lights{display:flex;gap:9px;margin-right:6px}.w-lights i{width:12px;height:12px;border-radius:50%}
.w-glass{background:rgba(58,58,60,.72);border:0.5px solid rgba(255,255,255,.1);border-radius:999px;box-shadow:0 1px 3px rgba(0,0,0,.35)}
.w-round{display:inline-flex;align-items:center;justify-content:center;width:36px;height:36px;border-radius:50%;background:rgba(58,58,60,.72);border:0.5px solid rgba(255,255,255,.1)}
.w-tab{width:380px;height:28px;display:flex;align-items:center;padding:0 12px;border-radius:999px;background:rgba(58,58,60,.55);font-weight:600;font-size:12.5px}
.w-runbox{position:relative;margin-left:auto;display:flex;align-items:center;gap:10px;height:34px;padding:0 12px 0 0;border-radius:999px;background:rgba(58,58,60,.72);border:0.5px solid rgba(255,255,255,.1)}
.w-run{position:relative;display:block;width:232px;height:32px}
.w-run .cv-run{background:none!important;border:0!important;box-shadow:none!important;backdrop-filter:none!important}
.w-sep{width:1px;height:16px;background:var(--separator-strong)}
.w-right{display:flex;align-items:center;gap:12px;height:34px;padding:0 12px;margin-left:72px}
.w-item{display:inline-flex;align-items:center;gap:5px;white-space:nowrap}
.w-zoom{position:absolute;left:10px;bottom:10px;display:flex;align-items:center;gap:8px;height:28px;padding:0 10px}
.w-status{position:absolute;right:${WIN.chat + 10}px;bottom:10px;display:flex;align-items:center;gap:22px;height:30px;padding:0 14px;font-weight:600}
.w-status .w-item:last-child{gap:12px;font-weight:400}
.w-slot{position:relative;display:inline-block;width:92px;height:16px}
.w-chat{position:absolute;right:0;top:0;width:${WIN.chat}px;height:${WIN.height}px;background:#262628;border-left:0.5px solid rgba(255,255,255,.12)}
.w-chat-head{display:flex;align-items:center;gap:10px;height:52px;margin-top:18px;padding:0 14px;font-size:13px}
.w-chat-hello{padding:10px 14px;border-top:0.5px solid var(--separator)}
.w-chat-hello p{margin:6px 0;font-weight:600;line-height:1.45}.w-chat-hello p.secondary{font-weight:400;font-size:11px}
.w-input{position:absolute;left:10px;right:10px;bottom:10px;height:58px;display:flex;flex-direction:column;justify-content:space-between;padding:8px 12px;border-radius:12px;background:#2f2f31;border:0.5px solid rgba(255,255,255,.12);font-size:13px}
.w-input-row{display:flex;align-items:center;gap:8px}
.w-caption{font:12px ${FONT};fill:#8b949e}
${r.anim()}</style>
<defs><clipPath id="win"><rect width="${WIN.width}" height="${WIN.height}" rx="12"/></clipPath>${icons.defs}</defs>
<g clip-path="url(#win)">
<foreignObject width="${WIN.width}" height="${WIN.height}">
${icons.body}
</foreignObject>
</g>
<rect x="0.5" y="0.5" width="${WIN.width - 1}" height="${WIN.height - 1}" rx="12" fill="none" stroke="#4a4a4c"/>
<text class="w-caption" x="4" y="${WIN.height + 21}">${esc(caption)}</text>
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
  if (id === LARGE) {
    console.log(`docs/assets/${id}-demo.svg ${kb(write(path.join(OUT, `${id}-demo.svg`), largeSvg(id)))}`)
    console.log(`docs/assets/${id}-window.svg ${kb(write(path.join(OUT, `${id}-window.svg`), windowSvg(id)))}`)
  }
  const n = write(path.join(OUT, 'demos', `${id}.svg`), tileSvg(id))
  tiles += n
  console.log(`docs/assets/demos/${id}.svg ${kb(n)}`)
}
console.log(`${ids.length} tiles, ${kb(tiles)} in all`)
