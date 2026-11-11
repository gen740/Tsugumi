// The figures of the story pages, ported from the app's Learn about Tsugumi window
// (macOS/Sources/App/Explainer/): a scene is a pure function of its story
// position `p` (where the reading line is in the figure) and the clock `t`
// (idle motion only), so scrolling back plays it back. In 1000-wide coordinates.
import {
  Palette, Polyline, birdGape, childText, clamp01, curve, drawBird, drawBranch, drawTemari, easeInOut, easeOut, ellipse, faded,
  fillRect, fillRound, glow, hex, lin, mix, mixColor, mixP, mixR, Motion, poly, pop, pose, pt, rect, rgb, roundRect, seeded, smooth, span,
  stroke, strokeRound, temariLead, text, trace,, textWidth } from './art.js'

export const scene = { since: -1 }
const artW = 1600, artH = 900
const Ink = {
  cream: hex(Palette.cream), mist: hex(Palette.mist), paper: rgb(0xf4ecdc), canvas: rgb(0x1d1e21), penC: hex(0x2b3a40), pen: rgb(0x2b3a40),
  cardFill: rgb(0x2b2c30), cardStroke: rgb(0x6d7177), wire: 0xe0893a, done: 0x5fbf6a, badge: 0xf0a050, ai: rgb(0x8fc6d6),
}
const css = (c) => mixColor(c, c, 0)
const maxX = (r) => r.x + r.w, maxY = (r) => r.y + r.h, midX = (r) => r.x + r.w / 2, midY = (r) => r.y + r.h / 2

// ---- A person, seen a little from the side, facing right ----

/** A limb in two facets, so it has a lit side and a shaded side. */
function limb(g, a, b, w0, w1, light, dark) {
  const dx = b.x - a.x, dy = b.y - a.y, l = Math.hypot(dx, dy) || 1, nx = -dy / l, ny = dx / l
  poly(g, [pt(a.x + nx * w0, a.y + ny * w0), pt(b.x + nx * w1, b.y + ny * w1), b, a], dark)
  poly(g, [pt(a.x - nx * w0, a.y - ny * w0), pt(b.x - nx * w1, b.y - ny * w1), b, a], light)
  ellipse(g, b.x, b.y, w1, w1, dark)
}

/** A hand: a palm, a block of fingers and a thumb, pointing along `angle`. */
function hand(g, at, angle, skin, shade) {
  g.save()
  g.translate(at.x, at.y)
  g.rotate(angle)
  poly(g, [pt(-1, -5.2), pt(8, -5.6), pt(9, 4.6), pt(-1, 4.4)], skin)
  poly(g, [pt(8, -5.6), pt(16, -4.6), pt(17, 3.4), pt(9, 4.6)], shade)
  poly(g, [pt(1, -5), pt(8, -9), pt(11, -6.5), pt(6, -3)], skin)
  g.restore()
}

export const Person = {
  skin: 0xf0dcc0, shade: 0xd9bf9e, hair: 0x2f2722, hairLight: 0x453a31,
  coat: [0xa9c9c3, 0x8bb0ab, 0x6d938f],
  /** `puzzled` brings the near hand to the chin; `reach`, a point relative to the feet, takes it there instead. Returns the top of the shoulders. */
  draw(g, f, t, { scale = 1.2, puzzled = 0, reach = null, coat = Person.coat, trousers = [0x24464a, 0x1b3739], tilt = null, hair = Person.hair } = {}) {
    g.save()
    g.translate(f.x, f.y)
    g.scale(scale, scale)
    const breathe = Math.sin(t * 1.6) * 0.9
    const skin = hex(Person.skin), shade = hex(Person.shade), c0 = hex(coat[0]), c1 = hex(coat[1]), c2 = hex(coat[2])
    const shoe = hex(0x2a2320), shoeTop = hex(0x3a302a)
    const sh = -170 + breathe, waist = -100
    // the far arm and far leg, behind everything
    const farShoulder = pt(-20, sh + 8), farElbow = pt(-29, sh + 50), farWrist = pt(-25, sh + 88)
    limb(g, farShoulder, farElbow, 9, 7.5, c2, hex(coat[2]))
    limb(g, farElbow, farWrist, 7.5, 5.5, c2, hex(coat[2]))
    hand(g, pt(farWrist.x, farWrist.y + 2), 1.72, shade, shade)
    limb(g, pt(-9, waist), pt(-11, -52), 10.5, 8, hex(trousers[1]), hex(trousers[1]))
    limb(g, pt(-11, -52), pt(-11, -8), 8, 6, hex(trousers[1]), hex(trousers[1]))
    poly(g, [pt(-19, 0), pt(10, 0), pt(9, -6), pt(-3, -10), pt(-18, -10)], shoe)
    // the near leg
    limb(g, pt(9, waist), pt(10, -52), 11, 8.5, hex(trousers[0]), hex(trousers[1]))
    limb(g, pt(10, -52), pt(10, -8), 8.5, 6.5, hex(trousers[0]), hex(trousers[1]))
    poly(g, [pt(1, 0), pt(33, 0), pt(32, -6), pt(19, -11), pt(2, -11)], shoe)
    poly(g, [pt(19, -11), pt(32, -6), pt(24, -6), pt(14, -8)], shoeTop)
    // the coat
    poly(g, [pt(-27, sh + 10), pt(-30, -78), pt(-4, -74), pt(-2, sh + 4)], c1)
    poly(g, [pt(-2, sh + 4), pt(-4, -74), pt(30, -78), pt(26, sh + 10)], c0)
    poly(g, [pt(26, sh + 10), pt(30, -78), pt(18, -76), pt(16, sh + 22)], c2)
    poly(g, [pt(-27, sh + 10), pt(-14, sh + 1), pt(-2, sh + 4), pt(-16, sh + 26)], c0)
    poly(g, [pt(26, sh + 10), pt(14, sh + 1), pt(-2, sh + 4), pt(12, sh + 26)], c1)
    poly(g, [pt(-8, sh + 3), pt(9, sh + 3), pt(1, sh + 34)], hex(0xf3ead6))
    stroke(g, [pt(1, sh + 34), pt(0, -78)], hex(coat[2], 0.8), 1)
    // the head
    g.save()
    g.translate(1, sh + 1)
    g.rotate(tilt ?? puzzled * 0.1 * (1 + 0.15 * Math.sin(t * 1.3)))
    poly(g, [pt(-7, 5), pt(8, 5), pt(7, -9), pt(-6, -9)], shade)
    const face = [pt(-11, -41), pt(13, -41), pt(17, -30), pt(21, -22), pt(17, -19), pt(16, -12), pt(10, -5), pt(-5, -5), pt(-13, -16)]
    poly(g, face, skin)
    poly(g, [pt(-13, -16), pt(-5, -5), pt(10, -5), pt(4, -15)], shade)
    poly(g, [pt(17, -30), pt(21, -22), pt(17, -19), pt(13, -24)], shade)
    const hairDark = hex(hair)
    poly(g, [pt(-6, -50), pt(10, -49), pt(18, -39), pt(15, -35), pt(5, -38), pt(-4, -34), pt(-7, -21), pt(-15, -18), pt(-19, -31), pt(-15, -44)], hairDark)
    poly(g, [pt(-6, -50), pt(10, -49), pt(18, -39), pt(5, -43), pt(-9, -42)], hex(Person.hairLight))
    ellipse(g, -6, -22, 3.4, 4.6, shade)
    ellipse(g, 11, -29, 1.7, 2.3, hex(0x2a2320))
    stroke(g, [pt(7, -34.5), pt(15, -35.5)], hairDark, 1.3)
    stroke(g, [pt(10, -13), pt(14.5, -13.5)], hex(0xb08a70), 1.1)
    g.restore()
    // the near arm: down at the side, up to the chin, or out to what it reaches for
    const shoulder = pt(19, sh + 8)
    const wristDown = pt(24, sh + 90), elbowDown = pt(25, sh + 50)
    const wristChin = pt(13, sh - 5), elbowChin = pt(44, sh + 46)
    let wrist = mixP(wristDown, wristChin, puzzled), elbow = mixP(elbowDown, elbowChin, puzzled)
    if (reach) {
      // an arm has its length: the hand goes as far as that, and the elbow bends down to take up the rest
      const upper = 42, fore = 40
      const dx = reach.x / scale - shoulder.x, dy = reach.y / scale - shoulder.y
      const far = Math.hypot(dx, dy) || 1, d = Math.min(far, upper + fore - 1)
      const ux = dx / far, uy = dy / far
      const along = (upper * upper - fore * fore + d * d) / (2 * d), rise = Math.sqrt(Math.max(0, upper * upper - along * along))
      const side = -ux >= 0 ? -1 : 1  // towards the ground, whichever way it reaches
      wrist = pt(shoulder.x + ux * d, shoulder.y + uy * d)
      elbow = pt(shoulder.x + ux * along - uy * rise * side, shoulder.y + uy * along + ux * rise * side)
    }
    limb(g, shoulder, elbow, 9.5, 7.8, c0, c2)
    limb(g, elbow, wrist, 7.8, 5.6, c0, c2)
    const angle = Math.atan2(wrist.y - elbow.y, wrist.x - elbow.x)
    fillRound(g, rect(wrist.x - 5, wrist.y - 3, 10, 6), 2, c1)
    hand(g, wrist, angle, skin, shade)
    g.restore()
    return f.y + sh * scale
  },
}

// ---- The wall: a researcher, an AI, its output, a heap of reasons ----

export const Wall = {
  groundY: 700, human: pt(165, 700), wallX: [362, 638], brickH: 30, rows: 13, heap: null, over: pt(505, 268),
  temari: { center: pt(268, 678), radius: 22 }, aiCenter: pt(835, 450), tie: pt(758, 452),
  get wallTop() { return this.groundY - this.rows * this.brickH },
  get temariTop() { return temariLead(this.temari.center, this.temari.radius) },

  draw(g, p, t) {
    const enter = span(p, -0.45, 0.15)
    Wall.drawGround(g, enter)
    const ai = pt(Wall.aiCenter.x, Wall.aiCenter.y + Math.sin(t * 1.4) * 8)
    Wall.drawSheets(g, p, t, ai)
    faded(g, enter, () => Wall.drawAI(g, pt(ai.x, ai.y + (1 - enter) * 30), t, span(p, 4.7, 5.2)))
    faded(g, enter, () => Wall.drawHuman(g, Wall.human, t, span(p, 0.45, 1.2) * (1 - span(p, 4.6, 5.0)), p))
    const temariIn = pop(lin(p, 3.55, 3.85))
    if (temariIn > 0.01) drawTemari(g, Wall.temari.center, Wall.temari.radius * temariIn, -lin(p, 4.0, 4.7) * 16)
    Wall.drawWall(g, p)
    Wall.drawStones(g, p)
  },

  drawGround(g, o) {
    faded(g, o, () => {
      const rng = seeded(11)
      const xs = [0]
      while (xs[xs.length - 1] < artW) xs.push(xs[xs.length - 1] + rng(70, 140))
      for (let i = 0; i < xs.length - 1; i++) {
        const a = xs[i], b = xs[i + 1], m = (a + b) / 2, top = Wall.groundY + rng(-3, 3)
        poly(g, [pt(a, Wall.groundY), pt(b, Wall.groundY), pt(m, top + 60)], hex(0x1d3a3e, 0.9))
        poly(g, [pt(a, Wall.groundY), pt(m, top + 60), pt(a, artH)], hex(0x173136, 0.9))
        poly(g, [pt(b, Wall.groundY), pt(m, top + 60), pt(b, artH)], hex(0x142b30, 0.9))
        poly(g, [pt(a, artH), pt(m, top + 60), pt(b, artH)], hex(0x11262b, 0.9))
      }
      stroke(g, [pt(0, Wall.groundY), pt(artW, Wall.groundY)], hex(Palette.olive, 0.35), 1.5)
    })
  },

  drawHuman(g, f, t, puzzled, p) {
    const sh = Person.draw(g, f, t, { scale: 1.2, puzzled })
    for (let i = 0; i < 3; i++) {
      const o = span(p, 0.5 + i * 0.25, 0.7 + i * 0.25) * puzzled
      if (o <= 0.01) continue
      const bob = Math.sin(t * 2 + i * 1.7) * 4
      text(g, '?', pt(f.x + 34 + i * 30, sh - 62 - i * 26 + bob), 34 + i * 7, { weight: 800, opacity: o * 0.85 })
    }
    const lit = span(p, 4.75, 5.05)
    if (lit > 0.01) {
      glow(g, f.x + 42, sh - 66, 40, 40, 0xf6d79a, 0.6 * lit)
      text(g, '!', pt(f.x + 42, sh - 66), 40, { weight: 800, color: hex(0xf6d79a), opacity: lit })
    }
  },

  /** A faceted gem turning slowly. */
  drawAI(g, c, t, calm) {
    ellipse(g, c.x, Wall.groundY - 1, 70, 9, 'rgba(0,0,0,0.25)')
    const [ar, ag, ab] = Ink.ai
    g.save()
    g.translate(c.x, c.y)
    g.scale(120, 120)
    const grad = g.createRadialGradient(0, 0, 0, 0, 0, 1)
    const a0 = 0.35 + 0.1 * Math.sin(t * 2)
    const col = (a) => `rgba(${ar * 255},${ag * 255},${ab * 255},${a})`
    grad.addColorStop(0, col(a0)); grad.addColorStop(1, col(0))
    g.fillStyle = grad
    g.beginPath(); g.arc(0, 0, 1, 0, Math.PI * 2); g.fill()
    g.restore()
    const r = 78
    const verts = [[0, -1.25, 0], [1, 0, 0], [0, 0, 1], [-1, 0, 0], [0, 0, -1], [0, 1.1, 0]]
    const faces = [[0, 1, 2], [0, 2, 3], [0, 3, 4], [0, 4, 1], [5, 2, 1], [5, 3, 2], [5, 4, 3], [5, 1, 4]]
    const a = t * (0.5 - 0.25 * calm), tilt = 0.35
    const view = (v) => {
      const x = v[0] * Math.cos(a) + v[2] * Math.sin(a), z = -v[0] * Math.sin(a) + v[2] * Math.cos(a)
      return [x, v[1] * Math.cos(tilt) - z * Math.sin(tilt), v[1] * Math.sin(tilt) + z * Math.cos(tilt)]
    }
    const vs = verts.map(view)
    const light = [-0.5, -0.6, 0.65]
    const drawn = []
    for (const [i, j, k] of faces) {
      const A = vs[i], B = vs[j], C = vs[k]
      const e1 = [B[0] - A[0], B[1] - A[1], B[2] - A[2]], e2 = [C[0] - A[0], C[1] - A[1], C[2] - A[2]]
      const n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
      const len = Math.hypot(n[0], n[1], n[2])
      n[0] /= len; n[1] /= len; n[2] /= len
      if (n[2] < 0) continue
      const k0 = 0.55 + 0.55 * Math.max(0, n[0] * light[0] + n[1] * light[1] + n[2] * light[2])
      const shade = `rgb(${Math.min(1, ar * k0) * 255},${Math.min(1, ag * k0) * 255},${Math.min(1, ab * k0) * 255})`
      drawn.push([(A[2] + B[2] + C[2]) / 3, [A, B, C].map((q) => pt(c.x + q[0] * r, c.y + q[1] * r)), shade])
    }
    drawn.sort((x, y) => x[0] - y[0]).forEach((d) => poly(g, d[1], d[2]))
    text(g, 'AI', pt(c.x, c.y + 116), 18, { weight: 600, color: Ink.mist })
  },

  /** Code and reports flying out of the AI and piling up beside it. */
  drawSheets(g, p, t, c) {
    const rng = seeded(3)
    for (let i = 0; i < 30; i++) {
      const start = 0.2 + i * 0.04
      const u = lin(p, start, start + 0.32)
      const jx = rng(-14, 14), rot = rng(-0.25, 0.25), kind = rng(0, 1)
      if (u <= 0) continue
      const land = pt(622 + (i % 4) * 27 + jx * 0.4, Wall.groundY - 8 - Math.floor(i / 4) * 9)
      const top = pt((c.x + land.x) / 2 + jx * 3, c.y - 170 - (i % 5) * 14)
      const e = easeInOut(u)
      const s = 1 - e
      const w = [s * s * s, 3 * s * s * e, 3 * s * e * e, e * e * e]
      const a = c, b1 = pt(c.x + jx, top.y), b2 = pt(land.x, top.y), d = land
      const q = pt(w[0] * a.x + w[1] * b1.x + w[2] * b2.x + w[3] * d.x, w[0] * a.y + w[1] * b1.y + w[2] * b2.y + w[3] * d.y)
      g.save()
      g.translate(q.x, q.y)
      g.rotate(rot + (u < 1 ? Math.sin(t * 9 + i) * 0.35 : 0) + (1 - e) * 2.5)
      g.globalAlpha *= Math.min(1, u * 4)
      fillRound(g, rect(-15, -10, 30, 20), 2, hex(kind < 0.5 ? 0xe9edf0 : 0xd8e6ea))
      for (let l = 0; l < 3; l++) fillRect(g, rect(-11, -6 + l * 5, l === 2 ? 12 : 20, 1.6), hex(kind < 0.5 ? 0x7c8b92 : 0x5c8fa0))
      g.restore()
    }
  },

  /** A heap, not a wall: bricks of any length, askew, thrown on top of each other. */
  bricks() {
    if (Wall.heap) return Wall.heap
    const rng = seeded(41), out = []
    const rows = Wall.rows
    for (let r = 0; r < rows; r++) {
      const up = r / (rows - 1)
      const half = 138 * Math.pow(1 - up, 0.55) + 26
      const y = Wall.groundY - (r + 1) * Wall.brickH + rng(-2, 2)
      let x = 500 - half + rng(-14, 14) + (r % 3 - 1) * 7
      while (x < 500 + half - 14) {
        const w = rng(38, 78), tilt = rng(-1, 1) * (0.04 + 0.16 * up + (rng(0, 1) < 0.2 ? 0.1 : 0))
        out.push({ rect: rect(x, y + rng(-5, 5), w, Wall.brickH + rng(-3, 2)), tilt, shade: rng(-0.08, 0.08), lean: rng(-14, 14), row: r, jitter: rng(0, 1) })
        x += w + rng(-6, 3)
      }
    }
    // what rolled off the heap
    for (const [x, w, tilt] of [[338, 46, -0.26], [386, 36, 0.32], [630, 52, 0.2], [604, 30, -0.34]]) {
      out.push({ rect: rect(x, Wall.groundY - 25, w, 25), tilt, shade: rng(-0.08, 0.08), lean: 8, row: 0, jitter: rng(0, 1) })
    }
    out.sort((a, b) => a.row + a.jitter * 0.8 - (b.row + b.jitter * 0.8))
    out.forEach((b, k) => { b.order = k })
    Wall.heap = out
    return out
  },

  drawWall(g, p) {
    const all = Wall.bricks()
    for (const b of all) {
      const start = 1.05 + (b.order / all.length) * 2.05
      const u = lin(p, start, start + 0.14)
      if (u <= 0) continue
      const drop = (1 - easeOut(u)) * -90
      const r = b.rect
      faded(g, Math.min(1, u * 3), () => {
        g.save()
        g.translate(midX(r), midY(r) + drop)
        g.rotate(b.tilt + (1 - u) * b.tilt * 3)
        const hw = r.w / 2 - 0.5, hh = r.h / 2 - 0.5
        const base = 0.36 + b.shade
        const c = (k1, k2, k3) => `rgb(${Math.min(1, base * k1) * 255},${Math.min(1, base * k2) * 255},${Math.min(1, base * k3) * 255})`
        const top = c(1.06, 1.0, 0.96), left = c(0.92, 0.86, 0.84), right = c(0.74, 0.7, 0.72), bottom = c(0.6, 0.58, 0.62)
        const m = pt(b.lean * 0.5, -hh * 0.2)
        poly(g, [pt(-hw, -hh), pt(hw, -hh), m], top)
        poly(g, [pt(hw, -hh), pt(hw, hh), m], right)
        poly(g, [pt(hw, hh), pt(-hw, hh), m], bottom)
        poly(g, [pt(-hw, hh), pt(-hw, -hh), m], left)
        g.restore()
      })
    }
  },

  drawStones(g, p) {
    for (const [label, y, at] of [['Not reproducible', 632, 1.45], ['Process unseen', 534, 2.05], ['Data cannot go', 436, 2.65]]) {
      const u = pop(lin(p, at - 0.12, at + 0.12))
      if (u <= 0.01) continue
      const w = textWidth(g, label, 19, { weight: 700 }) + 36, h = 46  // the size of its words, so it lies on the heap wherever the heap has narrowed
      g.save()
      g.translate(500, y)
      g.scale(u, u)
      g.rotate(y === 534 ? 0.018 : -0.016)
      const r = rect(-w / 2, -h / 2, w, h)
      fillRound(g, rect(r.x, r.y + 4, w, h), 6, 'rgba(0,0,0,0.3)')
      poly(g, [pt(r.x, r.y), pt(maxX(r), r.y), pt(maxX(r) - 20, 0), pt(r.x + 30, 4)], hex(0x5d4b40))
      poly(g, [pt(r.x, r.y), pt(r.x + 30, 4), pt(r.x, maxY(r))], hex(0x4a3b32))
      poly(g, [pt(maxX(r), r.y), pt(maxX(r), maxY(r)), pt(maxX(r) - 20, 0)], hex(0x433630))
      poly(g, [pt(r.x, maxY(r)), pt(r.x + 30, 4), pt(maxX(r) - 20, 0), pt(maxX(r), maxY(r))], hex(0x3b2f29))
      text(g, label, pt(0, 0), 20, { weight: 700 })
      g.restore()
    }
  },

  /** What travels on the thread, both ways. */
  drawBeads(g, p, t, line) {
    const o = span(p, 4.7, 5.05)
    if (o <= 0.01) return
    const beads = [['What I want done', true], ['What was run', false], ['What to change next', true]]
    beads.forEach(([label, toAI], i) => {
      let f = (t * 0.075 + i / 3) % 1
      if (!toAI) f = 1 - f
      const q = line.at(f)
      const w = textWidth(g, label, 14.5, { weight: 600 }) + 54, h = 30
      faded(g, o * smooth(Math.min(f, 1 - f) / 0.06), () => {
        const r = rect(q.x - w / 2, q.y - h - 8, w, h)
        fillRound(g, rect(r.x, r.y + 3, w, h), 15, 'rgba(0,0,0,0.25)')
        fillRound(g, r, 15, hex(toAI ? 0xf6ead2 : 0xd7ecef))
        ellipse(g, q.x, q.y, 4, 4, hex(Palette.thread))
        text(g, toAI ? `${label} →` : `← ${label}`, pt(midX(r), midY(r)), 14.5, { weight: 600, color: hex(0x24363b) })
      })
    })
  },
}

// ---- What if what you wrote took shape just as it is? ----

export const CasA = {
  boxes: [
    { hand: 'download\nthe data', name: 'load', summary: 'Pin the observation with Nix', input: 'start', output: 'events', stack: 'Nix · FITS' },
    { hand: 'split it\nby energy', name: 'bands', summary: 'Split into energy bands', input: 'input', output: 'images', stack: 'Python · numpy · astropy' },
    { hand: 'plot it', name: 'rgb', summary: 'Make an RGB image', input: 'input', output: 'rgb', stack: 'Python · pillow' },
  ],
  paper: rect(70, 150, 860, 590),
  canvas: rect(24, 110, 952, 690),
  sketchRect: (k) => rect(137 + k * 265, 400, 196, 140),
  cardRect: (k) => rect(195 + k * 240, 312, 210, 92),
  startRect: rect(52, 339, 104, 38),
  runButton: rect(396, 130, 92, 28),
  get startPort() { return pt(maxX(this.startRect), midY(this.startRect)) },
  inPort: (r) => pt(r.x, r.y + 64),
  outPort: (r) => pt(maxX(r), r.y + 64),
  drawn: (k) => [0.8 + k * 0.18, 0.98 + k * 0.18],
  morph: [1.85, 2.4],
  // the bird takes its time over each Card, and the run comes after it has gone
  implement: (k) => [2.8 + k * 0.42, 3.1 + k * 0.42],
  press: 4.0,
  run: (k) => [4.06 + k * 0.2, 4.26 + k * 0.2],
  // What the Agent writes as it crosses each Card: lines of the demo's own files.
  code: [
    { file: 'load/default.nix', lines: ['raw = pkgs.fetchurl {', '  inherit url;', '  curlOptsList = [ "--ssl-reqd" ];', '  hash = params.hash;', '};'] },
    { file: 'bands/src/main.py', lines: ['import numpy as np', 'from astropy.io import fits', '', 'def main(ctx):', '    events = ctx.input("input", Events)'] },
    { file: 'rgb/src/main.py', lines: ['from PIL import Image', '', 'def main(ctx):', '    p = ctx.params()', '    percentile, q = p.Percentile(), p.Q()'] },
  ],
  state(k, p) { const r = CasA.run(k); return p < r[0] ? 'idle' : p < r[1] ? 'running' : 'done' },
  slant: 0.4,
  wipeX(u, y) { return CasA.paper.x - 300 + (CasA.paper.w + 700) * u - (y - CasA.paper.y) * CasA.slant },
  clipSide(g, u, dark) {
    const top = -100, bottom = artH + 100, far = dark ? -3000 : 4000
    trace(g, [pt(far, top), pt(CasA.wipeX(u, top), top), pt(CasA.wipeX(u, bottom), bottom), pt(far, bottom)], true)
    g.clip()
  },
  boxU(k, u) {
    const r = CasA.sketchRect(k), c = pt(midX(r), midY(r))
    const at = (c.x + (c.y - CasA.paper.y) * CasA.slant - CasA.paper.x + 300) / (CasA.paper.w + 700)
    return span(u, at - 0.03, at + 0.17)
  },

  draw(g, p, t) {
    const enter = span(p, -0.42, 0.1)
    const u = span(p, CasA.morph[0], CasA.morph[1])
    faded(g, enter, () => {
      CasA.drawSheet(g, u, (1 - enter) * 40)
      CasA.drawToolbar(g, p, u)
      g.save()
      CasA.clipSide(g, u, false)
      CasA.drawTitle(g, p)
      CasA.drawDoodle(g, t)
      g.restore()
      const us = [0, 1, 2].map((k) => CasA.boxU(k, u))
      const su = span(us[0], 0.5, 1)
      if (su > 0.01) faded(g, su, () => CasA.drawStart(g, p))
      for (let k = 0; k < 3; k++) CasA.drawArrow(g, k, p, k === 0 ? us[0] : (us[k - 1] + us[k]) / 2, t)
      for (let k = 0; k < 3; k++) CasA.drawBox(g, k, p, us[k], t)
      CasA.drawResults(g, p)
      CasA.drawHandLabels(g, p)
    })
  },

  drawSheet(g, u, rise) {
    const base = mixR(CasA.paper, CasA.canvas, smooth(u))
    const r = rect(base.x, base.y + rise, base.w, base.h)
    const corner = mix(6, 14, u)
    fillRound(g, rect(r.x - 4, r.y + 6, r.w + 8, r.h + 10), corner + 4, `rgba(0,0,0,${0.22 - 0.1 * u})`)
    g.save()
    CasA.clipSide(g, u, false)
    fillRound(g, r, corner, css(Ink.paper))
    for (let y = r.y + 46; y < maxY(r) - 10; y += 36) stroke(g, [pt(r.x + 14, y), pt(maxX(r) - 14, y)], hex(0x7aa0c0, 0.16), 1, { cap: 'butt' })
    stroke(g, [pt(r.x + 46, r.y + 8), pt(r.x + 46, maxY(r) - 8)], hex(0xd77b6a, 0.25), 1, { cap: 'butt' })
    g.restore()
    if (u <= 0.001) return
    g.save()
    CasA.clipSide(g, u, true)
    fillRound(g, r, corner, css(Ink.canvas))
    g.fillStyle = 'rgba(255,255,255,0.13)'
    g.beginPath()
    for (let y = r.y + 12; y < maxY(r) - 6; y += 22) {
      for (let x = r.x + 12; x < maxX(r) - 6; x += 22) { g.moveTo(x + 0.9, y); g.arc(x, y, 0.9, 0, Math.PI * 2) }
    }
    g.fill()
    g.restore()
    if (u < 0.999) {
      g.save()
      roundRect(g, r, corner)
      g.clip()
      const edge = [pt(CasA.wipeX(u, r.y), r.y), pt(CasA.wipeX(u, maxY(r)), maxY(r))]
      stroke(g, edge, hex(0xf6d79a, 0.22), 26, { cap: 'butt' })
      stroke(g, edge, hex(0xf6d79a, 0.4), 12, { cap: 'butt' })
      stroke(g, edge, hex(0xfff3d6, 0.9), 2, { cap: 'butt' })
      g.restore()
    }
  },

  drawToolbar(g, p, u) {
    const o = span(u, 0.7, 1)
    faded(g, o, () => {
      const C = CasA.canvas, B = CasA.runButton
      const bar = rect(C.x + 16, C.y + 16, 250, 36)
      fillRound(g, bar, 18, 'rgba(255,255,255,0.07)')
      text(g, 'Cas A · Chandra RGB', pt(bar.x + 18, midY(bar)), 14, { weight: 600, color: hex(0xe8e8ea), align: 'left' })
      fillRound(g, rect(B.x - 8, C.y + 16, 330, 36), 18, 'rgba(255,255,255,0.07)')
      const pressed = 1 - smooth(Math.abs(p - CasA.press) / 0.05)
      g.save()
      g.translate(midX(B), midY(B))
      g.scale(1 - 0.06 * pressed, 1 - 0.06 * pressed)
      fillRound(g, rect(-B.w / 2, -B.h / 2, B.w, B.h), 7, hex(pressed > 0.1 ? 0x5a9cff : 0x2f7cf6))
      text(g, '▶  Run', pt(0, 0), 14, { weight: 600, color: '#fff' })
      g.restore()
      const sx = maxX(B) + 14, sy = midY(B)
      if (p >= CasA.run(0)[0]) {
        const doneAll = CasA.state(2, p) === 'done'
        const elapsed = doneAll ? 2.41 : ((p - CasA.run(0)[0]) / (CasA.run(2)[1] - CasA.run(0)[0])) * 2.41
        text(g, doneAll ? '✓ Succeeded' : '◌ Running', pt(sx, sy), 13, { weight: 600, color: hex(doneAll ? Ink.done : Ink.badge), align: 'left' })
        text(g, `00:00:${elapsed.toFixed(2).padStart(5, '0')}`, pt(sx + 98, sy), 13, { color: hex(0xc8c8cc), align: 'left', mono: true })
      }
    })
  },

  drawTitle(g, p) {
    const w = lin(p, 0.15, 0.75)
    if (w <= 0) return
    g.save()
    g.beginPath()
    g.rect(0, 0, 140 + 520 * w, artH)
    g.clip()
    childText(g, 'Cas A in 3 colours!', pt(140, 222), 38, Ink.penC, { seed: 3, leading: true })
    const under = new Polyline(Array.from({ length: 31 }, (_, i) => { const s = i / 30; return pt(136 + s * 450, 258 + Math.sin(s * 9) * 3.5 + s * 6) }))
    stroke(g, under.upTo(lin(p, 0.55, 0.9)), hex(0xd9844a, 0.85), 5)
    g.restore()
  },

  /** Cas A in crayon: it draws itself once it crosses the reading line. */
  drawDoodle(g, t) {
    const since = scene.since
    if (since <= 0) return
    const d = clamp01(since / 2.2)
    const c = pt(790, 226), R = 50
    const rng = seeded(9)
    const arc = (a0, len, rr, seed) => new Polyline(Array.from({ length: 11 }, (_, k) => {
      const a = a0 + (len * k) / 10
      const r = rr * (1 + 0.06 * Math.sin(a * 7 + seed)) + 1.5 * Math.sin(k * 1.7 + seed)
      return pt(c.x + Math.cos(a) * r, c.y + Math.sin(a) * r * 0.94)
    }))
    const shade = clamp01(d / 0.3)
    for (let i = 0; i < 26; i++) {
      const y = c.y - R * 1.05 + (i * R * 2.1) / 26
      const half = Math.sqrt(Math.max(0, Math.pow(R * 1.08, 2) - Math.pow(y - c.y, 2)))
      const j1 = rng(-3, 3), j2 = rng(-3, 3)
      if (half <= 2 || i / 26 >= shade) continue
      stroke(g, [pt(c.x - half + j1, y + 3), pt(c.x + half + j2, y - 3)], hex(0x262038, 0.82), 5)
    }
    const n = 40
    for (let i = 0; i < n; i++) {
      const a0 = rng(0, 2 * Math.PI), len = rng(0.35, 1), rr = R * rng(0.5, 0.95), pick = rng(0, 1)
      const col = pick < 0.42 ? 0xe2533f : pick < 0.78 ? 0x5ec25a : 0xc25ad0
      const w = rng(2.6, 5)
      const u = clamp01((d - (0.25 + (i / n) * 0.5)) / 0.22)
      if (u <= 0) continue
      const bright = 0.6 + 0.4 * Math.max(0, Math.cos(a0 - 3.4))
      stroke(g, arc(a0, len, rr, i).upTo(u), hex(col, 0.9 * bright), w)
    }
    for (let i = 0; i < 7; i++) {
      const u = clamp01((d - 0.72 - i * 0.02) / 0.15)
      if (u > 0) stroke(g, arc(i * 0.95, 1.1, R * 1.04, i + 40).upTo(u), hex(0x5b8bf0, 0.85), 1.8)
    }
    const j = clamp01((d - 0.7) / 0.2)
    if (j > 0) {
      const line = new Polyline([pt(c.x + Math.cos(-0.95) * R * 0.95, c.y + Math.sin(-0.95) * R * 0.95), pt(c.x + Math.cos(-0.9) * R * 1.5, c.y + Math.sin(-0.9) * R * 1.5)])
      stroke(g, line.upTo(j), hex(0x5ec25a, 0.9), 2.4)
      ellipse(g, c.x, c.y, 2.5, 2.5, `rgba(255,255,255,${0.9 * j})`)
    }
    const shine = clamp01((since - 2) / 0.6)
    if (shine > 0) {
      for (let k = 0; k < 5; k++) {
        const a = k * 1.3 + 0.4, r = R * (0.7 + 0.06 * (k % 3))
        const q = pt(c.x + Math.cos(a) * r, c.y + Math.sin(a) * r)
        const o = shine * Math.max(0, Math.sin(t * 2.2 + k * 1.9))
        const s = 3 + 3 * o
        stroke(g, [pt(q.x - s, q.y), pt(q.x + s, q.y)], hex(0xf2c14e, o), 1.8)
        stroke(g, [pt(q.x, q.y - s), pt(q.x, q.y + s)], hex(0xf2c14e, o), 1.8)
      }
    }
    childText(g, 'Cas A', pt(c.x + 62, c.y + 46), 20, Ink.penC, { opacity: clamp01((d - 0.8) / 0.2), seed: 21, leading: true })
  },

  wire(a, b) {
    const k = Math.max(30, Math.abs(b.x - a.x) * 0.5)
    return curve(a, pt(a.x + k, a.y), pt(b.x - k, b.y), b, 28)
  },

  drawArrow(g, k, p, u, t) {
    if (k === 0) {
      const o = span(u, 0.6, 1)
      faded(g, o, () => CasA.drawWire(g, new Polyline(CasA.wire(CasA.startPort, CasA.inPort(CasA.cardRect(0)))), p, t, 0, 1, 1))
      return
    }
    const from = CasA.sketchRect(k - 1), to = CasA.sketchRect(k)
    const a = pt(maxX(from) + 10, midY(from)), b = pt(to.x - 12, midY(to))
    const d1 = CasA.drawn(k - 1)[1]
    const drawnF = lin(p, d1 - 0.05, d1 + 0.12)
    if (drawnF <= 0) return
    const n = 28
    const sketch = Array.from({ length: n + 1 }, (_, i) => {
      const s = i / n
      // a pen stroke: almost straight, a slight bow and a small tremor, no hump
      return pt(mix(a.x, b.x, s), a.y + (Math.sin(s * 9 + k * 2) * 1.2 - 3) * Math.sin(s * Math.PI) * (1 - u))
    })
    const target = CasA.wire(CasA.outPort(CasA.cardRect(k - 1)), CasA.inPort(CasA.cardRect(k)))
    CasA.drawWire(g, new Polyline(sketch.map((q, i) => mixP(q, target[i], u))), p, t, k, u, drawnF)
    const head = (1 - span(u, 0, 0.35)) * span(drawnF, 0.8, 1)
    if (head > 0.01) {
      // the head follows the line's last direction, its two barbs a touch uneven like a hand's
      const tail = sketch[n - 3], dir = Math.atan2(b.y - tail.y, b.x - tail.x)
      const barb = (da, len) => pt(b.x - Math.cos(dir + da) * len, b.y - Math.sin(dir + da) * len)
      faded(g, head, () => stroke(g, [barb(-0.55, 13), b, barb(0.5, 12)], Ink.penC, 3))
    }
  },

  drawWire(g, line, p, t, into, ink, drawn) {
    const color = mixColor(Ink.pen, rgb(0xe0893a), ink)
    const width = mix(3.2, 2, ink)
    if (ink > 0.99 && CasA.state(into, p) === 'running') {
      stroke(g, line.points, hex(Ink.wire, 0.35), width, { cap: 'butt' })
      stroke(g, line.points, hex(0xffb36b), width + 0.6, { dash: [8, 6], dashPhase: t * 40 })
    } else {
      stroke(g, line.upTo(drawn), color, width)
    }
  },

  check(g, q, size, color) {
    ellipse(g, q.x, q.y, size, size, color)
    stroke(g, [pt(q.x - size * 0.5, q.y), pt(q.x - size * 0.12, q.y + size * 0.42), pt(q.x + size * 0.52, q.y - size * 0.42)], '#1d1e21', Math.max(1.2, size * 0.28))
  },

  drawStart(g, p) {
    const r = CasA.startRect
    const done = p >= CasA.run(0)[0]
    fillRound(g, r, r.h / 2, css(Ink.cardFill))
    strokeRound(g, r, r.h / 2, done ? hex(Ink.done) : css(Ink.cardStroke), 1.4)
    const c = pt(r.x + 19, midY(r))
    ellipse(g, c.x, c.y, 9, 9, hex(0x2f7cf6))
    trace(g, [pt(c.x - 3, c.y - 4.5), pt(c.x + 5, c.y), pt(c.x - 3, c.y + 4.5)], true)
    g.fillStyle = '#fff'
    g.fill()
    text(g, 'start', pt(r.x + 36, midY(r)), 13, { weight: 600, color: hex(0xe8e8ea), align: 'left' })
    if (done) CasA.check(g, pt(maxX(r) - 14, midY(r)), 6, hex(Ink.done))
    ellipse(g, maxX(r), midY(r), 4, 4, hex(0x3b82f6))
  },

  sketchOutline(r, radius, wobble, seed, n = 120) {
    const rr = Math.min(radius, Math.min(r.w, r.h) / 2)
    const sw = r.w - 2 * rr, sh = r.h - 2 * rr, arc = (Math.PI / 2) * rr
    const per = 2 * sw + 2 * sh + 4 * arc
    const at = (d0) => {
      let d = d0 % per
      const safe = Math.max(rr, 0.001)
      const segs = [
        [sw, (s) => [pt(r.x + rr + s, r.y), pt(0, -1)]],
        [arc, (s) => { const a = -Math.PI / 2 + s / safe; return [pt(maxX(r) - rr + rr * Math.cos(a), r.y + rr + rr * Math.sin(a)), pt(Math.cos(a), Math.sin(a))] }],
        [sh, (s) => [pt(maxX(r), r.y + rr + s), pt(1, 0)]],
        [arc, (s) => { const a = s / safe; return [pt(maxX(r) - rr + rr * Math.cos(a), maxY(r) - rr + rr * Math.sin(a)), pt(Math.cos(a), Math.sin(a))] }],
        [sw, (s) => [pt(maxX(r) - rr - s, maxY(r)), pt(0, 1)]],
        [arc, (s) => { const a = Math.PI / 2 + s / safe; return [pt(r.x + rr + rr * Math.cos(a), maxY(r) - rr + rr * Math.sin(a)), pt(Math.cos(a), Math.sin(a))] }],
        [sh, (s) => [pt(r.x, maxY(r) - rr - s), pt(-1, 0)]],
        [arc, (s) => { const a = Math.PI + s / safe; return [pt(r.x + rr + rr * Math.cos(a), r.y + rr + rr * Math.sin(a)), pt(Math.cos(a), Math.sin(a))] }],
      ]
      for (const [len, f] of segs) {
        if (d <= len) return f(d)
        d -= len
      }
      return segs[0][1](0)
    }
    const overshoot = wobble > 0.01 ? 0.04 : 0
    return Array.from({ length: n + 1 }, (_, i) => {
      const f = (i / n) * (1 + overshoot)
      const [q, nrm] = at(f * per)
      const w = wobble * (Math.sin(f * 13 + seed) * 0.6 + Math.sin(f * 29 + seed * 1.7) * 0.4) + wobble * 1.4 * (f > 1 ? (f - 1) * 25 : 0)
      return pt(q.x + nrm.x * w, q.y + nrm.y * w)
    })
  },

  drawBox(g, k, p, u, t) {
    const box = CasA.boxes[k]
    const [d0, d1] = CasA.drawn(k)
    const df = lin(p, d0, d1)
    if (df <= 0) return
    const r = mixR(CasA.sketchRect(k), CasA.cardRect(k), u)
    const [i0, i1] = CasA.implement(k)
    // How much of the Card is real by now, left to right: the seam is under the bird.
    const made = lin(p, i0, i1)
    const st = CasA.state(k, p)
    const wobble = 7 * (1 - smooth(u * 1.25))
    const outline = new Polyline(CasA.sketchOutline(r, mix(18, 10, u), wobble, k * 2.3))
    const shape = outline.upTo(u > 0.01 ? 1 : df)
    if (u > 0.01) faded(g, u, () => fillRound(g, r, 10, css(Ink.cardFill)))
    const draftShown = span(u, 0.5, 0.85)
    let color = mixColor(Ink.pen, Ink.cardStroke, u), width = mix(3.2, 1.4, u)
    if (st === 'running') {
      const pulse = 0.5 + 0.5 * Math.sin(t * 7)
      color = `rgb(230,${(0.55 + 0.1 * pulse) * 255},64)`
      width = 2
    } else if (st === 'done') {
      color = hex(Ink.done)
      width = 1.8
    }
    const solid = () => stroke(g, shape, color, width)
    const dashed = () => strokeRound(g, r, 10, hex(0x9aa0a8), 1.4, [6, 4])
    const seam = r.x + r.w * made
    const between = (x0, x1, draw) => {
      g.save()
      g.beginPath()
      g.rect(x0, r.y - 14, x1 - x0, r.h + 28)
      g.clip()
      draw()
      g.restore()
    }
    if (made <= 0) {
      faded(g, 1 - draftShown, solid)
      if (draftShown > 0.01) faded(g, draftShown, dashed)
    }
    if (st === 'running') strokeRound(g, r, 10, hex(Ink.wire, 0.25 + 0.15 * Math.sin(t * 7)), 6)

    const hand = span(p, d1 - 0.08, d1 + 0.12) * (1 - span(u, 0, 0.45))
    if (hand > 0.01) {
      childText(g, box.hand, pt(midX(r), k === 0 ? r.y + 50 : midY(r)), 27, Ink.penC, { opacity: hand, seed: 11 + k })
      if (k === 0) {
        const a = lin(p, d1, d1 + 0.18)
        const down = new Polyline(curve(pt(midX(r) + 2, r.y + 86), pt(midX(r) + 4, r.y + 98), pt(midX(r) + 3, r.y + 110), pt(midX(r) - 1, r.y + 122), 12))
        faded(g, hand, () => {
          stroke(g, down.upTo(a), hex(0xd9844a), 4)
          if (a > 0.8) stroke(g, [pt(midX(r) - 13, r.y + 108), pt(midX(r) - 1, r.y + 123), pt(midX(r) + 11, r.y + 106)], hex(0xd9844a), 4)
        })
      }
    }

    const face = span(u, 0.55, 1)
    const faceOf = (real) => faded(g, face, () => {
      const title = hex(0xe8e8ea), sub = hex(0x9a9ca2)
      text(g, real ? '</>' : '✎', pt(r.x + 18, r.y + 17), 10, { weight: 600, color: sub, mono: true })
      text(g, box.name, pt(r.x + 34, r.y + 17), 14, { weight: 600, color: title, align: 'left' })
      if (!real) {
        const br = rect(r.x + 34 + box.name.length * 8.4 + 8, r.y + 9, 40, 16)
        fillRound(g, br, 8, hex(Ink.badge, 0.22))
        text(g, 'Draft', pt(midX(br), midY(br)), 9.5, { weight: 600, color: hex(0xf3c088) })
      }
      const sr = pt(maxX(r) - 16, r.y + 17)
      if (st === 'running') {
        g.beginPath()
        g.arc(sr.x, sr.y, 6, t * 6, t * 6 + 4.2)
        g.strokeStyle = hex(Ink.badge)
        g.lineWidth = 2
        g.lineCap = 'round'
        g.stroke()
      } else if (st === 'done') {
        CasA.check(g, sr, 6.5 * pop(lin(p, CasA.run(k)[1], CasA.run(k)[1] + 0.08)), hex(Ink.done))
      }
      text(g, box.summary, pt(r.x + 14, r.y + 41), 11.5, { color: sub, align: 'left' })
      text(g, box.input, pt(r.x + 12, r.y + 64), 10, { color: sub, align: 'left' })
      text(g, box.output, pt(maxX(r) - 12, r.y + 64), 10, { color: sub, align: 'right' })
      const ip = CasA.inPort(r), op = CasA.outPort(r)
      fillRect(g, rect(ip.x - 3.5, ip.y - 3.5, 7, 7), hex(0xd8d8dc))
      ellipse(g, op.x, op.y, 4, 4, hex(0x3b82f6))
      if (real) text(g, st === 'done' ? 'Run #1 · succeeded' : box.stack, pt(r.x + 14, r.y + 81), 9.5, { color: st === 'done' ? hex(Ink.done, 0.9) : sub, align: 'left' })
    })
    if (made <= 0) faceOf(false)
    else if (made >= 1) {
      solid()
      faceOf(true)
      const settle = 1 - span(p, i1, i1 + 0.12)
      if (settle > 0.01 && st === 'idle') strokeRound(g, r, 10, hex(0xf6d79a, 0.75 * settle), 3)
    } else {
      // the seam: real behind the bird, still the Draft ahead of it
      between(r.x - 14, seam, () => { solid(); faceOf(true) })
      between(seam, maxX(r) + 14, () => { dashed(); faceOf(false) })
      stroke(g, [pt(seam, r.y - 3), pt(seam, maxY(r) + 3)], hex(0xf6d79a, 0.28), 9, { cap: 'butt' })
      stroke(g, [pt(seam, r.y - 3), pt(seam, maxY(r) + 3)], hex(0xfff3d6, 0.95), 1.6, { cap: 'butt' })
    }
    CasA.drawCode(g, k, p)
  },

  /** The file being written above the Card: it types itself as the bird crosses. */
  drawCode(g, k, p) {
    const [i0, i1] = CasA.implement(k)
    const o = span(p, i0 - 0.05, i0 + 0.02) * (1 - span(p, i1 + 0.04, i1 + 0.11))
    if (o <= 0.01) return
    const c = CasA.code[k], card = CasA.cardRect(k)
    const r = rect(midX(card) - 140, card.y - 122 - (1 - o) * 8, 280, 108)
    const total = c.lines.reduce((a, l) => a + l.length, 0)
    let left = Math.floor(total * lin(p, i0 + 0.01, i1 - 0.01))
    faded(g, o, () => {
      fillRound(g, rect(r.x - 2, r.y + 4, r.w + 4, r.h + 2), 9, 'rgba(0,0,0,0.3)')
      fillRound(g, r, 8, hex(0x17181a))
      strokeRound(g, r, 8, hex(0x4d5157), 1)
      text(g, c.file, pt(r.x + 12, r.y + 14), 9.5, { color: hex(0x9a9ca2), align: 'left', mono: true })
      c.lines.forEach((line, n) => {
        const shown = line.slice(0, Math.max(0, left))
        left -= line.length
        if (shown) text(g, shown, pt(r.x + 12, r.y + 32 + n * 15), 10.5, { color: hex(0xd6dde0), align: 'left', mono: true })
      })
    })
    // the line where the Card meets what is written for it
    faded(g, o, () => stroke(g, [pt(midX(card), r.y + r.h), pt(midX(card), card.y)], hex(0xf6d79a, 0.5), 1.2, { dash: [3, 3] }))
  },

  panel(g, r, title, sub) {
    fillRound(g, rect(r.x - 2, r.y + 4, r.w + 4, r.h + 3), 9, 'rgba(0,0,0,0.3)')
    fillRound(g, r, 8, hex(0x232427))
    strokeRound(g, r, 8, hex(0xa45ad8, 0.75), 1.2)
    text(g, title, pt(r.x + 12, r.y + 15), 10.5, { weight: 600, color: hex(0xe8e8ea), align: 'left' })
    text(g, sub, pt(r.x + 18 + title.length * 6.5, r.y + 15), 9, { color: hex(0x9a9ca2), align: 'left' })
  },
  connector(g, a, b) {
    stroke(g, [a, b], hex(0xa45ad8, 0.75), 1.2, { cap: 'butt', dash: [5, 4] })
    ellipse(g, a.x, a.y, 3.5, 3.5, hex(0xc06cf0))
  },

  drawResults(g, p) {
    const end1 = CasA.run(1)[1], end2 = CasA.run(2)[1]
    const tb = span(p, end1, end1 + 0.18)
    faded(g, tb, () => {
      const r = rect(435, 452 - (1 - tb) * 12, 210, 118)
      const c1 = CasA.cardRect(1)
      CasA.connector(g, pt(midX(c1), maxY(c1)), pt(midX(r), r.y))
      CasA.panel(g, r, 'bands', 'Run #1')
      const cols = [r.x + 12, r.x + 76, maxX(r) - 12], head = hex(0xc8c8cc), cell = hex(0xe0e0e4)
      text(g, 'Band', pt(cols[0], r.y + 36), 9.5, { weight: 600, color: head, align: 'left' })
      text(g, 'Energy (eV)', pt(cols[1], r.y + 36), 9.5, { weight: 600, color: head, align: 'left' })
      text(g, 'Photons', pt(cols[2], r.y + 36), 9.5, { weight: 600, color: head, align: 'right' })
      ;[['soft', '600–1650', '101,187', 0xe0574f], ['medium', '1650–2250', '157,471', 0x5fb35a], ['hard', '2250–7500', '110,852', 0x4a7fd6]].forEach((row, i) => {
        const y = r.y + 56 + i * 18
        const o = span(p, end1 + 0.05 + i * 0.04, end1 + 0.12 + i * 0.04)
        ellipse(g, cols[0] + 3, y, 3, 3, hex(row[3], o))
        text(g, row[0], pt(cols[0] + 10, y), 10, { color: cell, align: 'left', opacity: o, mono: true })
        text(g, row[1], pt(cols[1], y), 10, { color: cell, align: 'left', opacity: o, mono: true })
        text(g, row[2], pt(cols[2], y), 10, { color: cell, align: 'right', opacity: o, mono: true })
      })
    })
    const ti = span(p, end2, end2 + 0.15)
    faded(g, ti, () => {
      const r = rect(675, 452 - (1 - ti) * 12, 210, 244)
      const c2 = CasA.cardRect(2)
      CasA.connector(g, pt(midX(c2), maxY(c2)), pt(midX(r), r.y))
      CasA.panel(g, r, 'rgb', 'Run #1')
      const img = rect(r.x + 6, r.y + 34, r.w - 12, r.w - 12)
      fillRect(g, img, '#000')
      const develop = span(p, end2 + 0.05, end2 + 0.45)
      if (casaImage.complete && casaImage.naturalWidth) {
        faded(g, develop, () => {
          const rr = img.w * 0.75 * easeOut(develop)
          g.beginPath()
          g.rect(img.x, img.y, img.w, img.h)
          g.clip()
          g.beginPath()
          g.arc(midX(img), midY(img), Math.max(0.1, rr), 0, Math.PI * 2)
          g.clip()
          g.drawImage(casaImage, img.x, img.y, img.w, img.h)
        })
      }
    })
  },

  drawHandLabels(g, p) {
    const o = span(p, 5.1, 5.45)
    if (o <= 0.01) return
    const words = ['download the data', 'split it by energy', 'plot it']
    for (let k = 0; k < 3; k++) {
      const r = CasA.cardRect(k)
      const ok = o * span(p, 5.1 + k * 0.08, 5.3 + k * 0.08)
      text(g, words[k], pt(midX(r), r.y - 30), 20, { color: Ink.cream, opacity: ok, family: '"Yusei Magic Subset", "Hiragino Maru Gothic ProN", cursive' })
      stroke(g, [pt(midX(r), r.y - 16), pt(midX(r), r.y - 4)], hex(Palette.cream, 0.6 * ok), 2, { dash: [3, 3] })
    }
  },
}
/** The rgb Card's own output from a run of the demo (ObsID 25628, the counts in the table). */
export const casaImage = new Image()
casaImage.src = new URL('../assets/casa-rgb-25628.png', import.meta.url).href

// ---- The author: Yasuda Auditorium under stars joined into a network ----

export const Author = {
  groundY: 640,
  brick: [0x9b5b3e, 0x8c5035, 0xa86448, 0x7d4630, 0xb26c4a],
  shade(h, k) { const c = rgb(h); return `rgb(${Math.min(1, c[0] * k) * 255},${Math.min(1, c[1] * k) * 255},${Math.min(1, c[2] * k) * 255})` },
  masses: [rect(255, 500, 150, 140), rect(595, 500, 150, 140), rect(405, 452, 190, 188), rect(438, 290, 124, 162), rect(430, 274, 140, 16), rect(430, 258, 16, 16), rect(554, 258, 16, 16)],

  draw(g, p, t) {
    Author.drawSky(g, p, t)
    g.save()
    g.translate(0, Author.groundY - Wall.groundY)
    Wall.drawGround(g, span(p, 0, 0.15))
    g.restore()
    Author.drawTree(g, 150, p, t, 3)
    Author.drawTree(g, 850, p, t, 8)
    Author.drawAuditorium(g, p, t)
    const s = span(p, 0.62, 0.78)
    faded(g, s, () => {
      const feet = pt(318, Author.groundY)
      g.save()
      g.translate(feet.x, feet.y); g.scale(0.6, 0.6); g.translate(-feet.x, -feet.y)
      Wall.drawHuman(g, feet, t, 0, 0)
      g.restore()
    })
  },

  drawSky(g, p, t) {
    const o = span(p, 0, 0.3)
    if (o <= 0.01) return
    const rng = seeded(31)
    for (let i = 0; i < 90; i++) {
      const x = rng(30, 970), y = rng(100, 430), r = rng(0.7, 2.2), speed = rng(0.8, 2.2)
      ellipse(g, x, y, r, r, hex(Palette.cream, o * (0.55 + 0.45 * Math.sin(t * speed + i)) * 0.8))
    }
    const gal = span(p, 0.2, 0.5)
    if (gal > 0.01) {
      g.save()
      g.translate(815, 175)
      g.rotate(-0.45)
      g.scale(1, 0.42)
      glow(g, 0, 0, 40, 40, 0xd7c6f0, 0.35 * gal)
      const r2 = seeded(5)
      for (let k = 0; k < 160; k++) {
        const s = r2(0.05, 1), a = (k % 2) * Math.PI + s * 4.2 + r2(-0.25, 0.25), rad = 8 + s * 62
        ellipse(g, Math.cos(a) * rad, Math.sin(a) * rad, 1.3, 1.3, hex(k % 3 === 0 ? 0xb9c8ff : 0xf1e3ff, gal * (1 - s * 0.6)))
      }
      g.restore()
    }
    const nodes = [pt(120, 170), pt(230, 120), pt(300, 210), pt(410, 150), pt(520, 230), pt(600, 135), pt(690, 250), pt(760, 330), pt(880, 300), pt(560, 345), pt(380, 300), pt(250, 330)]
    const edges = [[0, 1], [1, 2], [2, 3], [3, 5], [3, 4], [4, 5], [4, 6], [5, 6], [6, 7], [7, 8], [4, 9], [9, 10], [10, 2], [10, 11], [11, 0], [9, 7]]
    const net = lin(p, 0.72, 1)
    edges.forEach((e, n) => {
      const u = clamp01(net * edges.length - n)
      if (u > 0) stroke(g, [nodes[e[0]], mixP(nodes[e[0]], nodes[e[1]], u)], hex(0x9fd3e6, 0.45), 1.2)
    })
    nodes.forEach((q, i) => {
      const lit = edges.some((e, n) => (e[0] === i || e[1] === i) && net * edges.length > n)
      if (lit) glow(g, q.x, q.y, 10, 10, 0x9fd3e6, 0.6)
      const r = lit ? 3.4 : 2.4
      ellipse(g, q.x, q.y, r, r, hex(Palette.cream, o))
    })
  },

  drawTree(g, x, p, t, seed) {
    const u = pop(lin(p, 0.48, 0.68))
    if (u <= 0.01) return
    g.save()
    g.translate(x, Author.groundY)
    g.scale(u, u)
    poly(g, [pt(-6, 0), pt(6, 0), pt(3, -70), pt(-3, -70)], hex(0x4e3624))
    const rng = seeded(seed)
    const golds = [0xe8b931, 0xd9a21e, 0xf2cf4a, 0xc98f17]
    for (let n = 0; n < 26; n++) {
      const c = pt(rng(-48, 48), rng(-150, -60)), s = rng(14, 26), a = rng(0, 2 * Math.PI)
      poly(g, [0, 1, 2].map((k) => pt(c.x + s * Math.cos(a + k * 2.1), c.y + s * Math.sin(a + k * 2.1))), hex(golds[Math.floor(rng(0, 3.99))]))
    }
    for (let k = 0; k < 3; k++) {
      const f = (t * 0.12 + k * 0.33 + seed * 0.1) % 1
      g.save()
      g.translate(Math.sin(f * 9 + k) * 30 + k * 20 - 20, -120 + f * 120)
      g.rotate(f * 8)
      g.globalAlpha *= Math.sin(f * Math.PI)
      poly(g, [pt(0, -5), pt(5, 3), pt(-5, 3)], hex(0xf2cf4a))
      g.restore()
    }
    g.restore()
  },

  arch(x, w, top, bottom) {
    const q = new Path2D()
    const r = w / 2
    q.moveTo(x, bottom)
    q.lineTo(x, top + r)
    q.arc(x + r, top + r, r, Math.PI, 0)
    q.lineTo(x + w, bottom)
    q.closePath()
    return q
  },

  drawAuditorium(g, p, t) {
    const rng = seeded(13)
    Author.masses.forEach((m, mi) => {
      const cell = mi >= 4 ? 8 : 24
      for (let y = m.y; y < maxY(m) - 0.5; y += Math.min(cell, maxY(m) - y)) {
        const h = Math.min(cell, maxY(m) - y)
        for (let x = m.x; x < maxX(m) - 0.5; x += Math.min(cell, maxX(m) - x)) {
          const w = Math.min(cell, maxX(m) - x)
          const appear = 0.1 + ((Author.groundY - (y + h)) / 390) * 0.5
          const u = lin(p, appear, appear + 0.07)
          const s0 = rng(0, 4.99), s1 = rng(0, 4.99)
          if (u <= 0) continue
          faded(g, u, () => {
            g.save()
            g.translate(0, (1 - easeOut(u)) * -14)
            const k = 1.08 - 0.16 * ((x - m.x) / Math.max(1, m.w))
            poly(g, [pt(x, y), pt(x + w, y), pt(x, y + h)], Author.shade(Author.brick[Math.floor(s0)], k * 1.04))
            poly(g, [pt(x + w, y), pt(x + w, y + h), pt(x, y + h)], Author.shade(Author.brick[Math.floor(s1)], k * 0.94))
            if (mi >= 4) poly(g, [pt(x, y), pt(x + w, y), pt(x + w, y + h), pt(x, y + h)], Author.shade(0x6e3c28, 1))
            g.restore()
          })
        }
      }
    })
    faded(g, span(p, 0.55, 0.72), () => {
      const dark = Author.shade(0x6e3c28, 1), windowColor = hex(0x2e1d16)
      for (const r of [rect(250, 494, 160, 8), rect(590, 494, 160, 8), rect(400, 446, 200, 8)]) fillRect(g, r, dark)
      const arch = Author.arch(452, 96, 472, Author.groundY)
      g.fillStyle = hex(0x2b1b15)
      g.fill(arch)
      glow(g, 500, 575, 48, 80, 0xf2b56a, 0.4 + 0.06 * Math.sin(t * 1.3))
      g.strokeStyle = dark
      g.lineWidth = 4
      g.stroke(arch)
      for (const x of [462, 492, 522]) { g.fillStyle = windowColor; g.fill(Author.arch(x, 16, 318, 428)) }
      for (const wing of [255, 595]) {
        for (let k = 0; k < 5; k++) {
          const x = wing + 16 + k * 26
          g.fillStyle = windowColor
          g.fill(Author.arch(x, 12, 540, 606))
          fillRect(g, rect(x, 516, 12, 12), windowColor)
        }
      }
      ellipse(g, 500, 305, 11, 11, windowColor)
    })
  },
}

// ---- The name: the thread becomes the wires between Cards ----

export const Name = {
  feet: pt(318, 556), scale: 0.36, temari: { center: pt(196, 664), radius: 34 },
  card: (k) => rect(480 + k * 150, 418, 124, 56),
  names: ['load', 'bands', 'rgb'],

  draw(g, p, t) {
    Wall.drawGround(g, 1)
    drawBranch(g, 90, 560, Name.feet.y + 3, t)
    const out = 1 - span(p, 1.25, 1.45)
    for (const [w, x, at] of [['carry on', 250, 0.6], ['stack', 470, 0.75], ['assemble', 690, 0.9]]) {
      const u = pop(lin(p, at - 0.1, at + 0.1))
      if (u <= 0.01 || out <= 0.01) continue
      g.save()
      g.translate(x, 190)
      g.scale(u, u)
      text(g, w, pt(0, 0), 48, { weight: 700, opacity: 0.92 * out })
      g.restore()
    }
    const spin = span(p, 1.35, 1.6) * (1 - span(p, 2.25, 2.5))
    if (spin > 0.01) {
      const s = pop(lin(p, 1.35, 1.6))
      g.save()
      g.translate(470, 190)
      g.scale(s, s)
      text(g, 'spin', pt(0, 0), 92, { weight: 800, color: hex(0xf0b57a), opacity: spin })
      g.restore()
    }
    const mark = span(p, 2.3, 2.6)
    if (mark > 0.01) {
      text(g, 'Tsugumi', pt(500, 190), 76, { weight: 700, opacity: mark })
      text(g, 'tsugumi · the thrush', pt(500, 252), 22, { weight: 500, color: Ink.mist, opacity: mark })
    }
    const birdPose = pose({ blink: Motion.blink(t, 3.4), tilt: 0.03 * Math.sin(t * 0.9) })
    const gape = birdGape(Name.feet, Name.scale, birdPose)
    const drawnF = lin(p, 1.3, 2.15)
    drawTemari(g, Name.temari.center, Name.temari.radius, -drawnF * 6)
    drawBird(g, Name.feet, Name.scale, birdPose)
    for (let k = 0; k < 3; k++) {
      const r0 = Name.card(k)
      const reach = (r0.x - gape.x) / (maxX(Name.card(2)) - gape.x)
      const u = span(drawnF, reach - 0.02, reach + 0.1)
      faded(g, u, () => {
        const r = rect(r0.x, r0.y + (1 - u) * 10, r0.w, r0.h)
        fillRound(g, r, 9, hex(0x1d3a3e, 0.92))
        strokeRound(g, r, 9, hex(Palette.olive, 0.85), 1.4)
        text(g, '</>', pt(r.x + 16, r.y + 17), 9, { weight: 600, color: Ink.mist, mono: true })
        text(g, Name.names[k], pt(r.x + 30, r.y + 17), 13, { weight: 600, align: 'left' })
        CasA.check(g, pt(maxX(r) - 14, r.y + 17), 5.5, hex(Palette.olive))
        fillRound(g, rect(r.x + 12, r.y + 35, 70, 3), 1.5, hex(Palette.cream, 0.25))
        fillRound(g, rect(r.x + 12, r.y + 44, 50, 3), 1.5, hex(Palette.cream, 0.25))
      })
    }
  },
}

// ---- The figures: which story each shows, and where the page's thread runs ----

const wallCrop = rect(0, 150, 1000, 610)
const casaCrop = rect(0, 95, 1000, 720)
const inP = (k) => CasA.inPort(CasA.cardRect(k)), outP = (k) => CasA.outPort(CasA.cardRect(k))
const nameGape = birdGape(Name.feet, Name.scale, pose())
const wallTall = rect(90, 20, 830, 880)
/** The thread through the Cards, in step with CasA.implement: each port is reached as that Card begins and ends becoming real. */
const wiring = (start, end) => [...start, [CasA.startPort, 0.055], [inP(0), 0.069], [outP(0), 0.172], [inP(1), 0.214], [outP(1), 0.317], [inP(2), 0.359], [outP(2), 0.462], ...end]
const cardsOccluding = () => [CasA.startRect, CasA.cardRect(0), CasA.cardRect(1), CasA.cardRect(2)]

// `pace` is how much scrolling a figure takes; `stops`, where its story is at each slide of the slide show; `deck`, how it fits the slide: a crop for its half of the screen, or `wide` to take the width above the words.
export const Figures = {
  author: { draw: Author.draw, from: 0, to: 1, crop: rect(0, 90, 1000, 620), route: [[pt(40, 90), 0], [pt(40, 710), 1]], deck: { crop: rect(120, 20, 760, 760) } },
  wall: {
    draw: Wall.draw, from: 0.15, to: 3.3, crop: wallCrop, route: [[pt(40, 150), 0], [pt(40, 760), 1]], pace: 0.55,
    flights: [{ item: 'Reproducibility is not assured', label: 'Not reproducible', to: pt(500, 632), at: 1.45 },
      { item: 'The path to the conclusion is invisible', label: 'Process unseen', to: pt(500, 534), at: 2.05 },
      { item: 'Handing data to an AI is frightening', label: 'Data cannot go', to: pt(500, 436), at: 2.65 }],
    stops: [3.0, 3.3], deck: { crop: wallTall },
  },
  thread: {
    draw: Wall.draw, from: 3.45, to: 5.4, crop: wallCrop, pace: 1.7,
    // up the heap from the temari, over the top and down to the AI
    route: [[pt(40, 150), 0], [pt(46, 690), 0.1], [pt(236, 696), 0.15], [Wall.temariTop, 0.19], [pt(372, 452), 0.3], [Wall.over, 0.44], [pt(650, 340), 0.55], [Wall.tie, 0.66], [pt(770, 712), 0.82], [pt(160, 758), 1]],
    beads: { from: 0.19, to: 0.66, appear: 4.7 },
    stops: [4.75, 5.4], deck: { crop: wallTall },
  },
  sketch: {
    draw: CasA.draw, from: 0.1, to: 1.7, crop: casaCrop, route: [[pt(10, 95), 0], [pt(10, 815), 1]], trigger: pt(792, 224), triggerAt: 0.6, pace: 0.9,
    deck: { wide: true },
  },
  draft: {
    draw: CasA.draw, from: 1.7, to: 2.5, crop: casaCrop, route: [[pt(10, 95), 0], [pt(10, 815), 1]], doodleDone: true, pace: 1.3,
    deck: { wide: true },
  },
  run: {
    draw: CasA.draw, from: 2.6, to: 5.5, crop: casaCrop, pace: 1.9, birdOverCards: true,
    route: wiring([[pt(10, 95), 0], [pt(14, 330), 0.03], [pt(CasA.startRect.x, midY(CasA.startRect)), 0.045]], [[pt(915, 430), 0.5], [pt(918, 700), 0.78], [pt(160, 800), 1]]),
    occluders: cardsOccluding,
    cues: [{ item: 'Implement.', from: 2.75, to: 3.98 }, { item: 'Check.', from: 4.0, to: 5.0 }],
    doodleDone: true, listStops: [2.6, 3.98, 5.5],
    deck: { wide: true },
  },
  name: {
    draw: Name.draw, from: 0.4, to: 2.7, crop: rect(0, 120, 1000, 610),
    route: [[pt(60, 120), 0], [pt(110, 690), 0.12], [pt(170, 698), 0.17], [temariLead(Name.temari.center, Name.temari.radius), 0.22],
      [nameGape, 0.36], [pt(Name.card(0).x, midY(Name.card(0))), 0.46], [pt(maxX(Name.card(0)), midY(Name.card(0))), 0.5],
      [pt(Name.card(1).x, midY(Name.card(1))), 0.57], [pt(maxX(Name.card(1)), midY(Name.card(1))), 0.62],
      [pt(Name.card(2).x, midY(Name.card(2))), 0.68], [pt(maxX(Name.card(2)), midY(Name.card(2))), 0.73],
      [pt(925, 480), 0.77], [pt(918, 670), 0.86], [pt(860, 722), 0.9], [pt(160, 728), 1]],
    occluders: () => [Name.card(0), Name.card(1), Name.card(2)],
    birdAway: [0, 1], pace: 1.1,
    // the name / "tsugumi" / carry on, stack, assemble / spin, and the temari and thread
    stops: [0.45, 0.5, 1.12, 2.7], deck: { crop: rect(60, -40, 900, 840) },
  },
}

/** The route as a smooth line (scene coordinates), each point with the share of the figure at which the thread gets there. */
export function figurePath(f) {
  const points = [], times = []
  const R = f.route
  for (let k = 0; k < R.length - 1; k++) {
    const [a, ta] = R[k], [d, td] = R[k + 1]
    const prev = k > 0 ? R[k - 1][0] : a, next = k + 2 < R.length ? R[k + 2][0] : d
    const b = pt(a.x + (d.x - prev.x) / 6, a.y + (d.y - prev.y) / 6), c = pt(d.x - (next.x - a.x) / 6, d.y - (next.y - a.y) / 6)
    curve(a, b, c, d, 16).forEach((q, j) => {
      if (k === 0 || j > 0) { points.push(q); times.push(ta + ((td - ta) * j) / 16) }
    })
  }
  return { points, times }
}
