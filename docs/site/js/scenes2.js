// The figures that only the site has, drawn like those from the app (scenes.js):
// low-poly, on the same ground, with the same person, gem, bird and thread. A
// scene is a pure function of its story position `p` and the clock `t`.
import {
  HAND, Motion, Palette, Polyline, childText, clamp01, curve, drawBird, drawTemari, easeOut, ellipse, faded, fillRound, glow, hex, lin, mix, mixP, poly, pop, pose,
  pt, rect, seeded, smooth, span, stroke, strokeRound, temariLead, text,
} from './art.js'
import { CasA, Figures, Person, Wall, casaImage } from './scenes.js'

const maxX = (r) => r.x + r.w, maxY = (r) => r.y + r.h, midX = (r) => r.x + r.w / 2, midY = (r) => r.y + r.h / 2
const GROUND = Wall.groundY
const green = 0x5fbf6a, wire = 0xe0893a, amber = 0xf0a050

/** A small faceted gem, for when the AI is not the whole figure. */
function gem(g, c, r, t) {
  glow(g, c.x, c.y, r * 2, r * 2, 0x8fc6d6, 0.4 + 0.1 * Math.sin(t * 2))
  const top = pt(c.x, c.y - r * 1.25), bottom = pt(c.x, c.y + r * 1.1)
  const swing = Math.sin(t * 0.8) * 0.25 * r
  const left = pt(c.x - r, c.y), right = pt(c.x + r, c.y), front = pt(c.x + swing, c.y + r * 0.05)
  poly(g, [top, left, front], hex(0xb6e2ee))
  poly(g, [top, front, right], hex(0x8fc6d6))
  poly(g, [bottom, front, left], hex(0x79aebd))
  poly(g, [bottom, right, front], hex(0x5f94a4))
}

/** One of the AI's pages. */
function sheet(g, q, rot, alpha = 1) {
  faded(g, alpha, () => {
    g.save()
    g.translate(q.x, q.y)
    g.rotate(rot)
    fillRound(g, rect(-15, -10, 30, 20), 2, hex(0xe9edf0))
    for (let l = 0; l < 3; l++) { g.fillStyle = hex(0x7c8b92); g.fillRect(-11, -6 + l * 5, l === 2 ? 12 : 20, 1.6) }
    g.restore()
  })
}

/** A Card, simply: a name, its state, and ports for the thread. */
function card(g, r, name, { state = 'idle', draft = false, dim = 0 } = {}) {
  faded(g, 1 - dim * 0.65, () => {
    fillRound(g, rect(r.x - 2, r.y + 4, r.w + 4, r.h + 2), 11, 'rgba(0,0,0,0.25)')
    fillRound(g, r, 10, hex(0x2b2c30))
    if (draft) strokeRound(g, r, 10, hex(0x9aa0a8), 1.4, [6, 4])
    else strokeRound(g, r, 10, state === 'done' ? hex(green) : state === 'running' ? hex(amber) : hex(0x6d7177), state === 'idle' ? 1.3 : 2)
    text(g, draft ? '✎' : '</>', pt(r.x + 18, r.y + 18), 10, { weight: 600, color: hex(0x9a9ca2), mono: true })
    text(g, name, pt(r.x + 34, r.y + 18), 14, { weight: 600, color: hex(0xe8e8ea), align: 'left' })
    if (state === 'done') CasA.check(g, pt(maxX(r) - 16, r.y + 18), 6.5, hex(green))
    fillRound(g, rect(r.x + 14, r.y + 36, r.w * 0.55, 3), 1.5, hex(Palette.cream, 0.22))
    fillRound(g, rect(r.x + 14, r.y + 45, r.w * 0.38, 3), 1.5, hex(Palette.cream, 0.22))
    ellipse(g, r.x, midY(r), 4, 4, hex(0xd8d8dc))
    ellipse(g, maxX(r), midY(r), 4, 4, hex(0x3b82f6))
  })
}
const wireBetween = (a, b) => { const k = Math.max(26, Math.abs(b.x - a.x) * 0.5); return curve(a, pt(a.x + k, a.y), pt(b.x - k, b.y), b, 20) }
const label = (g, s, at, size = 20, o = 1, color) => text(g, s, at, size, { opacity: o, family: HAND, color: color ?? hex(Palette.cream) })

// ---- 足場: AI が難問に挑み、人が確かめられる形で残る ----

export const Summit = {
  peak: pt(590, 200),
  stair: Array.from({ length: 6 }, (_, i) => rect(286 + i * 50, 628 - i * 76, 128, 13)),
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const rise = easeOut(lin(p, 0, 0.7))
    faded(g, rise, () => {
      g.save()
      g.translate(0, (1 - rise) * 60)
      const P = Summit.peak, A = pt(250, GROUND), B = pt(395, 470), C = pt(490, 340), D = pt(690, 350), E = pt(800, 500), F = pt(930, GROUND), M = pt(600, 520)
      poly(g, [A, B, M], hex(0x2e5659)); poly(g, [B, C, M], hex(0x386569)); poly(g, [C, P, M], hex(0x44767a))
      poly(g, [P, D, M], hex(0x2a4f53)); poly(g, [D, E, M], hex(0x234449)); poly(g, [E, F, M], hex(0x1c393e))
      poly(g, [A, M, pt(600, GROUND)], hex(0x254a4e)); poly(g, [pt(600, GROUND), M, F], hex(0x1a353a))
      poly(g, [mixP(P, C, 0.26), P, mixP(P, M, 0.22)], hex(0xeef0ea)); poly(g, [P, mixP(P, D, 0.26), mixP(P, M, 0.22)], hex(0xc4d1d1))
      g.restore()
    })
    faded(g, rise, () => gem(g, pt(Summit.peak.x, Summit.peak.y - 66), 34, t))
    // what the AI sends down the mountain is proved, one piece at a time, and each piece holds a step
    Summit.stair.forEach((r, i) => {
      const at = 0.75 + i * 0.22, u = pop(lin(p, at, at + 0.2))
      if (u <= 0.01) return
      g.save()
      g.translate(midX(r), midY(r))
      g.scale(u, u)
      const q = rect(-r.w / 2, -r.h / 2, r.w, r.h)
      poly(g, [pt(q.x, q.y), pt(maxX(q), q.y), pt(maxX(q) - 6, maxY(q)), pt(q.x + 6, maxY(q))], hex(0xd9b98a))
      poly(g, [pt(q.x + 6, maxY(q)), pt(maxX(q) - 6, maxY(q)), pt(maxX(q) - 8, maxY(q) + 6), pt(q.x + 8, maxY(q) + 6)], hex(0x8f6e43))
      stroke(g, [pt(q.x + 20, maxY(q)), pt(q.x + 20, GROUND - midY(r))], hex(0x8f6e43, 0.7), 3)
      stroke(g, [pt(maxX(q) - 20, maxY(q)), pt(maxX(q) - 20, GROUND - midY(r))], hex(0x8f6e43, 0.7), 3)
      CasA.check(g, pt(0, q.y - 9), 8, hex(green))
      g.restore()
    })
    // and a person can go up them
    const path = new Polyline([pt(196, GROUND), ...Summit.stair.map((r) => pt(midX(r), r.y))])
    const climb = span(p, 2.1, 3)
    const f = path.at(climb)
    Person.draw(g, f, t, { scale: 0.66, tilt: -0.14, puzzled: 0 })
  },
}

// ---- 速度: AI の出力は、読む速さを追い越していく ----

export const Outpace = {
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const ai = pt(836, 470 + Math.sin(t * 1.4) * 8)
    Wall.drawAI(g, ai, t, 0)
    const n = Math.floor(lin(p, 0.15, 2.8) * 42)
    const rng = seeded(7)
    const jitter = Array.from({ length: 48 }, () => rng(-7, 7))
    const place = (i) => pt(372 + jitter[i], GROUND - 8 - i * 8.6)
    for (let i = 0; i < n; i++) sheet(g, place(i), jitter[i] * 0.015)
    // the page on its way
    for (let i = 0; i < 3; i++) {
      const u = (t * 0.75 + i / 3) % 1
      const to = place(Math.min(47, n))
      const q = curve(ai, pt(ai.x - 120, ai.y - 240), pt(to.x + 160, to.y - 200), to, 30)[Math.floor(easeOut(u) * 30)]
      if (p > 0.12 && n < 44) sheet(g, q, u * 5 + i, Math.min(1, u * 5) * (1 - span(u, 0.92, 1)))
    }
    const f = pt(196, GROUND)
    const sh = Person.draw(g, f, t, { scale: 1.2, reach: pt(70, -170) })
    g.save()
    g.translate(f.x + 88, f.y - 176)
    g.rotate(-0.2)
    fillRound(g, rect(-22, -28, 44, 56), 2, hex(0xe9edf0))
    for (let l = 0; l < 7; l++) { g.fillStyle = hex(0x7c8b92, 0.8); g.fillRect(-16, -22 + l * 7, l % 3 === 2 ? 20 : 32, 1.8) }
    g.restore()
    // two speeds
    const bars = [['AI が試した数', clamp01(lin(p, 0.15, 2.4)), 0x8fc6d6], ['人が理解した数', 0.14 * clamp01(lin(p, 0.15, 3)), 0xf0b57a]]
    bars.forEach(([name, v, color], i) => {
      const r = rect(290, 176 + i * 52, 440, 16)
      text(g, name, pt(r.x, r.y - 11), 14, { weight: 600, align: 'left' })
      fillRound(g, r, 8, 'rgba(255,255,255,0.08)')
      if (v > 0.004) fillRound(g, rect(r.x, r.y, Math.max(12, r.w * v), r.h), 8, hex(color))
    })
    if (sh && p > 2.5) text(g, '?', pt(f.x + 30, sh - 86), 40, { weight: 800, opacity: span(p, 2.5, 2.8) * 0.85 })
  },
}

// ---- Level 3: 任せていた人が、突然引き継ぎを求められる ----

export const Driver = {
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const level = mix(0, 3, span(p, 0.2, 1.8))
    // the levels
    for (let i = 0; i <= 5; i++) {
      const r = rect(240 + i * 88, 158, 78, 32), on = i <= Math.floor(level + 0.02), cur = i === Math.round(level)
      fillRound(g, r, 7, on ? hex(cur ? 0xf0b57a : 0x4b6a6d) : 'rgba(255,255,255,0.06)')
      text(g, `Level ${i}`, pt(midX(r), midY(r)), 13, { weight: 700, color: on && cur ? '#1b1d1f' : hex(Palette.cream, on ? 0.95 : 0.4) })
    }
    // the road
    const roadY = GROUND - 4
    for (let i = 0; i < 12; i++) {
      const x = ((i * 130 - t * 180) % 1560 + 1560) % 1560 - 200
      stroke(g, [pt(x, roadY + 26), pt(x + 66, roadY + 26)], hex(Palette.cream, 0.4), 4, { cap: 'butt' })
    }
    // the car
    const bump = Math.sin(t * 9) * 1.2
    g.save()
    g.translate(0, bump)
    const y = GROUND - 34
    poly(g, [pt(240, y), pt(262, y - 76), pt(742, y - 84), pt(790, y - 22), pt(790, y)], hex(0xc97d3d))
    poly(g, [pt(240, y), pt(790, y), pt(790, y - 22), pt(250, y - 34)], hex(0xa35c29))
    poly(g, [pt(346, y - 76), pt(420, y - 172), pt(610, y - 172), pt(690, y - 82)], hex(0xd58b49))
    poly(g, [pt(372, y - 80), pt(430, y - 158), pt(508, y - 158), pt(508, y - 80)], hex(0x24363b))
    poly(g, [pt(520, y - 80), pt(520, y - 158), pt(602, y - 158), pt(664, y - 82)], hex(0x2d4348))
    // the driver in the window, looking down and hands off as the level rises
    const away = span(level, 1.6, 3)
    const alert = span(p, 2.15, 2.4)
    const head = pt(575, y - 118 + (away - alert) * 7)
    g.save()
    g.translate(head.x, head.y)
    g.rotate((away - alert) * 0.42)
    poly(g, [pt(-16, -18), pt(14, -20), pt(19, -2), pt(13, 16), pt(-10, 16), pt(-17, 4)], hex(Person.skin))
    poly(g, [pt(-18, -22), pt(16, -24), pt(14, -12), pt(-4, -10), pt(-10, 6), pt(-19, 2)], hex(Person.hair))
    ellipse(g, 8, -4, 1.7, 2.4, hex(0x2a2320))
    g.restore()
    const wheel = pt(632, y - 98)
    ellipse(g, wheel.x, wheel.y, 4, 22, hex(0x14191b))
    const handAt = mixP(wheel, pt(600, y - 80), away - alert * 0.4)
    stroke(g, [pt(566, y - 82), handAt], hex(Person.coat[1]), 10)
    ellipse(g, handAt.x, handAt.y, 6, 6, hex(Person.skin))
    for (const x of [344, 660]) {
      ellipse(g, x, y + 2, 38, 38, hex(0x14191b))
      ellipse(g, x, y + 2, 17, 17, hex(0x7d8a8d))
      const a = t * 8
      stroke(g, [pt(x + Math.cos(a) * 15, y + 2 + Math.sin(a) * 15), pt(x - Math.cos(a) * 15, y + 2 - Math.sin(a) * 15)], hex(0x3b4648), 2.5)
    }
    g.restore()
    // the request
    faded(g, alert, () => {
      const pulse = 1 + 0.03 * Math.sin(t * 8)
      g.save()
      g.translate(500, 282)
      g.scale(pulse, pulse)
      fillRound(g, rect(-196, -28, 392, 56), 14, hex(0xc8493a))
      text(g, 'ここからは、あなたが判断してください', pt(0, 0), 19, { weight: 700, color: '#fff' })
      g.restore()
    })
    if (p > 2.45) for (let i = 0; i < 3; i++) text(g, '?', pt(650 + i * 32, 400 - i * 26 + Math.sin(t * 2 + i) * 3), 28 + i * 6, { weight: 800, opacity: span(p, 2.45 + i * 0.12, 2.65 + i * 0.12) * 0.9 })
  },
}

// ---- きれいなグラフ: 選ばれた点だけの線と、全部の点の線 ----

export const Curve = {
  frame: rect(210, 170, 580, 470),
  points: (() => {
    const rng = seeded(23), out = []
    for (let i = 0; i < 58; i++) {
      const x = rng(0.04, 0.96)
      const y = clamp01(0.5 + (x - 0.5) * 0.12 + rng(-0.36, 0.36))
      out.push({ x, y, picked: Math.abs(y - (0.14 + x * 0.72)) < 0.055 })
    }
    return out
  })(),
  fit(pts) {
    const n = pts.length, mx = pts.reduce((a, q) => a + q.x, 0) / n, my = pts.reduce((a, q) => a + q.y, 0) / n
    let sxy = 0, sxx = 0, syy = 0
    for (const q of pts) { sxy += (q.x - mx) * (q.y - my); sxx += (q.x - mx) ** 2; syy += (q.y - my) ** 2 }
    const m = sxy / sxx
    return { m, b: my - m * mx, r: sxy / Math.sqrt(sxx * syy) }
  },
  draw(g, p, t) {
    const F = Curve.frame
    const at = (q) => pt(F.x + 34 + q.x * (F.w - 58), maxY(F) - 34 - q.y * (F.h - 60))
    fillRound(g, rect(F.x, F.y + 6, F.w, F.h), 12, 'rgba(0,0,0,0.25)')
    fillRound(g, F, 12, hex(0x232427))
    stroke(g, [pt(F.x + 34, F.y + 20), pt(F.x + 34, maxY(F) - 34), pt(maxX(F) - 20, maxY(F) - 34)], hex(Palette.cream, 0.4), 1.4)
    const all = Curve.points, picked = all.filter((q) => q.picked)
    const show = span(p, 0, 0.7), others = span(p, 1.05, 1.9)
    for (const q of all) {
      const c = at(q)
      if (q.picked) ellipse(g, c.x, c.y, 5 * show, 5 * show, hex(0xf0b57a))
      else if (others > 0.01) ellipse(g, c.x, c.y, 4.2, 4.2, hex(0x8fc6d6, 0.75 * others))
    }
    const a = Curve.fit(picked), b = Curve.fit(all)
    const u = span(p, 1.3, 2)
    const m = mix(a.m, b.m, u), c0 = mix(a.b, b.b, u), r = mix(a.r, b.r, u)
    const line = (mm, bb, color, w) => stroke(g, [at(pt(0, bb)), at(pt(1, mm + bb))], color, w)
    if (show > 0.5) line(m, c0, hex(0xf6ead2, 0.95), 3)
    text(g, `r = ${r.toFixed(2)}`, pt(maxX(F) - 30, F.y + 34), 22, { weight: 700, align: 'right', opacity: show, mono: true })
    text(g, others > 0.5 ? '除かれていた点も入れると' : '選ばれた点だけ', pt(F.x + 56, F.y + 34), 16, { weight: 600, align: 'left', opacity: show })
    // and with the seed changed, the line moves
    const rng = seeded(61)
    const more = span(p, 2.1, 2.9)
    if (more > 0.01) {
      for (let k = 0; k < 9; k++) {
        const sample = all.filter(() => rng(0, 1) < 0.65)
        const f = Curve.fit(sample)
        line(f.m, f.b, hex(0xf0b57a, 0.32 * span(more, k / 9, (k + 2) / 9)), 1.6)
      }
      text(g, 'シードや条件を変えて、確かめる', pt(F.x + 56, F.y + 62), 15, { color: hex(0xf0b57a), align: 'left', opacity: more })
    }
    void t
  },
}

// ---- AI-driven science: 問いは人から、試行は AI が回し、意味は人が与える ----

export const Driven = {
  steps: ['仮説', '検証', '条件を変える'],
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const f = pt(200, GROUND), c = pt(650, 400), R = 170
    const sh = Person.draw(g, f, t, { scale: 1.2, reach: pt(80, -150) })
    // the question goes out as a thread
    const out = span(p, 0.25, 0.9)
    const start = pt(f.x + 86, f.y - 150)
    if (out > 0.01) stroke(g, wireBetween(start, pt(c.x - R, c.y)).slice(0, Math.ceil(out * 21)), hex(wire), 2.6)
    const bubble = 1 - span(p, 1.9, 2.3)
    faded(g, span(p, 0, 0.4) * bubble, () => {
      fillRound(g, rect(f.x - 58, sh - 118, 116, 52), 14, hex(0xf4ecdc))
      text(g, '問い', pt(f.x, sh - 92), 22, { weight: 700, color: '#1b1d1f' })
    })
    // the loop the AI keeps turning
    const loop = span(p, 0.7, 1.2)
    faded(g, loop, () => {
      gem(g, c, 50, t)
      const ring = Array.from({ length: 61 }, (_, i) => { const a = (i / 60) * Math.PI * 2; return pt(c.x + Math.cos(a) * R, c.y + Math.sin(a) * R) })
      stroke(g, ring, hex(Palette.cream, 0.25), 2)
      Driven.steps.forEach((name, i) => {
        const a = -Math.PI / 2 + (i / 3) * Math.PI * 2, q = pt(c.x + Math.cos(a) * R, c.y + Math.sin(a) * R)
        const w = name.length * 19 + 28
        fillRound(g, rect(q.x - w / 2, q.y - 20, w, 40), 12, hex(0x2b2c30))
        strokeRound(g, rect(q.x - w / 2, q.y - 20, w, 40), 12, hex(0x6d7177), 1.3)
        text(g, name, q, 18, { weight: 600, color: hex(0xe8e8ea) })
      })
      const speed = mix(0.5, 3.2, span(p, 1, 2))
      for (let i = 0; i < 4; i++) {
        const a = t * speed + (i / 4) * Math.PI * 2
        ellipse(g, c.x + Math.cos(a) * R, c.y + Math.sin(a) * R, 6, 6, hex(amber))
      }
    })
    const n = Math.floor(lin(p, 0.9, 2.4) * 128)
    if (n > 0) text(g, `試行 ${Math.min(128, n)}`, pt(c.x, c.y + R + 54), 20, { weight: 700, mono: true, opacity: loop })
    // what comes back, and what only the person gives it
    const back = span(p, 1.9, 2.5)
    if (back > 0.01) {
      for (let i = 0; i < 5; i++) {
        const u = clamp01(back * 1.4 - i * 0.08)
        sheet(g, mixP(pt(c.x - 40, c.y + 10), pt(f.x + 120 + (i % 2) * 8, GROUND - 12 - i * 9), easeOut(u)), (i - 2) * 0.05, u)
      }
    }
    ;['意味を与える', '責任を持つ', '次の問いを立てる'].forEach((s, i) => {
      text(g, s, pt(f.x + 92, 300 + i * 40), 20, { weight: 700, align: 'center', color: hex(0xf0b57a), opacity: span(p, 2.3 + i * 0.2, 2.55 + i * 0.2) })
    })
  },
}

// ---- 手が届くことと、わかっていること: 運転でも、研究でも ----

export const Grasp = {
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const research = span(p, 1.5, 2.1), L = pt(285, 400), Rt = pt(715, 400)
    const title = (x, a, b) => {
      text(g, a, pt(x, 214), 24, { weight: 700, opacity: 1 - research })
      text(g, b, pt(x, 214), 24, { weight: 700, opacity: research })
    }
    title(L.x, 'ハンドルを握れる', '結論を確認できる')
    title(Rt.x, '状況がわかっている', '過程がわかっている')
    // the wheel, with a hand on it
    faded(g, 1 - research, () => {
      strokeRound(g, rect(L.x - 84, L.y - 84, 168, 168), 84, hex(0x7d8a8d), 15)
      ellipse(g, L.x, L.y, 22, 22, hex(0x3b4648))
      stroke(g, [pt(L.x - 78, L.y), pt(L.x + 78, L.y)], hex(0x7d8a8d), 9)
      stroke(g, [pt(L.x, L.y), pt(L.x, L.y + 78)], hex(0x7d8a8d), 9)
      const u = span(p, 0.1, 0.55)
      const hand = mixP(pt(L.x - 150, L.y + 120), pt(L.x - 78, L.y - 22), easeOut(u))
      stroke(g, [pt(L.x - 190, L.y + 180), hand], hex(Person.coat[1]), 15)
      ellipse(g, hand.x, hand.y, 16, 16, hex(Person.skin))
    })
    // the report, with its stamp
    faded(g, research, () => {
      const page = rect(L.x - 76, L.y - 100, 152, 200)
      fillRound(g, page, 5, hex(0xe9edf0))
      for (let l = 0; l < 6; l++) { g.fillStyle = hex(0x7c8b92, 0.8); g.fillRect(page.x + 18, page.y + 24 + l * 18, l % 3 === 2 ? 70 : 116, 3) }
      CasA.check(g, pt(L.x + 34, L.y + 62), 22 * pop(span(p, 1.8, 2.2)), hex(green))
    })
    // what the road ahead looks like when you come back to it
    const clear = 0.3 * span(p, 0.5, 1.4)
    faded(g, 1 - research, () => {
      const w = rect(Rt.x - 130, Rt.y - 90, 260, 180)
      fillRound(g, w, 16, hex(0x24363b))
      poly(g, [pt(Rt.x - 110, maxY(w)), pt(Rt.x - 12, w.y + 60), pt(Rt.x + 12, w.y + 60), pt(Rt.x + 110, maxY(w))], hex(0x4b5a5c))
      fillRound(g, w, 16, hex(0xdfe5e6, 0.82 * (1 - clear)))
    })
    // the steps behind the conclusion, unseen
    faded(g, research, () => {
      ;['load', 'filter', 'fit'].forEach((name, i) => card(g, rect(Rt.x - 90, Rt.y - 110 + i * 78, 180, 64), name, { dim: 0.8 }))
      fillRound(g, rect(Rt.x - 124, Rt.y - 124, 248, 248), 16, hex(0xdfe5e6, 0.2 + 0.04 * Math.sin(t * 1.3)))
    })
    if (p > 0.6) text(g, '?', pt(Rt.x, Rt.y), 72, { weight: 800, color: '#1b1d1f', opacity: 0.7 * span(p, 0.6, 1) })
    // how far each has come
    const bars = [[L.x, span(p, 0.1, 0.55), green], [Rt.x, clear, amber]]
    bars.forEach(([x, v, color]) => {
      const r = rect(x - 150, 590, 300, 16)
      fillRound(g, r, 8, 'rgba(255,255,255,0.08)')
      if (v > 0.004) fillRound(g, rect(r.x, r.y, Math.max(12, r.w * v), r.h), 8, hex(color))
    })
  },
}

// ---- 同じ一つ: 人が描くもの、AI が編集するもの、実際に動くものが重なる ----

export const OneGraph = {
  names: ['load', 'bands', 'rgb'],
  box: (k, y) => rect(250 + k * 200, y, 150, 74),
  y: 330,
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const merge = span(p, 1.0, 2.0)
    const layers = [['人が描く', -150], ['AI が編集する', 0], ['実際に動く', 150]]
    layers.forEach(([name, dy], i) => {
      const y = OneGraph.y + dy * (1 - merge)
      const o = span(p, i * 0.22, 0.25 + i * 0.22)
      faded(g, o, () => {
        label(g, name, pt(150, y + 37), 20, 1 - merge)
        OneGraph.names.forEach((n, k) => {
          const r = OneGraph.box(k, y)
          if (i === 0) {
            faded(g, 1 - span(merge, 0.6, 1), () => {
              strokeRound(g, r, 18, hex(Palette.cream, 0.9), 2.4)
              childText(g, n, pt(midX(r), midY(r)), 24, hex(Palette.cream), { seed: 5 + k })
            })
          } else if (i === 1) {
            faded(g, 1 - span(merge, 0.8, 1), () => {
              fillRound(g, r, 10, hex(0x1d3a3e, 0.95))
              const s = seeded(9 + k)
              for (let l = 0; l < 5; l++) { g.fillStyle = hex(0x8fc6d6, 0.8); g.fillRect(r.x + 14, r.y + 13 + l * 11, s(40, r.w - 40), 3) }
            })
          } else {
            const done = p >= 2.05 + k * 0.28
            card(g, r, n, { state: done ? 'done' : 'idle' })
          }
        })
      })
    })
    const m = span(p, 2.7, 3)
    text(g, '三つは、同じ Workflow', pt(500, 520), 26, { weight: 700, opacity: m })
    void t
  },
}

// ---- 途中から: 保存された入力で、その Card から ----

export const Partial = {
  names: ['load', 'clean', 'fit', 'plot'],
  box: (k) => rect(112 + k * 200, 300, 160, 78),
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const stateOf = (k) => {
      const first = [0.1 + k * 0.2, 0.3 + k * 0.2]
      if (p < first[0]) return 'idle'
      if (p < first[1]) return 'running'
      if (p < 1.15) return 'done'
      if (k < 2) return p >= 2.55 + k * 0.1 ? 'done' : 'done'
      const re = [1.45 + (k - 2) * 0.32, 1.77 + (k - 2) * 0.32]
      if (p < re[0]) return 'idle'
      if (p < re[1]) return 'running'
      return 'done'
    }
    const upstream = span(p, 1.2, 1.4) * (1 - span(p, 2.5, 2.7))
    for (let k = 0; k < 3; k++) {
      const a = Partial.box(k), b = Partial.box(k + 1)
      const lit = k >= 1 ? 1 : 1 - upstream * 0.7
      stroke(g, wireBetween(pt(maxX(a), midY(a)), pt(b.x, midY(b))), hex(wire, 0.85 * lit), 2)
    }
    Partial.names.forEach((n, k) => {
      const r = Partial.box(k)
      card(g, r, n, { state: stateOf(k), dim: k < 2 ? upstream : 0 })
      // what the run kept: the input of each Card
      const kept = span(p, 0.3 + k * 0.2, 0.45 + k * 0.2)
      faded(g, kept * (k === 0 ? 0 : 1), () => {
        const box = rect(midX(r) - 20, maxY(r) + 26, 40, 28)
        const hot = k === 2 ? span(p, 1.25, 1.45) : 0
        poly(g, [pt(box.x, box.y), pt(maxX(box), box.y), pt(maxX(box), maxY(box)), pt(box.x, maxY(box))], hex(hot > 0.5 ? 0xd9b98a : 0x4b6a6d))
        poly(g, [pt(box.x - 3, box.y), pt(midX(box), box.y - 12), pt(maxX(box) + 3, box.y)], hex(hot > 0.5 ? 0xf0d9a8 : 0x5e8084))
        stroke(g, [pt(midX(box), box.y - 4), pt(midX(box), maxY(r) + 2)], hex(Palette.cream, 0.3 + 0.5 * hot), 1.6, { dash: [3, 3] })
      })
    })
    const pen = span(p, 1.15, 1.3) * (1 - span(p, 1.45, 1.6))
    if (pen > 0.01) {
      const r = Partial.box(2)
      text(g, '✎', pt(midX(r) + 8, r.y - 22), 30, { color: hex(0xf0b57a), opacity: pen })
    }
    label(g, '保存された入力', pt(midX(Partial.box(2)), 470), 19, span(p, 1.2, 1.45) * (1 - span(p, 2.5, 2.7)))
    const lastStep = span(p, 2.55, 2.85)
    label(g, '最後は、全体を通して確かめる', pt(500, 240), 24, lastStep, hex(0xf0b57a))
    if (lastStep > 0) {
      const f = lin(p, 2.55, 3)
      const a = Partial.box(0), d = Partial.box(3)
      const x = mix(a.x, maxX(d), f)
      stroke(g, [pt(a.x, maxY(a) + 12), pt(x, maxY(a) + 12)], hex(green), 3)
    }
    void t
  },
}

// ---- 枝分かれ: 一つの結果から、別の仮説へ ----

export const Branches = {
  lines: [
    { a: [pt(110, 540), pt(250, 540), pt(390, 540), pt(530, 540), pt(670, 540), pt(830, 540)], from: 0, to: 1.25 },
    { a: [pt(390, 540), pt(470, 410), pt(610, 380), pt(750, 380), pt(880, 380)], from: 1.25, to: 2.1 },
    { a: [pt(530, 540), pt(600, 660), pt(740, 668), pt(890, 668)], from: 2.1, to: 3 },
  ],
  draw(g, p, t) {
    Wall.drawGround(g, 0.7)
    const rng = seeded(31)
    let tip = pt(110, 540), heading = 1, flying = false
    Branches.lines.forEach((l, li) => {
      const f = lin(p, l.from, l.to)
      const smoothPts = []
      for (let i = 0; i < l.a.length - 1; i++) {
        const A = l.a[i], B = l.a[i + 1], k = (B.x - A.x) * 0.45
        smoothPts.push(...curve(A, pt(A.x + k, A.y), pt(B.x - k, B.y), B, 16).slice(i ? 1 : 0))
      }
      const line = new Polyline(smoothPts)
      if (f > 0) {
        stroke(g, line.upTo(f), hex(Palette.thread), 2.4)
        if (f < 1) { tip = line.at(f); flying = true; heading = 1 }
      }
      // a version at each knot, with what it showed
      l.a.forEach((q, i) => {
        if (li > 0 && i === 0) return
        const reach = i / (l.a.length - 1)
        const u = pop(lin(f, reach - 0.02, reach + 0.16))
        const curveSeed = rng(0, 6.28)
        if (u <= 0.01) return
        g.save()
        g.translate(q.x, q.y)
        g.scale(u, u)
        const r = rect(-30, -60, 60, 42)
        fillRound(g, r, 6, hex(0x232427))
        strokeRound(g, r, 6, hex(li === 0 ? 0x6d7177 : li === 1 ? 0xf0b57a : 0x8fc6d6, 0.9), 1.2)
        const pts = Array.from({ length: 12 }, (_, k) => pt(r.x + 6 + k * 4.4, r.y + 22 + Math.sin(k * (0.4 + li * 0.18) + curveSeed) * (8 - li)))
        stroke(g, pts, hex(li === 0 ? 0xc8c8cc : li === 1 ? 0xf0b57a : 0x8fc6d6), 1.4)
        ellipse(g, 0, 0, 5, 5, hex(Palette.thread))
        g.restore()
      })
    })
    label(g, '別のモデルを試す', pt(700, 318), 19, span(p, 1.6, 1.9))
    label(g, '以前の方針へ戻って、条件を変える', pt(700, 732), 19, span(p, 2.45, 2.75))
    if (p < 3) drawBird(g, pt(tip.x - 12, tip.y - 2), 0.1, flying ? Motion.flying(t, 0.12) : Motion.idle(t * 0.5), heading < 0)
  },
}

// ---- 引き継ぎ: 意図とコードと環境を、毬ごと渡す ----

export const Handoff = {
  box: (k) => rect(250 + k * 180, 250, 140, 66),
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const coatB = [0xe2c49a, 0xc9a877, 0xa98a5e]
    const A = pt(160, GROUND), B = pt(830, GROUND)
    const roll = span(p, 0.9, 1.9)
    const ball = pt(mix(270, 730, roll), GROUND - 24 - Math.abs(Math.sin(roll * Math.PI * 3)) * 14 * (1 - roll))
    const given = roll > 0.97
    // the work: three Cards on the thread
    const names = ['集計', '確認', 'レポート']
    const shown = span(p, 0, 0.5)
    for (let k = 0; k < 3; k++) {
      const r = Handoff.box(k)
      if (k < 2) faded(g, shown, () => stroke(g, wireBetween(pt(maxX(r), midY(r)), pt(Handoff.box(k + 1).x, midY(r))), hex(wire, 0.85), 2))
      faded(g, shown, () => card(g, r, names[k], { state: p > 2.3 + k * 0.14 ? 'done' : p > 0.4 ? 'done' : 'idle' }))
    }
    // the thread from the Cards to the temari, which goes across
    const end = Handoff.box(0)
    faded(g, shown, () => stroke(g, curve(pt(end.x, midY(end)), pt(end.x - 60, midY(end) + 150), pt(ball.x - 120, ball.y - 80), temariLead(ball, 24), 26), hex(Palette.thread), 2.2))
    drawTemari(g, ball, 24, -roll * 14)
    // the first person lets it go; the second changes a condition and runs it
    Person.draw(g, A, t, { scale: 1.1, reach: roll < 0.1 ? pt(62, -92) : null })
    g.save()
    g.translate(B.x, 0); g.scale(-1, 1); g.translate(-B.x, 0)
    Person.draw(g, B, t, { scale: 1.1, coat: coatB, trousers: [0x3a3f52, 0x2c3040], hair: 0x4a2f22, reach: given ? pt(70, -176) : null, puzzled: given ? 0 : 0.8 })
    g.restore()
    const edit = span(p, 2.0, 2.25)
    if (edit > 0.01) {
      const r = Handoff.box(1)
      text(g, '✎', pt(midX(r) + 6, r.y - 18), 28, { color: hex(0xf0b57a), opacity: edit * (1 - span(p, 2.6, 2.85)) })
    }
    label(g, '動かして確かめられる形で、渡す', pt(500, 170), 24, span(p, 2.45, 2.8), hex(0xf0b57a))
  },
}

// ---- 教材: 条件を変えると、結論がどう動くか ----

export const Teach = {
  frame: rect(370, 170, 470, 360),
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const F = Teach.frame
    fillRound(g, rect(F.x, F.y + 6, F.w, F.h), 12, 'rgba(0,0,0,0.25)')
    fillRound(g, F, 12, hex(0x232427))
    const v = 0.2 + 0.75 * (0.5 - 0.5 * Math.cos(clamp01(p / 3) * Math.PI * 2.5))
    const plot = rect(F.x + 28, F.y + 24, F.w - 56, F.h - 110)
    stroke(g, [pt(plot.x, plot.y), pt(plot.x, maxY(plot)), pt(maxX(plot), maxY(plot))], hex(Palette.cream, 0.4), 1.4)
    const rng = seeded(4)
    const signal = (x) => Math.exp(-((x - 0.55) ** 2) / 0.012) * 0.62
    const data = Array.from({ length: 34 }, (_, i) => { const x = (i + 0.5) / 34; return { x, y: 0.5 * (1 - x) + 0.08 + signal(x) + rng(-0.022, 0.022) } })
    for (const q of data) {
      const y = q.y - v * 0.5 * (1 - q.x)
      ellipse(g, plot.x + q.x * plot.w, maxY(plot) - y * plot.h, 3.3, 3.3, hex(0x8fc6d6))
    }
    const fit = Array.from({ length: 50 }, (_, i) => { const x = i / 49; const y = (1 - v) * 0.5 * (1 - x) + 0.08 + signal(x) * (0.72 + 0.28 * v); return pt(plot.x + x * plot.w, maxY(plot) - y * plot.h) })
    stroke(g, fit, hex(0xf0b57a), 2.6)
    // the slider, in the learner's hand
    const S = rect(F.x + 28, maxY(F) - 46, F.w - 56, 6)
    fillRound(g, S, 3, 'rgba(255,255,255,0.15)')
    const knob = pt(S.x + v * S.w, midY(S))
    fillRound(g, rect(S.x, S.y, v * S.w, S.h), 3, hex(0xf0b57a))
    ellipse(g, knob.x, knob.y, 9, 9, hex(0xf6ead2))
    text(g, '背景の見積もり', pt(S.x, S.y - 16), 14, { weight: 600, align: 'left' })
    const f = pt(272, GROUND)
    const understood = span(p, 2.3, 2.8)
    Person.draw(g, f, t, { scale: 1.2, reach: pt(102, -214), tilt: 0 })
    if (understood > 0.01) {
      glow(g, f.x + 34, f.y - 318, 34, 34, 0xf6d79a, 0.5 * understood)
      text(g, '!', pt(f.x + 34, f.y - 318), 36, { weight: 800, color: hex(0xf6d79a), opacity: understood })
    }
  },
}

// ---- 鳥のツグミ: 跳ねては止まり、確かめる。そして語らない ----

export const Tsugumi = {
  stops: [120, 300, 480, 640],
  /** Where the bird is at p: 0–1.8 hops along with stops; then it stays. */
  at(p) {
    const n = Tsugumi.stops.length - 1
    const u = clamp01(p / 1.8) * n, i = Math.min(n - 1, Math.floor(u)), f = u - i
    const move = smooth(clamp01(f / 0.55))
    return { x: mix(Tsugumi.stops[i], Tsugumi.stops[i + 1], move), f, moving: f < 0.55 && u < n, i }
  },
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    // winter
    const snow = span(p, 1.9, 2.2)
    if (snow > 0.01) {
      const rng = seeded(13)
      for (let i = 0; i < 46; i++) {
        const x = rng(0, 1000), v = rng(16, 34), ph = rng(0, 700)
        const y = 130 + ((ph + t * v) % 570)
        ellipse(g, x + Math.sin(t + i) * 8, y, 2, 2, hex(0xffffff, 0.55 * snow))
      }
    }
    // what is said, and what is left
    const talk = span(p, 2.6, 3.0) * (1 - span(p, 3.45, 3.7))
    const ai = pt(830, 440 + Math.sin(t * 1.4) * 8)
    const aiO = span(p, 2.4, 2.7)
    faded(g, aiO, () => gem(g, ai, 42, t))
    for (let i = 0; i < 4; i++) {
      const u = (t * 0.16 + i / 4) % 1
      const q = pt(ai.x - 90 - u * 300, ai.y - 90 - u * 170 + Math.sin(u * 6 + i) * 12)
      faded(g, talk * Math.sin(u * Math.PI), () => {
        const r = rect(q.x - 60, q.y - 22, 120, 44)
        fillRound(g, r, 12, hex(0xe9edf0))
        for (let l = 0; l < 3; l++) { g.fillStyle = hex(0x7c8b92); g.fillRect(r.x + 12, r.y + 10 + l * 9, l === 2 ? 60 : 96, 2.2) }
      })
    }
    const left = span(p, 3.3, 3.9)
    if (left > 0.01) {
      const names = ['load', 'bands', 'rgb']
      const box = (k) => rect(350 + k * 190, 280, 150, 72)
      for (let k = 0; k < 3; k++) {
        const r = box(k)
        faded(g, span(left, k * 0.25, 0.3 + k * 0.25), () => {
          if (k < 2) stroke(g, wireBetween(pt(maxX(r), midY(r)), pt(box(k + 1).x, midY(r))), hex(wire, 0.9), 2)
          card(g, r, names[k], { state: 'done' })
        })
      }
    }
    // the bird
    const b = Tsugumi.at(p)
    let poseNow
    if (b.moving) {
      const hops = 3, h = (clamp01(b.f / 0.55) * hops) % 1
      poseNow = Motion.hop(h, 26)
    } else {
      poseNow = pose({ tilt: -0.12 * span(b.f, 0.55, 0.7), blink: Motion.blink(t, 3.1) })
    }
    const looksBack = !b.moving && b.i % 2 === 0 && b.f > 0.72 && b.f < 0.92
    const size = p > 2.3 ? mix(0.3, 0.26, span(p, 2.3, 2.6)) : 0.3
    drawBird(g, pt(b.x, GROUND), size, poseNow, looksBack)
    const temari = span(p, 3.3, 3.7)
    if (temari > 0.01) drawTemari(g, pt(b.x - 90, GROUND - 22), 22 * pop(temari), 0)
  },
}

// ---- Where they go in the documents ----

const gutter = (top, bottom) => [[pt(40, top), 0], [pt(40, bottom), 1]]
const square = { crop: rect(60, 40, 880, 800) }
const plain = { crop: rect(0, 120, 1000, 640) }
const entries = {
  summit: { draw: Summit.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.8, stops: [1.0, 2.1, 3], deck: square },
  outpace: { draw: Outpace.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.8, stops: [1.2, 3], deck: square },
  driver: { draw: Driver.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.9, deck: square },
  curve: { draw: Curve.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.8, stops: [0.9, 3], deck: square },
  driven: { draw: Driven.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.9, stops: [1.6, 3], deck: square },
  grasp: { draw: Grasp.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.9, stops: [1.5, 3], deck: square },
  oneGraph: { draw: OneGraph.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.9, deck: square },
  partial: { draw: Partial.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.9, deck: square },
  branches: { draw: Branches.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.9, deck: square },
  handoff: { draw: Handoff.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.9, deck: square },
  teach: { draw: Teach.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.8, deck: square },
  // 名前の後半: 跳ねては止まる鳥、語らない鳥
  walk: { draw: Tsugumi.draw, from: 0, to: 1.8, ...plain, route: gutter(120, 760), pace: 1.2, stops: [0.55, 1.8], deck: square },
  hush: { draw: Tsugumi.draw, from: 1.9, to: 4, ...plain, route: gutter(120, 760), pace: 1, stops: [2.3, 3.1, 4], deck: square },
}
Object.assign(Figures, entries)

// ---- 「私にしかない深さ」の図: 言葉は添えない ----

/** The night over a scene: the same stars from frame to frame. */
function stars(g, t, o = 1, top = 110, bottom = 640) {
  const rng = seeded(71)
  for (let i = 0; i < 120; i++) {
    const x = rng(0, 1000), y = rng(top - 60, bottom), r = rng(0.7, 2), ph = rng(0, 6.28)
    ellipse(g, x, y, r, r, hex(0xf3ede2, o * (0.28 + 0.5 * (0.5 + 0.5 * Math.sin(t * 1.2 + ph)))))
  }
}

/** Cas A as the rgb Card drew it, opening from its centre. */
function casa(g, c, radius, o) {
  if (o <= 0.01 || !casaImage.complete || !casaImage.naturalWidth) return
  faded(g, o, () => {
    g.save()
    g.beginPath()
    g.arc(c.x, c.y, radius, 0, Math.PI * 2)
    g.clip()
    g.drawImage(casaImage, c.x - radius * 1.2, c.y - radius * 1.2, radius * 2.4, radius * 2.4)
    g.restore()
  })
}

/** A telescope in orbit: its barrel, its wings of panels, its open end towards `angle`. */
function telescope(g, c, angle, k = 1) {
  g.save()
  g.translate(c.x, c.y)
  g.rotate(angle)
  g.scale(k, k)
  for (const side of [-1, 1]) {
    poly(g, [pt(-38, side * 24), pt(26, side * 24), pt(34, side * 92), pt(-30, side * 92)], hex(side < 0 ? 0x2d5c86 : 0x244c70))
    for (let i = 1; i < 4; i++) stroke(g, [pt(-38 + i * 16, side * 24), pt(-30 + i * 16, side * 92)], hex(0x8fc6d6, 0.4), 1)
  }
  poly(g, [pt(-96, -24), pt(96, -24), pt(96, 0), pt(-96, 0)], hex(0xe6dfd0))
  poly(g, [pt(-96, 0), pt(96, 0), pt(96, 24), pt(-96, 24)], hex(0xb5ad9c))
  poly(g, [pt(96, -24), pt(122, -30), pt(122, 0), pt(96, 0)], hex(0xd9b98a))
  poly(g, [pt(96, 0), pt(122, 0), pt(122, 30), pt(96, 24)], hex(0xa98a5e))
  poly(g, [pt(-96, -24), pt(-126, -14), pt(-126, 14), pt(-96, 24)], hex(0x6d7177))
  ellipse(g, 122, 0, 6, 30, hex(0x14191b))
  g.restore()
}

/** 触れがたい宇宙: 遠い光が届くが、ノイズに埋もれている。 */
export const Orbit = {
  draw(g, p, t) {
    stars(g, t)
    const source = pt(800, 240), sat = pt(300, 430)
    glow(g, source.x, source.y, 170, 170, 0x8f6fd0, 0.28)
    casa(g, source, 86, span(p, 0, 0.5))
    telescope(g, sat, -0.42, 0.95)
    const mouth = pt(sat.x + Math.cos(-0.42) * 118, sat.y + Math.sin(-0.42) * 118)
    // what arrives from there, thinned by the way
    for (let i = 0; i < 24; i++) {
      const u = (t * 0.2 + i / 24) % 1
      const q = mixP(source, mouth, u)
      const n = Math.sin(i * 12.9) * 26 * (1 - u)
      ellipse(g, q.x + n, q.y + n * 0.6, 2.4, 2.4, hex(i % 3 === 0 ? 0xe2533f : i % 3 === 1 ? 0x5ec25a : 0x5b8bf0, 0.85 * span(p, 0.3, 0.8) * Math.sin(u * Math.PI)))
    }
    // the detector, and what else it records
    const panel = rect(430, 500, 330, 130)
    faded(g, span(p, 0.8, 1.3), () => {
      fillRound(g, panel, 10, hex(0x17181a, 0.95))
      strokeRound(g, panel, 10, hex(0x4d5157), 1)
      const noise = span(p, 1.2, 2.6)
      const clock = Math.floor(t * 6)
      const rng = seeded(83 + clock)
      for (let i = 0; i < 40 + noise * 420; i++) ellipse(g, panel.x + 8 + rng(0, panel.w - 16), panel.y + 8 + rng(0, panel.h - 16), 1.2, 1.2, hex(0xe9edf0, 0.6))
      const fixed = seeded(5)
      for (let i = 0; i < 22; i++) ellipse(g, panel.x + panel.w * 0.5 + fixed(-46, 46), panel.y + panel.h * 0.5 + fixed(-34, 34), 2, 2, hex(0xf0b57a))
    })
  },
}

/** みんなのもの: 誰の頭の上にも、同じ宇宙がある。 */
export const Sky = {
  people: [[150, 0xa9c9c3], [320, 0xe2c49a], [500, 0xc9a0b4], [670, 0x9fb070], [850, 0x9db4d8]],
  draw(g, p, t) {
    stars(g, t)
    glow(g, 500, 250, 200, 170, 0x8f6fd0, 0.3)
    casa(g, pt(500, 250), 92, span(p, 1.6, 2.6))
    Wall.drawGround(g, 1)
    Sky.people.forEach(([x, c], i) => {
      const o = span(p, i * 0.3, 0.35 + i * 0.3)
      faded(g, o, () => Person.draw(g, pt(x, GROUND), t + i, {
        scale: 0.86, tilt: -0.2, coat: [c, c - 0x151515, c - 0x2a2a2a],
        trousers: i % 2 ? [0x3a3f52, 0x2c3040] : [0x24464a, 0x1b3739], hair: [0x2f2722, 0x4a2f22, 0x1c1a1a, 0x6b5a48, 0x2f2722][i],
      }))
    })
  },
}

/** 手の届くところ: 降ってきたデータが、手元で像になる。 */
export const Reach = {
  draw(g, p, t) {
    stars(g, t, 0.8)
    Wall.drawGround(g, 1)
    const sat = pt(760, 200)
    telescope(g, sat, 2.45, 0.62)
    // the desk and the machine
    const top = GROUND - 92
    poly(g, [pt(330, top), pt(620, top), pt(610, top + 12), pt(340, top + 12)], hex(0x8f6e43))
    stroke(g, [pt(356, top + 12), pt(356, GROUND)], hex(0x6d5230), 7)
    stroke(g, [pt(594, top + 12), pt(594, GROUND)], hex(0x6d5230), 7)
    poly(g, [pt(420, top), pt(560, top), pt(574, top - 6), pt(410, top - 6)], hex(0x7d8a8d))
    const screen = rect(424, top - 96, 136, 88)
    fillRound(g, rect(screen.x - 6, screen.y - 6, screen.w + 12, screen.h + 12), 7, hex(0x3b4648))
    fillRound(g, screen, 3, hex(0x101214))
    const typed = lin(p, 0.15, 0.75)
    for (let l = 0; l < 3; l++) {
      const w = [78, 52, 96][l] * clamp01(typed * 3 - l)
      if (w > 0) { g.fillStyle = hex(0x8fc6d6, 0.9 * (1 - span(p, 1.7, 2))); g.fillRect(screen.x + 10, screen.y + 14 + l * 11, w, 3) }
    }
    casa(g, pt(midX(screen), midY(screen)), 40, span(p, 1.7, 2.3))
    // down from the sky
    const line = new Polyline(curve(pt(sat.x - 60, sat.y + 50), pt(700, 420), pt(620, 360), pt(midX(screen) + 40, screen.y - 8), 30))
    const flow = span(p, 0.7, 1.1) * (1 - span(p, 2.2, 2.6))
    stroke(g, line.points, hex(Palette.thread, 0.25 * flow), 1.4, { dash: [4, 7] })
    for (let i = 0; i < 9; i++) {
      const q = line.at((t * 0.22 + i / 9) % 1)
      ellipse(g, q.x, q.y, 3, 3, hex(i % 3 === 0 ? 0xe2533f : i % 3 === 1 ? 0x5ec25a : 0x5b8bf0, flow))
    }
    // the same picture, as large as it deserves
    casa(g, pt(800, 480), 104, span(p, 2.2, 2.9))
    const lit = span(p, 2.4, 2.9)
    Person.draw(g, pt(345, GROUND), t, { scale: 1.14, reach: pt(68, -100), tilt: -0.04 })
    if (lit > 0.01) {
      glow(g, 392, 400, 34, 34, 0xf6d79a, 0.5 * lit)
      text(g, '!', pt(392, 400), 36, { weight: 800, color: hex(0xf6d79a), opacity: lit })
    }
  },
}

/** 人にできること: AI は仕上がったものを積んでいく。手の中には毬がある。 */
export const Asking = {
  draw(g, p, t) {
    Wall.drawGround(g, 1)
    const ai = pt(780, 400 + Math.sin(t * 1.4) * 8)
    gem(g, ai, 64, t)
    const n = Math.floor(lin(p, 0, 2) * 30)
    for (let i = 0; i < n; i++) sheet(g, pt(640 + (i % 2) * 6, GROUND - 8 - i * 8), (i % 3 - 1) * 0.02)
    const f = pt(250, GROUND)
    const puzzled = span(p, 1.6, 2.4)
    const sh = Person.draw(g, f, t, { scale: 1.2, reach: pt(84, -128), tilt: 0.16 })
    drawTemari(g, pt(f.x + 106, f.y - 150), 24, Math.sin(t * 0.6) * 0.3)
    for (let i = 0; i < 3; i++) {
      const o = span(p, 1.6 + i * 0.25, 1.85 + i * 0.25)
      text(g, '?', pt(f.x + 40 + i * 30, sh - 66 - i * 26 + Math.sin(t * 2 + i * 1.7) * 4), 32 + i * 6, { weight: 800, opacity: o * 0.85 * puzzled })
    }
  },
}

/** 数えきれない試作と、足りないあと一歩。そして、つながる。 */
export const Trials = {
  box: (k) => rect(92 + k * 170, 300, 138, 78),
  draw(g, p, t) {
    Wall.drawGround(g, 0.8)
    const box = Trials.box, grid = 1 - span(p, 1.5, 1.85)
    if (grid > 0.01) {
      faded(g, grid, () => {
        // every earlier try, each stopped at the same place
        const earlier = Math.floor(lin(p, 0.05, 1.4) * 7)
        for (let i = 0; i < earlier; i++) {
          const y = 266 - i * 21
          faded(g, 0.42 - i * 0.045, () => {
            for (let k = 0; k < 5; k++) {
              const r = rect(box(k).x, y, 138, 14)
              if (k < 4) fillRound(g, r, 4, hex(0x6d7177))
              else strokeRound(g, r, 4, hex(0x9aa0a8), 1, [4, 3])
              if (k < 3) stroke(g, [pt(maxX(r), y + 7), pt(box(k + 1).x, y + 7)], hex(wire), 1.6)
            }
          })
        }
        // this try: it gets as far as the others, and the last one stays empty
        const u = (t * 0.2) % 1, built = span(u, 0, 0.55) * 4, leaving = span(u, 0.9, 1)
        faded(g, 1 - leaving, () => {
          for (let k = 0; k < 4; k++) {
            if (built > k) card(g, box(k), '')
            else card(g, box(k), '', { draft: true, dim: 0.5 })
            if (k < 3 && built > k + 1) stroke(g, [pt(maxX(box(k)), 339), pt(box(k + 1).x, 339)], hex(wire), 2.4)
          }
          const reach = span(u, 0.55, 0.68)
          if (reach > 0) stroke(g, [pt(maxX(box(3)), 339), pt(maxX(box(3)) + 16 * reach, 339)], hex(wire), 2.4)
        })
        card(g, box(4), '', { draft: true })
        const pulse = 0.5 + 0.5 * Math.sin(t * 3), gap = span(u, 0.62, 0.72) * (1 - leaving)
        strokeRound(g, rect(box(4).x - 6, 294, 150, 90), 14, hex(0xf0b57a, (0.35 + 0.5 * pulse) * gap), 2.4)
      })
    }
    // the one that holds together
    const whole = span(p, 1.75, 2.05)
    if (whole > 0.01) {
      const box = (k) => rect(92 + k * 170, 300, 138, 78)
      const f = lin(p, 2.0, 2.8)
      const line = new Polyline([pt(40, 339), ...[0, 1, 2, 3, 4].flatMap((k) => [pt(box(k).x, 339), pt(maxX(box(k)), 339)]), pt(960, 339)])
      faded(g, whole, () => {
        for (let k = 0; k < 5; k++) card(g, box(k), '', { draft: f * 5 < k + 0.2, state: f * 5 >= k + 0.9 ? 'done' : 'idle' })
        stroke(g, line.upTo(f), hex(Palette.thread), 2.4)
        for (let k = 0; k < 5; k++) {
          const r = box(k)
          if (f * 5 >= k + 0.2) card(g, r, '', { state: f * 5 >= k + 0.9 ? 'done' : 'idle' })
        }
      })
      if (f > 0 && f < 1) {
        const q = line.at(f)
        drawBird(g, pt(q.x - 22, q.y - 12), 0.13, Motion.flying(t, 0.1))
      } else if (f >= 1) {
        drawBird(g, pt(900, GROUND), 0.2, Motion.idle(t * 0.5))
      }
    }
    const lit = span(p, 2.7, 3)
    const a = pt(110, GROUND)
    Person.draw(g, a, t, { scale: 0.86, tilt: -0.06, puzzled: span(p, 0.7, 1.4) * grid })
    if (lit > 0.01) text(g, '!', pt(a.x + 28, a.y - 206), 32, { weight: 800, color: hex(0xf6d79a), opacity: lit })
  },
}

/** 同志へ: それぞれの手から糸が出て、星をつないでいく。 */
export const Fellows = {
  stars: [pt(170, 250), pt(320, 170), pt(480, 230), pt(640, 150), pt(800, 260)],
  people: [[160, 0xa9c9c3], [330, 0xe2c49a], [500, 0xc9a0b4], [670, 0x9fb070], [840, 0x9db4d8]],
  draw(g, p, t) {
    stars(g, t, 0.75)
    Wall.drawGround(g, 1)
    Fellows.people.forEach(([x, c], i) => {
      const star = Fellows.stars[i]
      const hand = pt(x + 42, GROUND - 190)
      const o = span(p, i * 0.15, 0.3 + i * 0.15)
      const f = lin(p, 0.5 + i * 0.28, 1.1 + i * 0.28)
      faded(g, o, () => {
        const line = new Polyline(curve(hand, pt(hand.x - 10, hand.y - 110), pt(star.x + 8, star.y + 110), star, 24))
        if (f > 0) stroke(g, line.upTo(f), hex(Palette.thread, 0.9), 1.8)
        Person.draw(g, pt(x, GROUND), t + i, { scale: 0.86, tilt: -0.16, coat: [c, c - 0x151515, c - 0x2a2a2a], reach: pt(42, -190), trousers: i % 2 ? [0x3a3f52, 0x2c3040] : [0x24464a, 0x1b3739], hair: [0x2f2722, 0x4a2f22, 0x1c1a1a, 0x6b5a48, 0x2f2722][i] })
        if (f >= 1) { glow(g, star.x, star.y, 26, 26, 0xf6d79a, 0.7); ellipse(g, star.x, star.y, 3.6, 3.6, hex(0xfff3d6)) }
      })
    })
    const joined = lin(p, 2.1, 2.9)
    if (joined > 0) stroke(g, new Polyline(Fellows.stars).upTo(joined), hex(0xf6d79a, 0.8), 1.6)
  },
}

// ---- たとえば、カシオペヤA: the constellation, the star that went, its light reaching the Earth, the remnant still spreading,
// and the photons a telescope takes in, in three bands laid together again. Drawn in a 1920 x 1080 frame, set in the figure. ----

const CAS = pt(1330, 330), EARTH = pt(330, 860), W_STARS = [pt(560, 300), pt(700, 400), pt(850, 290), pt(1000, 410), pt(1130, 280)]
const BANDS = [['0.6–1.65 keV', 0xff4a3a, 0xff8a7a], ['1.65–2.25 keV', 0x52e05a, 0x8ff09a], ['2.25–7.5 keV', 0x4f86ff, 0x8fb4ff]]
let tinted = null
/** The remnant through each band's colour, to be laid together again ('lighter'). */
function tintedCasa() {
  if (tinted || !casaImage.complete || !casaImage.naturalWidth) return tinted
  tinted = BANDS.map(([, c]) => {
    const k = document.createElement('canvas')
    k.width = k.height = 360
    const o = k.getContext('2d')
    o.drawImage(casaImage, 0, 0, 360, 360)
    o.globalCompositeOperation = 'multiply'
    o.fillStyle = hex(c)
    o.fillRect(0, 0, 360, 360)
    return k
  })
  return tinted
}
function disc(g, image, c, r, o, mode = 'source-over') {
  if (o <= 0.01 || !image) return
  g.save()
  g.globalAlpha *= o
  g.globalCompositeOperation = mode
  g.beginPath(); g.arc(c.x, c.y, r, 0, Math.PI * 2); g.clip()
  g.drawImage(image, c.x - r * 1.2, c.y - r * 1.2, r * 2.4, r * 2.4)
  g.restore()
}
function arrowOut(g, c, a, r0, r1, o) {
  if (o <= 0.01) return
  const p0 = pt(c.x + Math.cos(a) * r0, c.y + Math.sin(a) * r0), p1 = pt(c.x + Math.cos(a) * r1, c.y + Math.sin(a) * r1)
  g.save(); g.globalAlpha *= o; g.strokeStyle = hex(0xf6d79a); g.lineWidth = 4; g.lineCap = 'round'
  g.beginPath(); g.moveTo(p0.x, p0.y); g.lineTo(p1.x, p1.y)
  for (const s of [-0.5, 0.5]) { g.moveTo(p1.x, p1.y); g.lineTo(p1.x - Math.cos(a + s) * 16, p1.y - Math.sin(a + s) * 16) }
  g.stroke(); g.restore()
}
/** Where the view is for each `p`: on the constellation, back to take in the Earth, close on the remnant, aside for the telescope. */
const CAS_VIEW = [[0, 1, 960, 470], [1, 1.18, 1060, 420], [1.15, 1.18, 1060, 420], [1.45, 0.92, 860, 560], [2.05, 0.92, 860, 560], [2.5, 2.4, CAS.x, CAS.y + 10], [3.0, 2.4, CAS.x, CAS.y + 10], [3.35, 1.5, CAS.x - 260, CAS.y + 150]]
function casView(p) {
  for (let i = 1; i < CAS_VIEW.length; i++) {
    if (p > CAS_VIEW[i][0]) continue
    const [p0, k0, x0, y0] = CAS_VIEW[i - 1], [p1, k1, x1, y1] = CAS_VIEW[i]
    const u = smooth((p - p0) / (p1 - p0)), k = Math.exp(mix(Math.log(k0), Math.log(k1), u))
    const w = Math.abs(k1 - k0) < 1e-3 ? u : (1 / k0 - 1 / k) / (1 / k0 - 1 / k1)  // what it goes to moves straight across
    return [k, mix(x0, x1, w), mix(y0, y1, w)]
  }
  return CAS_VIEW[CAS_VIEW.length - 1].slice(1)
}

export const CasASky = {
  draw(g, p, t) {
    g.save()
    g.translate(0, 158.75)
    g.scale(1000 / 1920, 1000 / 1920)
    const [k, cx, cy] = casView(p)
    g.translate(960 - cx * k, 540 - cy * k)
    g.scale(k, k)
    const rng = seeded(91)
    for (let i = 0; i < 420; i++) {
      const x = rng(-500, 2400), y = rng(-300, 1400), r = rng(0.5, 1.8) / Math.sqrt(k), ph = rng(0, 6.28)
      ellipse(g, x, y, r, r, hex(0xf3ede2, 0.2 + 0.45 * (0.5 + 0.5 * Math.sin(t * 1.1 + ph))))
    }
    // カシオペヤ座, star to star, and where カシオペヤA is
    const wOut = 1 - span(p, 2.05, 2.4)
    for (let i = 0; i < 4; i++) {
      const u = clamp01(lin(p, 0.05, 0.75) * 4 - i)
      if (u > 0) stroke(g, [W_STARS[i], mixP(W_STARS[i], W_STARS[i + 1], u)], hex(0x9fd3e6, 0.5 * wOut), 2 / k)
    }
    for (const q of W_STARS) { glow(g, q.x, q.y, 16, 16, 0xf3ede2, 0.5 * wOut); ellipse(g, q.x, q.y, 4.5, 4.5, hex(0xf8f2e6, wOut)) }
    text(g, 'カシオペヤ座', pt(845, 480), 40, { weight: 600, color: hex(0x9fd3e6), opacity: 0.85 * span(p, 0.5, 0.8) * wOut })
    const mark = span(p, 0.75, 0.95) * (1 - span(p, 1.0, 1.1))
    if (mark > 0.01) {
      g.save(); g.globalAlpha *= mark; g.strokeStyle = hex(0xf6d79a); g.lineWidth = 2.4 / k; g.setLineDash([6 / k, 6 / k])
      g.beginPath(); g.arc(CAS.x, CAS.y, 32, 0, Math.PI * 2); g.stroke(); g.restore()
      text(g, 'カシオペヤA', pt(CAS.x, CAS.y - 64), 40, { weight: 700, color: hex(0xf6d79a), opacity: mark })
    }
    // the star, and the moment it went
    const star = 1 - span(p, 1.05, 1.12)
    if (star > 0.01) { glow(g, CAS.x, CAS.y, 22, 22, 0xf3ede2, 0.6 * star); ellipse(g, CAS.x, CAS.y, 4, 4, hex(0xffffff, star)) }
    const blast = span(p, 1.02, 1.12) * (1 - span(p, 1.2, 1.55))
    if (blast > 0.01) {
      glow(g, CAS.x, CAS.y, 260 * blast + 40, 260 * blast + 40, 0xfff2d0, 0.95 * blast)
      for (let i = 0; i < 12; i++) {
        const a = (i / 12) * Math.PI * 2, r1 = 60 + 140 * blast
        stroke(g, [pt(CAS.x + Math.cos(a) * 20, CAS.y + Math.sin(a) * 20), pt(CAS.x + Math.cos(a) * r1, CAS.y + Math.sin(a) * r1)], hex(0xfff6e0, 0.6 * blast), 3)
      }
    }
    // its light, on its way here, and here
    const far = Math.hypot(EARTH.x - CAS.x, EARTH.y - CAS.y)
    const go = lin(p, 1.1, 1.75), ring = 1 - span(p, 1.75, 1.9)
    if (go > 0 && ring > 0.01) { g.save(); g.globalAlpha *= 0.75 * ring; g.strokeStyle = hex(0xfff0c8); g.lineWidth = 5; g.beginPath(); g.arc(CAS.x, CAS.y, far * go, 0, Math.PI * 2); g.stroke(); g.restore() }
    const earth = span(p, 1.15, 1.35) * (1 - span(p, 2.05, 2.3))
    if (earth > 0.01) {
      const hit = span(p, 1.72, 1.76) * (1 - span(p, 1.85, 2.1))
      faded(g, earth, () => {
        glow(g, EARTH.x, EARTH.y, 70, 70, 0xfff0c8, 0.8 * hit)
        ellipse(g, EARTH.x, EARTH.y, 34, 34, hex(0x3f7fb8)); ellipse(g, EARTH.x - 9, EARTH.y - 6, 13, 9, hex(0x5fbf6a)); ellipse(g, EARTH.x + 12, EARTH.y + 10, 9, 6, hex(0x5fbf6a))
        text(g, '地球', pt(EARTH.x, EARTH.y + 70), 36, { weight: 600, color: hex(0xd7ecef) })
      })
    }
    // what is left of it, still spreading
    const left = span(p, 1.8, 2.1)
    const r = 84 * left * (1 + 0.12 * smooth(lin(p, 2.3, 3)))
    const split = span(p, 3.4, 3.6) * (1 - span(p, 3.75, 3.95))
    if (left > 0.01) {
      glow(g, CAS.x, CAS.y, r * 1.8, r * 1.8, 0x8f6fd0, 0.3 * left)
      disc(g, casaImage, CAS, r, 1 - split)
    }
    const spread = span(p, 2.4, 2.55) * (1 - span(p, 2.95, 3.1))
    for (let i = 0; i < 8; i++) {
      const d = (t * 0.6 + i * 0.37) % 1
      arrowOut(g, CAS, (i / 8) * Math.PI * 2 + 0.2, r * (1.05 + 0.1 * d), r * (1.3 + 0.1 * d), spread)
    }
    text(g, '秒速 数千 km', pt(CAS.x + r * 1.25, CAS.y - r * 1.15), 26, { weight: 700, color: hex(0xf6d79a), opacity: span(p, 2.55, 2.7) * spread })
    // the observation: photons into the telescope, in three bands, laid together again
    const tel = pt(CAS.x - 520, CAS.y + 330), angle = Math.atan2(CAS.y - tel.y, CAS.x - tel.x)
    const tIn = span(p, 3.0, 3.2)
    if (tIn > 0.01) {
      faded(g, tIn, () => telescope(g, tel, angle, 0.62))
      const mouth = pt(tel.x + Math.cos(angle) * 76, tel.y + Math.sin(angle) * 76)
      const flow = span(p, 3.1, 3.2) * (1 - span(p, 3.6, 3.75))
      for (let i = 0; i < 30; i++) {
        const u = (t * 0.45 + i / 30) % 1
        const q = mixP(CAS, mouth, u), n = Math.sin(i * 12.9) * 18 * (1 - u)
        ellipse(g, q.x + n, q.y + n * 0.6, 3, 3, hex([0xff5a48, 0x5fe065, 0x5b8bf0][i % 3], 0.9 * flow * Math.sin(u * Math.PI)))
      }
    }
    const bands = split > 0.01 ? tintedCasa() : null
    if (bands) {
      const offs = [pt(-1, -0.6), pt(1, -0.6), pt(0, 1)]
      bands.forEach((image, i) => {
        const c = pt(CAS.x + offs[i].x * r * 1.25 * split, CAS.y + offs[i].y * r * 1.25 * split)
        disc(g, image, c, r, Math.min(1, split * 1.4), 'lighter')
        text(g, BANDS[i][0], pt(c.x, c.y + (offs[i].y > 0 ? r + 26 : -r - 20)), 20, { weight: 600, color: hex(BANDS[i][2]), opacity: span(split, 0.7, 1) })
      })
    }
    g.restore()
  },
}

Object.assign(Figures, {
  asking: { draw: Asking.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.8, deck: square },
  orbit: { draw: Orbit.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.8, deck: square },
  sky: { draw: Sky.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 0.8, deck: square },
  reach: { draw: Reach.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 1, deck: square },
  trials: { draw: Trials.draw, from: 0, to: 1.5, ...plain, route: gutter(120, 760), pace: 1.2, deck: square },
  formed: { draw: Trials.draw, from: 1.5, to: 3, ...plain, route: gutter(120, 760), pace: 1.3, birdAway: [0, 1], deck: square },
  fellows: { draw: Fellows.draw, from: 0, to: 3, ...plain, route: gutter(120, 760), pace: 1, deck: square },
  casaSky: { draw: CasASky.draw, from: 0, to: 4, ...plain, route: gutter(120, 760), pace: 1.1, stops: [1, 2.1, 3, 4], deck: square },
})
