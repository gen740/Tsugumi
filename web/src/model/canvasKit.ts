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
  /** The canvas grid's spacing while the grid is on: the ports follow it. */
  portGrid?: number
}

export const isRouting = (d: CardNodeData) => d.kind === 'fork' || d.kind === 'branch' || d.kind === 'merge'

export type CardShape = 'card' | 'branch' | 'fork' | 'merge'
export const shapeOf = (k: CardKind): CardShape => (k === 'branch' || k === 'fork' || k === 'merge' ? k : 'card')

/** The nearest grid line. Not `Math.round`: that goes up on a tie and Swift's
 *  `.rounded()` goes away from zero, and a Card on a negative half would land
 *  one grid apart on the two. */
export const snapToGrid = (v: number, grid: number) => Math.floor(v / grid + 0.5) * grid

/** An even number of grids, so half of it is on a grid line. */
const gridLengthNear = (length: number, grid: number) => 2 * grid * Math.floor(length / (2 * grid) + 0.5)
const gridLengthAtLeast = (length: number, grid: number) => 2 * grid * Math.ceil(length / (2 * grid))

/** Size, outline and port positions of a Card (world units, relative to its
 *  top-left corner). Shared by drawing, wires, the selection and hit-tests.
 *
 *  With the grid on, a port's place along its edge is on grid/n of the canvas:
 *  a Card's corner is on a grid line, so every offset here is a multiple of
 *  grid/n from the corner, and the Card's length across the flow is an even
 *  number of grids so the middle of the edge is on a line too. */
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
  /** Ports this far apart have room for a whole name between them, while there are few. */
  static readonly labelPitch = 60
  /** Port marks are 8 wide: closer than this they would touch. */
  static readonly minPortPitch = 10
  /** Kept free at the ends of a Card's edge (its rounded corners). */
  static readonly portMargin = 12

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
  readonly grid?: number

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
    this.grid = d.portGrid
  }

  /** Width across the flow and length along it of a routing Card's outline. */
  private get routingSize() {
    switch (this.shape) {
      // Its ports are at a quarter and three quarters of this.
      case 'branch': return { width: this.grid ? 4 * CardGeometry.rowPitch(this.grid) : 60, height: 48 }
      case 'fork': return { width: this.fanWidth(this.outputs.length), height: 40 }
      case 'merge': return { width: this.fanWidth(this.inputs.length), height: 40 }
      default: return { width: 0, height: 0 }
    }
  }

  /** The wide edge of a fork or merge: it grows with its ports, so they are never squeezed. */
  private fanWidth(count: number) {
    if (!this.grid) return Math.max(56, count * 30 + 12)
    return gridLengthAtLeast(Math.max(56, (count - 1) * this.fanPitch(count, this.grid) + 16), this.grid)
  }

  /** The closest ports can be with a row each (grid/n, or whole grids on a fine grid). */
  static rowPitch(grid: number) {
    return grid < CardGeometry.portRow ? Math.ceil(CardGeometry.portRow / grid) * grid : grid / Math.floor(grid / CardGeometry.portRow)
  }

  /** Ports of a fork or merge. Two or four around the middle take twice the
   *  pitch to stay on the same lines as a Card's ports across a wire; more
   *  of them would make the mark too wide, and close up onto grid/2 instead. */
  private fanPitch(count: number, grid: number) {
    return CardGeometry.rowPitch(grid) * (count === 2 || count === 4 ? 2 : 1)
  }

  /** Ports of a Card's edge across the flow, gathered around its middle in
   *  a bit over half its length: far enough apart for their names while few, then
   *  by the grid, grid/2, grid/3 and so on. Past `minPortPitch` they stop
   *  closing up and spread further along the edge instead. */
  private edgePitch(count: number, grid: number) {
    const room = gridLengthNear(CardGeometry.width, grid) * 0.6
    // An even number of grids: two ports around the middle land on grid lines.
    let pitch = gridLengthAtLeast(CardGeometry.labelPitch, grid)
    let n = 1
    while ((count - 1) * pitch > room) {
      let next = pitch - grid
      if (pitch <= grid) {
        n += 1
        next = grid / n
      }
      if (next < CardGeometry.minPortPitch) break
      pitch = next
    }
    return pitch
  }

  /** The Card's length across the flow; it grows if its ports would not fit. */
  get cardWidth() {
    if (!this.grid) return CardGeometry.width
    let w = gridLengthNear(CardGeometry.width, this.grid)
    if (this.flow === 'topToBottom') {
      for (const count of [this.inputs.length, this.outputs.length]) {
        if (count > 1) w = Math.max(w, gridLengthAtLeast((count - 1) * this.edgePitch(count, this.grid) + 2 * CardGeometry.portMargin, this.grid))
      }
    }
    return w
  }

  /** With the grid on, the place of a port along an edge across the flow. */
  private alongGrid(index: number, count: number, output: boolean): number | undefined {
    if (!this.grid) return undefined
    const middle = index - (count - 1) / 2
    if (this.shape === 'card') return this.cardWidth / 2 + middle * this.edgePitch(count, this.grid)
    // A triangle's apex is one point: everything on that side is on it.
    const w = this.routingSize.width
    return (this.shape === 'fork') === !output ? w / 2 : w / 2 + middle * this.fanPitch(count, this.grid)
  }

  private get bandIn() {
    return this.shape !== 'card' && this.flow === 'topToBottom' && this.showLabels(true) ? CardGeometry.portRow : 0
  }
  private get bandOut() {
    return this.shape !== 'card' && this.flow === 'topToBottom' && this.showLabels(false) ? CardGeometry.portRow : 0
  }

  get portBand() {
    if (this.flow !== 'leftToRight' || this.shape !== 'card') return 0
    const count = Math.max(this.inputs.length, this.outputs.length, 1)
    if (this.draft) {
      // A Draft has no port names to give a row: its ports are at the middle
      // of its sides. On the grid they do not close up, so it makes room.
      if (!this.grid) return 0
      return Math.max(0, count * CardGeometry.rowPitch(this.grid) + 8 - this.contentHeight)
    }
    if (!this.grid) return count * CardGeometry.portRow
    const p = CardGeometry.rowPitch(this.grid)
    return this.firstRow(p) - this.rowsTop + (count - 1) * p + CardGeometry.portRow / 2
  }
  /** Where the rows of a left-to-right Card begin, under its header and summary. */
  private get rowsTop() {
    return CardGeometry.header + (this.summary ? CardGeometry.summaryRow : 0)
  }
  /** The first grid line a port's row fits under them. */
  private firstRow(pitch: number) {
    return Math.ceil((this.rowsTop + CardGeometry.portRow / 2) / pitch) * pitch
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
    if (this.shape !== 'card') {
      const r = this.routingSize
      return this.flow === 'topToBottom' ? { width: r.width, height: this.bandIn + r.height + this.bandOut } : { width: r.height + 60, height: r.width }
    }
    if (this.collapsed) return { width: this.cardWidth, height: CardGeometry.header }
    return { width: this.cardWidth, height: this.contentHeight + this.portBand }
  }
  /** A Card's height without its port rows. */
  private get contentHeight() {
    return this.topLabels + this.headerRow + (this.summary ? CardGeometry.summaryRow : 0) + this.draftRow + this.params * CardGeometry.paramRow +
      (this.result ? CardGeometry.resultRow : 0) + (this.stages ? CardGeometry.stageRow : 0) + this.bottomLabels + 4
  }
  get height() {
    return this.size.height
  }

  /** The outline's frame inside the Card's box. */
  get outlineRect(): Rect {
    const s = this.size
    if (this.shape === 'card') return { x: 0, y: 0, ...s }
    if (this.flow === 'topToBottom') return { x: 0, y: this.bandIn, width: s.width, height: s.height - this.bandIn - this.bandOut }
    return { x: 30, y: 0, width: s.width - 60, height: s.height }
  }

  /** The outline (Card box, diamond, triangle) inside `r`, as SVG path data. */
  outline(r: Rect): string {
    const P = (x: number, y: number): Pt => ({ x, y })
    switch (this.shape) {
      case 'card':
        return roundedRect(r, 9)
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
    if (this.shape === 'card' || this.shape === 'branch') {
      const w = this.shape === 'card' ? this.cardWidth : this.routingSize.width
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
    if (list.length === 1) return Math.max(span.width, 90)
    if (this.grid && this.shape === 'card') return this.edgePitch(list.length, this.grid) - 4
    return span.width / (list.length + 1) - 4
  }

  /** Labels are shown when there is room for them; otherwise on hover. */
  showLabels(input: boolean): boolean {
    const list = input ? this.inputs : this.outputs
    if (!list.length) return false
    // A Draft's ports come from its wires: their names say nothing yet (shown on hover).
    if (this.draft) return false
    if (this.shape === 'card' && this.collapsed) return false
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
      ? { x: this.cardWidth / 2, y: output ? r.y + r.height : r.y }
      : { x: output ? this.cardWidth : 0, y: this.grid ? snapToGrid(r.y + r.height / 2, CardGeometry.rowPitch(this.grid)) : r.y + r.height / 2 }
  }

  /** A Draft's ports are around the middle of its side, closer together when there are many. */
  private draftPortY(index: number, count: number) {
    const r = this.outlineRect
    if (this.grid) {
      // The height is not on the grid: the nearest lines to the middle.
      const p = CardGeometry.rowPitch(this.grid)
      return snapToGrid(r.y + r.height / 2 - ((count - 1) * p) / 2, p) + index * p
    }
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
    if (this.shape === 'branch') {
      // value / condition on the two upper edges, a / b on the two lower edges.
      const first = index === 0
      if (this.flow === 'topToBottom') return { x: r.x + r.width * (first ? 0.25 : 0.75), y: r.y + r.height * (output ? 0.75 : 0.25) }
      return { x: r.x + r.width * (output ? 0.75 : 0.25), y: r.y + r.height * (first ? 0.25 : 0.75) }
    }
    const along = this.alongGrid(index, list.length, output)
    if (this.flow === 'topToBottom') {
      const y = output ? r.y + r.height : r.y
      if (along !== undefined) return { x: along, y }
      const span = this.edgeSpan(!output)
      return { x: span.start + (span.width * (index + 1)) / (n + 1), y }
    }
    if (this.shape !== 'card') {
      if (along !== undefined) return { x: output ? r.x + r.width : r.x, y: r.y + along }
      const narrowSide = (this.shape === 'fork') === !output
      const h = narrowSide ? 0 : r.height
      return { x: output ? r.x + r.width : r.x, y: r.y + r.height / 2 - h / 2 + (h * (index + 1)) / (n + 1) }
    }
    const x = output ? this.cardWidth : 0
    if (this.collapsed) return { x, y: this.grid ? snapToGrid(CardGeometry.header / 2, CardGeometry.rowPitch(this.grid)) : CardGeometry.header / 2 }
    if (this.draft) return { x, y: this.draftPortY(index, list.length) }
    if (this.grid) {
      const p = CardGeometry.rowPitch(this.grid)
      return { x, y: this.firstRow(p) + index * p }
    }
    return { x, y: this.rowsTop + index * CardGeometry.portRow + CardGeometry.portRow / 2 }
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

/** Routes wires in world units around the Cards. Forward wires (downstream)
 *  are curves along the flow; wires going back upstream (feedback loops)
 *  leave the graph sideways and run in their own lane outside it, so they
 *  never cross Cards or each other. The same path draws and hit-tests a wire. */
export class WireRouter {
  private lanes = new Map<boolean, number>()
  readonly bounds: Rect | null

  constructor(readonly flow: FlowDirection, readonly obstacles: Rect[]) {
    this.bounds = obstacles.reduce<Rect | null>((u, r) => (u ? union(u, r) : r), null)
  }

  isForward(a: Pt, b: Pt) {
    return this.flow === 'topToBottom' ? b.y >= a.y + 24 : b.x >= a.x + 24
  }

  /** Points of the wire (for hit-testing) and its SVG path data. */
  route(a: Pt, b: Pt): { d: string; pts: Pt[] } {
    if (this.isForward(a, b) || !this.bounds) return this.curve(a, b)
    // Computed top-to-bottom; left-to-right swaps x and y.
    const swap = this.flow === 'leftToRight'
    const t = (p: Pt): Pt => (swap ? { x: p.y, y: p.x } : p)
    const tr = (r: Rect): Rect => (swap ? { x: r.y, y: r.x, width: r.height, height: r.width } : r)
    const A = t(a), B = t(b)
    const box = tr(this.bounds)
    const obs = this.obstacles.map(tr)
    const left = (A.x + B.x) / 2 < box.x + box.width / 2
    const lane = this.lanes.get(left) ?? 0
    this.lanes.set(left, lane + 1)
    const laneX = left ? box.x - 34 - lane * 16 : box.x + box.width + 34 + lane * 16
    const stagger = lane * 5
    const y1 = freeY(A, laneX, A.y + 16 + stagger, 9, obs)
    const y2 = freeY(B, laneX, B.y - 16 - stagger, -9, obs)
    const pts = [A, { x: A.x, y: y1 }, { x: laneX, y: y1 }, { x: laneX, y: y2 }, { x: B.x, y: y2 }, B].map(t)
    return { d: orthogonal(pts, 10), pts }
  }

  curve(a: Pt, b: Pt): { d: string; pts: Pt[] } {
    let c1: Pt, c2: Pt
    if (this.flow === 'topToBottom') {
      const dy = Math.max(30, Math.abs(b.y - a.y) / 2)
      c1 = { x: a.x, y: a.y + dy }
      c2 = { x: b.x, y: b.y - dy }
    } else {
      const dx = Math.max(30, Math.abs(b.x - a.x) / 2)
      c1 = { x: a.x + dx, y: a.y }
      c2 = { x: b.x - dx, y: b.y }
    }
    const pts: Pt[] = []
    for (let i = 0; i <= 32; i++) pts.push(bezier(a, c1, c2, b, i / 32))
    return { d: `M${a.x},${a.y}C${c1.x},${c1.y} ${c2.x},${c2.y} ${b.x},${b.y}`, pts }
  }
}

function freeY(p: Pt, laneX: number, start: number, step: number, obstacles: Rect[]): number {
  let y = start
  for (let i = 0; i < 40; i++) {
    const horiz = { x: Math.min(p.x, laneX), y: y - 3, width: Math.abs(laneX - p.x), height: 6 }
    const vert = { x: p.x - 2, y: Math.min(p.y, y) + 6, width: 4, height: Math.max(0, Math.abs(y - p.y) - 12) }
    const blocked = obstacles.some((o) => {
      const r = { x: o.x - 8, y: o.y - 6, width: o.width + 16, height: o.height + 12 }
      return intersects(r, horiz) || (vert.height > 0 && intersects(r, vert))
    })
    if (!blocked) return y
    y += step
  }
  return start
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
export const intersects = (a: Rect, b: Rect) => a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height

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
