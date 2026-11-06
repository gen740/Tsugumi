// The 整列 button: a layered drawing of a scope's graph, the way a person lays
// one out by hand. Wires that can be straight are, what comes after a fork
// stays in its own column, gaps follow the real size of each Card, and output
// frames and notes go in free room near what they belong to.
//
// Everything here is plain arithmetic on the numbers in `ArrangeInput`, in the
// same order as `macOS/Sources/App/Canvas/CanvasArrange.swift`, so the two give
// the same result (tests/fixtures/arrange-contract.json checks it). Nothing
// iterates a map or a set, and sorts are total orders.
import type { FlowDirection } from './prefs'

export interface ArrangePort { name: string; x: number; y: number }
export interface ArrangeCard {
  id: string
  width: number
  height: number
  /** Where each port is on the Card (from `CardGeometry`). */
  inputs: ArrangePort[]
  outputs: ArrangePort[]
}
export interface ArrangeWire { from: string; fromPort: string; to: string; toPort: string }
/** An output frame (`id` is its Card's) or a note. */
export interface ArrangeBox { id: string; width: number; height: number }
export interface ArrangeInput {
  flow: FlowDirection
  /** Where frames, notes and layers go. */
  grid: number
  /** The grid is on: Cards are drawn on it (`placedPosition`), so they are placed on it. */
  snap: boolean
  cards: ArrangeCard[]
  wires: ArrangeWire[]
  frames: ArrangeBox[]
  notes: ArrangeBox[]
  /** Inside a Card in Card: the Cards its inputs go to and its outputs come from, by bar. */
  boundary?: { inputs: { card: string; port: string }[]; outputs: { card: string; port: string }[] }
}
export interface ArrangeAt { id: string; x: number; y: number }
export interface ArrangeResult { cards: ArrangeAt[]; frames: ArrangeAt[]; notes: ArrangeAt[] }

/** Space between Cards side by side, and more than this between layers. */
const SIDE_GAP = 40
const LAYER_GAP = 30
/** Room a wire passing a layer keeps from the Cards and wires beside it. */
const LANE_GAP = 30
/** Room around frames and notes, and around wires they sit next to. */
const BOX_GAP = 20
const WIRE_ROOM = 12
/** What a frame on the other side of its Card costs (it may be free and near). */
const FRAME_OTHER_SIDE = 400
/** How much a straight wire counts: a long wire bending is the worst to read. */
const WEIGHT_FEEDBACK = 1
const WEIGHT_NEAR = 4
const WEIGHT_LONG_END = 8
const WEIGHT_LONG = 32
/** Sweeps of the ordering, and the longest search of the coordinates. */
const ORDER_SWEEPS = 24
const SIMPLEX_LIMIT = 4000
/** A row of unconnected graphs is at least this wide before it wraps. */
const SHELF_MIN = 1200

/** A Card, a bar of a Card in Card, or a point of a wire passing a layer. */
interface Node {
  card: number // index in the input, -1 otherwise
  bar: boolean
  across: number // size across the flow
  along: number // size along it
  frameAcross: number // the output frame that goes with it (0: none)
  frameAlong: number
  inputs: number[] // where its ports are across the flow
  outputs: number[]
  rank: number
  order: number
  at: number // position across the flow
  layerAt: number // position along it
}

/** A wire in flow direction. `feedback`: it runs against the flow. */
interface Edge { from: number; fromSlot: number; to: number; toSlot: number; feedback: boolean }
/** One piece of a wire, between neighbouring layers. */
interface Piece { from: number; fromAt: number; to: number; toAt: number; weight: number }

interface Rect { across: number; along: number; acrossSize: number; alongSize: number }

const roundNear = (v: number) => Math.floor(v + 0.5)
const ceilTo = (v: number, g: number) => Math.ceil(v / g - 1e-9) * g
const floorTo = (v: number, g: number) => Math.floor(v / g + 1e-9) * g
const nearTo = (v: number, g: number) => roundNear(v / g) * g
const tenth = (v: number) => roundNear(v * 10) / 10 + 0

export function arrangeCards(input: ArrangeInput): ArrangeResult {
  const vertical = input.flow === 'topToBottom'
  const grid = input.grid > 0 ? input.grid : 20
  const cardIndex = (id: string) => input.cards.findIndex((c) => c.id === id)
  const slot = (list: ArrangePort[], name: string): number => {
    const i = list.findIndex((p) => p.name === name)
    return i < 0 ? 0 : i
  }

  const nodes: Node[] = input.cards.map((c, i) => {
    const frame = input.frames.find((f) => f.id === c.id)
    const slots = (list: ArrangePort[]) => list.map((p) => (vertical ? p.x : p.y))
    return {
      card: i, bar: false, across: vertical ? c.width : c.height, along: vertical ? c.height : c.width,
      frameAcross: frame ? (vertical ? frame.width : frame.height) : 0, frameAlong: frame ? (vertical ? frame.height : frame.width) : 0,
      inputs: slots(c.inputs), outputs: slots(c.outputs), rank: 0, order: 0, at: 0, layerAt: 0,
    }
  })

  const edges: Edge[] = []
  for (const w of input.wires) {
    const f = cardIndex(w.from), t = cardIndex(w.to)
    if (f < 0 || t < 0 || f === t) continue
    edges.push({ from: f, fromSlot: slot(input.cards[f].outputs, w.fromPort), to: t, toSlot: slot(input.cards[t].inputs, w.toPort), feedback: false })
  }

  // Bars are drawn above and below the inner Cards when the flow runs down:
  // they take part in the order, and wires from them get a lane.
  if (vertical && input.boundary) {
    const addBar = (list: { card: string; port: string }[], top: boolean) => {
      const inner = list.map((e) => ({ card: cardIndex(e.card), port: e.port })).filter((e) => e.card >= 0)
      if (!inner.length) return
      const width = list.length * 110 + 40
      const slots = list.map((_, n) => (width * (n + 1)) / (list.length + 1))
      const bar = nodes.length
      nodes.push({ card: -1, bar: true, across: width, along: 0, frameAcross: 0, frameAlong: 0, inputs: top ? [] : slots, outputs: top ? slots : [], rank: 0, order: 0, at: 0, layerAt: 0 })
      list.forEach((e, n) => {
        const c = cardIndex(e.card)
        if (c < 0) return
        if (top) edges.push({ from: bar, fromSlot: n, to: c, toSlot: slot(input.cards[c].inputs, e.port), feedback: false })
        else edges.push({ from: c, fromSlot: slot(input.cards[c].outputs, e.port), to: bar, toSlot: n, feedback: false })
      })
    }
    addBar(input.boundary.inputs, true)
    addBar(input.boundary.outputs, false)
  }

  // Graphs that have nothing to do with each other are laid out apart.
  const parent = nodes.map((_, i) => i)
  const find = (i: number): number => {
    while (parent[i] !== i) {
      parent[i] = parent[parent[i]]
      i = parent[i]
    }
    return i
  }
  for (const e of edges) {
    const a = find(e.from), b = find(e.to)
    if (a !== b) parent[Math.max(a, b)] = Math.min(a, b)
  }
  const roots: number[] = []
  const members: number[][] = []
  nodes.forEach((_, i) => {
    const r = find(i)
    let k = roots.indexOf(r)
    if (k < 0) {
      k = roots.length
      roots.push(r)
      members.push([])
    }
    members[k].push(i)
  })

  const flowRect = (w: number, h: number): { acrossSize: number; alongSize: number } => (vertical ? { acrossSize: w, alongSize: h } : { acrossSize: h, alongSize: w })
  const worldRect = (x: number, y: number, w: number, h: number): Rect => (vertical ? { across: x, along: y, acrossSize: w, alongSize: h } : { across: y, along: x, acrossSize: h, alongSize: w })
  /** What a Card in Card draws above and below these Cards (`Scene.boundary`). */
  const barRects = (cards: Rect[]): Rect[] => {
    const inputs = input.boundary?.inputs.length ?? 0, outputs = input.boundary?.outputs.length ?? 0
    if ((!inputs && !outputs) || !cards.length) return []
    let box = cards[0]
    for (const r of cards) box = unionRect(box, r)
    const x = vertical ? box.across : box.along, y = vertical ? box.along : box.across
    const w = vertical ? box.acrossSize : box.alongSize, h = vertical ? box.alongSize : box.acrossSize
    const width = Math.max(w, Math.max(inputs, outputs) * 110 + 40)
    const left = x + w / 2 - width / 2
    return [worldRect(left, y - 96, width, 40), worldRect(left, y + h + 56, width, 40)]
  }
  const cardRect = (i: number): Rect => ({ across: nodes[i].at, along: nodes[i].layerAt, acrossSize: nodes[i].across, alongSize: nodes[i].along })

  // Each graph with its output frames, nearest the start of the flow first.
  const blocks = members.map((list) => {
    const graphEdges = edges.filter((e) => list.includes(e.from))
    const lines = new Layered(nodes, graphEdges, list, grid, input.snap).run()
    const cards = list.filter((i) => nodes[i].card >= 0)
    const rects = cards.map(cardRect)
    const room = [...rects, ...barRects(rects)]
    const frames: { id: string; rect: number }[] = []
    const owners = cards.filter((i) => nodes[i].frameAcross > 0).sort((a, b) => nodes[a].rank - nodes[b].rank || nodes[a].order - nodes[b].order)
    for (const i of owners) {
      const box = input.frames.find((f) => f.id === input.cards[nodes[i].card].id)!
      frames.push({ id: box.id, rect: room.length })
      room.push(placeBox(flowRect(box.width, box.height), cardRect(i), true, room, lines, grid))
    }
    let box = room[0]
    for (const r of room) box = unionRect(box, r)
    return { list, frames, room, lines, box }
  })

  // The biggest graph first, the others in rows beside it.
  const blockOrder = blocks.map((_, i) => i).sort((a, b) => blocks[b].list.length - blocks[a].list.length || a - b)
  let area = 0, shelf = SHELF_MIN
  for (const b of blocks) {
    area += (b.box.acrossSize + SIDE_GAP) * (b.box.alongSize + LAYER_GAP)
    shelf = Math.max(shelf, b.box.acrossSize)
  }
  shelf = Math.max(shelf, Math.sqrt(area) * 1.4)
  let rowAcross = 0, rowAlong = 0, rowEnd = 0
  const room: Rect[] = []
  const lines: Line[] = []
  for (const k of blockOrder) {
    const b = blocks[k]
    if (rowAcross > 0 && rowAcross + b.box.acrossSize > shelf) {
      rowAcross = 0
      rowAlong = ceilTo(rowEnd + LAYER_GAP + SIDE_GAP, grid)
    }
    const dc = rowAcross - floorTo(b.box.across, grid), da = rowAlong - floorTo(b.box.along, grid)
    for (const i of b.list) {
      nodes[i].at += dc
      nodes[i].layerAt += da
    }
    for (const r of b.room) {
      r.across += dc
      r.along += da
      room.push(r)
    }
    for (const l of b.lines) lines.push({ a: l.a + dc, b: l.b + da, c: l.c + dc, d: l.d + da })
    rowAcross = ceilTo(rowAcross + b.box.acrossSize + SIDE_GAP * 2, grid)
    rowEnd = Math.max(rowEnd, b.box.along + da + b.box.alongSize)
  }

  // Notes: free room next to everything else.
  const notes: ArrangeAt[] = []
  const at = (id: string, r: Rect): ArrangeAt => ({ id, x: tenth(vertical ? r.across : r.along), y: tenth(vertical ? r.along : r.across) })
  for (const n of input.notes) {
    let box: Rect = room.length ? room[0] : { across: 0, along: 0, acrossSize: 0, alongSize: 0 }
    for (const r of room) box = unionRect(box, r)
    const r = placeBox(flowRect(n.width, n.height), box, false, room, lines, grid)
    room.push(r)
    notes.push(at(n.id, r))
  }

  const cards: ArrangeAt[] = []
  for (const n of nodes) if (n.card >= 0) cards.push(at(input.cards[n.card].id, { across: n.at, along: n.layerAt, acrossSize: 0, alongSize: 0 }))
  const frames: ArrangeAt[] = []
  for (const b of blocks) for (const f of b.frames) frames.push(at(f.id, b.room[f.rect]))
  return { cards, frames, notes }
}

interface Line { a: number; b: number; c: number; d: number } // from (a, b) to (c, d): across, along

const unionRect = (r: Rect, o: Rect): Rect => {
  const lo = Math.min(r.across, o.across), top = Math.min(r.along, o.along)
  return { across: lo, along: top, acrossSize: Math.max(r.across + r.acrossSize, o.across + o.acrossSize) - lo, alongSize: Math.max(r.along + r.alongSize, o.along + o.alongSize) - top }
}

const hits = (r: Rect, o: Rect, gap: number) =>
  r.across < o.across + o.acrossSize + gap && o.across < r.across + r.acrossSize + gap &&
  r.along < o.along + o.alongSize + gap && o.along < r.along + r.alongSize + gap

/** Whether a segment comes within `room` of a rectangle. */
function lineHits(l: Line, r: Rect, room: number): boolean {
  const lo = r.across - room, hi = r.across + r.acrossSize + room, top = r.along - room, bottom = r.along + r.alongSize + room
  // Clipped to the rectangle along its length (Liang-Barsky).
  let t0 = 0, t1 = 1
  const dx = l.c - l.a, dy = l.d - l.b
  const clip = (p: number, q: number): boolean => {
    if (p === 0) return q >= 0
    const t = q / p
    if (p < 0) {
      if (t > t1) return false
      if (t > t0) t0 = t
    } else {
      if (t < t0) return false
      if (t < t1) t1 = t
    }
    return true
  }
  return clip(-dx, l.a - lo) && clip(dx, hi - l.a) && clip(-dy, l.b - top) && clip(dy, bottom - l.b)
}

/** The free place for a box nearest `near` that grows the drawing the least,
 *  on the side frames open to (right of a Card when the flow runs down). */
function placeBox(size: { acrossSize: number; alongSize: number }, near: Rect, frame: boolean, obstacles: Rect[], lines: Line[], grid: number): Rect {
  const { acrossSize, alongSize } = size
  const acrossAt: number[] = [ceilTo(near.across + near.acrossSize + BOX_GAP, grid), floorTo(near.across - acrossSize - BOX_GAP, grid)]
  const alongAt: number[] = [nearTo(near.along, grid), nearTo(near.along + near.alongSize - alongSize, grid)]
  for (const o of obstacles) {
    acrossAt.push(ceilTo(o.across + o.acrossSize + BOX_GAP, grid), floorTo(o.across - acrossSize - BOX_GAP, grid))
    alongAt.push(nearTo(o.along, grid), nearTo(o.along + o.alongSize - alongSize, grid), ceilTo(o.along + o.alongSize + BOX_GAP, grid), floorTo(o.along - alongSize - BOX_GAP, grid))
  }
  for (const l of lines) acrossAt.push(ceilTo(Math.max(l.a, l.c) + WIRE_ROOM, grid), floorTo(Math.min(l.a, l.c) - acrossSize - WIRE_ROOM, grid))
  const sorted = (list: number[]) => list.slice().sort((a, b) => a - b).filter((v, i, s) => i === 0 || v !== s[i - 1])
  const across = sorted(acrossAt), along = sorted(alongAt)
  let box: Rect | undefined
  for (const o of obstacles) box = box ? unionRect(box, o) : { ...o }
  let best: Rect | undefined
  let bestCost = Infinity
  for (const a of along) {
    for (const c of across) {
      const r: Rect = { across: c, along: a, acrossSize, alongSize }
      if (obstacles.some((o) => hits(r, o, BOX_GAP - 1e-6))) continue
      if (lines.some((l) => lineHits(l, r, WIRE_ROOM))) continue
      const gapAcross = Math.max(0, near.across - (c + acrossSize), c - (near.across + near.acrossSize))
      const gapAlong = Math.max(0, near.along - (a + alongSize), a - (near.along + near.alongSize))
      const u = box ? unionRect(box, r) : r
      const growth = box ? u.acrossSize - box.acrossSize + (u.alongSize - box.alongSize) : 0
      // A frame opens to one side of its Card and stays level with it if it can;
      // a note has no Card and goes towards where the flow ends.
      const place = frame ? (c + acrossSize / 2 < near.across + near.acrossSize / 2 ? FRAME_OTHER_SIDE : 0) + 0.1 * Math.abs(a - near.along) : -0.01 * a + 0.001 * c
      const cost = 2 * (gapAcross + gapAlong) + growth + place
      if (cost < bestCost - 1e-9) {
        bestCost = cost
        best = r
      }
    }
  }
  // (Past everything there is always room.)
  return best ?? { across: across[across.length - 1], along: along[0], acrossSize, alongSize }
}

/** One connected graph: layers, the order in them, and the coordinates. */
class Layered {
  private all: Node[]
  private layers: number[][] = []
  private pieces: Piece[] = []
  /** For each wire, the nodes it passes (its two ends and the points between). */
  private chains: { edge: Edge; path: number[] }[] = []

  constructor(nodes: Node[], private edges: Edge[], private list: number[], private grid: number, private snap: boolean) {
    this.all = nodes
  }

  /** Lays the graph out, and says where its wires run. */
  run(): Line[] {
    this.removeCycles()
    this.assignRanks()
    this.addLanes()
    this.orderLayers()
    this.assignAcross()
    this.assignAlong()
    return this.wireLines()
  }

  /** Wires against the flow: those that close a cycle, seen from the sources. */
  private removeCycles() {
    const state = new Map<number, number>()
    const hasIn = new Set<number>()
    for (const e of this.edges) hasIn.add(e.to)
    const visit = (root: number) => {
      const stack: { node: number; next: number }[] = [{ node: root, next: 0 }]
      state.set(root, 1)
      while (stack.length) {
        const top = stack[stack.length - 1]
        let pushed = false
        while (top.next < this.edges.length) {
          const e = this.edges[top.next++]
          if (e.from !== top.node || e.feedback) continue
          const s = state.get(e.to) ?? 0
          if (s === 1) e.feedback = true
          else if (s === 0) {
            state.set(e.to, 1)
            stack.push({ node: e.to, next: 0 })
            pushed = true
            break
          }
        }
        if (!pushed) {
          state.set(top.node, 2)
          stack.pop()
        }
      }
    }
    for (const n of this.list) if (!hasIn.has(n) && !state.has(n)) visit(n)
    for (const n of this.list) if (!state.has(n)) visit(n)
  }

  /** The layer of each node: after everything before it, and sources (or
   *  nodes that give to more than they take) as late as their wires allow. */
  private assignRanks() {
    const n = this.all
    const before = (e: Edge) => (e.feedback ? e.to : e.from)
    const after = (e: Edge) => (e.feedback ? e.from : e.to)
    for (const i of this.list) n[i].rank = 0
    for (let pass = 0; pass <= this.list.length; pass++) {
      let changed = false
      for (const e of this.edges) {
        if (n[after(e)].rank < n[before(e)].rank + 1) {
          n[after(e)].rank = n[before(e)].rank + 1
          changed = true
        }
      }
      if (!changed) break
    }
    for (let pass = 0; pass < this.list.length; pass++) {
      let changed = false
      for (let k = this.list.length - 1; k >= 0; k--) {
        const i = this.list[k]
        if (n[i].bar) continue
        const outs = this.edges.filter((e) => before(e) === i), ins = this.edges.filter((e) => after(e) === i)
        if (!outs.length || ins.length >= outs.length) continue
        let room = Infinity
        for (const e of outs) room = Math.min(room, n[after(e)].rank - 1)
        if (room > n[i].rank) {
          n[i].rank = room
          changed = true
        }
      }
      if (!changed) break
    }
    // The bars stay outside the inner Cards.
    let lo = Infinity, hi = -Infinity
    for (const i of this.list) {
      if (n[i].bar) continue
      lo = Math.min(lo, n[i].rank)
      hi = Math.max(hi, n[i].rank)
    }
    for (const i of this.list) {
      if (!n[i].bar) continue
      n[i].rank = n[i].inputs.length ? hi + 1 : lo - 1
    }
    let first = Infinity
    for (const i of this.list) first = Math.min(first, n[i].rank)
    for (const i of this.list) n[i].rank -= first
  }

  /** Points on the wires that pass a layer, so they have a lane through it. */
  private addLanes() {
    const n = this.all
    let last = 0
    for (const i of this.list) last = Math.max(last, n[i].rank)
    this.layers = Array.from({ length: last + 1 }, () => [])
    for (const i of this.list) this.layers[n[i].rank].push(i)
    for (const e of this.edges) {
      if (e.feedback) {
        this.chains.push({ edge: e, path: [e.from, e.to] })
        continue
      }
      const path = [e.from]
      for (let r = n[e.from].rank + 1; r < n[e.to].rank; r++) {
        const d = n.length
        n.push({ card: -1, bar: false, across: 0, along: 0, frameAcross: 0, frameAlong: 0, inputs: [], outputs: [], rank: r, order: 0, at: 0, layerAt: 0 })
        this.list.push(d)
        this.layers[r].push(d)
        path.push(d)
      }
      path.push(e.to)
      this.chains.push({ edge: e, path })
    }
    this.layers.forEach((l) => l.forEach((id, k) => (n[id].order = k)))
  }

  /** [position in the layer above, port] of each piece between two layers. */
  private links(rank: number): { top: number; topPort: number; bottom: number; bottomPort: number }[] {
    const n = this.all
    const out: { top: number; topPort: number; bottom: number; bottomPort: number }[] = []
    for (const c of this.chains) {
      if (c.edge.feedback) continue
      for (let k = 0; k + 1 < c.path.length; k++) {
        const a = c.path[k], b = c.path[k + 1]
        if (n[a].rank !== rank) continue
        const af = k === 0 ? (c.edge.fromSlot + 0.5) / Math.max(n[a].outputs.length, 1) : 0.5
        const bf = k + 2 === c.path.length ? (c.edge.toSlot + 0.5) / Math.max(n[b].inputs.length, 1) : 0.5
        out.push({ top: n[a].order, topPort: af, bottom: n[b].order, bottomPort: bf })
      }
    }
    return out
  }

  private crossings(): number {
    let total = 0
    for (let r = 0; r + 1 < this.layers.length; r++) {
      const l = this.links(r)
      for (let i = 0; i < l.length; i++) {
        for (let j = i + 1; j < l.length; j++) {
          const top = l[i].top + l[i].topPort - (l[j].top + l[j].topPort)
          const bottom = l[i].bottom + l[i].bottomPort - (l[j].bottom + l[j].bottomPort)
          if ((top < 0 && bottom > 0) || (top > 0 && bottom < 0)) total++
        }
      }
    }
    return total
  }

  /** Barycentres, sweeping down and up, keeping the order with the fewest crossings. */
  private orderLayers() {
    const n = this.all
    const save = () => this.layers.map((l) => l.slice())
    const restore = (s: number[][]) => {
      this.layers = s.map((l) => l.slice())
      this.layers.forEach((l) => l.forEach((id, k) => (n[id].order = k)))
    }
    let best = save()
    let bestCount = this.crossings()
    for (let sweep = 0; sweep < ORDER_SWEEPS && bestCount > 0; sweep++) {
      const down = sweep % 2 === 0
      const count = this.layers.length
      for (let k = 1; k < count; k++) {
        const r = down ? k : count - 1 - k
        this.sortLayer(r, down)
      }
      const c = this.crossings()
      if (c < bestCount) {
        bestCount = c
        best = save()
      }
    }
    restore(best)
    // Neighbours that are better the other way round.
    for (let pass = 0; pass < 8 && bestCount > 0; pass++) {
      let improved = false
      for (const l of this.layers) {
        for (let k = 0; k + 1 < l.length; k++) {
          const a = l[k], b = l[k + 1]
          l[k] = b
          l[k + 1] = a
          n[a].order = k + 1
          n[b].order = k
          const c = this.crossings()
          if (c < bestCount) {
            bestCount = c
            improved = true
          } else {
            l[k] = a
            l[k + 1] = b
            n[a].order = k
            n[b].order = k + 1
          }
        }
      }
      if (!improved) break
    }
  }

  private sortLayer(r: number, down: boolean) {
    const n = this.all
    const l = this.layers[r]
    const key = new Map<number, number>()
    for (const id of l) {
      let sum = 0, count = 0
      for (const c of this.chains) {
        if (c.edge.feedback) continue
        for (let k = 0; k + 1 < c.path.length; k++) {
          const a = c.path[k], b = c.path[k + 1]
          if (down && b === id) {
            const f = k === 0 ? (c.edge.fromSlot + 0.5) / Math.max(n[a].outputs.length, 1) : 0.5
            sum += n[a].order + f
            count++
          } else if (!down && a === id) {
            const f = k + 2 === c.path.length ? (c.edge.toSlot + 0.5) / Math.max(n[b].inputs.length, 1) : 0.5
            sum += n[b].order + f
            count++
          }
        }
      }
      key.set(id, count ? sum / count : n[id].order + 0.5)
    }
    const sorted = l.map((id, k) => ({ id, k, v: key.get(id)! })).sort((a, b) => a.v - b.v || a.k - b.k)
    sorted.forEach((s, k) => {
      l[k] = s.id
      n[s.id].order = k
    })
  }

  /** Positions across the flow: the least (weighted) bending of the wires that
   *  keeps the order and the room in each layer. */
  private assignAcross() {
    const n = this.all
    const ports = (id: number, list: number[], slot: number): number => list[slot] ?? n[id].across / 2
    this.pieces = []
    for (const c of this.chains) {
      const last = c.path.length - 1
      for (let k = 0; k < last; k++) {
        const a = c.path[k], b = c.path[k + 1]
        const fromAt = k === 0 ? ports(a, n[a].outputs, c.edge.fromSlot) : 0
        const toAt = k + 1 === last ? ports(b, n[b].inputs, c.edge.toSlot) : 0
        const lane = (i: number) => k + i > 0 && k + i < last
        const weight = c.edge.feedback ? WEIGHT_FEEDBACK : lane(0) && lane(1) ? WEIGHT_LONG : lane(0) || lane(1) ? WEIGHT_LONG_END : WEIGHT_NEAR
        this.pieces.push({ from: a, fromAt, to: b, toAt, weight })
      }
    }
    // The network: one variable per node, one more per piece of wire, in whole
    // grids when Cards are on it (a wire whose ports are not a whole number of
    // grids apart stays as near as it can).
    const unit = this.snap ? this.grid : 1
    const vars = this.list.slice()
    const index = new Map<number, number>()
    vars.forEach((v, i) => index.set(v, i))
    const arcs: Arc[] = []
    for (const l of this.layers) {
      for (let k = 0; k + 1 < l.length; k++) {
        const a = n[l[k]], b = n[l[k + 1]]
        const lane = a.across === 0 || b.across === 0
        // A Card keeps the room for its frame beside it.
        const frame = a.frameAcross > 0 ? a.frameAcross + BOX_GAP + this.grid : 0
        arcs.push({ from: index.get(l[k])!, to: index.get(l[k + 1])!, min: Math.ceil((a.across + frame + (lane ? LANE_GAP : SIDE_GAP)) / unit - 1e-9), weight: 0 })
      }
    }
    let count = vars.length
    for (const p of this.pieces) {
      const apart = roundNear((p.fromAt - p.toAt) / unit)
      const aux = count++
      arcs.push({ from: aux, to: index.get(p.from)!, min: apart >= 0 ? 0 : -apart, weight: p.weight })
      arcs.push({ from: aux, to: index.get(p.to)!, min: apart >= 0 ? apart : 0, weight: p.weight })
    }
    const solved = solveNetwork(count, arcs)
    for (let i = 0; i < vars.length; i++) n[vars[i]].at = solved[i] * unit
    if (!this.snap) this.settleStraight(index)
  }

  /** Without the grid: wires that came out straight stay exactly straight (ports
   *  are not whole numbers), and each straight run moves as one, most of its
   *  Cards on the lines of the grid. */
  private settleStraight(index: Map<number, number>) {
    const n = this.all
    const parent = this.list.map((_, i) => i)
    const find = (i: number): number => {
      while (parent[i] !== i) {
        parent[i] = parent[parent[i]]
        i = parent[i]
      }
      return i
    }
    const straight = (p: Piece) => p.weight !== WEIGHT_FEEDBACK && n[p.to].at - n[p.from].at === roundNear(p.fromAt - p.toAt)
    const links: Piece[] = this.pieces.filter(straight)
    for (const p of links) {
      const a = find(index.get(p.from)!), b = find(index.get(p.to)!)
      if (a !== b) parent[Math.max(a, b)] = Math.min(a, b)
    }
    const groups: number[][] = this.list.map(() => [])
    this.list.forEach((id, i) => groups[find(i)].push(id))
    for (const group of groups) {
      if (!group.length) continue
      // Exact positions from the first member, following its straight wires.
      const seen = new Set<number>([group[0]])
      const queue = [group[0]]
      while (queue.length) {
        const u = queue.shift()!
        for (const p of links) {
          if (p.from === u && !seen.has(p.to)) {
            n[p.to].at = n[u].at + p.fromAt - p.toAt
            seen.add(p.to)
            queue.push(p.to)
          } else if (p.to === u && !seen.has(p.from)) {
            n[p.from].at = n[u].at + p.toAt - p.fromAt
            seen.add(p.from)
            queue.push(p.from)
          }
        }
      }
      // The shift that puts the most Cards on the grid, the smallest of those.
      let bestShift = 0, bestScore = -1
      const cards = group.filter((id) => n[id].card >= 0)
      for (const id of cards.length ? cards : group) {
        const shift = nearTo(n[id].at, this.grid) - n[id].at
        let score = 0
        for (const o of cards) {
          const v = (n[o].at + shift) / this.grid
          if (Math.abs(v - roundNear(v)) < 1e-6) score += n[o].across
        }
        if (score > bestScore + 1e-9 || (Math.abs(score - bestScore) <= 1e-9 && Math.abs(shift) < Math.abs(bestShift) - 1e-9)) {
          bestScore = score
          bestShift = shift
        }
      }
      for (const id of group) n[id].at += bestShift
    }
  }

  /** Positions along the flow: each layer just past the one before it, and
   *  past the frames of earlier layers that reach it. */
  private assignAlong() {
    const n = this.all
    let at = 0
    const reach: { lo: number; hi: number; end: number }[] = []
    for (const l of this.layers) {
      let size = 0
      let any = false
      for (const id of l) {
        if (n[id].card >= 0) {
          size = Math.max(size, n[id].along)
          any = true
        }
      }
      const spans = (id: number): { lo: number; hi: number }[] => {
        const v = n[id]
        const own = { lo: v.at, hi: v.at + v.across }
        return v.frameAcross > 0 ? [own, { lo: v.at + v.across + BOX_GAP, hi: v.at + v.across + BOX_GAP + v.frameAcross }] : [own]
      }
      for (const id of l) {
        if (n[id].card < 0) continue
        for (const s of spans(id)) {
          for (const r of reach) {
            if (s.lo < r.hi + BOX_GAP && r.lo < s.hi + BOX_GAP) at = Math.max(at, ceilTo(r.end + BOX_GAP, this.grid))
          }
        }
      }
      for (const id of l) {
        n[id].layerAt = at
        const v = n[id]
        if (v.frameAcross > 0) reach.push({ lo: v.at + v.across + BOX_GAP, hi: v.at + v.across + BOX_GAP + v.frameAcross, end: at + v.frameAlong })
      }
      if (any) at = ceilTo(at + size + LAYER_GAP, this.grid)
    }
  }

  private wireLines(): Line[] {
    const n = this.all
    const lines: Line[] = []
    const point = (c: { edge: Edge; path: number[] }, k: number): { across: number; along: number } => {
      const id = c.path[k]
      if (k === 0) return { across: n[id].at + (n[id].outputs[c.edge.fromSlot] ?? n[id].across / 2), along: n[id].layerAt + n[id].along }
      if (k === c.path.length - 1) return { across: n[id].at + (n[id].inputs[c.edge.toSlot] ?? n[id].across / 2), along: n[id].layerAt }
      return { across: n[id].at, along: n[id].layerAt }
    }
    for (const c of this.chains) {
      if (c.edge.feedback) continue
      for (let k = 0; k + 1 < c.path.length; k++) {
        const a = point(c, k), b = point(c, k + 1)
        lines.push({ a: a.across, b: a.along, c: b.across, d: b.along })
      }
    }
    return lines
  }
}

interface Arc { from: number; to: number; min: number; weight: number }

/** Network simplex: minimises the sum of weight * (x[to] - x[from]) with
 *  x[to] - x[from] >= min, over whole numbers. */
export function solveNetwork(count: number, arcs: Arc[]): number[] {
  const x = new Array<number>(count).fill(0)
  // A first solution: every variable as low as its arcs allow.
  for (let pass = 0; pass <= count; pass++) {
    let changed = false
    for (const a of arcs) {
      if (x[a.to] < x[a.from] + a.min) {
        x[a.to] = x[a.from] + a.min
        changed = true
      }
    }
    if (!changed) break
  }
  const slack = (a: Arc) => x[a.to] - x[a.from] - a.min
  // A spanning tree of tight arcs.
  const inTree = new Array<boolean>(count).fill(false)
  const tree: number[] = []
  inTree[0] = true
  let size = 1
  while (size < count) {
    let grew = false
    for (let i = 0; i < arcs.length; i++) {
      const a = arcs[i]
      if (inTree[a.from] !== inTree[a.to] && slack(a) === 0) {
        inTree[a.from] = true
        inTree[a.to] = true
        tree.push(i)
        size++
        grew = true
      }
    }
    if (grew) continue
    let best = -1, bestSlack = Infinity
    for (let i = 0; i < arcs.length; i++) {
      const a = arcs[i]
      if (inTree[a.from] !== inTree[a.to] && slack(a) < bestSlack) {
        best = i
        bestSlack = slack(a)
      }
    }
    if (best < 0) {
      // Not connected (does not happen for one graph): carry on from the next node.
      const next = inTree.indexOf(false)
      inTree[next] = true
      size++
      continue
    }
    const delta = inTree[arcs[best].from] ? bestSlack : -bestSlack
    for (let v = 0; v < count; v++) if (inTree[v]) x[v] += delta
  }
  // Exchanges: an arc of the tree whose cut would gain from growing leaves it.
  const adjacent: number[][] = Array.from({ length: count }, () => [])
  const rebuild = () => {
    for (const l of adjacent) l.length = 0
    for (const t of tree) {
      adjacent[arcs[t].from].push(arcs[t].to)
      adjacent[arcs[t].to].push(arcs[t].from)
    }
  }
  rebuild()
  const side = new Array<boolean>(count).fill(false)
  const flood = (start: number, without: Arc) => {
    side.fill(false)
    side[start] = true
    const stack = [start]
    while (stack.length) {
      const v = stack.pop()!
      for (const w of adjacent[v]) {
        if (side[w]) continue
        if ((v === without.from && w === without.to) || (v === without.to && w === without.from)) continue
        side[w] = true
        stack.push(w)
      }
    }
  }
  for (let step = 0; step < SIMPLEX_LIMIT; step++) {
    let leaving = -1
    for (let k = 0; k < tree.length && leaving < 0; k++) {
      const e = arcs[tree[k]]
      flood(e.from, e) // `side`: the part with the arc's start
      let cut = 0
      for (const a of arcs) {
        if (side[a.from] && !side[a.to]) cut += a.weight
        else if (!side[a.from] && side[a.to]) cut -= a.weight
      }
      if (cut < 0) leaving = k
    }
    if (leaving < 0) break
    let entering = -1, bestSlack = Infinity
    for (let i = 0; i < arcs.length; i++) {
      const a = arcs[i]
      if (!side[a.from] && side[a.to] && slack(a) < bestSlack) {
        entering = i
        bestSlack = slack(a)
      }
    }
    if (entering < 0) break
    for (let v = 0; v < count; v++) if (!side[v]) x[v] += bestSlack
    tree[leaving] = entering
    rebuild()
  }
  let low = Infinity
  for (let i = 0; i < count; i++) low = Math.min(low, x[i])
  return x.map((v) => v - low)
}
