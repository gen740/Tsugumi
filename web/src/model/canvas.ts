// Camera, scene geometry and canvas-wide actions of one tab (fit all, zoom,
// add note, find, group, Draft), shared by the canvas, its floating
// controls, the menus and the shortcuts.
import { arr, bool, dbl, get, isObj, J, real, str } from '../shared/json'
import { D } from '../shared/rpc'
import { Camera, CardGeometry, CardNodeData, displayLink, fitCamera, Pt, Rect, union, WireRouter, snapToGrid } from './canvasKit'
import { Observable } from './observable'
import { prefs, ShortcutAction } from './prefs'
import { CardKey, DefinitionTemplate, key, ProjectSession } from './session'
import type { WorkflowTab } from './workspace'

export type { Camera }

// ---- Scene: where everything of a scope is (world units) ------------------------------

export type WireStyle = 'normal' | 'carried' | 'active' | 'selected' | 'display'

export interface RoutedWire {
  /** Index in the scope's connections (undefined: boundary wire or display link). */
  connection?: number
  d: string
  pts: Pt[]
  style: WireStyle
}

export interface BoundaryPort {
  name: string
  /** World units, on the bar. */
  point: Pt
  inner?: [CardKey, string]
  type?: string
}

export interface Boundary {
  top: Rect
  bottom: Rect
  inputs: BoundaryPort[]
  outputs: BoundaryPort[]
}

/** Moves in progress (world units): the scene draws them before they are saved. */
export interface SceneDrag {
  /** The selection on the move: its Cards and the frames that go with them. */
  cards?: { keys: Set<CardKey>; frames?: Set<CardKey>; dx: number; dy: number }
  boundary?: { input: boolean; dx: number; dy: number }
}

const LIVE = [D.CardState.waiting, D.CardState.ready, D.CardState.preparing, D.CardState.running]

const intersects = (a: Rect, b: Rect) => a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height

export const noteRect = (n: J | undefined): Rect => ({
  x: dbl(get(n, 'x')) ?? 0, y: dbl(get(n, 'y')) ?? 0, width: dbl(get(n, 'width')) ?? 240, height: dbl(get(n, 'height')) ?? 120,
})

const inset = (r: Rect, dx: number, dy: number): Rect => ({ x: r.x + dx, y: r.y + dy, width: r.width - 2 * dx, height: r.height - 2 * dy })
const contains = (r: Rect, p: Pt) => p.x >= r.x && p.x <= r.x + r.width && p.y >= r.y && p.y <= r.y + r.height

/** Everything a canvas draws and hit-tests, computed once per update. */
export class Scene {
  readonly flow
  readonly cardKeys: CardKey[]
  private datas = new Map<CardKey, CardNodeData>()
  private geos = new Map<CardKey, CardGeometry>()
  private _outputs?: CardKey[]
  private _boundary?: Boundary | null
  private _wires?: RoutedWire[]

  constructor(readonly session: ProjectSession, readonly scope: string[], readonly selectedConnection?: number, readonly drag: SceneDrag = {}) {
    this.flow = session.flow
    this.cardKeys = session.cards(scope).map((c) => str(get(c, 'id'))).filter((x): x is string => !!x).map((id) => key([...scope, id]))
  }

  data(k: CardKey): CardNodeData {
    let d = this.datas.get(k)
    if (!d) {
      d = this.session.nodeData(k)
      this.datas.set(k, d)
    }
    return d
  }

  geometry(k: CardKey): CardGeometry {
    let g = this.geos.get(k)
    if (!g) {
      g = new CardGeometry(this.data(k), this.flow)
      this.geos.set(k, g)
    }
    return g
  }

  origin(k: CardKey): Pt {
    const p = this.session.placedPosition(k)
    const d = this.drag.cards
    return d && d.keys.has(k) ? { x: p.x + d.dx, y: p.y + d.dy } : p
  }

  cardRect(k: CardKey): Rect {
    return { ...this.origin(k), ...this.geometry(k).size }
  }

  /** A Card's outline (routing Cards are smaller than their box). */
  outlineRect(k: CardKey): Rect {
    const o = this.origin(k), r = this.geometry(k).outlineRect
    return { x: r.x + o.x, y: r.y + o.y, width: r.width, height: r.height }
  }

  portWorld(k: CardKey, port: string, output: boolean): Pt {
    const o = this.origin(k), p = this.geometry(k).port(port, output)
    return { x: o.x + p.x, y: o.y + p.y }
  }

  /** Cards with an output frame: once their code has shown something (display SDK). */
  get outputKeys(): CardKey[] {
    this._outputs ??= this.cardKeys.filter((k) => {
      const f = this.session.displayFrame(k)
      return (this.session.displays.has(k) || f !== undefined) && bool(get(f, 'hidden')) !== true
    })
    return this._outputs
  }

  /** Output frame of a Card: saved place, or beside the Card. */
  frameRect(k: CardKey): Rect {
    const o = this.origin(k)
    const f = this.session.displayFrame(k)
    const g = this.geometry(k)
    const width = dbl(get(f, 'width')) ?? 320, height = dbl(get(f, 'height')) ?? 170
    const fx = dbl(get(f, 'x')), fy = dbl(get(f, 'y'))
    const r = this.flow === 'topToBottom'
      ? { x: fx ?? o.x + g.size.width + 40, y: fy ?? o.y, width, height }
      : { x: fx ?? o.x, y: fy ?? o.y + g.height + 30, width, height }
    const m = this.drag.cards
    return m?.frames?.has(k) && !this.session.frameFollowsCard(k, m.keys) ? { ...r, x: r.x + m.dx, y: r.y + m.dy } : r
  }

  /** The nearest place to `point` where the Card `k` covers no other Card,
   *  output frame or sticky note: `point` itself when it is free, else on rings
   *  around it. On the grid when the grid is on. The Mac's freeSpot is the same search. */
  freeSpot(point: Pt, k: CardKey): Pt {
    const step = prefs.showGrid ? prefs.gridSize : 20
    const start = prefs.showGrid ? { x: snapToGrid(point.x, step), y: snapToGrid(point.y, step) } : point
    const size = this.geometry(k).size
    const margin = 16
    const taken = [
      ...this.cardKeys.filter((c) => c !== k).map((c) => this.cardRect(c)),
      ...this.outputKeys.filter((c) => c !== k).map((c) => this.frameRect(c)),
      ...this.session.notes(this.scope).map((n) => noteRect(n)),
    ]
    const free = (p: Pt) => {
      const r = { x: p.x - margin, y: p.y - margin, width: size.width + 2 * margin, height: size.height + 2 * margin }
      return !taken.some((t) => intersects(r, t))
    }
    if (free(start)) return start
    for (let ring = 1; ring <= 200; ring++) {
      const spots: { dx: number; dy: number }[] = []
      for (let i = -ring; i <= ring; i++) spots.push({ dx: i, dy: -ring }, { dx: i, dy: ring })
      for (let i = 1 - ring; i < ring; i++) spots.push({ dx: -ring, dy: i }, { dx: ring, dy: i })
      // Nearest first; among equals, the same order on the Mac and on the web.
      spots.sort((a, b) => (a.dx * a.dx + a.dy * a.dy) - (b.dx * b.dx + b.dy * b.dy) || b.dy - a.dy || b.dx - a.dx)
      for (const s of spots) {
        const p = { x: start.x + s.dx * step, y: start.y + s.dy * step }
        if (free(p)) return p
      }
    }
    return start
  }

  /** What a marquee (world units) touches. */
  itemsIn(r: Rect): { cards: CardKey[]; notes: string[]; frames: CardKey[] } {
    return {
      cards: this.cardKeys.filter((k) => intersects(r, { ...this.session.placedPosition(k), ...this.geometry(k).size })),
      notes: this.session.notes(this.scope).filter((n) => intersects(r, noteRect(n))).map((n) => str(get(n, 'id'))).filter((x): x is string => !!x),
      frames: this.outputKeys.filter((k) => intersects(r, this.frameRect(k))),
    }
  }

  /** A group's frame around its Cards (with room for its title). */
  groupRect(members: CardKey[]): Rect {
    let r: Rect | null = null
    for (const m of members) r = r ? union(r, this.cardRect(m)) : this.cardRect(m)
    const b = inset(r ?? { x: 0, y: 0, width: 200, height: 100 }, -18, -18)
    return { x: b.x, y: b.y - 22, width: b.width, height: b.height + 22 }
  }

  groupMembers(g: J): CardKey[] {
    return (arr(get(g, 'cards')) ?? []).map((c) => str(c)).filter((x): x is string => !!x)
      .map((c) => key([...this.scope, c])).filter((k) => this.session.instance(k) !== undefined)
  }

  /** The outer Card's inputs (above the inner graph) and outputs (below it). */
  get boundary(): Boundary | undefined {
    if (this._boundary === undefined) this._boundary = this.computeBoundary() ?? null
    return this._boundary ?? undefined
  }

  private computeBoundary(): Boundary | undefined {
    if (!this.scope.length) return undefined
    const scopeKey = key(this.scope)
    const b = this.session.boundary(scopeKey)
    const bi = get(b, 'inputs'), bo = get(b, 'outputs')
    const ins = isObj(bi) ? bi.entries() : [], outs = isObj(bo) ? bo.entries() : []
    if (!ins.length && !outs.length) return undefined
    let box: Rect | null = null
    for (const k of this.cardKeys) {
      const r = { ...this.session.placedPosition(k), ...this.geometry(k).size }
      box = box ? union(box, r) : r
    }
    const bx = box ?? { x: 0, y: 0, width: 240, height: 120 }
    const width = Math.max(bx.width, Math.max(ins.length, outs.length) * 110 + 40)
    const x = bx.x + bx.width / 2 - width / 2
    // Placed above / below the inner Cards unless moved (layout.json "boundary").
    const placed = (side: 'inputs' | 'outputs', auto: Pt): Pt => {
      const p = { ...(this.session.boundaryPosition(this.scope, side) ?? auto) }
      const d = this.drag.boundary
      if (d && d.input === (side === 'inputs')) {
        p.x += d.dx
        p.y += d.dy
      }
      return p
    }
    const top = { ...placed('inputs', { x, y: bx.y - 96 }), width, height: 40 }
    const bottom = { ...placed('outputs', { x, y: bx.y + bx.height + 56 }), width, height: 40 }
    const resolved = this.session.resolution.get(scopeKey)
    const ports = (list: [string, J][], bar: Rect, output: boolean): BoundaryPort[] => list.map(([name, v], i) => {
      const px = bar.x + (bar.width * (i + 1)) / (list.length + 1)
      const c = str(get(v, 'card')), p = str(get(v, 'port'))
      const t = (output ? resolved?.outputs : resolved?.inputs)?.find((x) => x.name === name)?.rootType
      return { name, point: { x: px, y: output ? bar.y : bar.y + bar.height }, inner: c && p ? [key([...this.scope, c]), p] : undefined, type: t }
    })
    return { top, bottom, inputs: ports(ins, top, false), outputs: ports(outs, bottom, true) }
  }

  /** Every wire of the scope, routed once and used for drawing, hover and clicks alike. */
  get wires(): RoutedWire[] {
    if (this._wires) return this._wires
    const s = this.session
    const obstacles = [...this.cardKeys.map((k) => this.cardRect(k)), ...this.outputKeys.map((k) => this.frameRect(k))]
    const b = this.boundary
    if (b) obstacles.push(b.top, b.bottom)
    // Routed together: a wire's path must not depend on the order of the connections.
    const ends: { a: Pt; b: Pt; wire: Omit<RoutedWire, 'd' | 'pts'> }[] = []
    s.connections(this.scope).forEach((c, i) => {
      const fc = str(get(c, 'from', 'card')), fp = str(get(c, 'from', 'port')), tc = str(get(c, 'to', 'card')), tp = str(get(c, 'to', 'port'))
      if (!fc || !fp || !tc || !tp) return
      const fk = key([...this.scope, fc]), tk = key([...this.scope, tc])
      ends.push({ a: this.portWorld(fk, fp, true), b: this.portWorld(tk, tp, false), wire: { connection: i, style: i === this.selectedConnection ? 'selected' : this.style(fk, fp, tk) } })
    })
    if (b) {
      for (const p of b.inputs) {
        if (!p.inner || !s.instance(p.inner[0])) continue
        ends.push({ a: p.point, b: this.portWorld(p.inner[0], p.inner[1], false), wire: { style: 'normal' } })
      }
      for (const p of b.outputs) {
        if (!p.inner || !s.instance(p.inner[0])) continue
        ends.push({ a: this.portWorld(p.inner[0], p.inner[1], true), b: p.point, wire: { style: this.style(p.inner[0], p.inner[1], undefined) } })
      }
    }
    const routes = new WireRouter(this.flow, obstacles).routeAll(ends)
    const out: RoutedWire[] = ends.map((e, i) => ({ ...e.wire, d: routes[i].d, pts: routes[i].pts }))
    // Display links: which Card a result frame belongs to (not a computation).
    for (const k of this.outputKeys) {
      const d = displayLink(this.outlineRect(k), this.frameRect(k))
      out.push({ d, pts: linkEnds(d), style: 'display' })
    }
    this._wires = out
    return out
  }

  /** A wire is colored only when a value really went through it in this run
   *  (a branch colors only the side it chose), and animated while the value
   *  is on its way to a Card that has not finished with it yet. */
  style(from: CardKey, port: string, to: CardKey | undefined): WireStyle {
    const s = this.session
    // A Card in Card sends what the inner Card behind that output sent.
    if (s.kind(from) === 'subgraph') {
      const src = s.boundarySource(from, port)
      if (src) return this.style(src[0], src[1], to)
    }
    const st = s.cardStates.get(from)
    if (!st) return 'normal'
    if (s.runActive && st.sent.has(port)) {
      const dest = to !== undefined ? s.cardStates.get(to) : undefined
      if (dest) {
        if (LIVE.includes(dest.state)) return 'active'
      } else if (to !== undefined) return 'active'
    }
    return st.sentInRun.has(port) ? 'carried' : 'normal'
  }

  /** The Card under a world point (the top-most). */
  cardAt(w: Pt): CardKey | undefined {
    for (let i = this.cardKeys.length - 1; i >= 0; i--) if (contains(this.cardRect(this.cardKeys[i]), w)) return this.cardKeys[i]
    return undefined
  }

  /** The connection drawn within `tolerance` (world units) of a point. */
  wireAt(w: Pt, tolerance: number): number | undefined {
    return this.wires.find((r) => r.connection !== undefined && distTo(w, r.pts) <= tolerance)?.connection
  }

  /** Everything shown: Cards, output frames, notes, groups, the boundary bars and the wires. */
  bounds(): Rect | null {
    let r: Rect | null = null
    const add = (x: Rect) => (r = r ? union(r, x) : x)
    for (const k of this.cardKeys) add(this.cardRect(k))
    for (const k of this.outputKeys) add(this.frameRect(k))
    for (const n of this.session.notes(this.scope)) add(noteRect(n))
    for (const g of this.session.groups(this.scope)) for (const m of this.groupMembers(g)) add(inset(this.cardRect(m), -18, -40))
    const b = this.boundary
    if (b) {
      add(b.top)
      add(b.bottom)
    }
    for (const w of this.wires) if (w.style !== 'display' && w.pts.length) add(ptsBounds(w.pts))
    return r
  }
}

function ptsBounds(pts: Pt[]): Rect {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity
  for (const p of pts) {
    x0 = Math.min(x0, p.x); y0 = Math.min(y0, p.y); x1 = Math.max(x1, p.x); y1 = Math.max(y1, p.y)
  }
  return { x: x0, y: y0, width: x1 - x0, height: y1 - y0 }
}

function distTo(p: Pt, pts: Pt[]): number {
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

const PAIR = /(-?\d*\.?\d+(?:e[-+]?\d+)?),(-?\d*\.?\d+(?:e[-+]?\d+)?)/g

/** The two ends of a display link's path. */
export function linkEnds(d: string): Pt[] {
  const all = [...d.matchAll(PAIR)].map((m) => ({ x: Number(m[1]), y: Number(m[2]) }))
  return all.length ? [all[0], all[all.length - 1]] : []
}

/** Path data (world units, made of x,y pairs) on screen. */
export function pathToScreen(d: string, c: Camera): string {
  return d.replace(PAIR, (_m, x, y) => `${(Number(x) * c.zoom + c.x).toFixed(2)},${(Number(y) * c.zoom + c.y).toFixed(2)}`)
}

/** The point halfway along a polyline. */
export function midPoint(pts: Pt[]): Pt | undefined {
  if (!pts.length) return undefined
  let total = 0
  for (let i = 1; i < pts.length; i++) total += Math.hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y)
  let left = total / 2
  for (let i = 1; i < pts.length; i++) {
    const l = Math.hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y)
    if (l >= left && l > 0) {
      const t = left / l
      return { x: pts[i - 1].x + (pts[i].x - pts[i - 1].x) * t, y: pts[i - 1].y + (pts[i].y - pts[i - 1].y) * t }
    }
    left -= l
  }
  return pts[pts.length - 1]
}

// ---- Controller --------------------------------------------------------------------

/** What the canvas shows above itself: an editor, the search box, details. */
export interface CanvasOverlay {
  editingNote?: string
  editingGroup?: string
  /** ⌘F search box (undefined: closed). */
  findText?: string
  /** ⌘I: details of a Card without the pointer. */
  details?: CardKey
}

export class CanvasController extends Observable implements CanvasOverlay {
  size = { width: 0, height: 0 }
  editingNote?: string
  editingGroup?: string
  findText?: string
  details?: CardKey
  /** Bumped to ask the canvas to focus its search field. */
  findRequest = 0
  /** The canvas has the keyboard focus: its single-key shortcuts work there. */
  focused = false

  constructor(readonly session: ProjectSession, readonly tab: WorkflowTab) {
    super()
  }

  get scopeKey() {
    return key(this.tab.scope)
  }

  get camera(): Camera {
    return this.session.cameras.get(this.scopeKey) ?? { x: 40, y: 40, zoom: 1 }
  }

  get hasCamera(): boolean {
    return this.session.cameras.has(this.scopeKey)
  }

  setCamera(c: Camera, save = true) {
    this.session.cameras.set(this.scopeKey, c)
    this.changed()
    if (save) this.saveView()
  }

  saveView() {
    this.session.saveViewSoon(this.tab.scope, this.tab.selection)
  }

  set<K extends keyof CanvasOverlay>(k: K, v: CanvasOverlay[K]) {
    ;(this as CanvasOverlay)[k] = v
    this.changed()
  }

  scene(drag?: SceneDrag): Scene {
    return new Scene(this.session, this.tab.scope, this.tab.selectedConnection, drag)
  }

  screen(w: Pt): Pt {
    const c = this.camera
    return { x: w.x * c.zoom + c.x, y: w.y * c.zoom + c.y }
  }

  world(p: Pt): Pt {
    const c = this.camera
    return { x: (p.x - c.x) / c.zoom, y: (p.y - c.y) / c.zoom }
  }

  get center(): Pt {
    return { x: this.size.width / 2, y: this.size.height / 2 }
  }

  /** Keeps `around` (a screen point; default the middle) fixed. */
  setZoom(z: number, around?: Pt, save = true) {
    const c = this.camera
    const p = around ?? this.center
    const nz = Math.max(0.2, Math.min(3, z))
    this.setCamera({ x: p.x - ((p.x - c.x) * nz) / c.zoom, y: p.y - ((p.y - c.y) * nz) / c.zoom, zoom: nz }, save)
  }

  zoomBy(f: number, around?: Pt, save = true) {
    this.setZoom(this.camera.zoom * f, around, save)
  }

  zoomIn() {
    this.zoomBy(1.2)
  }

  zoomOut() {
    this.zoomBy(1 / 1.2)
  }

  actualSize() {
    this.setZoom(1)
  }

  /** The zoom buttons scale around the origin of the view, as in the macOS controls. */
  zoomFromControls(z: number) {
    const c = this.camera
    const nz = Math.max(0.2, Math.min(3, z))
    this.setCamera({ x: (c.x * nz) / c.zoom, y: (c.y * nz) / c.zoom, zoom: nz })
  }

  /** Frames everything shown in the scope in the part of the canvas its floating controls leave free. */
  fitAll() {
    const r = this.scene().bounds()
    if (!r || this.size.width <= 0) return
    const c = fitCamera(r, this.size, { top: 52, left: 8, bottom: 48, right: 8 }, 1.2, 24)
    if (c) this.setCamera(c)
  }

  /** A scope shown for the first time (no saved camera) shows its whole flow. */
  fitIfNew() {
    if (this.hasCamera || this.size.width <= 0 || !this.session.cards(this.tab.scope).length) return
    this.fitAll()
  }

  /** Centres a Card in the view when it is not fully visible (zoom kept). */
  reveal(k: CardKey) {
    if (!this.session.instance(k) || this.size.width <= 0) return
    const r = this.scene().cardRect(k)
    const c = this.camera
    const sx = r.x * c.zoom + c.x, sy = r.y * c.zoom + c.y
    if (sx >= 20 && sy >= 50 && sx + r.width * c.zoom <= this.size.width - 20 && sy + r.height * c.zoom <= this.size.height - 50) return
    this.setCamera({ ...c, x: this.size.width / 2 - (r.x + r.width / 2) * c.zoom, y: this.size.height / 2 - (r.y + r.height / 2) * c.zoom })
  }

  /** Starts writing a Draft on its Card, at a zoom where the text can be read. */
  writeDraft(k: CardKey) {
    this.session.startDraftEdit(k)
    const r = this.scene().cardRect(k)
    if (this.camera.zoom < 0.8) this.setZoom(1, this.screen({ x: r.x + r.width / 2, y: r.y + r.height / 2 }))
    this.reveal(k)
  }

  /** Where a new item lands at the middle of the view. */
  middle(): Pt {
    return this.world({ x: this.size.width / 2 - 120, y: this.size.height / 2 - 60 })
  }

  addNote(at?: Pt) {
    const id = this.session.addNote(this.tab.scope, at ?? this.middle())
    this.set('editingNote', id)
  }

  /** Where a definition placed from the sidebar lands (near the top left of the view). */
  placementPoint(): Pt {
    const c = this.camera
    return { x: (240 - c.x) / c.zoom, y: (160 - c.y) / c.zoom }
  }

  /** ⌘F: the Card search box. */
  find() {
    this.findText = this.findText ?? ''
    this.findRequest++
    this.changed()
  }

  /** ⌘I: details of the selected Card (or `k`). */
  showDetails(k?: CardKey) {
    if (k !== undefined) {
      if (!this.session.instance(k)) return
      this.tab.setSelection([k])
    }
    if (this.tab.selection.size !== 1) return
    this.set('details', [...this.tab.selection][0])
  }

  /** A built-in Card in the middle of the view (or at `at`); returns it. */
  addBuiltin(t: DefinitionTemplate, at?: Pt): CardKey | undefined {
    if (!this.session.isEditable) return undefined
    const k = this.addClear(t, at ?? this.middle())
    this.tab.setSelection([k])
    return k
  }

  /** E: a new Draft Card in the middle of the canvas. */
  addDraft(): CardKey | undefined {
    if (!this.session.isEditable) return undefined
    const k = this.addClear('draft', this.world(this.center))
    this.tab.setSelection([k])
    return k
  }

  /** A Card put on the canvas without being aimed at this very spot: where it covers nothing. */
  private addClear(t: DefinitionTemplate, at: Pt): CardKey {
    const scope = this.tab.scope
    return this.session.addCard(t, scope, at, undefined, (k) => new Scene(this.session, scope).freeSpot(at, k))
  }

  /** G: the selected Cards become a group; a selection that is one group's Cards is ungrouped. */
  toggleGroup() {
    const sel = this.tab.selection
    if (!sel.size) return
    const ids = new Set([...sel].map((k) => k.split('/').pop()!))
    const g = this.session.groups(this.tab.scope).find((g) => {
      const cards = new Set((arr(g.get('cards')) ?? []).map((c) => str(c)).filter((x): x is string => !!x))
      return cards.size === ids.size && [...ids].every((i) => cards.has(i))
    })
    const gid = g ? str(g.get('id')) : undefined
    if (gid) this.session.ungroup(gid)
    else this.session.addGroup(sel, this.tab.scope)
  }

  /** "選択をグループ化": a new group, its editor open. */
  groupSelection() {
    if (!this.tab.selection.size) return
    this.set('editingGroup', this.session.addGroup(this.tab.selection, this.tab.scope))
  }

  /** Moves a selection (world units) as one edit: one save, one undo. Layout
   *  only: no computation is re-resolved for a move. With a grid (`snap`) the
   *  Cards go by whole grids from where they are drawn, and what moves with them
   *  goes by the same step, so the arrangement stays. Notes and frames alone land
   *  on whole numbers, as when dragged alone. */
  moveSelection(cards: Set<CardKey>, notes: Set<string>, frames: Set<CardKey>, movedX: number, movedY: number, snap?: number) {
    const onGrid = snap && cards.size ? snap : undefined
    const dx = onGrid ? snapToGrid(movedX, onGrid) : movedX, dy = onGrid ? snapToGrid(movedY, onGrid) : movedY
    const scene = this.scene()
    const shown = new Set(scene.outputKeys)
    this.session.edit((x) => {
      for (const k of frames) {
        if (!shown.has(k) || x.frameFollowsCard(k, cards)) continue
        // Worked out from its Card, so before the Cards move.
        const r = scene.frameRect(k)
        x.changeDisplayFrame(k, (f) => {
          f.set('x', real(Math.round(r.x + dx)))
          f.set('y', real(Math.round(r.y + dy)))
          f.set('width', real(r.width))
          f.set('height', real(r.height))
        })
      }
      for (const k of cards) {
        const p = x.position(k)
        const from = snap ? { x: snapToGrid(p.x, snap), y: snapToGrid(p.y, snap) } : p
        x.setPosition(k, { x: from.x + dx, y: from.y + dy })
      }
      for (const id of notes) {
        x.changeNote(id, (n) => {
          const r = noteRect(n)
          n.set('x', real(Math.round(r.x + dx)))
          n.set('y', real(Math.round(r.y + dy)))
        })
      }
    })
  }

  /** Closes a frame, a step of an edit; its Card stays. The layout keeps a frame
   *  only with its place and size, so one still drawn beside its Card gets them now. */
  private hideFrame(x: ProjectSession, r: Rect, k: CardKey) {
    x.changeDisplayFrame(k, (f) => {
      f.set('x', real(Math.round(r.x)))
      f.set('y', real(Math.round(r.y)))
      f.set('width', real(r.width))
      f.set('height', real(r.height))
      f.set('hidden', true)
    })
  }

  closeFrame(k: CardKey) {
    const r = this.scene().frameRect(k)
    this.session.edit((x) => this.hideFrame(x, r, k))
  }

  deleteSelection() {
    const s = this.session
    const t = this.tab
    // Notes and frames are layout: they can go during a run, as from their own menus.
    if (!s.isEditable && (t.selection.size || t.selectedConnection !== undefined)) {
      s.notice = '実行中は Card を削除できません。'
      s.changed()
      return
    }
    const i = this.tab.selectedConnection
    if (i !== undefined) {
      s.removeConnection(i, this.tab.scope)
      this.tab.selectedConnection = undefined
      this.tab.changed()
      return
    }
    // A frame alone is closed and its Card stays (the frame's own 枠を削除);
    // a deleted Card takes its frame with it.
    const scene = this.scene()
    const closing = scene.outputKeys.filter((k) => t.selectedFrames.has(k) && !t.selection.has(k)).map((k) => [k, scene.frameRect(k)] as const)
    s.edit((x) => {
      x.removeCards(t.selection, t.scope)
      for (const [k, r] of closing) this.hideFrame(x, r, k)
      for (const id of t.selectedNotes) x.dropNote(id)
    })
    t.setSelection([])
  }

  toggleCollapse() {
    const sel = [...this.tab.selection]
    if (!sel.length) return
    this.session.setCollapsed(sel, !sel.every((k) => this.session.collapsed(k)))
  }

  async duplicateSelection() {
    if (!this.session.isEditable || !this.tab.selection.size) return
    this.tab.setSelection(await this.session.duplicate(this.tab.selection, this.tab.scope))
  }

  /** Esc: cancel what is going on, then clear the selection (the canvas cancels a wire first). */
  escape() {
    if (this.findText !== undefined) this.set('findText', undefined)
    else if (this.details !== undefined) this.set('details', undefined)
    else {
      this.tab.selectedConnection = undefined
      this.tab.setSelection([])
    }
  }

  noteIds(): string[] {
    return this.session.notes(this.tab.scope).map((n) => str(get(n, 'id'))).filter((x): x is string => !!x)
  }

  groupIds(): string[] {
    return this.session.groups(this.tab.scope).map((g) => str(g.get('id'))).filter((x): x is string => !!x)
  }

  groupMembers(id: string): CardKey[] {
    const g = this.session.groups(this.tab.scope).find((g) => str(g.get('id')) === id)
    return g ? this.scene().groupMembers(g) : []
  }

  /** The canvas-wide part of a shortcut; false when it is not one of these. */
  handle(a: ShortcutAction): boolean {
    switch (a) {
      case 'fitAll': this.fitAll(); return true
      case 'actualSize': this.actualSize(); return true
      case 'zoomIn': this.zoomIn(); return true
      case 'zoomOut': this.zoomOut(); return true
      case 'arrange': if (this.session.connected) this.session.arrange(this.tab.scope); return true
      case 'group': this.toggleGroup(); return true
      case 'toggleCollapse': this.toggleCollapse(); return true
      case 'duplicate': void this.duplicateSelection(); return true
      case 'addNote': this.addNote(); return true
      case 'find': this.find(); return true
      case 'showDetails': this.showDetails(); return true
      default: return false
    }
  }
}

const controllers = new WeakMap<WorkflowTab, CanvasController>()
export function canvasFor(session: ProjectSession, tab: WorkflowTab): CanvasController {
  let c = controllers.get(tab)
  if (!c || c.session !== session) {
    c = new CanvasController(session, tab)
    controllers.set(tab, c)
  }
  return c
}
