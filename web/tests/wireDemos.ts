// Every wire of every bundled demo, placed with the real CardGeometry: how many
// Cards each crosses and how far it wanders, plus the fixed cases Mac and web
// must route to the very same path (tests/fixtures/wire-routes.json).
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { CardGeometry, CardNodeData, Pt, Rect, snapToGrid, union, WireRouter } from '../src/model/canvasKit'
import type { FlowDirection } from '../src/model/prefs'

const root = process.argv[2]
type Wire = { a: Pt; b: Pt; name: string }
type Scene = { name: string; flow: FlowDirection; obstacles: Rect[]; wires: Wire[] }
type Layout = { flow?: string; cards?: Record<string, Partial<Pt> & { collapsed?: boolean }>; displays?: Record<string, Partial<Rect> & { hidden?: boolean }>; boundary?: Record<string, Pt> }

const read = (file: string) => (fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, 'utf8')) : undefined)

/** The Card's ports as the core gives them (core/src/graph.cc), read from its files. */
function node(dir: string, id: string): CardNodeData {
  const base = path.join(dir, 'definitions', id)
  const link = read(path.join(base, '_link.json'))
  const def = read(path.join(base, '_card.json'))
  const meta = read(path.join(base, '_meta.json'))
  const named = (list?: { name?: string }[]) => (list ?? []).flatMap((p) => (p.name ? [{ name: p.name }] : []))
  const fbs = path.join(base, '_outputs.fbs')
  const typed = fs.existsSync(fbs)
    ? [...(/table Outputs\s*\{([^}]*)\}/.exec(fs.readFileSync(fbs, 'utf8').replace(/\/\*.*?\*\//gs, ''))?.[1] ?? '').matchAll(/(\w+)\s*:/g)].map((m) => ({ name: m[1] }))
    : []
  const blank = { summary: def?.description || meta?.description || undefined, params: [], invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false, hasDisplay: false, title: id }
  if (link) {
    const b = read(path.join(dir, 'externals', link.external, 'tsugumi.json'))?.boundary ?? {}
    return { ...blank, kind: 'subgraph', inputs: Object.keys(b.inputs ?? {}).map((name) => ({ name })), outputs: Object.keys(b.outputs ?? {}).map((name) => ({ name })) }
  }
  const declared = named(def?.inputs)
  switch (def?.template) {
    case 'trigger': return { ...blank, kind: 'trigger', inputs: [], outputs: [{ name: 'out' }] }
    case 'input': {
      const params = (def.params ?? []).filter((p: { type: string }) => ['int64', 'uint64', 'double', 'bool', 'string'].includes(p.type)).map((p: { name: string; type: string }) => ({ ...p, value: '' }))
      return { ...blank, kind: 'input', params, inputs: [{ name: 'start' }], outputs: typed }
    }
    case 'fork': {
      const branches: string[] = def.params?.find((p: { name: string }) => p.name === 'branches')?.data ?? ['out1', 'out2']
      return { ...blank, kind: 'fork', inputs: [{ name: 'in' }], outputs: branches.map((name) => ({ name })) }
    }
    case 'branch': return { ...blank, kind: 'branch', inputs: [{ name: 'value' }, { name: 'condition' }], outputs: [{ name: 'a' }, { name: 'b' }] }
    case 'merge': return { ...blank, kind: 'merge', inputs: declared, outputs: [{ name: 'out' }] }
    case 'collect': return { ...blank, kind: 'collect', inputs: [{ name: 'in' }], outputs: [{ name: 'out' }] }
    case 'draft': case undefined: break
    default: throw new Error(`template ${def.template}`)
  }
  if (def?.language === undefined) {
    const outs = [...typed, ...named(meta?.outputs).filter((o) => !typed.some((t) => t.name === o.name))]
    const untitled = /^draft(-[0-9]+)?$/.test(id)
    return { ...blank, kind: 'draft', summary: meta?.content || blank.summary, untitled, inputs: declared, outputs: outs }
  }
  return { ...blank, kind: 'code', inputs: declared, outputs: typed }
}

/** One scope as `Scene` in model/canvas.ts lays it out, before any run. With a
 *  grid, as `placedPosition` and `nodeData` in model/session.ts do: Cards on the
 *  nearest line and sized by it, frames where they were saved. */
function scene(name: string, dir: string, flow: FlowDirection | undefined, moved: Record<string, Pt> = {}, grid?: number): { scene: Scene; flow: FlowDirection } {
  const project = read(path.join(dir, 'tsugumi.json'))
  const layout: Layout = read(path.join(dir, 'layout.json')) ?? {}
  const ids: string[] = project.canvas.cards.map((c: { id: string }) => c.id)
  const saved = (id: string): Pt => moved[id] ?? { x: layout.cards?.[id]?.x ?? 0, y: layout.cards?.[id]?.y ?? 0 }
  const place = (id: string): Pt => {
    const p = saved(id)
    return grid ? { x: snapToGrid(p.x, grid), y: snapToGrid(p.y, grid) } : p
  }
  const conns: { from: { card: string; port: string }; to: { card: string; port: string } }[] = project.canvas.connections
  if (!flow) {
    if (layout.flow === 'topToBottom' || layout.flow === 'leftToRight') flow = layout.flow
    else {
      let across = 0, down = 0
      for (const c of conns) {
        // From where they were saved: the flow does not change with the grid.
        across += Math.abs(saved(c.to.card).x - saved(c.from.card).x)
        down += Math.abs(saved(c.to.card).y - saved(c.from.card).y)
      }
      flow = across > down ? 'leftToRight' : 'topToBottom'
    }
  }
  const datas = new Map(ids.map((id) => {
    const d = node(dir, id)
    const frame = layout.displays?.[id]
    return [id, { ...d, collapsed: layout.cards?.[id]?.collapsed ?? false, hasDisplay: !!frame && frame.hidden !== true, portGrid: grid }]
  }))
  const geos = new Map(ids.map((id) => [id, new CardGeometry(datas.get(id)!, flow!)]))
  const cardRect = (id: string): Rect => ({ ...place(id), ...geos.get(id)!.size })
  const port = (id: string, p: string, output: boolean): Pt => {
    const list = output ? datas.get(id)!.outputs : datas.get(id)!.inputs
    assert.ok(list.some((x) => x.name === p), `${name}: ${id} has no ${output ? 'output' : 'input'} ${p}`)
    const o = place(id), q = geos.get(id)!.port(p, output)
    return { x: o.x + q.x, y: o.y + q.y }
  }
  const obstacles = ids.map(cardRect)
  for (const id of ids) {
    const f = layout.displays?.[id]
    if (!f || f.hidden === true) continue
    const o = place(id), g = geos.get(id)!
    const width = f.width ?? 320, height = f.height ?? 170
    obstacles.push(flow === 'topToBottom'
      ? { x: f.x ?? o.x + g.size.width + 40, y: f.y ?? o.y, width, height }
      : { x: f.x ?? o.x, y: f.y ?? o.y + g.height + 30, width, height })
  }
  const wires: Wire[] = conns.map((c) => ({ a: port(c.from.card, c.from.port, true), b: port(c.to.card, c.to.port, false), name: `${c.from.card}.${c.from.port}>${c.to.card}.${c.to.port}` }))
  const bi = Object.entries<{ card: string; port: string }>(project.boundary?.inputs ?? {}), bo = Object.entries<{ card: string; port: string }>(project.boundary?.outputs ?? {})
  if (bi.length || bo.length) {
    const box = ids.map(cardRect).reduce<Rect | null>((u, r) => (u ? union(u, r) : r), null) ?? { x: 0, y: 0, width: 240, height: 120 }
    const width = Math.max(box.width, Math.max(bi.length, bo.length) * 110 + 40)
    const x = box.x + box.width / 2 - width / 2
    const top = { ...(layout.boundary?.inputs ?? { x, y: box.y - 96 }), width, height: 40 }
    const bottom = { ...(layout.boundary?.outputs ?? { x, y: box.y + box.height + 56 }), width, height: 40 }
    obstacles.push(top, bottom)
    bi.forEach(([n, v], i) => {
      if (ids.includes(v.card)) wires.push({ a: { x: top.x + (width * (i + 1)) / (bi.length + 1), y: top.y + top.height }, b: port(v.card, v.port, false), name: `in.${n}>${v.card}.${v.port}` })
    })
    bo.forEach(([n, v], i) => {
      if (ids.includes(v.card)) wires.push({ a: port(v.card, v.port, true), b: { x: bottom.x + (width * (i + 1)) / (bo.length + 1), y: bottom.y }, name: `${v.card}.${v.port}>out.${n}` })
    })
  }
  return { scene: { name, flow, obstacles, wires }, flow }
}

/** A demo's top scope and the scopes of its Cards in Card. */
function scopes(name: string, dir: string, grid?: number): Scene[] {
  const top = scene(name, dir, undefined, {}, grid)
  const out = [top.scene]
  const externals = path.join(dir, 'externals')
  if (fs.existsSync(externals)) for (const e of fs.readdirSync(externals).sort()) out.push(scene(`${name}/${e}`, path.join(externals, e), top.flow, {}, grid).scene)
  return out
}

/** `report` with share directly above the column of generate, aggregate, render and archive. */
function stacked(shareX: number): Scene {
  const dir = path.join(root, 'demos/ja/report')
  return scene(`report stacked, share at ${shareX}`, dir, undefined, {
    start: { x: 0, y: -190 }, settings: { x: 0, y: 0 }, share: { x: shareX, y: 285 },
    generate: { x: 0, y: 380 }, aggregate: { x: 0, y: 570 }, render: { x: 0, y: 760 }, archive: { x: 0, y: 950 },
  }).scene
}

/** `report` with its fork lowered, so that its output is `gap` above generate's input. */
function forkNearGenerate(gap: number): Scene {
  return scene(`report, fork ${gap} above generate`, path.join(root, 'demos/ja/report'), undefined, { share: { x: 300, y: 380 - gap - 40 } }).scene
}

/** `casa` with the box of its fork ending just where the next Card begins, on a grid of 20. */
function forkAgainstCard(): Scene {
  return scene('casa, fork against rgb', path.join(root, 'demos/ja/casa'), undefined, { share: { x: 840, y: 40 }, rgb: { x: 940, y: 0 }, spectrum: { x: 1200, y: 0 } }, 20).scene
}

const length = (pts: Pt[]) => pts.slice(1).reduce((s, p, i) => s + Math.hypot(p.x - pts[i].x, p.y - pts[i].y), 0)

/** Cards the path enters and leaves again. A branch's port sits inside its box:
 *  a stay that starts or ends the path is the wire reaching its port. */
export function crossed(pts: Pt[], obstacles: Rect[]): number {
  const dense: Pt[] = [pts[0]]
  for (let i = 1; i < pts.length; i++) {
    const n = Math.max(1, Math.ceil(Math.hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y) / 2))
    for (let k = 1; k <= n; k++) dense.push({ x: pts[i - 1].x + ((pts[i].x - pts[i - 1].x) * k) / n, y: pts[i - 1].y + ((pts[i].y - pts[i - 1].y) * k) / n })
  }
  let count = 0
  for (const o of obstacles) {
    const inside = dense.map((p) => p.x > o.x + 2 && p.x < o.x + o.width - 2 && p.y > o.y + 2 && p.y < o.y + o.height - 2)
    for (let i = 0; i < inside.length; i++) {
      if (!inside[i]) continue
      let j = i
      while (j + 1 < inside.length && inside[j + 1]) j++
      if (i > 0 && j < inside.length - 1) count++
      i = j
    }
  }
  return count
}

const route = (s: Scene) => new WireRouter(s.flow, s.obstacles).routeAll(s.wires)

function wireCrossings(routes: { pts: Pt[] }[], wires: Wire[]): number {
  const hit = (p: Pt, q: Pt, r: Pt, s: Pt) => {
    const d = (q.x - p.x) * (s.y - r.y) - (q.y - p.y) * (s.x - r.x)
    if (!d) return false
    const t = ((r.x - p.x) * (s.y - r.y) - (r.y - p.y) * (s.x - r.x)) / d
    const u = ((r.x - p.x) * (q.y - p.y) - (r.y - p.y) * (q.x - p.x)) / d
    return t > 0 && t < 1 && u > 0 && u < 1
  }
  let n = 0
  for (let i = 0; i < routes.length; i++) {
    for (let j = i + 1; j < routes.length; j++) {
      if (wires[i].a.x === wires[j].a.x && wires[i].a.y === wires[j].a.y) continue
      const a = routes[i].pts, b = routes[j].pts
      search: for (let x = 1; x < a.length; x++) {
        for (let y = 1; y < b.length; y++) {
          if (hit(a[x - 1], a[x], b[y - 1], b[y])) {
            n++
            break search
          }
        }
      }
    }
  }
  return n
}

const index: { demos: { path: string }[] } = read(path.join(root, 'demos/ja/index.json'))
const demos = index.demos.flatMap((d) => scopes(d.path, path.join(root, 'demos', 'ja', d.path)))
const arrangements = [-60, 20, 60, 100, 150, 200].map(stacked)
const everything = [...demos, ...arrangements, forkNearGenerate(20), forkNearGenerate(-20)]

/** Prints how the wires of these scenes run, and gives those that cross a Card. */
function measure(label: string, scenes: Scene[]): string[] {
  let wires = 0, crossings = 0, between = 0
  const detours: number[] = []
  const crossers: string[] = []
  for (const s of scenes) {
    const routes = route(s)
    between += wireCrossings(routes, s.wires)
    s.wires.forEach((w, i) => {
      wires++
      const c = crossed(routes[i].pts, s.obstacles)
      if (c) crossers.push(`${s.name}: ${w.name} (${c})`)
      crossings += c
      const straight = Math.hypot(w.b.x - w.a.x, w.b.y - w.a.y)
      if (straight > 1) detours.push(length(routes[i].pts) / straight)
    })
  }
  detours.sort((p, q) => p - q)
  console.log(`wire routes, ${label}:`, JSON.stringify({
    scenes: scenes.length, wires, crossingWires: crossers.length, cardCrossings: crossings, wireCrossings: between,
    detourMean: +(detours.reduce((t, d) => t + d, 0) / detours.length).toFixed(3),
    detourP95: +detours[Math.floor(detours.length * 0.95)].toFixed(2), detourMax: +detours[detours.length - 1].toFixed(2),
  }))
  return crossers
}

// ---- Every wire of every demo --------------------------------------------------------
assert.deepEqual(measure('grid off', everything), [], 'no wire crosses a Card')
// The grid is on by default, and it moves and resizes every Card: as the app draws them.
const grids = [20, 30].map((grid) => ({ grid, scenes: index.demos.flatMap((d) => scopes(`${d.path}, grid ${grid}`, path.join(root, 'demos', 'ja', d.path), grid)) }))
for (const { grid, scenes } of grids) assert.deepEqual(measure(`grid ${grid}`, scenes), [], `no wire crosses a Card on a grid of ${grid}`)
assert.deepEqual(measure('fork against a Card', [forkAgainstCard()]), [], 'a wire out of a fork does not go on through the Card that begins where its box ends')

// The wires of the report asked for: short, in the free bands.
{
  const report = demos.find((s) => s.name === 'report')!
  const routes = route(report)
  const named = (n: string) => {
    const i = report.wires.findIndex((w) => w.name === n)
    assert.ok(i >= 0, n)
    return { wire: report.wires[i], pts: routes[i].pts }
  }
  const config = named('settings.config>share.in')
  assert.ok(length(config.pts) < 500, `settings to the fork is short: ${length(config.pts)}`)
  const render = named('share.render>render.config')
  assert.ok(length(render.pts) / Math.hypot(render.wire.b.x - render.wire.a.x, render.wire.b.y - render.wire.a.y) < 1.5, 'share to render takes no long way')
  for (const s of arrangements) {
    const i = s.wires.findIndex((w) => w.name === 'share.render>render.config')
    const w = route(s)[i].pts
    assert.ok(length(w) < 1000, `${s.name}: beside the column, not far: ${length(w)}`)
  }
  for (const gap of [20, -20]) {
    const s = forkNearGenerate(gap)
    const i = s.wires.findIndex((w) => w.name === 'share.generate>generate.config')
    const w = s.wires[i], pts = route(s)[i].pts
    assert.ok(length(pts) < Math.abs(w.a.x - w.b.x) + Math.abs(w.a.y - w.b.y) + 200, `${s.name}: straight left: ${length(pts)}`)
    assert.ok(!pts.some((p) => p.x < 0), `${s.name}: does not go around generate`)
  }
}

// ---- Order of the wires, and the same input twice --------------------------------------
for (const s of demos.filter((x) => x.name === 'live-stream' || x.name.startsWith('collatz/') || x.name === 'mnist-classifiers')) {
  const once = route(s)
  assert.deepEqual(route(s), once, `${s.name}: the same input gives the same paths`)
  const reversed = route({ ...s, wires: [...s.wires].reverse() }).reverse()
  assert.deepEqual(reversed, once, `${s.name}: the order of the wires changes nothing`)
}

// ---- The time of one frame -------------------------------------------------------------
{
  const big = [...demos].sort((a, b) => b.wires.length - a.wires.length)[0]
  const t0 = performance.now()
  const frames = 200
  for (let f = 0; f < frames; f++) {
    // As while dragging: the first Card moves, so nothing can be kept from the frame before.
    const obstacles = big.obstacles.map((r, i) => (i ? r : { ...r, x: r.x + f }))
    new WireRouter(big.flow, obstacles).routeAll(big.wires)
  }
  const perFrame = (performance.now() - t0) / frames
  console.log(`wire routes: ${big.name}, ${big.wires.length} wires, ${perFrame.toFixed(3)} ms per frame`)
  assert.ok(perFrame < 25, `routing keeps up with a frame: ${perFrame} ms`)
}

// ---- The cases Mac and web must route alike ---------------------------------------------
type Case = { name: string; flow: FlowDirection; obstacles: Rect[]; wires: { a: Pt; b: Pt }[]; paths: (Pt[] | null)[] }
const shared = ['report', 'live-stream', 'collatz/collatz-py', 'mnist-classifiers', 'rna-seq'].map((n) => demos.find((s) => s.name === n)!)
  .concat(stacked(60), stacked(150), forkNearGenerate(20), forkAgainstCard(), ...grids[0].scenes.filter((s) => s.name === 'casa, grid 20' || s.name === 'rna-seq, grid 20'))
const routed = (s: { flow: FlowDirection; obstacles: Rect[]; wires: { a: Pt; b: Pt }[] }) => {
  const routes = new WireRouter(s.flow, s.obstacles).routeAll(s.wires)
  return routes.map((r) => (r.curved ? null : r.pts))
}
const fixture = path.join(root, 'tests/fixtures/wire-routes.json')
// The scenes are samples kept in the file, so moving a Card of a demo does not
// change this check. WIRE_ROUTES_WRITE=1 npm test takes new samples from the demos.
if (process.env.WIRE_ROUTES_WRITE) {
  const samples: Case[] = shared.map((s) => ({ name: s.name, flow: s.flow, obstacles: s.obstacles, wires: s.wires.map(({ a, b }) => ({ a, b })), paths: routed(s) }))
  fs.writeFileSync(fixture, JSON.stringify(samples) + '\n')
}
const cases: Case[] = read(fixture)
assert.ok(cases.length > 0)
for (const c of cases) assert.deepEqual(routed(c), c.paths, `the paths Mac also has to find: ${c.name} (a change to the router on purpose: WIRE_ROUTES_WRITE=1 npm test)`)
console.log('wire routes: passed')
