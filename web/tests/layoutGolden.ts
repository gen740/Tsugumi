// Makes tests/fixtures/layout-golden.json from the TypeScript implementation of
// Arrange and Untangle Wires (src/model/arrange.ts, src/model/untangle.ts) before
// they move into tsugumid: what LayoutArrange and LayoutUntangle must answer for
// each scene, to the last digit. `arrangeScene` and `untangleScene` are what
// `ProjectSession.arrange`, `untangle` and `untangleAround` did, on a scene alone.
//
// Run it bundled as web/scripts/test.mjs bundles behavior.ts, with the repository
// root and --write as arguments.
import fs from 'node:fs'
import path from 'node:path'
import { arrangeCards, type ArrangeInput, type ArrangeWire } from '../src/model/arrange'
import { untanglePorts, type UntangleCard } from '../src/model/untangle'
import type { LayoutBar, LayoutCard, LayoutProposal, LayoutScene } from '../src/model/layoutScene'
import type { FlowDirection } from '../src/model/prefs'

const page = globalThis as Record<string, unknown>
page.document ??= { documentElement: { dataset: {}, style: {} }, addEventListener() {} }
page.window ??= globalThis
page.matchMedia ??= () => ({ matches: false, addEventListener() {} })
page.EventSource ??= class { addEventListener() {} close() {} }
const { CardGeometry, snapToGrid } = await import('../src/model/canvasKit')

/** The bars of a Card in Card as fixed Cards, and their wires (`untangledOrders`). */
function withBars(scene: LayoutScene, cards: UntangleCard[], wires: ArrangeWire[], placed: boolean) {
  const ins = scene.inputs?.ends ?? [], outs = scene.outputs?.ends ?? []
  if (!ins.length && !outs.length) return
  const inner = [...cards]
  const left = inner.length ? Math.min(...inner.map((c) => c.x)) : 0, top = inner.length ? Math.min(...inner.map((c) => c.y)) : 0
  const right = inner.length ? Math.max(...inner.map((c) => c.x + c.width)) : 240, bottom = inner.length ? Math.max(...inner.map((c) => c.y + c.height)) : 120
  const width = Math.max(right - left, Math.max(ins.length, outs.length) * 110 + 40), x = (left + right - width) / 2
  for (const output of [false, true]) {
    const bar: LayoutBar | undefined = output ? scene.outputs : scene.inputs, list = output ? outs : ins, id = '\u0000' + (output ? 'outputs' : 'inputs')
    const at = (placed ? bar?.at : undefined) ?? { x, y: output ? bottom + 56 : top - 96 }
    const ports = list.map((e, i) => ({ name: e.name, x: (width * (i + 1)) / (list.length + 1), y: output ? 0 : 40 }))
    cards.push({ id, ...at, width, height: 40, movable: false, inputs: output ? ports : [], outputs: output ? [] : ports })
    for (const e of list) {
      if (e.card && e.port) wires.push(output ? { from: e.card, fromPort: e.port, to: id, toPort: e.name } : { from: id, fromPort: e.name, to: e.card, toPort: e.port })
    }
  }
}

const movable = (c: LayoutCard, x = c.x, y = c.y): UntangleCard => ({ id: c.id, x, y, width: c.width, height: c.height, inputs: c.inputs, outputs: c.outputs, movable: c.reorderable })

export function untangleScene(scene: LayoutScene, around: string[]): LayoutProposal {
  const cards = scene.cards.map((c) => movable(c)), wires = [...scene.wires]
  withBars(scene, cards, wires, true)
  const done = (orders: LayoutProposal['portOrders']): LayoutProposal => ({ cards: [], frames: [], notes: [], portOrders: orders })
  if (!around.length) return done(untanglePorts(scene.flow, cards, wires, scene.frames))
  const moved = new Set(around)
  if (!wires.some((w) => moved.has(w.from) !== moved.has(w.to))) return done([])
  const neighbors = new Set(moved)
  for (const w of wires) {
    if (moved.has(w.from)) neighbors.add(w.to)
    if (moved.has(w.to)) neighbors.add(w.from)
  }
  return done(untanglePorts(scene.flow, cards.map((c) => ({ ...c, movable: c.movable && neighbors.has(c.id) })), wires, scene.frames))
}

export function arrangeScene(scene: LayoutScene): LayoutProposal {
  const input: ArrangeInput = {
    flow: scene.flow, grid: scene.grid, snap: scene.snap, wires: scene.wires,
    cards: scene.cards.map((c) => ({ id: c.id, width: c.width, height: c.height, inputs: c.inputs, outputs: c.outputs })),
    frames: scene.frames.map((f) => ({ id: f.id, width: f.width, height: f.height })),
    notes: scene.notes.map((n) => ({ id: n.id, width: n.width, height: n.height })),
    boundary: { inputs: (scene.inputs?.ends ?? []).map((e) => ({ card: e.card, port: e.port })), outputs: (scene.outputs?.ends ?? []).map((e) => ({ card: e.card, port: e.port })) },
  }
  let placed = arrangeCards(input)
  // Where the canvas then draws them (`placedPosition`).
  const drawn = (v: number) => (scene.snap ? snapToGrid(v, scene.grid) : v)
  const cards = scene.cards.map((c) => { const at = placed.cards.find((p) => p.id === c.id)!; return movable(c, drawn(at.x), drawn(at.y)) })
  const wires = [...scene.wires]
  withBars(scene, cards, wires, false)
  const frames = scene.frames.flatMap((f) => { const at = placed.frames.find((p) => p.id === f.id); return at ? [{ x: at.x, y: at.y, width: f.width, height: f.height }] : [] })
  const orders = untanglePorts(scene.flow, cards, wires, frames)
  if (orders.length) {
    placed = arrangeCards({ ...input, cards: input.cards.map((c) => {
      const o = orders.find((x) => x.id === c.id)
      return !o ? c : { ...c, inputs: c.inputs.map((p, i) => ({ ...p, name: o.inputs[i] })), outputs: c.outputs.map((p, i) => ({ ...p, name: o.outputs[i] })) }
    }) })
  }
  return { ...placed, portOrders: orders }
}

/** A stand-in for tsugumid's two requests, for the sessions of the tests. */
export function referenceClient(s: { client?: unknown }) {
  s.client = {
    layoutArrange: async (scene: LayoutScene) => arrangeScene(scene),
    layoutUntangle: async (scene: LayoutScene, around: string[]) => untangleScene(scene, around),
  }
}

// ---- The vectors ---------------------------------------------------------------------

interface NodeCard { id: string; kind: string; summary: boolean; params: number; inputs: string[]; outputs: string[]; display: boolean; collapsed?: boolean }
interface NodeCase { name: string; flow: FlowDirection; cards: NodeCard[]; wires: ArrangeWire[]; frames: { id: string; width: number; height: number }[]; notes: { id: string; width: number; height: number }[]; boundary?: { inputs: { card: string; port: string }[]; outputs: { card: string; port: string }[] } }
export interface GoldenCase { name: string; request: 'arrange' | 'untangle'; scene: LayoutScene; around?: string[]; expect: LayoutProposal }

/** The scene a canvas has for a graph before anything is placed, from `CardGeometry`. */
export function sceneOf(c: NodeCase, flow: FlowDirection, grid: number | undefined): LayoutScene {
  const bar = (list: { card: string; port: string }[]): LayoutBar => ({ ends: list.map((e, i) => ({ name: 'p' + i, card: e.card, port: e.port })) })
  return {
    flow, grid: grid ?? 20, snap: grid !== undefined, wires: c.wires,
    cards: c.cards.map((n) => {
      const g = new CardGeometry({
        title: n.id, kind: n.kind, summary: n.summary ? 's' : undefined, inputs: n.inputs.map((name) => ({ name })), outputs: n.outputs.map((name) => ({ name })),
        params: Array.from({ length: n.params }, (_, i) => ({ name: 'p' + i, type: 'int64', value: '0' })), invocation: 0n, runContinues: false, errors: [], warnings: [],
        collapsed: n.collapsed ?? false, hasDisplay: n.display, portGrid: grid,
      } as ConstructorParameters<typeof CardGeometry>[0], flow)
      const ports = (names: string[], output: boolean) => names.map((name) => ({ name, ...g.port(name, output) }))
      return { id: n.id, x: 0, y: 0, ...g.size, reorderable: ['code', 'draft', 'input', 'subgraph'].includes(n.kind), inputs: ports(n.inputs, false), outputs: ports(n.outputs, true) }
    }),
    frames: c.frames.map((f) => ({ ...f, x: 0, y: 0 })), notes: c.notes.map((n) => ({ ...n, x: 0, y: 0 })),
    ...(c.boundary ? { inputs: bar(c.boundary.inputs), outputs: bar(c.boundary.outputs) } : {}),
  }
}

const opposite = (f: FlowDirection): FlowDirection => (f === 'topToBottom' ? 'leftToRight' : 'topToBottom')
/** Each graph in its flow with the grid on and off, and in the other flow with the grid on. */
export const variants = (c: NodeCase): { name: string; flow: FlowDirection; grid: number | undefined }[] => [
  { name: `${c.name} ${c.flow} grid20`, flow: c.flow, grid: 20 },
  { name: `${c.name} ${c.flow} off`, flow: c.flow, grid: undefined },
  { name: `${c.name} ${opposite(c.flow)} grid20`, flow: opposite(c.flow), grid: 20 },
]

if (process.argv.includes('--write')) {
  const root = process.argv[2]
  const read = (name: string) => JSON.parse(fs.readFileSync(path.join(root, 'tests/fixtures', name), 'utf8'))
  const demos: NodeCase[] = read('arrange-demos.json'), written: NodeCase[] = read('arrange-cases.json')
  const out: GoldenCase[] = []
  const arrange = (name: string, scene: LayoutScene) => out.push({ name: 'arrange: ' + name, request: 'arrange', scene, expect: arrangeScene(scene) })
  const untangle = (name: string, scene: LayoutScene, around: string[] = []) => out.push({ name: 'untangle: ' + name, request: 'untangle', scene, ...(around.length ? { around } : {}), expect: untangleScene(scene, around) })
  /** Every port that may move, the other way round: wires to untangle. */
  const reversed = (scene: LayoutScene): LayoutScene => ({ ...scene, cards: scene.cards.map((c) => {
    const turn = (list: LayoutCard['inputs']) => list.map((p, i) => ({ ...p, name: list[list.length - 1 - i].name }))
    return c.reorderable ? { ...c, inputs: turn(c.inputs), outputs: turn(c.outputs) } : c
  }) })
  /** A scene as it is drawn after an answer was saved. */
  const drawn = (scene: LayoutScene, p: LayoutProposal): LayoutScene => ({
    ...scene,
    cards: scene.cards.map((c) => {
      const at = p.cards.find((x) => x.id === c.id)!, o = p.portOrders.find((x) => x.id === c.id)
      const named = (list: LayoutCard['inputs'], names: string[] | undefined) => (names ? list.map((q, i) => ({ ...q, name: names[i] })) : list)
      return { ...c, x: scene.snap ? snapToGrid(at.x, scene.grid) : at.x, y: scene.snap ? snapToGrid(at.y, scene.grid) : at.y, inputs: named(c.inputs, o?.inputs), outputs: named(c.outputs, o?.outputs) }
    }),
    frames: scene.frames.map((f) => ({ ...f, ...p.frames.find((x) => x.id === f.id)! })),
    notes: scene.notes.map((n) => ({ ...n, ...p.notes.find((x) => x.id === n.id)! })),
  })

  // The demos and the shapes written by hand: chains, forks and merges, feedback,
  // Cards in Cards, notes and frames, nothing at all, one Card; both directions.
  for (const c of [...demos, ...written]) for (const v of variants(c)) arrange(v.name, sceneOf(c, v.flow, v.grid))
  // Another spacing of the grid.
  for (const c of written) for (const flow of [c.flow, opposite(c.flow)]) arrange(`${c.name} ${flow} grid30`, sceneOf(c, flow, 30))
  for (const c of demos.slice(0, 6)) arrange(`${c.name} ${c.flow} grid30`, sceneOf(c, c.flow, 30))
  // Ports in an order that crosses wires: the second placing has work to do.
  for (const c of [...demos, ...written]) {
    if (!c.cards.some((n) => ['code', 'draft', 'input', 'subgraph'].includes(n.kind) && (n.inputs.length > 1 || n.outputs.length > 1))) continue
    for (const v of variants(c).slice(0, 2)) arrange(`${v.name} reversed ports`, reversed(sceneOf(c, v.flow, v.grid)))
  }
  arrange('one Card topToBottom grid20', sceneOf({ name: '', flow: 'topToBottom', cards: [written.find((c) => c.name === 'isolated')!.cards[0]], wires: [], frames: [], notes: [] }, 'topToBottom', 20))
  arrange('one Card leftToRight off', sceneOf({ name: '', flow: 'leftToRight', cards: [written.find((c) => c.name === 'isolated')!.cards[0]], wires: [], frames: [], notes: [] }, 'leftToRight', undefined))

  // Untangle Wires alone: the cases of the old contract (Cards where they were put).
  const tangles: { name: string; flow: FlowDirection; cards: UntangleCard[]; wires: ArrangeWire[]; extra?: { x: number; y: number; width: number; height: number }[] }[] = read('untangle-contract.json')
  const tangled = tangles.map((c) => ({
    name: `${c.name} ${c.flow}`,
    scene: {
      flow: c.flow, grid: 20, snap: false, wires: c.wires, notes: [],
      cards: c.cards.map((n) => ({ id: n.id, x: n.x, y: n.y, width: n.width, height: n.height, reorderable: n.movable, inputs: n.inputs, outputs: n.outputs })),
      frames: (c.extra ?? []).map((r, i) => ({ id: 'frame' + i, ...r })),
    } satisfies LayoutScene,
  }))
  for (const c of tangled) untangle(c.name, c.scene)
  // The end of a drag: only what moved and what is wired to it.
  for (const c of tangled.filter((x) => x.name.startsWith('tangle') || x.name.startsWith('one Card') || x.name.startsWith('multiple'))) {
    const ids = c.scene.cards.map((n) => n.id)
    untangle(`${c.name} around the first Card`, c.scene, [ids[0]])
    untangle(`${c.name} around two Cards`, c.scene, [ids[1], ids[ids.length - 2]])
    untangle(`${c.name} around every Card`, c.scene, ids)
    untangle(`${c.name} around a Card that is not there`, c.scene, ['nobody'])
  }
  // After arranging, with the ports turned round: Cards, frames and bars as drawn.
  for (const c of [...demos, ...written]) {
    const v = variants(c)[0], scene = sceneOf(c, v.flow, v.grid)
    const after = reversed(drawn(scene, arrangeScene(scene)))
    untangle(`${v.name} arranged, reversed ports`, after)
    if (!c.boundary) continue
    untangle(`${v.name} arranged, around the first Card`, after, [c.cards[0].id])
    const moved = (b: LayoutBar | undefined, x: number, y: number): LayoutBar | undefined => b && { ...b, at: { x, y } }
    untangle(`${v.name} arranged, bars put by hand`, { ...after, inputs: moved(after.inputs, 400.5, -300), outputs: moved(after.outputs, -220, 900) })
  }
  untangle('nothing topToBottom', { flow: 'topToBottom', grid: 20, snap: true, cards: [], wires: [], frames: [], notes: [] })
  untangle('bars around nothing leftToRight', { flow: 'leftToRight', grid: 20, snap: true, cards: [], wires: [], frames: [], notes: [], inputs: { ends: [{ name: 'a', card: '', port: '' }] }, outputs: { ends: [] } })

  fs.writeFileSync(path.join(root, 'tests/fixtures/layout-golden.json'), '[\n' + out.map((c) => JSON.stringify(c)).join(',\n') + '\n]\n')
  const moved = out.filter((c) => c.expect.portOrders.length).length
  console.log(`layout-golden.json: ${out.length} cases (${out.filter((c) => c.request === 'arrange').length} arrange, ${out.filter((c) => c.request === 'untangle').length} untangle; ${moved} reorder ports)`)
}
