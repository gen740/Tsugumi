// Builds the demo player of the documentation site from the Web UI's sources and
// the demos' files:
//   docs/site/js/player.js, docs/site/player.css   the player
//   docs/site/player-data/<id>.json, index.json    each demo's real project, with
//     its recorded run when harvest.py has one (player-data/assets/)
// Needs web/node_modules and web/src/gen (`nix develop --command npm run gen` in web/).
//   node docs/site/player/build.mjs
import fs from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const SITE = path.dirname(HERE)
const ROOT = path.resolve(SITE, '..', '..')
const WEB = path.join(ROOT, 'web')
const DATA = path.join(SITE, 'player-data')
const ASSETS = path.join(DATA, 'assets')
const require = createRequire(path.join(WEB, 'package.json'))
const esbuild = require('esbuild')
const katex = require('katex')
const { marked } = require('marked')

/** Characters of one source file and of a Card's description kept in the data. */
const MAX_SOURCE = 6000

// ---- Reading a project ------------------------------------------------------------------

const readJson = (p, fallback) => (fs.existsSync(p) ? JSON.parse(fs.readFileSync(p, 'utf8')) : fallback)

/** A source file for the panel, cut when long. */
function sourceFile(rel, text) {
  return text.length > MAX_SOURCE ? { path: rel, text: text.slice(0, MAX_SOURCE), cut: text.length } : { path: rel, text }
}

/** A Card's files in the order they are worth reading: its code, its output types, its Nix, its manifest. */
function cardFiles(dir) {
  const rank = (f) => (f.startsWith('src/') ? 0 : f === '_outputs.fbs' ? 1 : f === 'default.nix' ? 2 : f === '_card.json' ? 3 : 4)
  const walk = (d, prefix = '') => fs.readdirSync(d, { withFileTypes: true }).flatMap((e) =>
    (e.isDirectory() ? walk(path.join(d, e.name), `${prefix}${e.name}/`) : [`${prefix}${e.name}`]))
  if (!fs.existsSync(dir)) return []
  return walk(dir).filter((f) => f !== '_meta.json' && f !== '_link.json').sort((a, b) => rank(a) - rank(b) || a.localeCompare(b)).flatMap((f) => {
    const bytes = fs.readFileSync(path.join(dir, f))
    return bytes.includes(0) ? [] : [sourceFile(f, bytes.toString('utf8'))]
  })
}

const KINDS = new Set(['trigger', 'input', 'fork', 'branch', 'merge', 'logger', 'draft'])

/** As definitionKind in web/src/model/session.ts. */
function kindOf(dir, def) {
  if (fs.existsSync(path.join(dir, '_link.json'))) return 'subgraph'
  if (KINDS.has(def?.template)) return def.template
  return def?.language === undefined ? 'draft' : 'code'
}

/** As paramText in web/src/model/session.ts. */
const paramText = (v) => (typeof v === 'string' ? v : typeof v === 'number' || typeof v === 'boolean' ? String(v) : Array.isArray(v) ? JSON.stringify(v) : '')

const INLINE_TYPES = new Set(['int64', 'uint64', 'double', 'bool', 'string'])

function readCard(root, id, ports, wires) {
  const dir = path.join(root, 'definitions', id)
  const def = readJson(path.join(dir, '_card.json'), {})
  const meta = readJson(path.join(dir, '_meta.json'), {})
  const kind = kindOf(dir, def)
  const note = (list, name) => (meta[list] ?? []).find((x) => x.name === name)?.description
  const description = meta.description
  const allParams = (def.params ?? []).filter((p) => p.name && p.type).map((p) => ({ name: p.name, type: p.type, value: paramText(p.data) }))
  // Resolved ports are the core's; a Draft's are its declared ones and its wires (as portData in session.ts).
  const resolved = kind === 'draft' ? undefined : ports?.[id]
  if (kind !== 'draft' && !resolved) throw new Error(`no resolved ports for ${id}: run harvest.py --ports`)
  const inputs = resolved
    ? resolved.inputs.map((p) => ({ name: p.name, typeName: p.rootType || undefined, optional: p.optional || undefined, note: note('inputs', p.name) }))
    : [...new Set([...(def.inputs ?? []).map((i) => i.name), ...wires.filter((w) => w.to === id).map((w) => w.toPort)])].map((name) => ({ name, note: note('inputs', name) }))
  const outputs = resolved
    ? resolved.outputs.map((p) => ({ name: p.name, typeName: p.rootType || undefined, optional: p.optional || undefined, note: note('outputs', p.name) }))
    : [...new Set([...(meta.outputs ?? []).map((o) => o.name), ...wires.filter((w) => w.from === id).map((w) => w.fromPort)])].map((name) => ({ name, note: note('outputs', name) }))
  const summary = kind === 'draft' ? meta.content || description : description
  const details = [description, kind === 'draft' ? meta.content : undefined].filter(Boolean)
  const permissions = []
  if (def.network === true) permissions.push('network')
  if (def.sandbox === false) permissions.push('sandbox_off')
  return {
    id, kind, symbol: def.symbol || undefined, summary: summary || undefined, details, inputs, outputs,
    params: kind === 'input' ? allParams.filter((p) => INLINE_TYPES.has(p.type)) : [], allParams, permissions,
    untitled: (kind === 'draft' && /^draft(-[0-9]+)?$/.test(id)) || undefined,
    files: kind === 'subgraph' ? [] : cardFiles(dir),
  }
}

function readWires(doc) {
  return (doc.canvas?.connections ?? []).map((c) => ({ from: c.from.card, fromPort: c.from.port, to: c.to.card, toPort: c.to.port }))
}

function readProject(root, ports, prefix = '') {
  const doc = readJson(path.join(root, 'tsugumi.json'))
  const wires = readWires(doc)
  const scoped = Object.fromEntries(Object.entries(ports ?? {}).filter(([k]) => k.startsWith(prefix) && !k.slice(prefix.length).includes('/')).map(([k, v]) => [k.slice(prefix.length), v]))
  const cards = doc.canvas.cards.map((c) => {
    const card = readCard(root, c.id, scoped, wires)
    if (card.kind === 'subgraph') {
      const ext = readJson(path.join(root, 'definitions', c.id, '_link.json')).external
      const inner = readProject(path.join(root, 'externals', ext), ports, `${prefix}${c.id}/`)
      card.boundary = inner.doc.boundary ?? {}
      card.inner = inner.cards.map((x) => ({ id: x.id, kind: x.kind, summary: x.summary, files: x.files.filter((f) => f.path !== '_card.json') }))
    }
    return card
  })
  return { doc, wires, cards }
}

// ---- The written project of docs/CardGuide.md ----------------------------------------------

/** Its Python Card, parsed as guide_files() in tests/python/test_docs.py does. */
function guideFiles() {
  const page = fs.readFileSync(path.join(ROOT, 'docs', 'CardGuide.md'), 'utf8')
  const out = {}
  for (const m of page.matchAll(/^`([^`\n]+)`(?:（([^）\n]+)）)?\n\n```\w*\n([\s\S]*?)^```/gm)) {
    if (!m[2]) out[m[1]] = m[3]
  }
  return out
}

function writeGuideProject(dir) {
  fs.rmSync(dir, { recursive: true, force: true })
  for (const [name, text] of Object.entries(guideFiles())) {
    const p = path.join(dir, name)
    fs.mkdirSync(path.dirname(p), { recursive: true })
    fs.writeFileSync(p, text)
  }
  fs.writeFileSync(path.join(dir, 'layout.json'), '{}')
}

// ---- Layout ---------------------------------------------------------------------------------

/** Layers from the start Cards along forward wires (a wire back to a Card already seen does not count). */
function layers(cards, wires) {
  const incoming = new Map(cards.map((c) => [c.id, wires.filter((w) => w.to === c.id).map((w) => w.from)]))
  const layer = new Map()
  const depth = (id, seen = new Set()) => {
    if (layer.has(id)) return layer.get(id)
    if (seen.has(id)) return -1
    seen.add(id)
    const d = Math.max(-1, ...(incoming.get(id) ?? []).map((f) => depth(f, new Set(seen)))) + 1
    layer.set(id, d)
    return d
  }
  for (const c of cards) depth(c.id)
  return layer
}

/** As ProjectSession.flow in session.ts. */
function flowOf(layout, cards, wires) {
  if (layout.flow === 'topToBottom' || layout.flow === 'leftToRight') return layout.flow
  const pos = new Map(cards.map((c) => [c.id, c]))
  let across = 0, down = 0
  for (const w of wires) {
    const a = pos.get(w.from), b = pos.get(w.to)
    if (a && b) { across += Math.abs(b.x - a.x); down += Math.abs(b.y - a.y) }
  }
  return across > down ? 'leftToRight' : 'topToBottom'
}

// ---- Notes -----------------------------------------------------------------------------------

/** Markdown with TeX to HTML at build time: MathML needs neither KaTeX's code nor its fonts on the page. */
function markdownHtml(text) {
  const math = []
  const stash = (tex, displayMode) => {
    math.push(katex.renderToString(tex, { output: 'mathml', displayMode, throwOnError: false }))
    return `@@MATH${math.length - 1}@@`
  }
  const prepared = text.replace(/\$\$([\s\S]+?)\$\$/g, (_, x) => stash(x, true)).replace(/(?<![\\$])\$([^$\n]+?)\$/g, (_, x) => stash(x, false))
  return marked.parse(prepared, { async: false }).replace(/@@MATH(\d+)@@/g, (_, i) => math[Number(i)])
}

// ---- The run --------------------------------------------------------------------------------

/** A top Card's span in a recorded run (a Card in Card: the Cards inside it). */
function spanOf(timeline, id) {
  const entries = Object.entries(timeline).filter(([k]) => k === id || k.startsWith(`${id}/`)).map(([, v]) => v)
  const starts = entries.map((e) => e.startMs).filter((v) => v !== null), ends = entries.map((e) => e.endMs).filter((v) => v !== null)
  return starts.length && ends.length ? { start: Math.min(...starts), end: Math.max(...ends), entries } : undefined
}

/** Steps of the replay from a real run: Cards whose spans overlap ran at the same time. */
function stepsFromRun(cards, timeline) {
  const spans = cards.flatMap((c) => {
    const s = spanOf(timeline, c.id)
    return s ? [{ id: c.id, ...s }] : []
  }).sort((a, b) => a.start - b.start || a.end - b.end)
  const steps = []
  for (const s of spans) {
    const cur = steps.at(-1)
    if (cur && s.start < cur.endMs) {
      cur.cards.push(s.id)
      cur.endMs = Math.max(cur.endMs, s.end)
    } else steps.push({ cards: [s.id], startMs: s.start, endMs: s.end })
  }
  return steps
}

/** Steps from the wires alone, for a demo with no recorded run. */
function stepsFromGraph(cards, wires) {
  const layer = layers(cards, wires)
  const steps = []
  for (const c of cards) (steps[layer.get(c.id)] ??= { cards: [] }).cards.push(c.id)
  return steps.filter(Boolean)
}

function finalsFromRun(cards, timeline) {
  const out = {}
  for (const c of cards) {
    const span = spanOf(timeline, c.id)
    if (!span) continue
    if (c.kind === 'subgraph') {
      const states = span.entries.map((e) => e.lastState)
      // A Card in Card sends what the inner Card behind that output sent (Scene.style).
      const sent = c.outputs.map((p) => p.name).filter((p) => {
        const ep = c.boundary?.outputs?.[p]
        return ep && timeline[`${c.id}/${ep.card}`]?.sent.includes(ep.port)
      })
      out[c.id] = { state: states.includes('failed') ? 'failed' : states.includes('blocked') ? 'blocked' : 'succeeded', count: 1, sent, stages: [], ms: span.end - span.start }
      continue
    }
    const e = timeline[c.id]
    out[c.id] = { state: e.lastState, count: e.count, sent: e.sent, stages: e.stages, ms: e.endMs - e.startMs }
  }
  return out
}

function finalsFromGraph(cards) {
  return Object.fromEntries(cards.map((c) => [c.id, { state: 'succeeded', count: 1, sent: c.outputs.map((p) => p.name), stages: [] }]))
}

function say(steps, narration, cards) {
  const known = Object.keys(narration?.cards ?? {})
  for (const s of steps) {
    const key = known.find((k) => s.cards.includes(k))
    s.say = key ? narration.cards[key] : s.cards.map((id) => cards.find((c) => c.id === id)?.summary ?? id).join(' / ')
  }
}

// ---- One demo --------------------------------------------------------------------------------

function buildDemo(entry, root, portsOf, narrations) {
  const { doc, wires, cards } = readProject(root, portsOf[entry.id])
  const layout = readJson(path.join(root, 'layout.json'), {})
  // A Draft stops the run at it: such a project is shown as a plan.
  const draftOnly = cards.some((c) => c.kind === 'draft')
  const layer = layers(cards, wires)
  for (const c of cards) {
    const p = layout.cards?.[c.id]
    // A project written by hand may have no layout yet: one under the other, as the demos' pitch.
    c.x = p?.x ?? 0
    c.y = p?.y ?? (layer.get(c.id) ?? 0) * 190
  }
  const flow = flowOf(layout, cards, wires)
  const notes = (layout.notes ?? []).filter((n) => !n.scope).map((n) => ({
    id: n.id, x: n.x ?? 0, y: n.y ?? 0, width: n.width ?? 240, height: n.height ?? 120, color: ['yellow', 'blue', 'green', 'pink', 'gray'].includes(n.color) ? n.color : 'yellow',
    fontSize: n.fontSize ?? 13, html: markdownHtml(n.text ?? ''),
  }))
  const recorded = readJson(path.join(ASSETS, `${entry.id}.run.json`))
  const narration = narrations[entry.id] ?? (draftOnly ? narrations.draft : undefined)
  let steps = [], final = {}, run, outputs = {}
  if (!draftOnly) {
    if (recorded?.run?.state === 'succeeded') {
      steps = stepsFromRun(cards, recorded.timeline)
      final = finalsFromRun(cards, recorded.timeline)
      for (const c of cards) {
        for (const x of c.inner ?? []) x.count = recorded.timeline[`${c.id}/${x.id}`]?.count
      }
      outputs = Object.fromEntries(Object.entries(recorded.outputs).filter(([k]) => cards.some((c) => c.id === k)))
      for (const o of Object.values(outputs)) if (o.type === 'text' && o.markdown) o.html = markdownHtml(o.text)
      const { id, state, date, host, sandbox, commit } = recorded.run
      run = { id, state, date, host, sandbox, commit, realOrder: true, totalMs: Math.max(...steps.map((s) => s.endMs)) }
    } else {
      steps = stepsFromGraph(cards, wires)
      final = finalsFromGraph(cards)
    }
    say(steps, narration, cards)
  }
  for (const c of cards) delete c.boundary
  return {
    id: entry.id, title: entry.title, description: entry.description, tags: entry.tags ?? [], flow, draftOnly,
    cards, wires, notes, frames: layout.displays ?? {}, run, steps, final, outputs,
    intro: narration?.intro ?? entry.description ?? '', finale: narration?.finale ?? '',
  }
}

// ---- Styles --------------------------------------------------------------------------------

/** The Web UI's colors for dark, scoped to the player: the site is dark and its rules for
 *  body, h1, pre and the like must not reach the page. */
function scopedStyles() {
  const css = fs.readFileSync(path.join(WEB, 'src', 'styles.css'), 'utf8')
  const block = (selector) => {
    const at = css.indexOf(`${selector} {`)
    if (at < 0) throw new Error(`styles.css has no ${selector}`)
    return css.slice(css.indexOf('{', at) + 1, css.indexOf('}', at))
  }
  const helpers = ['.icon', '.secondary', '.mono', '.mono-digits', '.caption', '.nowrap', '.material', '.capsule', '.btn', '.btn:disabled', '.btn:not(:disabled):active',
    '.btn-small', '.btn-prominent', '.btn-danger', '.btn-borderless', '.btn-borderless:not(:disabled):hover', '.btn-icon', '.label', '.label-text']
  const rules = helpers.map((h) => `.tp-root ${h} {${block(h)}}`).join('\n')
  // canvas.css is all class selectors of its own (.cv-*, .display-*).
  const canvas = fs.readFileSync(path.join(WEB, 'src', 'styles', 'canvas.css'), 'utf8')
  const own = fs.readFileSync(path.join(HERE, 'player.src.css'), 'utf8')
  return `.tp-root {${block(':root')}${block(':root[data-theme="dark"]')}}\n${rules}\n${canvas}\n${own}`
}

// ---- Main -------------------------------------------------------------------------------------

if (!fs.existsSync(path.join(WEB, 'src', 'gen', 'tsugumi'))) {
  console.error('web/src/gen is missing: run `nix develop --command npm run gen` in web/ first')
  process.exit(1)
}

await esbuild.build({
  entryPoints: [path.join(HERE, 'main.tsx')],
  outfile: path.join(SITE, 'js', 'player.js'),
  bundle: true, platform: 'browser', format: 'esm', target: 'es2022', minify: true, jsx: 'automatic',
  nodePaths: [path.join(WEB, 'node_modules')], absWorkingDir: WEB, tsconfig: path.join(WEB, 'tsconfig.json'),
  define: { 'process.env.NODE_ENV': '"production"' }, logLevel: 'warning',
})
const css = await esbuild.transform(scopedStyles(), { loader: 'css', minify: true })
fs.writeFileSync(path.join(SITE, 'player.css'), css.code)

const narrations = readJson(path.join(HERE, 'narration.json'))
const portsOf = readJson(path.join(ASSETS, 'ports.json'), {})
fs.mkdirSync(DATA, { recursive: true })
const guideDir = fs.mkdtempSync(path.join(process.env.TMPDIR ?? '/tmp', 'tsg-guide-'))
writeGuideProject(guideDir)
const entries = [
  ...readJson(path.join(ROOT, 'demos', 'index.json')).demos.map((d) => ({ ...d, root: path.join(ROOT, 'demos', d.path) })),
  { id: 'card-guide', title: 'Card を手で書く', description: 'docs/CardGuide.md の start → settings → multiply。', tags: ['Python', 'ガイド'], root: guideDir },
]
const index = []
for (const e of entries) {
  const demo = buildDemo(e, e.root, portsOf, narrations)
  fs.writeFileSync(path.join(DATA, `${e.id}.json`), JSON.stringify(demo))
  index.push({ id: e.id, title: e.title, description: e.description, tags: demo.tags, draftOnly: demo.draftOnly, recorded: !!demo.run, outputs: Object.keys(demo.outputs) })
}
fs.rmSync(guideDir, { recursive: true, force: true })
fs.writeFileSync(path.join(DATA, 'index.json'), JSON.stringify({ demos: index }))

const kb = (p) => `${(fs.statSync(p).size / 1024).toFixed(0)} KB`
console.log(`player.js ${kb(path.join(SITE, 'js', 'player.js'))}, player.css ${kb(path.join(SITE, 'player.css'))}`)
for (const d of index) console.log(`${d.id}.json ${kb(path.join(DATA, `${d.id}.json`))}${d.recorded ? `  recorded (${d.outputs.join(', ') || 'no display'})` : d.draftOnly ? '  Draft' : '  no recorded run'}`)
