// Drawing pieces of the loading card on a canvas, the same as the app's
// (macOS/Sources/App/Loading/TsugumiArt.swift and TsugumiLoadingCard.swift):
// the bird, its branch, the thread, the temari and the faint workflow behind.
// Nothing here knows about projects or sessions.

export const PALETTE = {
  tealTop: '#2d5a5c', tealBottom: '#132b33', cream: '#f6ead2', mist: '#a9c4bf', rufous: '#c97d3d',
  olive: '#a9b77a', oliveDark: '#7d9148', bark: '#7d5834', barkDark: '#5c3e24', barkShade: '#4e341e', thread: '#efe0c3',
}

type G = CanvasRenderingContext2D
interface Pt { x: number; y: number }

export function hex(v: number, a = 1) {
  return `rgba(${(v >> 16) & 0xff},${(v >> 8) & 0xff},${v & 0xff},${a})`
}
function rgba(color: string, a: number) {
  const v = parseInt(color.slice(1), 16)
  return hex(v, a)
}

/** The app's seeded generator, so the facets and leaves fall where they do there. */
export class SeededRandom {
  private state: bigint
  constructor(seed: number) {
    this.state = BigInt.asUintN(64, BigInt(seed) * 0x9e3779b97f4a7c15n)
  }
  next(lo: number, hi: number) {
    this.state = BigInt.asUintN(64, this.state * 6364136223846793005n + 1442695040888963407n)
    return lo + ((hi - lo) * Number(this.state >> 11n)) / 2 ** 53
  }
}

// ---- Bird ----------------------------------------------------------------------------

export interface BirdPose {
  hop: number // points lifted off the perch
  tilt: number // radians around the feet; + leans forward (peck)
  squash: number // + squashes on landing
  blink: number // 0 open … 1 closed
  flap: number // 0 … 1 wing raised
}
const still = (): BirdPose => ({ hop: 0, tilt: 0, squash: 0, blink: 0, flap: 0 })

type Part = 'body' | 'wing' | 'eye' | 'highlight'

/** The key-visual bird, polygon for polygon, in its SVG coordinates (facing right). */
export const TsugumiBird = (() => {
  const source: [Part, string, number, number][] = [
    ['body', '468,600 494,604 486,640 470,640', 0xd9c29e, 1], ['body', '474,640 484,640 478,702 472,702', 0xc08d68, 1],
    ['body', '472,702 478,694 512,704 476,708', 0xa8744f, 1], ['body', '472,700 476,708 444,708', 0x8e5f3e, 1],
    ['body', '528,592 554,596 546,636 530,636', 0xcdb48c, 1], ['body', '534,636 544,636 540,700 534,700', 0xb07c58, 1],
    ['body', '534,700 540,692 572,700 538,706', 0xa8744f, 1], ['body', '534,698 538,706 508,706', 0x8e5f3e, 1],
    ['body', '330,480 340,585 175,600', 0x4e3320, 1], ['body', '175,600 340,585 215,655', 0x3a2617, 1],
    ['body', '175,600 215,655 160,640', 0x2f1f13, 1],
    ['body', '520,330 655,452 420,400', 0x6f4a2c, 1], ['body', '655,452 605,545 420,400', 0xefe0c3, 1],
    ['body', '420,400 605,545 330,480', 0xe2cca4, 1], ['body', '330,480 605,545 560,610', 0xf4e9d4, 1],
    ['body', '330,480 560,610 470,628', 0xe9d8b8, 1], ['body', '330,480 470,628 340,585', 0xd4b98d, 1],
    ['body', '655,452 690,500 605,545', 0xe6d3b0, 1], ['body', '605,545 690,500 560,610', 0xf8efdd, 1],
    ['body', '610,470 628,478 612,490', 0x3b2718, 1], ['body', '575,500 594,505 580,520', 0x3b2718, 1],
    ['body', '630,512 648,516 636,532', 0x3b2718, 1], ['body', '548,540 566,546 552,560', 0x3b2718, 1],
    ['body', '596,552 614,556 602,572', 0x3b2718, 1], ['body', '520,575 536,580 524,592', 0x3b2718, 1],
    ['body', '570,590 586,593 576,606', 0x3b2718, 1],
    ['body', '590,298 665,320 628,366', 0x8a5d36, 1], ['body', '520,330 590,298 628,366', 0x7a5232, 1],
    ['body', '665,320 694,356 628,366', 0x9a6a3f, 1], ['body', '520,330 628,366 655,452', 0xc9a77a, 1],
    ['body', '628,366 694,356 688,402', 0x3b2718, 1], ['body', '628,366 688,402 655,452', 0xf3e6cc, 1],
    ['body', '655,452 688,402 690,500', 0xeadbbd, 1],
    ['body', '575,338 662,334 668,346 592,352', 0xf1e2c4, 1],
    ['eye', '628,354 641,361 641,376 628,383 615,376 615,361', 0x16100b, 1],
    ['highlight', '623,360 631,358 628,366', 0xffffff, 0.9],
    ['body', '694,356 776,394 728,384', 0x2e2117, 1], ['body', '688,402 694,356 728,384', 0x3d2c1f, 1],
    ['body', '688,402 728,384 776,394', 0xd9a441, 1],
    ['wing', '545,402 440,418 455,500', 0xb0672f, 1], ['wing', '545,402 455,500 525,522', 0xc97d3d, 1],
    ['wing', '440,418 298,560 455,500', 0x8f4f22, 1], ['wing', '455,500 298,560 525,522', 0xa35c29, 1],
    ['wing', '298,560 525,522 400,560', 0x6f3c1a, 1], ['wing', '545,402 525,522 565,470', 0xd58b49, 1],
    ['wing', '380,470 300,556 410,505', 0x5a3015, 0.6],
  ]
  const facets = source.map(([part, pts, v, a]) => ({
    part,
    points: pts.split(' ').map((p) => { const [x, y] = p.split(',').map(Number); return { x, y } }),
    color: hex(v, a),
  }))
  return {
    feet: { x: 505, y: 706 },
    beakTip: 776,
    tailTip: 160,
    /** The top of the bird (its crown), for the box the guide's bird lands on. */
    top: 298,
    shoulder: { x: 545, y: 402 },
    eyeCenter: { x: 628, y: 368 },
    /** Where the thread crosses the bill (on the gape line). */
    gape: { x: 752, y: 388 },
    upperBill: [{ x: 694, y: 356 }, { x: 776, y: 394 }, { x: 728, y: 384 }],
    billColor: hex(0x2e2117),
    facets,
  }
})()

function polygon(g: G, pts: Pt[]) {
  g.beginPath()
  pts.forEach((p, i) => (i ? g.lineTo(p.x, p.y) : g.moveTo(p.x, p.y)))
  g.closePath()
}

/** Where a point of the bird (SVG units) lands when drawn with `drawBird`. */
export function birdPoint(p: Pt, at: Pt, scale: number, pose: BirdPose): Pt {
  const B = TsugumiBird
  const x = (p.x - B.feet.x) * scale * (1 + pose.squash * 0.5), y = (p.y - B.feet.y) * scale * (1 - pose.squash)
  const c = Math.cos(pose.tilt), s = Math.sin(pose.tilt)
  return { x: at.x + x * c - y * s, y: at.y - pose.hop + x * s + y * c }
}

function birdSpace(g: G, at: Pt, scale: number, pose: BirdPose) {
  g.translate(at.x, at.y - pose.hop)
  g.rotate(pose.tilt)
  g.scale(scale * (1 + pose.squash * 0.5), scale * (1 - pose.squash))
  g.translate(-TsugumiBird.feet.x, -TsugumiBird.feet.y)
}

/** Draws with the feet at `at`; `holding` draws (in the caller's coordinates)
 *  over the bird and under its upper bill, so a thread through `gape` is held in the bill. */
export function drawBird(g: G, at: Pt, scale: number, pose: BirdPose, holding?: (g: G) => void) {
  const B = TsugumiBird
  const open = Math.max(0.1, 1 - pose.blink)
  g.save()
  birdSpace(g, at, scale, pose)
  g.lineWidth = 1.2
  for (const f of B.facets) {
    if (f.part === 'highlight' && open < 0.5) continue
    g.save()
    if (f.part === 'wing') {
      g.translate(B.shoulder.x, B.shoulder.y)
      g.rotate(-0.45 * pose.flap)
      g.translate(-B.shoulder.x, -B.shoulder.y)
    } else if (f.part === 'eye' || f.part === 'highlight') {
      g.translate(B.eyeCenter.x, B.eyeCenter.y)
      g.scale(1, open)
      g.translate(-B.eyeCenter.x, -B.eyeCenter.y)
    }
    polygon(g, f.points)
    g.fillStyle = f.color
    g.fill()
    // a same-colour hairline hides the seams between facets
    if (f.part !== 'highlight') { g.strokeStyle = f.color; g.stroke() }
    g.restore()
  }
  g.restore()
  if (!holding) return
  holding(g)
  // the upper bill again, over the thread: held between the bills
  g.save()
  birdSpace(g, at, scale, pose)
  polygon(g, B.upperBill)
  g.fillStyle = g.strokeStyle = B.billColor
  g.lineWidth = 1.2
  g.fill()
  g.stroke()
  g.restore()
}

export const BirdMotion = {
  /** A hop over `0…1`: lift, a little squash on take-off and landing, wing up. */
  hop(u: number, height: number): BirdPose {
    const p = still()
    p.hop = height * Math.sin(u * Math.PI)
    p.flap = Math.sin(u * Math.PI)
    if (u < 0.12) p.squash = (0.12 - u) * 0.6
    if (u > 0.88) p.squash = (u - 0.88) * 0.6
    return p
  },
  blink(t: number, every: number, offset = 0) {
    const u = (t + offset) % every
    return u < 0.16 ? Math.sin((u / 0.16) * Math.PI) : 0
  },
  /** Indeterminate loop (3.2 s): hop, hop, look up, peck, peck, blink. */
  idle(t: number): BirdPose {
    const u = t % 3.2
    let p = still()
    if (u < 0.45) p = BirdMotion.hop(u / 0.45, 14)
    else if (u < 0.55) p.squash = 0.03
    else if (u < 1.0) p = BirdMotion.hop((u - 0.55) / 0.45, 14)
    else if (u < 1.6) p.tilt = -0.1 * Math.sin(((u - 1.0) / 0.6) * Math.PI)
    else if (u >= 1.7 && u < 2.05) p.tilt = 0.38 * Math.sin(((u - 1.7) / 0.35) * Math.PI)
    else if (u >= 2.1 && u < 2.45) p.tilt = 0.38 * Math.sin(((u - 2.1) / 0.35) * Math.PI)
    p.blink = Math.max(p.blink, BirdMotion.blink(u, 3.2, 0.4))
    return p
  },
}

// ---- Scenery -------------------------------------------------------------------------

/** A faint DAG of Cards behind the bird. With a fraction, Cards complete in
 *  order and a signal flows only into the running one; without, signals flow
 *  through every wire. */
export function drawWorkflowBackdrop(g: G, w: number, h: number, t: number, fraction?: number) {
  const nodes = [
    { x: 0.10, y: 0.26, done: 0.0 }, { x: 0.30, y: 0.06, done: 0.22 }, { x: 0.24, y: 0.50, done: 0.40 },
    { x: 0.56, y: 0.03, done: 0.62 }, { x: 0.82, y: 0.16, done: 0.82 }, { x: 0.90, y: 0.50, done: 0.97 },
  ]
  const edges: [number, number][] = [[0, 1], [0, 2], [1, 3], [2, 3], [3, 4], [4, 5]]
  const cs = { w: 62, h: 36 }
  const center = (i: number) => ({ x: nodes[i].x * w, y: nodes[i].y * h + cs.h / 2 })
  const done = (i: number) => fraction !== undefined && fraction >= nodes[i].done
  const running = (i: number) => fraction !== undefined && !done(i) && edges.filter((e) => e[1] === i).every((e) => done(e[0]))
  const wire = (e: [number, number]) => {
    const a = { x: center(e[0]).x + cs.w / 2, y: center(e[0]).y }
    const b = { x: center(e[1]).x - cs.w / 2, y: center(e[1]).y }
    const k = Math.max(24, (b.x - a.x) * 0.5)
    return [a, { x: a.x + k, y: a.y }, { x: b.x - k, y: b.y }, b]
  }
  const at = (p: Pt[], s: number) => {
    const u = 1 - s
    return {
      x: u * u * u * p[0].x + 3 * u * u * s * p[1].x + 3 * u * s * s * p[2].x + s * s * s * p[3].x,
      y: u * u * u * p[0].y + 3 * u * u * s * p[1].y + 3 * u * s * s * p[2].y + s * s * s * p[3].y,
    }
  }

  const glow = nodes.map(() => 0)
  g.lineWidth = 1.4
  edges.forEach((e, n) => {
    const p = wire(e)
    const lit = done(e[0]) && done(e[1])
    g.beginPath()
    g.moveTo(p[0].x, p[0].y)
    g.bezierCurveTo(p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y)
    g.strokeStyle = rgba(lit ? PALETTE.olive : PALETTE.mist, lit ? 0.5 : 0.25)
    g.stroke()
    if (!(fraction === undefined || (done(e[0]) && running(e[1])))) return
    const s = (t * 0.45 + n * 0.37) % 1
    const q = at(p, s)
    g.beginPath()
    g.arc(q.x, q.y, 2.4, 0, 2 * Math.PI)
    g.fillStyle = rgba(PALETTE.cream, 0.75)
    g.fill()
    if (s > 0.8) glow[e[1]] = Math.max(glow[e[1]], (s - 0.8) / 0.2)
  })

  nodes.forEach((_, i) => {
    const c = center(i)
    const r = { x: c.x - cs.w / 2, y: c.y - cs.h / 2 }
    const isDone = done(i), isRunning = running(i)
    const pulse = isRunning ? 0.5 + 0.5 * Math.sin(t * 4) : glow[i]
    const card = () => { g.beginPath(); g.roundRect(r.x, r.y, cs.w, cs.h, 7) }
    card()
    g.fillStyle = rgba(PALETTE.tealBottom, 0.5)
    g.fill()
    g.fillStyle = rgba(isDone ? PALETTE.olive : PALETTE.cream, isDone ? 0.18 : 0.04 + 0.08 * pulse)
    g.fill()
    const stroke = isDone ? PALETTE.olive : isRunning ? PALETTE.rufous : PALETTE.mist
    g.strokeStyle = rgba(stroke, isDone ? 0.7 : 0.3 + 0.4 * pulse)
    g.lineWidth = 1.1
    g.stroke()
    // title and two text bars, like a Card on the canvas
    for (const [y, bw, bh, a] of [[8, 26, 4, 0.4], [17, 40, 3, 0.25], [24, 30, 3, 0.25]]) {
      g.beginPath()
      g.roundRect(r.x + 8, r.y + y, bw, bh, bh / 2)
      g.fillStyle = rgba(PALETTE.cream, a)
      g.fill()
    }
    if (isDone) {
      g.beginPath()
      g.moveTo(r.x + cs.w - 15, r.y + 10)
      g.lineTo(r.x + cs.w - 11, r.y + 14)
      g.lineTo(r.x + cs.w - 5, r.y + 6)
      g.strokeStyle = PALETTE.olive
      g.lineWidth = 1.6
      g.lineCap = g.lineJoin = 'round'
      g.stroke()
      g.lineCap = 'butt'
      g.lineJoin = 'miter'
    }
    for (const px of [r.x, r.x + cs.w]) {
      g.beginPath()
      g.arc(px, c.y, 2.5, 0, 2 * Math.PI)
      g.fillStyle = rgba(PALETTE.mist, 0.5)
      g.fill()
    }
  })
}

// ---- Thread --------------------------------------------------------------------------

/** The thread the bird holds: up from its temari below the picture, over the
 *  picture's edge, hanging slack across into the bill, with a short end out of the bill. */
export const TsugumiThread = {
  bendRadius: 9,
  /** The turn over the edge between the panel (front) and the picture
   *  (behind): a quarter circle from straight up to straight right whose
   *  middle lies on the edge `y`, so half of it is on each side. */
  bend(x: number, y: number) {
    const r = TsugumiThread.bendRadius, k = 0.5523
    const cy = y + r / Math.SQRT2
    const start = { x, y: cy }, end = { x: x + r, y: cy - r }
    const trace = (g: G) => g.bezierCurveTo(x, cy - k * r, x + r - k * r, cy - r, end.x, end.y)
    return { start, end, trace }
  },

  /** In front, on the light panel below the picture (its top edge at y = 0):
   *  straight up from the ball `a` into the bend over the edge at `x`. */
  near(g: G, a: Pt, x: number, w: number, h: number) {
    g.save()
    g.beginPath()
    g.rect(0, 0, w, h)
    g.clip()
    const turn = TsugumiThread.bend(x, 0)
    const path = () => {
      g.beginPath()
      g.moveTo(a.x, a.y)
      g.lineTo(turn.start.x, turn.start.y)
      turn.trace(g)
    }
    softShadow(g, 2.5, 1, () => { path(); g.strokeStyle = 'rgba(0,0,0,0.25)'; g.lineWidth = 2.4; g.stroke() })
    path()
    g.strokeStyle = hex(0x9c8460, 0.85)
    g.lineWidth = 3.6
    g.stroke()
    g.strokeStyle = PALETTE.thread
    g.lineWidth = 2.4
    g.stroke()
    g.restore()
  },

  /** Behind the panel (its top edge at the picture's bottom, `edge`): the
   *  rest of the bend over the edge at `x`, then hanging slack across to `back`. */
  receding(g: G, x: number, edge: number, back: Pt) {
    const turn = TsugumiThread.bend(x, edge)
    const from = turn.end
    // the slack: the lowest point sits below the straight line between the ends
    const sag = Math.min(26, Math.max(8, Math.hypot(back.x - from.x, back.y - from.y) * 0.12))
    const path = () => {
      g.beginPath()
      g.moveTo(turn.start.x, turn.start.y)
      turn.trace(g)
      g.bezierCurveTo(from.x + (back.x - from.x) * 0.3, from.y + (back.y - from.y) * 0.3 + sag * 1.6,
        from.x + (back.x - from.x) * 0.75, from.y + (back.y - from.y) * 0.75 + sag, back.x, back.y)
    }
    g.save()
    g.lineWidth = 2
    g.lineCap = 'round'
    g.lineJoin = 'round'
    softShadow(g, 1.5, 2.5, () => { path(); g.strokeStyle = 'rgba(0,0,0,0.3)'; g.stroke() })
    path()
    g.strokeStyle = PALETTE.thread
    g.stroke()
    g.restore()
  },

  /** From the ball's top `a` up to the bill `b`, bowing out a little toward
   *  the beak's side (the key visual's thread). */
  up(g: G, a: Pt, b: Pt) {
    g.save()
    g.beginPath()
    g.moveTo(a.x, a.y)
    g.bezierCurveTo(a.x + 3, a.y + (b.y - a.y) * 0.3, b.x + 1, a.y + (b.y - a.y) * 0.7, b.x, b.y)
    g.strokeStyle = PALETTE.thread
    g.lineWidth = 2
    g.lineCap = 'round'
    g.stroke()
    g.restore()
  },

  /** A let-go thread: from the ball's top `a`, sagging to its end `e`, then a
   *  short tail curling down. */
  slack(g: G, a: Pt, e: Pt, time: number) {
    const d = Math.hypot(e.x - a.x, e.y - a.y)
    const w = Math.sin(time * 1.3) * 1.2
    g.save()
    g.beginPath()
    g.moveTo(a.x, a.y)
    g.bezierCurveTo(a.x + (e.x - a.x) * 0.3, a.y - Math.min(6, d * 0.2), a.x + (e.x - a.x) * 0.8, e.y - Math.min(5, d * 0.15), e.x, e.y)
    g.bezierCurveTo(e.x + 5, e.y - 2, e.x + 7, e.y + 6, e.x + 6 + w, e.y + 12)
    g.strokeStyle = PALETTE.thread
    g.lineWidth = 2
    g.lineCap = 'round'
    g.lineJoin = 'round'
    g.stroke()
    g.restore()
  },

  /** A short loose end out of the bill `s`, swaying lightly. */
  loose(g: G, s: Pt, time: number) {
    const w = Math.sin(time * 1.3) * 3
    g.save()
    g.beginPath()
    g.moveTo(s.x, s.y)
    g.bezierCurveTo(s.x + 6, s.y - 12, s.x + 16, s.y - 2 + w, s.x + 26, s.y - 8 + w)
    g.strokeStyle = PALETTE.thread
    g.lineWidth = 2
    g.lineCap = 'round'
    g.stroke()
    g.restore()
  },
}

/** Draws `fn` offset and softly blurred (where the canvas can blur). */
function softShadow(g: G, dx: number, dy: number, fn: () => void) {
  g.save()
  g.translate(dx, dy)
  g.filter = `blur(${1.2 * g.getTransform().a}px)`
  fn()
  g.restore()
}

// ---- Temari --------------------------------------------------------------------------

type V = [number, number, number]
const add = (a: V, b: V): V => [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
const sub = (a: V, b: V): V => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
const mul = (a: V, k: number): V => [a[0] * k, a[1] * k, a[2] * k]
const dot = (a: V, b: V) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
const norm = (a: V): V => mul(a, 1 / Math.sqrt(dot(a, a)))
const cross = (a: V, b: V): V => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]

/** The temari (wound and stitched) the bird unwinds, low-poly like the bird: a ball of
 *  若草 thread with four wound bands in two tones (docs/assets/tsugumi-icon.svg #temari). */
export const TsugumiTemari = {
  /** Where the thread leaves the ball: its top. */
  lead(center: Pt, radius: number): Pt {
    return { x: center.x, y: center.y - radius }
  },

  /** Draws the ball resting on `center.y + radius`; `spin` (radians) turns it
   *  clockwise as thread comes off its top. */
  draw(g: G, center: Pt, radius: number, spin: number) {
    g.save()
    g.beginPath()
    g.ellipse(center.x, center.y + radius * 0.82 + radius * 0.15, radius * 0.95, radius * 0.15, 0, 0, 2 * Math.PI)
    g.fillStyle = 'rgba(0,0,0,0.28)'
    g.fill()
    g.translate(center.x, center.y)
    g.scale(radius, radius)
    const shape = buildTemari(spin)
    polygon(g, shape.outline)
    g.clip()
    g.lineWidth = 1 / radius
    for (const f of shape.facets) {
      polygon(g, f.points)
      g.fillStyle = g.strokeStyle = f.color
      g.fill()
      g.stroke()
    }
    g.restore()
  },
}

/** Facets of the unit ball (centre at the origin, y down). */
function buildTemari(spin: number) {
  const ry = (38 * Math.PI) / 180, rx = (-28 * Math.PI) / 180
  const view = (v: V): V => {
    const x = v[0] * Math.cos(ry) + v[2] * Math.sin(ry), z = -v[0] * Math.sin(ry) + v[2] * Math.cos(ry)
    const y = v[1] * Math.cos(rx) - z * Math.sin(rx), z2 = v[1] * Math.sin(rx) + z * Math.cos(rx)
    // the spin, clockwise on screen (the top moves toward the bird)
    return [x * Math.cos(spin) + y * Math.sin(spin), -x * Math.sin(spin) + y * Math.cos(spin), z2]
  }
  const screen = (v: V): Pt => ({ x: v[0], y: -v[1] })
  const shade = (v: number, k: number) => {
    const c = (shift: number) => Math.round(Math.min(1, ((v >> shift) & 0xff) / 255 * k) * 255)
    return `rgb(${c(16)},${c(8)},${c(0)})`
  }
  const light = norm([-0.45, 0.6, 0.66])
  const rng = new SeededRandom(5)
  const facets: { points: Pt[]; color: string }[] = []
  const tri = (a: V, b: V, c: V, color: number) => {
    if (!((a[2] + b[2] + c[2]) / 3 > 0.02)) return // the far side
    let n = norm(cross(sub(b, a), sub(c, a)))
    if (n[2] < 0) n = mul(n, -1)
    const k = 0.62 + 0.5 * Math.max(0, dot(n, light)) + rng.next(-0.04, 0.04)
    facets.push({ points: [screen(a), screen(b), screen(c)], color: shade(color, k) })
  }

  // the ball: an icosahedron subdivided once
  const t = (1 + Math.sqrt(5)) / 2
  const vs: V[] = ([[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t],
    [0, -1, -t], [0, 1, -t], [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]] as V[]).map(norm)
  let faces: [number, number, number][] = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4],
    [11, 10, 2], [10, 7, 6], [7, 1, 8], [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11],
    [6, 2, 10], [8, 6, 7], [9, 8, 1]]
  const mids = new Map<number, number>()
  const mid = (a: number, b: number) => {
    const key = Math.min(a, b) * 1000 + Math.max(a, b)
    const m = mids.get(key)
    if (m !== undefined) return m
    vs.push(norm(mul(add(vs[a], vs[b]), 0.5)))
    mids.set(key, vs.length - 1)
    return vs.length - 1
  }
  faces = faces.flatMap(([a, b, c]) => {
    const ab = mid(a, b), bc = mid(b, c), ca = mid(c, a)
    return [[a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]] as [number, number, number][]
  })
  const seen = vs.map(view)
  for (const [a, b, c] of faces) tri(seen[a], seen[b], seen[c], 0x9fb070)

  // the bands: strips along great circles, two tones (two groups of threads)
  const bands: [V, number, number][] = [[[1, 0, 0], 0xf6ecd8, 0xd8c49e], [[0, 1, 0], 0xd58b49, 0xa35c29],
    [[0, 0, 1], 0xf6ecd8, 0xd8c49e], [[1, 1, 1], 0xd58b49, 0xa35c29]]
  const steps = 14, spread = 0.15, lift = 1.025
  for (const [n0, lightHex, darkHex] of bands) {
    const n = norm(n0)
    const u = norm(cross(n, Math.abs(n[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0])), w = cross(n, u)
    const at = (i: number, d: number) => {
      const a = (2 * Math.PI * i) / steps, r = Math.sqrt(1 - d * d) * lift
      return view(add(mul(n, d * lift), mul(add(mul(u, Math.cos(a)), mul(w, Math.sin(a))), r)))
    }
    for (let i = 0; i < steps; i++) {
      for (const [lo, hi, color] of [[-spread, 0, lightHex], [0, spread, darkHex]]) {
        tri(at(i, lo), at(i + 1, lo), at(i + 1, hi), color)
        tri(at(i, lo), at(i + 1, hi), at(i, hi), color)
      }
    }
  }
  return { facets, outline: hull(seen.map(screen)) }
}

/** Convex hull (monotone chain): the ball's silhouette. */
function hull(points: Pt[]): Pt[] {
  const ps = [...points].sort((a, b) => a.x - b.x || a.y - b.y)
  const half = (seq: Pt[]) => {
    const h: Pt[] = []
    for (const p of seq) {
      while (h.length >= 2) {
        const a = h[h.length - 2], b = h[h.length - 1]
        if ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) > 0) break
        h.pop()
      }
      h.push(p)
    }
    return h
  }
  return [...half(ps).slice(0, -1), ...half([...ps].reverse()).slice(0, -1)]
}

// ---- Branch --------------------------------------------------------------------------

/** A faceted branch from x0 to x1; `grown` (0…1) is the part with bark and
 *  leaves. The rest is a faint outline. */
export function drawBranch(g: G, x0: number, x1: number, y: number, grown: number, time: number) {
  const segs = 14
  const fx = x0 + (x1 - x0) * grown
  const yAt = (x: number) => y + Math.sin(x * 0.045) * 2.5
  const poly = (pts: Pt[], color: string) => { polygon(g, pts); g.fillStyle = color; g.fill() }
  for (let i = 0; i < segs; i++) {
    const a = x0 + ((x1 - x0) * i) / segs, b = x0 + ((x1 - x0) * (i + 1)) / segs
    const top = [{ x: a, y: yAt(a) - 3.5 }, { x: b, y: yAt(b) - 3.5 }, { x: b, y: yAt(b) }, { x: a, y: yAt(a) }]
    const bot = [{ x: a, y: yAt(a) }, { x: b, y: yAt(b) }, { x: b, y: yAt(b) + 3.5 }, { x: a, y: yAt(a) + 3.5 }]
    if (b <= fx + 0.5) {
      poly(top, i % 2 === 0 ? PALETTE.bark : hex(0x6b4a2b))
      poly(bot, i % 2 === 0 ? PALETTE.barkDark : PALETTE.barkShade)
    } else {
      poly([...top, ...bot.reverse()], 'rgba(255,255,255,0.08)')
    }
  }
  // leaves sprout once the bark reaches them, scattered: seeded spacing,
  // side, angle, size and shade, sometimes two from one spot
  const rng = new SeededRandom(23)
  let sx = x0 + rng.next(8, 20)
  let i = 0
  while (sx < x1 - 6) {
    const count = rng.next(0, 1) < 0.3 ? 2 : 1
    for (let k = 0; k < count; k++, i++) {
      const up = rng.next(0, 1) < 0.55
      const angle = (up ? -1 : 1) * rng.next(0.35, 1.35)
      const size = rng.next(0.7, 1.25)
      const tone = rng.next(0, 1)
      const grow = Math.min(1, Math.max(0, (fx - sx) / 22))
      if (grow <= 0) continue
      g.save()
      g.translate(sx, yAt(sx) + (up ? -2.5 : 2.5))
      g.rotate(angle + Math.sin(time * 2 + i) * 0.08)
      g.scale(grow * size, grow * size)
      const [lightC, darkC] = tone < 0.4 ? [PALETTE.olive, PALETTE.oliveDark]
        : tone < 0.75 ? [hex(0xb6c28c), hex(0x879b52)] : [hex(0x9fb070), hex(0x6d8240)]
      poly([{ x: 0, y: 0 }, { x: 18, y: 0 }, { x: 9, y: -5 }], lightC)
      poly([{ x: 0, y: 0 }, { x: 18, y: 0 }, { x: 9, y: 4 }], darkC)
      g.restore()
    }
    sx += rng.next(10, 34)
  }
}

// ---- Scenes --------------------------------------------------------------------------

/** Where the bird of the progress scene stands: it stays on its step and,
 *  when the step moves on, hops there (a hop longer for a longer way). */
export class HopWalker {
  private from?: number
  private to = 0
  private start = 0
  private length = 0.36

  position(step: number, t: number, animated: boolean): [number, BirdPose] {
    if (!animated || this.from === undefined) {
      this.from = step
      this.to = step
      return [step, still()]
    }
    if (step !== this.to) {
      // from wherever it is now (mid-hop included) to the new step
      const here = this.current(t, this.from)
      this.from = here
      this.to = step
      this.start = t
      this.length = Math.min(0.6, 0.3 + Math.abs(step - here) / 400)
    }
    const u = (t - this.start) / this.length
    if (u >= 1) {
      this.from = this.to
      return [this.to, still()]
    }
    const height = Math.min(16, 7 + Math.abs(this.to - this.from) * 0.12)
    return [this.current(t, this.from), BirdMotion.hop(u, height)]
  }

  private current(t: number, from: number) {
    const u = Math.min(1, Math.max(0, (t - this.start) / this.length))
    const eased = u * u * (3 - 2 * u)
    return from + (this.to - from) * eased
  }
}

/** Where the scene's bird stands (in the picture) and its size. */
export function birdFeet(w: number, h: number, fraction: number | undefined, stride = 18): { feet: Pt; scale: number } {
  const B = TsugumiBird
  const y = h - 25
  if (fraction === undefined) {
    const s = 0.22
    return { feet: { x: w / 2 + (B.feet.x - (B.tailTip + B.beakTip) / 2) * s, y }, scale: s }
  }
  const s = 0.2, x0 = 24, x1 = w - 24
  const tip = x0 + (x1 - x0) * fraction
  const lo = x0 + (B.feet.x - B.tailTip) * s - 8
  const want = Math.max(lo, tip - (B.beakTip - B.feet.x) * s + 6)
  return { feet: { x: lo + Math.floor((want - lo) / stride) * stride, y }, scale: s }
}

/** Indeterminate: the bird hops and pecks on a short perch, holding the thread
 *  that catches on the lower edge at `catchX`. */
export function drawIdleScene(g: G, w: number, h: number, t: number, catchX: number, hidesBird: boolean) {
  const B = TsugumiBird, scale = 0.22
  const y = h - 22, mid = w / 2
  drawBranch(g, mid - 110, mid + 110, y, 1, t)
  const pose = BirdMotion.idle(t)
  // centre the body (tail…beak) over the perch; the shadow shrinks in the air
  const fx = mid + (B.feet.x - (B.tailTip + B.beakTip) / 2) * scale
  const s = 1 - pose.hop / 40
  g.beginPath()
  g.ellipse(fx, y - 3, 30 * s, 3, 0, 0, 2 * Math.PI)
  g.fillStyle = 'rgba(0,0,0,0.2)'
  g.fill()
  if (hidesBird) return
  const feet = { x: fx, y: y - 3 }
  const bill = birdPoint(B.gape, feet, scale, pose)
  // over the lower edge and away into the picture, in front of the bird
  drawBird(g, feet, scale, pose, (g) => {
    TsugumiThread.receding(g, catchX, h, bill)
    TsugumiThread.loose(g, bill, t)
  })
}

/** Determinate: the branch is the bar; the bird hops along the growing tip
 *  (a step at a time), drawing out the thread that catches on the lower edge at `catchX`. */
export function drawProgressScene(g: G, w: number, h: number, t: number, fraction: number, catchX: number,
  walker: HopWalker, moving: boolean, hidesBird: boolean) {
  const B = TsugumiBird, scale = 0.2, stride = 18
  const x0 = 24, x1 = w - 24, y = h - 22
  drawBranch(g, x0, x1, y, fraction, t)
  const step = birdFeet(w, h, fraction, stride).feet.x
  let [fx, pose] = walker.position(step, t, moving)
  if (fraction >= 1) {
    pose = BirdMotion.hop((t % 0.7) / 0.7, 18)
    pose.flap = 1
    fx = step
  }
  pose.blink = Math.max(pose.blink, BirdMotion.blink(t, 2.7))
  if (hidesBird) return
  const feet = { x: fx, y: y - 3 }
  const bill = birdPoint(B.gape, feet, scale, pose)
  drawBird(g, feet, scale, pose, (g) => {
    TsugumiThread.receding(g, catchX, h, bill)
    TsugumiThread.loose(g, bill, t)
  })
}

/** The temari beside the loading words: its thread runs straight up from the
 *  ball to `catchX` on the top edge, where it hooks over into the picture. */
export function drawSpool(g: G, w: number, h: number, t: number, fraction: number | undefined, x: number, catchX: number) {
  const r = 15 * (1 - 0.35 * (fraction ?? 0))
  const ball = { x, y: 46 }
  const spin = fraction !== undefined ? fraction * 5 * Math.PI : 0.25 * Math.sin((t * 2 * Math.PI) / 3.2)
  TsugumiTemari.draw(g, ball, r, spin)
  TsugumiThread.near(g, TsugumiTemari.lead(ball, r), catchX, w, h)
}

/** Home's bird with the key visual's temari (TsugumiTemariScene.swift). One
 *  cycle: the ball rolls away along the perch, paying out thread; the bird
 *  lets go and the end curls up over the ball; it hops about, pecks the end up
 *  again and winds the ball back in, leaning back. The ball turns as it rolls. */
export function drawTemariScene(g: G, w: number, h: number, t: number, birdScale = 0.19, distance = 30, period = 8) {
  const B = TsugumiBird, s = birdScale
  const smooth = (x: number) => { x = Math.min(1, Math.max(0, x)); return x * x * (3 - 2 * x) }
  const span = (u: number, a: number, b: number) => Math.min(1, Math.max(0, (u - a) / (b - a)))
  const y = h - 22, mid = w / 2
  drawBranch(g, mid - 110, mid + 110, y, 1, t)
  // centre the body (tail…beak) over the perch
  const fx = mid + (B.feet.x - (B.tailTip + B.beakTip) / 2) * s
  const feet = { x: fx, y: y - 3 }
  const u = (t / period) % 1

  // the ball: out while the end is let go, back while winding in
  const out = distance * (u < 0.30 ? smooth(u / 0.30) : u < 0.76 ? 1 : 1 - smooth((u - 0.76) / 0.24))
  const r = 70 * s
  const ball = { x: fx + 199 * s + out, y: y - 3 - r }
  const lead = TsugumiTemari.lead(ball, r)
  // wound thread keeps its curl: the let-go end stands in a small loop over
  // the top of the ball, a little forward, within reach of the bill
  const rest = { x: lead.x + r * 0.35, y: lead.y - r * 0.5 }
  // the tilt (with a little crouch) that brings the bill to that end
  const billAt = (tilt: number) => birdPoint(B.gape, feet, s, { ...still(), tilt, squash: 0.1 })
  let reach = 0, best = Infinity
  for (let i = 0; i <= 90; i++) {
    const p = billAt(i / 100), d = Math.hypot(p.x - rest.x, p.y - rest.y)
    if (d < best) { best = d; reach = i / 100 }
  }

  let pose = still()
  let holding = true
  if (u < 0.30) {                         // paying out, a small hop midway
    pose = BirdMotion.hop(span(u, 0.12, 0.22), 8)
  } else if (u < 0.37) {                  // let go: a little toss of the head
    pose.tilt = -0.14 * Math.sin(span(u, 0.30, 0.37) * Math.PI)
    holding = false
  } else if (u < 0.58) {                  // free: two hops, a look about
    holding = false
    if (u < 0.45) pose = BirdMotion.hop(span(u, 0.37, 0.45), 12)
    else if (u < 0.53) pose = BirdMotion.hop(span(u, 0.45, 0.53), 12)
    else pose.tilt = -0.08 * Math.sin(span(u, 0.53, 0.58) * Math.PI)
  } else if (u < 0.66) {                  // peck down to the end
    holding = false
    const k = smooth(span(u, 0.58, 0.66))
    pose.tilt = reach * k
    pose.squash = 0.1 * k
  } else if (u < 0.70) {                  // got it
    pose.tilt = reach
    pose.squash = 0.1
  } else if (u < 0.76) {                  // head up with it
    const k = 1 - smooth(span(u, 0.70, 0.76))
    pose.tilt = reach * k
    pose.squash = 0.1 * k
  } else {                                // winding in, leaning back
    pose.tilt = -0.10 * Math.sin(span(u, 0.76, 1) * Math.PI)
  }
  pose.blink = Math.max(pose.blink, BirdMotion.blink(t, period / 2, 0.4))
  const bill = birdPoint(B.gape, feet, s, pose)
  // the let-go end falls from where the bill let it go to its curl
  const from = birdPoint(B.gape, feet, s, still())
  const f = smooth(span(u, 0.30, 0.36))
  const dropped = { x: from.x + (rest.x - from.x) * f, y: from.y + (rest.y - from.y) * f }

  const k = 1 - pose.hop / 40 // the shadow shrinks in the air
  g.beginPath()
  g.ellipse(fx, y - 3, 30 * k, 3, 0, 0, 2 * Math.PI)
  g.fillStyle = 'rgba(0,0,0,0.2)'
  g.fill()
  drawBird(g, feet, s, pose, (g) => {
    if (!holding) return
    TsugumiThread.up(g, lead, bill)
    TsugumiThread.loose(g, bill, t)
  })
  TsugumiTemari.draw(g, ball, r, out / r)
  if (!holding) TsugumiThread.slack(g, lead, dropped, t)
}
