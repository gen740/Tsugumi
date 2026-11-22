// Check routing after the canvas places and sizes Cards on a grid.
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { CardGeometry, CardNodeData, Pt, Rect, snapToGrid, union, WireRouter } from '../src/model/canvasKit'
import type { FlowDirection } from '../src/model/prefs'

const root = process.argv[2]
type Wire = { a: Pt; b: Pt; name: string }
type Scene = { name: string; flow: FlowDirection; obstacles: Rect[]; wires: Wire[] }
type Layout = { flow?: string; cards?: Record<string, Partial<Pt> & { collapsed?: boolean }>; boundary?: Record<string, Pt> }

const read = (file: string) => (fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, 'utf8')) : undefined)

/** The Card's ports as the core gives them (tsugumid/document/src/graph.cc), read from its files. */
function node(dir: string, id: string): CardNodeData {
  const base = path.join(dir, 'definitions', id)
  const def = read(path.join(base, '_card.json'))
  const meta = read(path.join(base, '_meta.json'))
  const named = (list?: { name?: string }[]) => (list ?? []).flatMap((p) => (p.name ? [{ name: p.name }] : []))
  const fbs = path.join(base, '_outputs.fbs')
  const typed = fs.existsSync(fbs)
    ? [...(/table Outputs\s*\{([^}]*)\}/.exec(fs.readFileSync(fbs, 'utf8').replace(/\/\*.*?\*\//gs, ''))?.[1] ?? '').matchAll(/(\w+)\s*:/g)].map((m) => ({ name: m[1] }))
    : []
  const blank = { summary: def?.description || meta?.description || undefined, params: [], invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false, title: id }
  if (def?.type === 'subgraph') {
    const b = read(path.join(dir, 'externals', def.external, 'tsugumi.json'))?.boundary ?? {}
    return { ...blank, kind: 'subgraph', inputs: Object.keys(b.inputs ?? {}).map((name) => ({ name })), outputs: Object.keys(b.outputs ?? {}).map((name) => ({ name })) }
  }
  const declared = named(def?.inputs)
  switch (def?.type) {
    case 'trigger': return { ...blank, kind: 'trigger', inputs: [], outputs: [{ name: 'out' }] }
    case 'input': {
      const params = (def.params ?? []).filter((p: { type: string }) => ['int64', 'uint64', 'double', 'bool', 'string'].includes(p.type)).map((p: { name: string; type: string }) => ({ ...p, value: '' }))
      return { ...blank, kind: 'input', params, inputs: [{ name: 'start' }], outputs: typed }
    }
    case 'branch': return { ...blank, kind: 'branch', inputs: [{ name: 'value' }, { name: 'condition' }], outputs: [{ name: 'a' }, { name: 'b' }] }
    case 'merge': return { ...blank, kind: 'merge', inputs: declared, outputs: [{ name: 'out' }] }
    case 'collect': return { ...blank, kind: 'collect', inputs: [{ name: 'in' }], outputs: [{ name: 'out' }] }
    case 'code': return { ...blank, kind: 'code', inputs: declared, outputs: typed }
    case 'draft': {
      const untitled = /^draft(-[0-9]+)?$/.test(id)
      return { ...blank, kind: 'draft', summary: meta?.content || blank.summary, untitled, inputs: [], outputs: [] }
    }
    default: throw new Error(`type ${def?.type}`)
  }
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
    return [id, { ...d, collapsed: layout.cards?.[id]?.collapsed ?? false, portGrid: grid }]
  }))
  const geos = new Map(ids.map((id) => [id, new CardGeometry(datas.get(id)!, flow!)]))
  const cardRect = (id: string): Rect => ({ ...place(id), ...geos.get(id)!.size })
  const port = (id: string, p: string, output: boolean): Pt => {
    const o = place(id), q = geos.get(id)!.port(p, output)
    return { x: o.x + q.x, y: o.y + q.y }
  }
  const obstacles = ids.map(cardRect)
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

/** Cards the path enters and leaves again. A branch's port sits inside its box:
 *  a stay that starts or ends the path is the wire reaching its port. */
function crossed(pts: Pt[], obstacles: Rect[]): number {
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

for (const scene of [
  ...scopes('rna-seq on the grid', path.join(root, 'demos/ja/rna-seq'), 20),
]) {
  const routes = new WireRouter(scene.flow, scene.obstacles).routeAll(scene.wires)
  for (const [index, wire] of scene.wires.entries()) {
    const pts = routes[index].pts
    assert.deepEqual(pts[0], wire.a)
    assert.deepEqual(pts.at(-1), wire.b)
    assert.equal(crossed(pts, scene.obstacles), 0, `${scene.name}: ${wire.name}`)
  }
}

if (process.env.WIRE_ROUTES_WRITE || process.env.TSUGUMI_WIRE_BENCH === '1') {
  const index: { demos: { path: string }[] } = read(path.join(root, 'demos/ja/index.json'))
  const demos = index.demos.flatMap((d) => scopes(d.path, path.join(root, 'demos', 'ja', d.path)))
  if (process.env.WIRE_ROUTES_WRITE) {
    const shared = ['report', 'live-stream', 'collatz/collatz-py', 'mnist-classifiers', 'rna-seq']
      .map((name) => demos.find((scene) => scene.name === name)!)
      .concat(
        ...['casa', 'rna-seq'].map((name) => scene(`${name}, grid 20`, path.join(root, 'demos/ja', name), undefined, {}, 20).scene))
    const samples = shared.map(({ name, flow, obstacles, wires }) => ({ name, flow, obstacles, wires: wires.map(({ a, b }) => ({ a, b })) }))
    fs.writeFileSync(path.join(root, 'tests/fixtures/wire-routes.json'), JSON.stringify(samples) + '\n')
  }
  if (process.env.TSUGUMI_WIRE_BENCH === '1') {
    const { wireFrameBudget } = await import('../../../tests/bench/wire-frames')
    wireFrameBudget(demos.sort((a, b) => b.wires.length - a.wires.length)[0], WireRouter)
  }
}
console.log('wire routes: passed')
