// The app's bird, temari and branch (macOS/Sources/App/Loading/TsugumiArt.swift),
// polygon for polygon, and the drawing helpers of the scenes.

export const hex = (v, a = 1) => `rgba(${(v >> 16) & 255},${(v >> 8) & 255},${v & 255},${a})`
export const Palette = {
  tealTop: 0x2d5a5c, cream: 0xf6ead2, mist: 0xa9c4bf, rufous: 0xc97d3d, olive: 0xa9b77a,
  oliveDark: 0x7d9148, bark: 0x7d5834, barkDark: 0x5c3e24, barkShade: 0x4e341e, thread: 0xefe0c3,
}

export const pt = (x, y) => ({ x, y })
export const rect = (x, y, w, h) => ({ x, y, w, h })
export const clamp01 = (x) => Math.min(1, Math.max(0, x))
export const smooth = (x) => { const u = clamp01(x); return u * u * (3 - 2 * u) }
export const easeOut = (x) => 1 - Math.pow(1 - clamp01(x), 3)
export const easeInOut = (x) => { const u = clamp01(x); return u < 0.5 ? 4 * u * u * u : 1 - Math.pow(-2 * u + 2, 3) / 2 }
/** Back-out: overshoots a little, then settles. */
export const pop = (x) => { const u = clamp01(x), c1 = 1.70158, c3 = c1 + 1; return 1 + c3 * Math.pow(u - 1, 3) + c1 * Math.pow(u - 1, 2) }
export const span = (p, a, b) => smooth((p - a) / (b - a))
export const lin = (p, a, b) => clamp01((p - a) / (b - a))
export const mix = (a, b, u) => a + (b - a) * u
export const mixP = (a, b, u) => pt(mix(a.x, b.x, u), mix(a.y, b.y, u))
export const mixR = (a, b, u) => rect(mix(a.x, b.x, u), mix(a.y, b.y, u), mix(a.w, b.w, u), mix(a.h, b.h, u))
export const rgb = (v) => [((v >> 16) & 255) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255]
export const mixColor = (a, b, u, alpha = 1) => {
  const c = [0, 1, 2].map((i) => Math.round((a[i] + (b[i] - a[i]) * u) * 255))
  return `rgba(${c[0]},${c[1]},${c[2]},${alpha})`
}
export const bezier = (a, b, c, d, s) => {
  const u = 1 - s, w0 = u * u * u, w1 = 3 * u * u * s, w2 = 3 * u * s * s, w3 = s * s * s
  return pt(w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x, w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y)
}
export const curve = (a, b, c, d, n = 24) => Array.from({ length: n + 1 }, (_, i) => bezier(a, b, c, d, i / n))

/** Same sequence for a seed, so a scene stays put from frame to frame. */
export function seeded(seed) {
  let s = (seed * 0x9e3779b1) >>> 0
  return (lo, hi) => {
    s = (s + 0x6d2b79f5) >>> 0
    let t = s
    t = Math.imul(t ^ (t >>> 15), t | 1)
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
    return lo + (hi - lo) * (((t ^ (t >>> 14)) >>> 0) / 4294967296)
  }
}

export class Polyline {
  constructor(points) {
    this.points = points
    this.lengths = [0]
    for (let i = 1; i < points.length; i++) this.lengths.push(this.lengths[i - 1] + Math.hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y))
  }
  get total() { return this.lengths[this.lengths.length - 1] ?? 0 }
  at(f) {
    if (this.points.length < 2) return this.points[0] ?? pt(0, 0)
    const d = clamp01(f) * this.total
    let i = 1
    while (i < this.lengths.length - 1 && this.lengths[i] < d) i++
    const seg = Math.max(0.0001, this.lengths[i] - this.lengths[i - 1])
    return mixP(this.points[i - 1], this.points[i], (d - this.lengths[i - 1]) / seg)
  }
  /** The points up to a share of the length. */
  upTo(f = 1) {
    if (!this.points.length) return []
    const d = clamp01(f) * this.total, out = [this.points[0]]
    for (let i = 1; i < this.points.length; i++) {
      if (this.lengths[i] <= d) out.push(this.points[i])
      else { out.push(this.at(f)); break }
    }
    return out
  }
}

// ---- Canvas shorthands ----

/** Runs `fn` faded to `o` (multiplies); nothing when it would not show. */
export function faded(g, o, fn) {
  if (o <= 0.002) return
  g.save()
  g.globalAlpha *= o
  fn()
  g.restore()
}
export function trace(g, pts, close = false) {
  g.beginPath()
  pts.forEach((q, i) => (i ? g.lineTo(q.x, q.y) : g.moveTo(q.x, q.y)))
  if (close) g.closePath()
}
export function stroke(g, pts, color, width = 1, opts = {}) {
  if (pts.length < 2) return
  trace(g, pts)
  g.strokeStyle = color
  g.lineWidth = width
  g.lineCap = opts.cap ?? 'round'
  g.lineJoin = 'round'
  g.setLineDash(opts.dash ?? [])
  g.lineDashOffset = opts.dashPhase ?? 0
  g.stroke()
  g.setLineDash([])
}
/** A facet: filled, with a hairline of its colour hiding the seams. */
export function poly(g, pts, color) {
  trace(g, pts, true)
  g.fillStyle = color
  g.fill()
  g.strokeStyle = color
  g.lineWidth = 0.8
  g.stroke()
}
export function fillRect(g, r, color) { g.fillStyle = color; g.fillRect(r.x, r.y, r.w, r.h) }
export function roundRect(g, r, radius) {
  g.beginPath()
  g.roundRect(r.x, r.y, r.w, Math.max(0, r.h), radius)
}
export function fillRound(g, r, radius, color) { roundRect(g, r, radius); g.fillStyle = color; g.fill() }
export function strokeRound(g, r, radius, color, width = 1, dash = []) {
  roundRect(g, r, radius)
  g.strokeStyle = color
  g.lineWidth = width
  g.setLineDash(dash)
  g.stroke()
  g.setLineDash([])
}
export function ellipse(g, cx, cy, rx, ry, color) {
  g.beginPath()
  g.ellipse(cx, cy, rx, ry, 0, 0, Math.PI * 2)
  g.fillStyle = color
  g.fill()
}
/** A soft glow, in place of a blurred ellipse (works in every browser). */
export function glow(g, cx, cy, rx, ry, v, a) {
  g.save()
  g.translate(cx, cy)
  g.scale(rx, ry)
  const grad = g.createRadialGradient(0, 0, 0, 0, 0, 1)
  grad.addColorStop(0, hex(v, a))
  grad.addColorStop(0.55, hex(v, a * 0.55))
  grad.addColorStop(1, hex(v, 0))
  g.fillStyle = grad
  g.beginPath()
  g.arc(0, 0, 1, 0, Math.PI * 2)
  g.fill()
  g.restore()
}
const SANS = 'ui-sans-serif, -apple-system, BlinkMacSystemFont, "Hiragino Sans", "Noto Sans JP", sans-serif'
const MONO = 'ui-monospace, SFMono-Regular, Menlo, monospace'
export const HAND = '"Yusei Magic Subset", "Klee One", "Hiragino Maru Gothic ProN", cursive'
export function text(g, s, at, size, { weight = 400, color = hex(Palette.cream), align = 'center', opacity = 1, mono = false, family = null } = {}) {
  if (opacity <= 0.002) return
  g.save()
  g.globalAlpha *= opacity
  g.font = `${weight} ${size}px ${family ?? (mono ? MONO : SANS)}`
  g.textAlign = align
  g.textBaseline = 'middle'
  g.fillStyle = color
  const lines = String(s).split('\n')
  lines.forEach((line, i) => g.fillText(line, at.x, at.y + (i - (lines.length - 1) / 2) * size * 1.3))
  g.restore()
}
/** The width of a line of text as `text` would draw it. */
export function textWidth(g, s, size, { weight = 400, mono = false, family = null } = {}) {
  g.save()
  g.font = `${weight} ${size}px ${family ?? (mono ? MONO : SANS)}`
  const w = Math.max(...String(s).split('\n').map((line) => g.measureText(line).width))
  g.restore()
  return w
}
/** A child's handwriting: each character a little turned, sized and lifted differently. */
export function childText(g, s, at, size, color, { opacity = 1, seed = 1, leading = false } = {}) {
  if (opacity <= 0.002) return
  const rng = seeded(seed)
  g.save()
  g.globalAlpha *= opacity
  g.textAlign = 'center'
  g.textBaseline = 'middle'
  g.fillStyle = color
  const lines = s.split('\n')
  lines.forEach((line, li) => {
    const glyphs = [...line].map((ch) => {
      const sc = rng(0.94, 1.08)
      const em = ch === ' ' ? 0.3 : ch.charCodeAt(0) < 128 ? 0.56 : 0.98
      return { ch, sc, adv: em * size * sc + rng(-1, 2), lift: rng(-0.05, 0.05) * size, turn: rng(-0.06, 0.06) }
    })
    const width = glyphs.reduce((a, q) => a + q.adv, 0)
    let x = leading ? at.x : at.x - width / 2
    const y = at.y + (li - (lines.length - 1) / 2) * size * 1.38 + rng(-2, 2)
    for (const q of glyphs) {
      g.save()
      g.translate(x + q.adv / 2, y + q.lift)
      g.rotate(q.turn)
      g.font = `${size * q.sc}px ${HAND}`
      g.fillText(q.ch, 0, 0)
      g.restore()
      x += q.adv
    }
  })
  g.restore()
}

/** The thread proper, with a soft shadow. */
export function drawThread(g, pts, width = 2.4) {
  if (pts.length < 2) return
  g.save()
  g.translate(1.5, 2.5)
  stroke(g, pts, 'rgba(0,0,0,0.22)', width)
  g.restore()
  stroke(g, pts, hex(Palette.thread), width)
}

// ---- The bird ----

const SOURCE = [
  ['body', '468,600 494,604 486,640 470,640', 0xd9c29e], ['body', '474,640 484,640 478,702 472,702', 0xc08d68],
  ['body', '472,702 478,694 512,704 476,708', 0xa8744f], ['body', '472,700 476,708 444,708', 0x8e5f3e],
  ['body', '528,592 554,596 546,636 530,636', 0xcdb48c], ['body', '534,636 544,636 540,700 534,700', 0xb07c58],
  ['body', '534,700 540,692 572,700 538,706', 0xa8744f], ['body', '534,698 538,706 508,706', 0x8e5f3e],
  ['body', '330,480 340,585 175,600', 0x4e3320], ['body', '175,600 340,585 215,655', 0x3a2617],
  ['body', '175,600 215,655 160,640', 0x2f1f13],
  ['body', '520,330 655,452 420,400', 0x6f4a2c], ['body', '655,452 605,545 420,400', 0xefe0c3],
  ['body', '420,400 605,545 330,480', 0xe2cca4], ['body', '330,480 605,545 560,610', 0xf4e9d4],
  ['body', '330,480 560,610 470,628', 0xe9d8b8], ['body', '330,480 470,628 340,585', 0xd4b98d],
  ['body', '655,452 690,500 605,545', 0xe6d3b0], ['body', '605,545 690,500 560,610', 0xf8efdd],
  ['body', '610,470 628,478 612,490', 0x3b2718], ['body', '575,500 594,505 580,520', 0x3b2718],
  ['body', '630,512 648,516 636,532', 0x3b2718], ['body', '548,540 566,546 552,560', 0x3b2718],
  ['body', '596,552 614,556 602,572', 0x3b2718], ['body', '520,575 536,580 524,592', 0x3b2718],
  ['body', '570,590 586,593 576,606', 0x3b2718],
  ['body', '590,298 665,320 628,366', 0x8a5d36], ['body', '520,330 590,298 628,366', 0x7a5232],
  ['body', '665,320 694,356 628,366', 0x9a6a3f], ['body', '520,330 628,366 655,452', 0xc9a77a],
  ['body', '628,366 694,356 688,402', 0x3b2718], ['body', '628,366 688,402 655,452', 0xf3e6cc],
  ['body', '655,452 688,402 690,500', 0xeadbbd],
  ['body', '575,338 662,334 668,346 592,352', 0xf1e2c4],
  ['eye', '628,354 641,361 641,376 628,383 615,376 615,361', 0x16100b],
  ['highlight', '623,360 631,358 628,366', 0xffffff, 0.9],
  ['body', '694,356 776,394 728,384', 0x2e2117], ['body', '688,402 694,356 728,384', 0x3d2c1f],
  ['body', '688,402 728,384 776,394', 0xd9a441],
  ['wing', '545,402 440,418 455,500', 0xb0672f], ['wing', '545,402 455,500 525,522', 0xc97d3d],
  ['wing', '440,418 298,560 455,500', 0x8f4f22], ['wing', '455,500 298,560 525,522', 0xa35c29],
  ['wing', '298,560 525,522 400,560', 0x6f3c1a], ['wing', '545,402 525,522 565,470', 0xd58b49],
  ['wing', '380,470 300,556 410,505', 0x5a3015, 0.6],
]
const FACETS = SOURCE.map(([part, pts, color, a = 1]) => {
  const path = new Path2D()
  pts.split(' ').forEach((pair, i) => {
    const [x, y] = pair.split(',').map(Number)
    if (i) path.lineTo(x, y); else path.moveTo(x, y)
  })
  path.closePath()
  return { part, path, color: hex(color, a) }
})
export const Bird = { feet: pt(505, 706), shoulder: pt(545, 402), eye: pt(628, 368), gape: pt(752, 388) }

export const pose = (o = {}) => ({ hop: 0, tilt: 0, squash: 0, blink: 0, flap: 0, ...o })

/** Where a point of the bird lands when drawn with `drawBird`. */
export function birdPoint(q, at, scale, p) {
  const x = (q.x - Bird.feet.x) * scale * (1 + p.squash * 0.5), y = (q.y - Bird.feet.y) * scale * (1 - p.squash)
  const c = Math.cos(p.tilt), s = Math.sin(p.tilt)
  return pt(at.x + x * c - y * s, at.y - p.hop + x * s + y * c)
}
export const birdGape = (at, scale, p) => birdPoint(Bird.gape, at, scale, p)

export function drawBird(g, at, scale, p, mirrored = false) {
  g.save()
  if (mirrored) { g.translate(at.x, 0); g.scale(-1, 1); g.translate(-at.x, 0) }
  g.translate(at.x, at.y - p.hop)
  g.rotate(p.tilt)
  g.scale(scale * (1 + p.squash * 0.5), scale * (1 - p.squash))
  g.translate(-Bird.feet.x, -Bird.feet.y)
  const open = Math.max(0.1, 1 - p.blink)
  g.lineWidth = 1.2
  for (const f of FACETS) {
    if (f.part === 'highlight' && open < 0.5) continue
    g.save()
    if (f.part === 'wing') {
      g.translate(Bird.shoulder.x, Bird.shoulder.y); g.rotate(-0.45 * p.flap); g.translate(-Bird.shoulder.x, -Bird.shoulder.y)
    } else if (f.part === 'eye' || f.part === 'highlight') {
      g.translate(Bird.eye.x, Bird.eye.y); g.scale(1, open); g.translate(-Bird.eye.x, -Bird.eye.y)
    }
    g.fillStyle = f.color
    g.fill(f.path)
    if (f.part !== 'highlight') { g.strokeStyle = f.color; g.stroke(f.path) }
    g.restore()
  }
  g.restore()
}

export const Motion = {
  hop(u, height) {
    const p = pose({ hop: height * Math.sin(u * Math.PI), flap: Math.sin(u * Math.PI) })
    if (u < 0.12) p.squash = (0.12 - u) * 0.6
    if (u > 0.88) p.squash = (u - 0.88) * 0.6
    return p
  },
  blink(t, every, offset = 0) {
    const u = (t + offset) % every
    return u < 0.16 ? Math.sin((u / 0.16) * Math.PI) : 0
  },
  /** Indeterminate loop (3.2 s): hop, hop, look up, peck, peck, blink. */
  idle(t) {
    const u = t % 3.2
    let p = pose()
    if (u < 0.45) p = Motion.hop(u / 0.45, 14)
    else if (u < 0.55) p.squash = 0.03
    else if (u < 1) p = Motion.hop((u - 0.55) / 0.45, 14)
    else if (u < 1.6) p.tilt = -0.1 * Math.sin(((u - 1) / 0.6) * Math.PI)
    else if (u >= 1.7 && u < 2.05) p.tilt = 0.38 * Math.sin(((u - 1.7) / 0.35) * Math.PI)
    else if (u >= 2.1 && u < 2.45) p.tilt = 0.38 * Math.sin(((u - 2.1) / 0.35) * Math.PI)
    p.blink = Math.max(p.blink, Motion.blink(u, 3.2, 0.4))
    return p
  },
  flying(t, lean = 0.12) { return pose({ flap: 0.5 + 0.5 * Math.sin(t * 22), tilt: lean }) },
}

// ---- The temari ----

const v3 = {
  add: (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]],
  sub: (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]],
  mul: (a, k) => [a[0] * k, a[1] * k, a[2] * k],
  dot: (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2],
  cross: (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]],
  norm: (a) => v3.mul(a, 1 / Math.hypot(a[0], a[1], a[2])),
}

function buildTemari() {
  const { norm, cross, sub, add, mul, dot } = v3
  const ry = (38 * Math.PI) / 180, rx = (-28 * Math.PI) / 180
  const view = (v) => {
    const x = v[0] * Math.cos(ry) + v[2] * Math.sin(ry), z = -v[0] * Math.sin(ry) + v[2] * Math.cos(ry)
    return [x, v[1] * Math.cos(rx) - z * Math.sin(rx), v[1] * Math.sin(rx) + z * Math.cos(rx)]
  }
  const screen = (v) => pt(v[0], -v[1])
  const shade = (h, k) => {
    const c = (s) => Math.round(Math.min(1, (((h >> s) & 255) / 255) * k) * 255)
    return `rgb(${c(16)},${c(8)},${c(0)})`
  }
  const light = norm([-0.45, 0.6, 0.66])
  const rng = seeded(5)
  const facets = []
  const tri = (a, b, c, h) => {
    if ((a[2] + b[2] + c[2]) / 3 <= 0.02) return
    let n = norm(cross(sub(b, a), sub(c, a)))
    if (n[2] < 0) n = mul(n, -1)
    const k = 0.62 + 0.5 * Math.max(0, dot(n, light)) + rng(-0.04, 0.04)
    facets.push({ points: [screen(a), screen(b), screen(c)], color: shade(h, k) })
  }
  const t = (1 + Math.sqrt(5)) / 2
  const vs = [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t], [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]].map(norm)
  let faces = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8], [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1]]
  const mids = new Map()
  const mid = (a, b) => {
    const key = Math.min(a, b) * 1000 + Math.max(a, b)
    if (!mids.has(key)) { vs.push(norm(mul(add(vs[a], vs[b]), 0.5))); mids.set(key, vs.length - 1) }
    return mids.get(key)
  }
  faces = faces.flatMap(([a, b, c]) => {
    const ab = mid(a, b), bc = mid(b, c), ca = mid(c, a)
    return [[a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]]
  })
  const seen = vs.map(view)
  for (const [a, b, c] of faces) tri(seen[a], seen[b], seen[c], 0x9fb070)
  const bands = [[[1, 0, 0], 0xf6ecd8, 0xd8c49e], [[0, 1, 0], 0xd58b49, 0xa35c29], [[0, 0, 1], 0xf6ecd8, 0xd8c49e], [[1, 1, 1], 0xd58b49, 0xa35c29]]
  const steps = 14, spread = 0.15, lift = 1.025
  for (const [n0, lightHex, darkHex] of bands) {
    const n = norm(n0)
    const u = norm(cross(n, Math.abs(n[2]) < 0.9 ? [0, 0, 1] : [1, 0, 0])), w = cross(n, u)
    const at = (i, d) => {
      const a = (2 * Math.PI * i) / steps, r = Math.sqrt(1 - d * d) * lift
      return view(add(mul(n, d * lift), mul(add(mul(u, Math.cos(a)), mul(w, Math.sin(a))), r)))
    }
    for (let i = 0; i < steps; i++) {
      for (const [lo, hi, h] of [[-spread, 0, lightHex], [0, spread, darkHex]]) {
        tri(at(i, lo), at(i + 1, lo), at(i + 1, hi), h)
        tri(at(i, lo), at(i + 1, hi), at(i, hi), h)
      }
    }
  }
  return { facets: facets.map((f) => { const p = new Path2D(); f.points.forEach((q, i) => (i ? p.lineTo(q.x, q.y) : p.moveTo(q.x, q.y))); p.closePath(); return { path: p, color: f.color } }) }
}
let temari = null

/** The ball resting on center.y + radius; `spin` turns it as thread comes off its top. */
export function drawTemari(g, center, radius, spin) {
  temari ??= buildTemari()
  ellipse(g, center.x, center.y + radius * 0.97, radius * 0.95, radius * 0.15, 'rgba(0,0,0,0.28)')
  g.save()
  g.translate(center.x, center.y)
  g.scale(radius, radius)
  g.beginPath()
  g.arc(0, 0, 1.01, 0, Math.PI * 2)
  g.clip()
  g.rotate(spin)
  g.lineWidth = 1 / radius
  for (const f of temari.facets) {
    g.fillStyle = f.color
    g.fill(f.path)
    g.strokeStyle = f.color
    g.stroke(f.path)
  }
  g.restore()
}
export const temariLead = (center, radius) => pt(center.x, center.y - radius)

// ---- The branch ----

export function drawBranch(g, x0, x1, y, time) {
  const segs = 14
  const yAt = (x) => y + Math.sin(x * 0.045) * 2.5
  const fill = (pts, color) => { trace(g, pts, true); g.fillStyle = color; g.fill() }
  for (let i = 0; i < segs; i++) {
    const a = x0 + ((x1 - x0) * i) / segs, b = x0 + ((x1 - x0) * (i + 1)) / segs
    fill([pt(a, yAt(a) - 3.5), pt(b, yAt(b) - 3.5), pt(b, yAt(b)), pt(a, yAt(a))], i % 2 === 0 ? hex(Palette.bark) : hex(0x6b4a2b))
    fill([pt(a, yAt(a)), pt(b, yAt(b)), pt(b, yAt(b) + 3.5), pt(a, yAt(a) + 3.5)], i % 2 === 0 ? hex(Palette.barkDark) : hex(Palette.barkShade))
  }
  const rng = seeded(23)
  let sx = x0 + rng(8, 20), i = 0
  while (sx < x1 - 6) {
    const count = rng(0, 1) < 0.3 ? 2 : 1
    for (let c = 0; c < count; c++, i++) {
      const up = rng(0, 1) < 0.55
      const angle = (up ? -1 : 1) * rng(0.35, 1.35), size = rng(0.7, 1.25), shade = rng(0, 1)
      g.save()
      g.translate(sx, yAt(sx) + (up ? -2.5 : 2.5))
      g.rotate(angle + Math.sin(time * 2 + i) * 0.08)
      g.scale(size, size)
      const [light, dark] = shade < 0.4 ? [Palette.olive, Palette.oliveDark] : shade < 0.75 ? [0xb6c28c, 0x879b52] : [0x9fb070, 0x6d8240]
      fill([pt(0, 0), pt(18, 0), pt(9, -5)], hex(light))
      fill([pt(0, 0), pt(18, 0), pt(9, 4)], hex(dark))
      g.restore()
    }
    sx += rng(10, 34)
  }
}
