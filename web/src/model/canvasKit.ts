// The Card canvas building blocks shared by the main canvas, the appearance
// preview in Settings and the preview in Card settings: Card data, geometry
// and wire routing (world units; drawing multiplies by the zoom).
import { D } from '../shared/rpc'
import type { FlowDirection } from './prefs'
import type { CardKind } from './session'
import type { PermissionKind } from './permissions'

export type Pt = { x: number; y: number }
export type Rect = { x: number; y: number; width: number; height: number }

/** One execution stage of an invocation, in the order the core runs them. */
export enum Stage { waiting, prepare, evaluate, build, execute, validate, save, done }

export const STAGE_INFO: Record<Stage, { label: string; active: string; icon: string; activeIcon: string; stream: D.LogStream }> = {
  [Stage.waiting]: { label: '入力待ち', active: '入力待ち', icon: 'tray.and.arrow.down', activeIcon: 'tray.and.arrow.down', stream: D.LogStream.journal },
  [Stage.prepare]: { label: 'SDK 準備', active: 'SDK 準備中', icon: 'shippingbox', activeIcon: 'shippingbox', stream: D.LogStream.build },
  [Stage.evaluate]: { label: 'Nix 評価', active: 'Nix 評価中', icon: 'snowflake', activeIcon: 'snowflake', stream: D.LogStream.build },
  [Stage.build]: { label: 'build', active: 'build 中', icon: 'hammer', activeIcon: 'hammer', stream: D.LogStream.build },
  [Stage.execute]: { label: 'run', active: '実行中', icon: 'play', activeIcon: 'play.fill', stream: D.LogStream.stdout },
  [Stage.validate]: { label: 'validate', active: '出力を検証中', icon: 'checkmark.shield', activeIcon: 'checkmark.shield', stream: D.LogStream.journal },
  [Stage.save]: { label: '保存', active: '保存中', icon: 'externaldrive', activeIcon: 'externaldrive', stream: D.LogStream.journal },
  [Stage.done]: { label: '完了', active: '完了', icon: 'flag.checkered', activeIcon: 'flag.checkered', stream: D.LogStream.journal },
}

export function stageOfPhase(p: D.Phase): Stage | undefined {
  switch (p) {
    case D.Phase.prepare: return Stage.prepare
    case D.Phase.evaluate: return Stage.evaluate
    case D.Phase.build: return Stage.build
    case D.Phase.execute: return Stage.execute
    case D.Phase.validate: return Stage.validate
    case D.Phase.save: return Stage.save
    default: return undefined
  }
}

/** Stages shown for a Card, in order: build (SDK preparation, Nix
 *  evaluation and build), run, validate (output check and saving). */
export const applicableStages = (code: boolean): Stage[] => (code ? [Stage.build, Stage.execute, Stage.validate] : [Stage.execute, Stage.validate])

/** The core's stages a shown stage stands for. */
export function stageParts(s: Stage): Stage[] {
  if (s === Stage.build) return [Stage.prepare, Stage.evaluate, Stage.build]
  if (s === Stage.validate) return [Stage.validate, Stage.save]
  return [s]
}

export type StageStatus = 'pending' | 'running' | 'done' | 'cached' | 'reused' | 'failed' | 'cancelled' | 'notRun'
export const STAGE_STATUS_LABEL: Record<StageStatus, string> = {
  pending: '未開始', running: '進行中', done: '完了', cached: 'キャッシュを利用（処理なし）',
  reused: 'この Run で準備済みのものを再利用', failed: '失敗', cancelled: '停止', notRun: '実行されませんでした',
}

export interface StageItem {
  stage: Stage
  status: StageStatus
  seconds?: number
}

export interface StageProgress {
  items: StageItem[]
  workerReused: boolean
}

export const currentStage = (p: StageProgress | undefined) => p?.items.find((i) => i.status === 'running')

/** The status of a shown stage from the core's stages it covers. */
export function mergeStageStatus(parts: StageStatus[], ended: boolean): StageStatus {
  if (parts.includes('failed')) return 'failed'
  if (parts.includes('cancelled')) return 'cancelled'
  if (parts.includes('running')) return 'running'
  const reached = parts.filter((p) => p !== 'pending' && p !== 'notRun')
  if (!reached.length) return parts[0] ?? 'pending'
  // Part of it done, the rest still to come: in progress while the call runs.
  if (reached.length < parts.length) return ended ? (reached.includes('done') ? 'done' : reached[0]) : 'running'
  if (reached.includes('done')) return 'done'
  return reached.every((p) => p === 'reused') ? 'reused' : 'cached'
}

export interface PortData {
  name: string
  typeName?: string
  optional?: boolean
  note?: string
}

export interface ParamData {
  name: string
  type: string
  value: string
}

/** Everything a Card view needs, with no dependency on a session. */
/** What a wire names to end at a Draft's next port (`session.connect` makes the port). */
export const NEW_PORT = '+'

export interface CardNodeData {
  title: string
  kind: CardKind
  symbol?: string
  /** Short description (one line on the Card, a Draft's content in a few;
   *  the whole text on hover). */
  summary?: string
  inputs: PortData[]
  outputs: PortData[]
  params: ParamData[]
  result?: string
  state?: D.CardState
  stage?: StageProgress
  /** Invocation number in the current run (0: not run in this run). */
  invocation: bigint
  /** The run is still going on (a succeeded Card may run again). */
  runContinues: boolean
  failure?: string
  errors: string[]
  warnings: string[]
  collapsed: boolean
  /** The Card's result has a frame on the canvas (display anchor). */
  hasDisplay: boolean
  /** What it may do beyond computing (network, no sandbox, host folders). */
  permissions?: PermissionKind[]
  /** A Draft being written on the canvas: the Card opens up for it. */
  draftEditing?: boolean
  /** A Draft without a name of its own: its content is all it shows. */
  untitled?: boolean
}

export const isRouting = (d: CardNodeData) => d.kind === 'fork' || d.kind === 'branch' || d.kind === 'merge'

export type CardShape = 'card' | 'branch' | 'fork' | 'merge' | 'start'
export const shapeOf = (k: CardKind): CardShape => (k === 'trigger' ? 'start' : k === 'branch' || k === 'fork' || k === 'merge' ? k : 'card')

/** Size, outline and port positions of a Card (world units, relative to its
 *  top-left corner). Shared by drawing, wires, the selection and hit-tests. */
export class CardGeometry {
  static readonly width = 220
  static readonly header = 32
  static readonly summaryRow = 15
  static readonly portRow = 15
  static readonly paramRow = 26
  static readonly resultRow = 18
  static readonly stageRow = 22
  /** A Draft's content on the Card (three lines), and while it is being written. */
  static readonly draftRow = 50
  static readonly draftEditRow = 120
  /** What is left of the header over the content of a Draft without a name. */
  static readonly untitledTop = 8
  /** Labels narrower than this are hidden (shown on hover instead). */
  static readonly minLabel = 26

  readonly inputs: string[]
  readonly outputs: string[]
  readonly collapsed: boolean
  readonly summary: boolean
  readonly params: number
  readonly result: boolean
  readonly stages: boolean
  readonly shape: CardShape
  readonly draft: boolean
  readonly draftEditing: boolean
  readonly untitled: boolean

  constructor(d: CardNodeData, readonly flow: FlowDirection) {
    this.inputs = d.inputs.map((p) => p.name)
    this.outputs = d.outputs.map((p) => p.name)
    this.collapsed = d.collapsed
    this.draft = d.kind === 'draft'
    this.draftEditing = this.draft && !!d.draftEditing
    this.untitled = this.draft && !!d.untitled && !this.draftEditing && !d.collapsed
    this.summary = d.summary !== undefined && !isRouting(d) && !this.draft
    this.params = d.params.length
    // Rows are reserved by kind, so a run never makes a Card grow or shrink.
    this.result = d.kind === 'logger' || d.hasDisplay
    this.stages = d.kind === 'code'
    this.shape = shapeOf(d.kind)
  }

  /** Width across the flow and length along it of a routing Card's outline. */
  private get routingSize() {
    switch (this.shape) {
      case 'branch': return { width: 60, height: 48 }
      case 'fork': return { width: Math.max(56, this.outputs.length * 30 + 12), height: 40 }
      case 'merge': return { width: Math.max(56, this.inputs.length * 30 + 12), height: 40 }
      // The start Card: its name and a run button, nothing more.
      case 'start': return { width: 116, height: 34 }
      default: return { width: 0, height: 0 }
    }
  }

  private get bandIn() {
    return this.shape !== 'card' && this.flow === 'topToBottom' && this.showLabels(true) ? CardGeometry.portRow : 0
  }
  private get bandOut() {
    return this.shape !== 'card' && this.flow === 'topToBottom' && this.showLabels(false) ? CardGeometry.portRow : 0
  }

  get portBand() {
    // A Draft has no port names to give a row: its ports are at the middle of its sides.
    if (this.flow !== 'leftToRight' || this.shape !== 'card' || this.draft) return 0
    return Math.max(this.inputs.length, this.outputs.length, 1) * CardGeometry.portRow
  }
  /** The header, or what is left of it over the content of a Draft without a name. */
  get headerRow() {
    return this.untitled ? CardGeometry.untitledTop : CardGeometry.header
  }
  /** The room for a Draft's content under its header. */
  get draftRow() {
    return !this.draft ? 0 : this.draftEditing ? CardGeometry.draftEditRow : CardGeometry.draftRow
  }
  get topLabels() {
    return this.shape === 'card' && this.flow === 'topToBottom' && this.showLabels(true) ? CardGeometry.portRow : 0
  }
  get bottomLabels() {
    return this.shape === 'card' && this.flow === 'topToBottom' && this.showLabels(false) ? CardGeometry.portRow : 0
  }

  get size() {
    if (this.shape === 'start') return this.routingSize
    if (this.shape !== 'card') {
      const r = this.routingSize
      return this.flow === 'topToBottom' ? { width: r.width, height: this.bandIn + r.height + this.bandOut } : { width: r.height + 60, height: r.width }
    }
    if (this.collapsed) return { width: CardGeometry.width, height: CardGeometry.header }
    const h = this.topLabels + this.headerRow + (this.summary ? CardGeometry.summaryRow : 0) + this.draftRow + this.portBand + this.params * CardGeometry.paramRow +
      (this.result ? CardGeometry.resultRow : 0) + (this.stages ? CardGeometry.stageRow : 0) + this.bottomLabels + 4
    return { width: CardGeometry.width, height: h }
  }
  get height() {
    return this.size.height
  }

  /** The outline's frame inside the Card's box. */
  get outlineRect(): Rect {
    const s = this.size
    if (this.shape === 'card' || this.shape === 'start') return { x: 0, y: 0, ...s }
    if (this.flow === 'topToBottom') return { x: 0, y: this.bandIn, width: s.width, height: s.height - this.bandIn - this.bandOut }
    return { x: 30, y: 0, width: s.width - 60, height: s.height }
  }

  /** The outline (Card box, diamond, triangle) inside `r`, as SVG path data. */
  outline(r: Rect): string {
    const P = (x: number, y: number): Pt => ({ x, y })
    switch (this.shape) {
      case 'card':
        return roundedRect(r, 9)
      case 'start':
        return roundedRect(r, r.height / 2)
      case 'branch':
        return roundedPolygon([P(r.x + r.width / 2, r.y), P(r.x + r.width, r.y + r.height / 2), P(r.x + r.width / 2, r.y + r.height), P(r.x, r.y + r.height / 2)], 6)
      default: {
        // Triangle: apex upstream (fork) or downstream (merge).
        const fork = this.shape === 'fork'
        if (this.flow === 'topToBottom') {
          return roundedPolygon(fork
            ? [P(r.x + r.width / 2, r.y), P(r.x + r.width, r.y + r.height), P(r.x, r.y + r.height)]
            : [P(r.x, r.y), P(r.x + r.width, r.y), P(r.x + r.width / 2, r.y + r.height)], 4)
        }
        const inset = r.height / 2
        return roundedPolygon(fork
          ? [P(r.x, r.y + inset), P(r.x + r.width, r.y), P(r.x + r.width, r.y + r.height), P(r.x, r.y + r.height - inset)]
          : [P(r.x, r.y), P(r.x + r.width, r.y + inset), P(r.x + r.width, r.y + r.height - inset), P(r.x, r.y + r.height)], 7)
      }
    }
  }

  /** The segment ports of one side are spread along (top-to-bottom: an x range). */
  private edgeSpan(input: boolean) {
    if (this.shape === 'card' || this.shape === 'branch' || this.shape === 'start') {
      const w = this.shape === 'card' ? CardGeometry.width : this.routingSize.width
      return { start: 0, width: w }
    }
    const w = this.routingSize.width
    const narrowSide = (this.shape === 'fork') === input
    const span = narrowSide ? 0 : w
    return { start: (w - span) / 2, width: span }
  }

  /** Room for one port's label on its edge. */
  spacing(input: boolean): number {
    const list = input ? this.inputs : this.outputs
    if (!list.length) return 0
    if (this.shape === 'branch') return this.routingSize.width / 2 - 8
    const span = this.edgeSpan(input)
    // A single label may be as wide as the Card; several share the edge.
    const pitch = span.width / (list.length + 1)
    return list.length === 1 ? Math.max(pitch * 2, 90) : pitch - 4
  }

  /** Labels are shown when there is room for them; otherwise on hover. */
  showLabels(input: boolean): boolean {
    const list = input ? this.inputs : this.outputs
    if (!list.length) return false
    // A Draft's ports come from its wires: their names say nothing yet (shown on hover).
    if (this.draft) return false
    if (this.shape === 'card' && this.collapsed) return false
    if (this.shape === 'start') return false
    if (this.shape === 'fork' || this.shape === 'merge') return false
    if (this.shape === 'branch') return true
    return this.spacing(input) >= CardGeometry.minLabel
  }

  /** Where a wire starts from a side of a Draft that has no port yet (the middle
   *  of the side, where the port will be). A side with ports starts from them. */
  newPort(output: boolean): Pt | undefined {
    if (!this.draft || this.collapsed || (output ? this.outputs : this.inputs).length) return undefined
    const r = this.outlineRect
    return this.flow === 'topToBottom'
      ? { x: CardGeometry.width / 2, y: output ? r.y + r.height : r.y }
      : { x: output ? CardGeometry.width : 0, y: r.y + r.height / 2 }
  }

  /** A Draft's ports are around the middle of its side, closer together when there are many. */
  private draftPortY(index: number, count: number) {
    const r = this.outlineRect
    const pitch = Math.min(CardGeometry.portRow, (r.height - 8) / (count + 1))
    return r.y + r.height / 2 + (index - (count - 1) / 2) * pitch
  }

  port(name: string, output: boolean): Pt {
    if (name === NEW_PORT) {
      const p = this.newPort(output)
      if (p) return p
    }
    const list = output ? this.outputs : this.inputs
    const index = Math.max(0, list.indexOf(name))
    const n = Math.max(list.length, 1)
    const r = this.outlineRect
    if (this.shape === 'start') {
      // Its one output, in the middle of the downstream edge.
      return this.flow === 'topToBottom' ? { x: r.x + r.width / 2, y: r.y + r.height } : { x: r.x + r.width, y: r.y + r.height / 2 }
    }
    if (this.shape === 'branch') {
      // value / condition on the two upper edges, a / b on the two lower edges.
      const first = index === 0
      if (this.flow === 'topToBottom') return { x: r.x + r.width * (first ? 0.25 : 0.75), y: r.y + r.height * (output ? 0.75 : 0.25) }
      return { x: r.x + r.width * (output ? 0.75 : 0.25), y: r.y + r.height * (first ? 0.25 : 0.75) }
    }
    if (this.flow === 'topToBottom') {
      const span = this.edgeSpan(!output)
      return { x: span.start + (span.width * (index + 1)) / (n + 1), y: output ? r.y + r.height : r.y }
    }
    if (this.shape !== 'card') {
      const narrowSide = (this.shape === 'fork') === !output
      const h = narrowSide ? 0 : r.height
      return { x: output ? r.x + r.width : r.x, y: r.y + r.height / 2 - h / 2 + (h * (index + 1)) / (n + 1) }
    }
    const x = output ? CardGeometry.width : 0
    if (this.collapsed) return { x, y: CardGeometry.header / 2 }
    if (this.draft) return { x, y: this.draftPortY(index, list.length) }
    const top = CardGeometry.header + (this.summary ? CardGeometry.summaryRow : 0)
    return { x, y: top + index * CardGeometry.portRow + CardGeometry.portRow / 2 }
  }
}

function roundedRect(r: Rect, rad: number): string {
  const { x, y, width: w, height: h } = r
  const c = Math.min(rad, w / 2, h / 2)
  return `M${x + c},${y}H${x + w - c}A${c},${c} 0 0 1 ${x + w},${y + c}V${y + h - c}A${c},${c} 0 0 1 ${x + w - c},${y + h}H${x + c}A${c},${c} 0 0 1 ${x},${y + h - c}V${y + c}A${c},${c} 0 0 1 ${x + c},${y}Z`
}

/** A closed polygon with rounded corners (SVG path data). */
export function roundedPolygon(pts: Pt[], radius: number): string {
  const n = pts.length
  if (n < 3) return ''
  const mid = (a: Pt, b: Pt) => ({ x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 })
  const start = mid(pts[n - 1], pts[0])
  let d = `M${start.x},${start.y}`
  for (let i = 0; i < n; i++) {
    const prev = pts[(i - 1 + n) % n], c = pts[i], next = pts[(i + 1) % n]
    const toward = (a: Pt, b: Pt, len: number) => {
      const l = Math.hypot(b.x - a.x, b.y - a.y) || 1
      return { x: a.x + ((b.x - a.x) * len) / l, y: a.y + ((b.y - a.y) * len) / l }
    }
    const r = Math.min(radius, Math.hypot(c.x - prev.x, c.y - prev.y) / 2, Math.hypot(next.x - c.x, next.y - c.y) / 2)
    const p1 = toward(c, prev, r), p2 = toward(c, next, r)
    d += `L${p1.x},${p1.y}Q${c.x},${c.y} ${p2.x},${p2.y}`
  }
  return d + 'Z'
}

// ---- Wires -----------------------------------------------------------------------

/** Where a searched wire runs (world units) and what the search pays for. */
const WIRE = {
  /** Distance kept from Cards, and the length of the run straight out of a port. */
  clearance: 20,
  /** Distance between wires that would otherwise share a line. */
  lane: 12,
  /** One bend is worth this much length, so a path does not become a staircase. */
  turn: 30,
  /** One crossing of another wire is worth this much length. */
  crossing: 80,
  /** Extra cost per unit along another wire, and per unit closer to a Card than `clearance`. */
  overlap: 3,
  near: 0.25,
  /** How deep a routing Card's box holds its own port. A port deeper than this in
   *  a box is under a Card laid over it, which the wire cannot get out of along the flow. */
  portDepth: 44,
}

export type WireEnds = { a: Pt; b: Pt }
export type WireRoute = { d: string; pts: Pt[]; curved: boolean }

/** Routes wires in world units around the Cards. A wire going along the flow
 *  whose curve touches no Card stays that curve. Any other wire takes the
 *  cheapest path on a grid of lines beside the Cards: by length, bends and
 *  crossings of other wires, leaving and entering its ports along the flow.
 *  The same path draws and hit-tests a wire. */
export class WireRouter {
  /** The Cards as the search sees them: computed top-to-bottom (left-to-right swaps x and y). */
  private readonly frame: Rect[]
  private cardLines?: { xs: number[]; ys: number[] }
  /** The wires routed so far, for the search to keep off them (in the search's frame). */
  private routed: RoutedRun[] = []

  constructor(readonly flow: FlowDirection, readonly obstacles: Rect[]) {
    this.frame = flow === 'leftToRight' ? obstacles.map((r) => ({ x: r.y, y: r.x, width: r.height, height: r.width })) : obstacles
  }

  private toFrame(p: Pt): Pt {
    return this.flow === 'leftToRight' ? { x: p.y, y: p.x } : p
  }

  /** A wire that goes along the flow, however little, and crosses no Card on the short way. */
  isForward(a: Pt, b: Pt) {
    const along = this.flow === 'topToBottom' ? b.y - a.y : b.x - a.x
    return along >= 0 && !this.crossesCard(a, b)
  }

  /** The cheap first check: most wires end here, and only Cards near the curve are looked at. */
  private crossesCard(a: Pt, b: Pt) {
    const [c1, c2] = this.controls(a, b)
    const x0 = Math.min(a.x, b.x, c1.x, c2.x), x1 = Math.max(a.x, b.x, c1.x, c2.x)
    const y0 = Math.min(a.y, b.y, c1.y, c2.y), y1 = Math.max(a.y, b.y, c1.y, c2.y)
    // A port sits on its Card's edge: only the inside counts. A branch's port is
    // inside its box, and the wire has to get out of there anyway.
    const inside = (o: Rect, p: Pt) => p.x > o.x + 2 && p.x < o.x + o.width - 2 && p.y > o.y + 2 && p.y < o.y + o.height - 2
    const near = this.obstacles.filter((o) => x0 < o.x + o.width && x1 > o.x && y0 < o.y + o.height && y1 > o.y && !inside(o, a) && !inside(o, b))
    if (!near.length) return false
    let p = a
    for (let i = 1; i <= 32; i++) {
      const q = bezier(a, c1, c2, b, i / 32)
      if (near.some((o) => segmentEnters(p, q, o))) return true
      p = q
    }
    return false
  }

  /** Cards laid over each other, each covering the other's port: the wire is under them. */
  private underCards(a: Pt, b: Pt) {
    const inside = (o: Rect, p: Pt) => p.x > o.x && p.x < o.x + o.width && p.y > o.y && p.y < o.y + o.height
    const touches = (o: Rect, p: Pt) => p.x >= o.x && p.x <= o.x + o.width && p.y >= o.y && p.y <= o.y + o.height
    return this.obstacles.some((o) => inside(o, a) && touches(o, b)) && this.obstacles.some((o) => inside(o, b) && touches(o, a))
  }

  /** Leaves and enters along the flow: by half the distance, and by more only
   *  as far as the ports are apart across it, so close ports are joined straight. */
  private controls(a: Pt, b: Pt): [Pt, Pt] {
    if (this.flow === 'topToBottom') {
      const dy = Math.max(Math.abs(b.y - a.y) / 2, Math.min(30, Math.abs(b.x - a.x) / 2))
      return [{ x: a.x, y: a.y + dy }, { x: b.x, y: b.y - dy }]
    }
    const dx = Math.max(Math.abs(b.x - a.x) / 2, Math.min(30, Math.abs(b.y - a.y) / 2))
    return [{ x: a.x + dx, y: a.y }, { x: b.x - dx, y: b.y }]
  }

  /** One wire, keeping off those already routed. */
  route(a: Pt, b: Pt): WireRoute {
    return this.routeAll([{ a, b }])[0]
  }

  /** The wires of a frame. Curves depend on the Cards alone; the rest go shortest
   *  first, in an order of their own, so that the order of the connections changes
   *  nothing and long wires settle around short ones. */
  routeAll(wires: WireEnds[]): WireRoute[] {
    const out = wires.map((w): WireRoute | undefined => (this.isForward(w.a, w.b) || this.underCards(w.a, w.b) ? this.curve(w.a, w.b) : undefined))
    wires.forEach((w, i) => {
      // Enough of a curve to tell whether something crosses it.
      if (out[i]) this.remember(w.a, out[i]!.pts.filter((_, k) => k % 4 === 0), true)
    })
    const span = (w: WireEnds) => Math.abs(w.b.x - w.a.x) + Math.abs(w.b.y - w.a.y)
    const order = wires.map((_, i) => i).filter((i) => !out[i]).sort((i, j) => {
      const u = wires[i], v = wires[j]
      return span(u) - span(v) || u.a.x - v.a.x || u.a.y - v.a.y || u.b.x - v.b.x || u.b.y - v.b.y
    })
    for (const i of order) {
      const { a, b } = wires[i]
      const pts = this.search(this.toFrame(a), this.toFrame(b))
      if (!pts) {
        // Shut in by Cards touching all around: nothing is free to take.
        out[i] = this.curve(a, b)
        continue
      }
      this.remember(a, pts, false)
      const world = pts.map((p) => this.toFrame(p))
      out[i] = { d: orthogonal(world, 10), pts: world, curved: false }
    }
    return out as WireRoute[]
  }

  private remember(a: Pt, pts: Pt[], curved: boolean) {
    const frame = curved ? pts.map((p) => this.toFrame(p)) : pts
    const xs = frame.map((p) => p.x), ys = frame.map((p) => p.y)
    const x = Math.min(...xs), y = Math.min(...ys)
    this.routed.push({ a, pts: frame, box: { x, y, width: Math.max(...xs) - x, height: Math.max(...ys) - y }, curved })
  }

  /** How far along the flow a port's wire has to run to get out of a Card's box (0: not in it). */
  private depth(o: Rect, p: Pt, dir: 1 | -1) {
    if (p.x <= o.x || p.x >= o.x + o.width) return 0
    if (dir > 0) return o.y <= p.y && p.y < o.y + o.height ? o.y + o.height - p.y : 0
    return o.y < p.y && p.y <= o.y + o.height ? p.y - o.y : 0
  }

  /** Where a port's wire is out of the boxes it starts in (a Card's port is on the edge already). */
  private clear(p: Pt, dir: 1 | -1, cards: Rect[]): Pt {
    let y = p.y
    for (let moved = true; moved;) {
      moved = false
      for (const o of cards) {
        const d = this.depth(o, { x: p.x, y }, dir)
        if (d > 0) {
          y += dir * d
          moved = true
        }
      }
    }
    return { x: p.x, y }
  }

  /** The run straight out of a port: shorter where the next Card or the other port is close. */
  private stub(p: Pt, dir: 1 | -1, along: number, cards: Rect[]) {
    let s = along > 0 ? Math.min(WIRE.clearance, along / 2) : WIRE.clearance
    for (const o of cards) {
      if (p.x <= o.x || p.x >= o.x + o.width) continue
      const d = dir > 0 ? o.y - p.y : p.y - o.y - o.height
      if (d >= 0) s = Math.min(s, d / 2)
    }
    return s
  }

  /** The lines beside the Cards, and through gaps too narrow to keep clear of both sides. */
  private linesOfCards() {
    if (this.cardLines) return this.cardLines
    const lines = (edges: number[]) => {
      const sorted = [...edges].sort((p, q) => p - q)
      const out: number[] = []
      sorted.forEach((e, i) => {
        if (i && e > sorted[i - 1] && e - sorted[i - 1] < 2 * WIRE.clearance) out.push((e + sorted[i - 1]) / 2)
      })
      return out
    }
    const xe = this.frame.flatMap((o) => [o.x, o.x + o.width]), ye = this.frame.flatMap((o) => [o.y, o.y + o.height])
    this.cardLines = {
      xs: [...this.frame.flatMap((o) => [o.x - WIRE.clearance, o.x + o.width + WIRE.clearance]), ...lines(xe)],
      ys: [...this.frame.flatMap((o) => [o.y - WIRE.clearance, o.y + o.height + WIRE.clearance]), ...lines(ye)],
    }
    return this.cardLines
  }

  /** The cheapest path from port to port (A* over grid node and direction of travel),
   *  in the search's frame. Ties go to the lowest state, so nothing depends on chance. */
  private search(a: Pt, b: Pt): Pt[] | null {
    // A Card laid over a port is no wall for its wire: the wire runs under it.
    const obstacles = this.frame.filter((o) => this.depth(o, a, 1) <= WIRE.portDepth && this.depth(o, b, -1) <= WIRE.portDepth)
    const out = this.clear(a, 1, obstacles), into = this.clear(b, -1, obstacles)
    const along = into.y - out.y
    const first = { x: out.x, y: out.y + this.stub(out, 1, along, obstacles) }
    const last = { x: into.x, y: into.y - this.stub(into, -1, along, obstacles) }

    const cards = this.linesOfCards()
    const xLines = [...cards.xs, first.x, last.x, (first.x + last.x) / 2]
    const yLines = [...cards.ys, first.y, last.y, (first.y + last.y) / 2]
    const others = this.routed.filter((w) => w.a.x !== a.x || w.a.y !== a.y)
    for (const w of others) {
      if (w.curved) continue
      // A lane beside every run of another wire, for this one to take instead of sharing it.
      w.pts.forEach((p, i) => {
        if (!i) return
        if (p.x === w.pts[i - 1].x) xLines.push(p.x - WIRE.lane, p.x + WIRE.lane)
        else yLines.push(p.y - WIRE.lane, p.y + WIRE.lane)
      })
    }
    const xs = sortedUnique(xLines), ys = sortedUnique(yLines)
    const nx = xs.length, ny = ys.length

    const start = xs.indexOf(first.x) + ys.indexOf(first.y) * nx
    const goal = xs.indexOf(last.x) + ys.indexOf(last.y) * nx
    // Directions: 0 along the flow, 1 +x, 2 against the flow, 3 -x.
    const best = new Float64Array(nx * ny * 4).fill(Infinity)
    const from = new Int32Array(nx * ny * 4).fill(-1)
    // What a stretch between neighbouring nodes costs, worked out once (-1: not yet).
    const stretch = new Float64Array(nx * ny * 2).fill(-1)
    // What lies near each line, also once: most lines pass few Cards and wires.
    const nearY: (Near | undefined)[] = new Array(ny), nearX: (Near | undefined)[] = new Array(nx)
    const cost = (n: number, horizontal: boolean) => {
      const k = n * 2 + (horizontal ? 1 : 0)
      if (stretch[k] < 0) {
        const ix = n % nx, iy = (n - ix) / nx
        stretch[k] = horizontal
          ? this.runCost(xs[ix], xs[ix + 1], ys[iy], true, (nearY[iy] ??= nearLine(ys[iy], true, obstacles, others)))
          : this.runCost(ys[iy], ys[iy + 1], xs[ix], false, (nearX[ix] ??= nearLine(xs[ix], false, obstacles, others)))
      }
      return stretch[k]
    }
    // The length still to go, and the bend that still takes if the wire is not heading there.
    const h = (n: number, dir: number) => {
      const ix = n % nx, iy = (n - ix) / nx
      const dx = Math.abs(xs[ix] - last.x), dy = Math.abs(ys[iy] - last.y)
      return dx + dy + ((dir & 1 ? dy : dx) > 0 ? WIRE.turn : 0)
    }

    const open: HeapItem[] = []
    best[start * 4] = 0
    heapPush(open, h(start, 0), start * 4)
    let end = -1
    while (open.length) {
      const { f, id } = heapPop(open)
      const n = id >> 2, dir = id & 3
      const g = best[id]
      if (f > g + h(n, dir)) continue
      if (n === goal) {
        end = id
        break
      }
      const ix = n % nx, iy = (n - ix) / nx
      for (let d = 0; d < 4; d++) {
        if (d === ((dir + 2) & 3)) continue
        if (d === 0 ? iy + 1 >= ny : d === 1 ? ix + 1 >= nx : d === 2 ? iy === 0 : ix === 0) continue
        const m = d === 0 ? n + nx : d === 1 ? n + 1 : d === 2 ? n - nx : n - 1
        let g2 = g + cost(d === 0 || d === 1 ? n : m, d === 1 || d === 3)
        if (g2 === Infinity) continue
        if (d !== dir) g2 += WIRE.turn
        if (m === goal) {
          // It goes on into the port along the flow: never back along the way in.
          if (d === 2) continue
          if (d !== 0) g2 += WIRE.turn
        }
        const next = m * 4 + d
        if (g2 < best[next]) {
          best[next] = g2
          from[next] = id
          heapPush(open, g2 + h(m, d), next)
        }
      }
    }
    if (end < 0) return null

    const corners: Pt[] = [b, into, last]
    for (let id = from[end]; id >= 0; id = from[id]) {
      const n = id >> 2, ix = n % nx
      corners.push({ x: xs[ix], y: ys[(n - ix) / nx] })
    }
    corners.push(first, out, a)
    return straightened(corners.reverse())
  }

  /** The cost of a stretch along a grid line, from `u0` to `u1` at `v`; Infinity through a Card. */
  private runCost(u0: number, u1: number, v: number, horizontal: boolean, near: Near) {
    let cost = u1 - u0
    for (const o of near.cards) {
      const lo = horizontal ? o.x : o.y, hi = lo + (horizontal ? o.width : o.height)
      const vlo = horizontal ? o.y : o.x, vhi = vlo + (horizontal ? o.height : o.width)
      if (u0 < hi && u1 > lo && v > vlo && v < vhi) return Infinity
      const c = WIRE.clearance
      cost += WIRE.near * Math.max(0, Math.min(u1, hi + c) - Math.max(u0, lo - c))
    }
    for (const w of near.wires) {
      const lo = horizontal ? w.box.x : w.box.y, hi = lo + (horizontal ? w.box.width : w.box.height)
      if (u1 < lo || u0 > hi) continue
      for (let i = 1; i < w.pts.length; i++) {
        const su = horizontal ? w.pts[i - 1].x : w.pts[i - 1].y, sv = horizontal ? w.pts[i - 1].y : w.pts[i - 1].x
        const tu = horizontal ? w.pts[i].x : w.pts[i].y, tv = horizontal ? w.pts[i].y : w.pts[i].x
        // Half open on both sides: a crossing at a node or a corner counts once.
        if ((sv <= v) !== (tv <= v)) {
          const at = su + ((v - sv) * (tu - su)) / (tv - sv)
          if (at >= u0 && at < u1) cost += WIRE.crossing
        } else if (Math.abs(sv - v) < WIRE.lane / 2 && Math.abs(tv - v) < WIRE.lane / 2) {
          cost += WIRE.overlap * Math.max(0, Math.min(u1, Math.max(su, tu)) - Math.max(u0, Math.min(su, tu)))
        }
      }
    }
    return cost
  }

  curve(a: Pt, b: Pt): WireRoute {
    const [c1, c2] = this.controls(a, b)
    const pts: Pt[] = []
    for (let i = 0; i <= 32; i++) pts.push(bezier(a, c1, c2, b, i / 32))
    return { d: `M${a.x},${a.y}C${c1.x},${c1.y} ${c2.x},${c2.y} ${b.x},${b.y}`, pts, curved: true }
  }
}

type RoutedRun = { a: Pt; pts: Pt[]; box: Rect; curved: boolean }
type Near = { cards: Rect[]; wires: RoutedRun[] }

/** The Cards closer to a grid line than `clearance` and the wires that reach it. */
function nearLine(v: number, horizontal: boolean, cards: Rect[], wires: RoutedRun[]): Near {
  const c = WIRE.clearance, l = WIRE.lane
  return horizontal
    ? { cards: cards.filter((o) => v > o.y - c && v < o.y + o.height + c), wires: wires.filter((w) => v >= w.box.y - l && v <= w.box.y + w.box.height + l) }
    : { cards: cards.filter((o) => v > o.x - c && v < o.x + o.width + c), wires: wires.filter((w) => v >= w.box.x - l && v <= w.box.x + w.box.width + l) }
}

/** Whether a segment goes through the inside of a Card (its edge is where ports sit). */
function segmentEnters(p: Pt, q: Pt, o: Rect) {
  let t0 = 0, t1 = 1
  const axis = (from: number, to: number, lo: number, hi: number) => {
    const d = to - from
    if (d === 0) return from > lo && from < hi
    const ta = (lo - from) / d, tb = (hi - from) / d
    t0 = Math.max(t0, Math.min(ta, tb))
    t1 = Math.min(t1, Math.max(ta, tb))
    return t0 <= t1
  }
  return axis(p.x, q.x, o.x + 2, o.x + o.width - 2) && axis(p.y, q.y, o.y + 2, o.y + o.height - 2)
}

function sortedUnique(values: number[]) {
  const sorted = [...values].sort((p, q) => p - q)
  return sorted.filter((v, i) => !i || v !== sorted[i - 1])
}

/** A path without repeated points or corners that are not corners. */
function straightened(pts: Pt[]) {
  const out: Pt[] = []
  for (const p of pts) {
    if (out.length && out[out.length - 1].x === p.x && out[out.length - 1].y === p.y) continue
    const n = out.length
    if (n >= 2 && ((out[n - 2].x === out[n - 1].x && out[n - 1].x === p.x) || (out[n - 2].y === out[n - 1].y && out[n - 1].y === p.y))) out[n - 1] = p
    else out.push(p)
  }
  return out
}

/** Binary heap on (f, id): a total order, so ties come out the same on Mac and web. */
type HeapItem = { f: number; id: number }
const less = (p: HeapItem, q: HeapItem) => p.f < q.f || (p.f === q.f && p.id < q.id)

function heapPush(heap: HeapItem[], f: number, id: number) {
  heap.push({ f, id })
  let i = heap.length - 1
  while (i > 0) {
    const up = (i - 1) >> 1
    if (!less(heap[i], heap[up])) break
    ;[heap[i], heap[up]] = [heap[up], heap[i]]
    i = up
  }
}

function heapPop(heap: HeapItem[]) {
  const top = heap[0]
  const tail = heap.pop()!
  if (heap.length) {
    heap[0] = tail
    let i = 0
    for (;;) {
      const l = 2 * i + 1, r = l + 1
      let m = i
      if (l < heap.length && less(heap[l], heap[m])) m = l
      if (r < heap.length && less(heap[r], heap[m])) m = r
      if (m === i) break
      ;[heap[i], heap[m]] = [heap[m], heap[i]]
      i = m
    }
  }
  return top
}

/** A polyline with rounded corners (SVG path data). */
function orthogonal(pts: Pt[], radius: number): string {
  let d = `M${pts[0].x},${pts[0].y}`
  for (let i = 1; i < pts.length; i++) {
    if (i === pts.length - 1) {
      d += `L${pts[i].x},${pts[i].y}`
      continue
    }
    const a = pts[i - 1], c = pts[i], n = pts[i + 1]
    const r = Math.max(0.1, Math.min(radius, Math.hypot(c.x - a.x, c.y - a.y) / 2, Math.hypot(n.x - c.x, n.y - c.y) / 2))
    const toward = (from: Pt, to: Pt) => {
      const l = Math.hypot(to.x - from.x, to.y - from.y) || 1
      return { x: from.x + ((to.x - from.x) * r) / l, y: from.y + ((to.y - from.y) * r) / l }
    }
    const p1 = toward(c, a), p2 = toward(c, n)
    d += `L${p1.x},${p1.y}Q${c.x},${c.y} ${p2.x},${p2.y}`
  }
  return d
}

export function bezier(a: Pt, c1: Pt, c2: Pt, b: Pt, t: number): Pt {
  const u = 1 - t
  return {
    x: u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x,
    y: u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y,
  }
}

/** Distance from a point to a polyline. */
export function distToPolyline(p: Pt, pts: Pt[]): number {
  let best = Infinity
  for (let i = 1; i < pts.length; i++) {
    const a = pts[i - 1], b = pts[i]
    const vx = b.x - a.x, vy = b.y - a.y
    const l2 = vx * vx + vy * vy
    const t = l2 ? Math.max(0, Math.min(1, ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2)) : 0
    best = Math.min(best, Math.hypot(p.x - (a.x + t * vx), p.y - (a.y + t * vy)))
  }
  return best
}

export const union = (a: Rect, b: Rect): Rect => {
  const x = Math.min(a.x, b.x), y = Math.min(a.y, b.y)
  return { x, y, width: Math.max(a.x + a.width, b.x + b.width) - x, height: Math.max(a.y + a.height, b.y + b.height) - y }
}

/** The ends of the link between a Card and its result frame: on the edges
 *  that face each other, each in the middle of the stretch the two share. */
export function displayLink(card: Rect, frame: Rect): string {
  const cMaxX = card.x + card.width, cMaxY = card.y + card.height, fMaxX = frame.x + frame.width, fMaxY = frame.y + frame.height
  const gx = Math.max(frame.x - cMaxX, card.x - fMaxX)
  const gy = Math.max(frame.y - cMaxY, card.y - fMaxY)
  const clamp = (v: number, lo: number, hi: number) => (lo > hi ? (lo + hi) / 2 : Math.min(Math.max(v, lo), hi))
  let a: Pt, b: Pt, horizontal: boolean
  if (gx >= gy) {
    const right = frame.x + frame.width / 2 >= card.x + card.width / 2
    const lo = Math.max(card.y, frame.y), hi = Math.min(cMaxY, fMaxY)
    const ay = lo <= hi ? (lo + hi) / 2 : clamp(frame.y + frame.height / 2, card.y + 10, cMaxY - 10)
    const by = lo <= hi ? (lo + hi) / 2 : clamp(card.y + card.height / 2, frame.y + 10, fMaxY - 10)
    a = { x: right ? cMaxX : card.x, y: ay }
    b = { x: right ? frame.x : fMaxX, y: by }
    horizontal = true
  } else {
    const down = frame.y + frame.height / 2 >= card.y + card.height / 2
    const lo = Math.max(card.x, frame.x), hi = Math.min(cMaxX, fMaxX)
    const ax = lo <= hi ? (lo + hi) / 2 : clamp(frame.x + frame.width / 2, card.x + 10, cMaxX - 10)
    const bx = lo <= hi ? (lo + hi) / 2 : clamp(card.x + card.width / 2, frame.x + 10, fMaxX - 10)
    a = { x: ax, y: down ? cMaxY : card.y }
    b = { x: bx, y: down ? frame.y : fMaxY }
    horizontal = false
  }
  if (horizontal) {
    const dx = (b.x - a.x) / 2
    return `M${a.x},${a.y}C${a.x + dx},${a.y} ${b.x - dx},${b.y} ${b.x},${b.y}`
  }
  const dy = (b.y - a.y) / 2
  return `M${a.x},${a.y}C${a.x},${a.y + dy} ${b.x},${b.y - dy} ${b.x},${b.y}`
}

// ---- Camera ------------------------------------------------------------------------

export interface Camera {
  x: number
  y: number
  zoom: number
}

/** Frames `r` (world units) in the part of a view of `size` not covered by floating controls. */
export function fitCamera(r: Rect, size: { width: number; height: number }, insets = { top: 0, left: 0, bottom: 0, right: 0 }, maxZoom = 1.5, margin = 40): Camera | undefined {
  if (size.width <= 0 || size.height <= 0) return undefined
  const rr = { x: r.x - margin, y: r.y - margin, width: r.width + 2 * margin, height: r.height + 2 * margin }
  const w = Math.max(80, size.width - insets.left - insets.right)
  const h = Math.max(80, size.height - insets.top - insets.bottom)
  const z = Math.max(0.2, Math.min(maxZoom, Math.min(w / rr.width, h / rr.height)))
  return { zoom: z, x: insets.left + w / 2 - (rr.x + rr.width / 2) * z, y: insets.top + h / 2 - (rr.y + rr.height / 2) * z }
}
