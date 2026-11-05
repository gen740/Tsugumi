// The story pages: one thread runs down the page, out of a temari, through a
// needle hole at each paragraph and through the figures, drawn down to the
// reading line with the tsugumi at its end (ported from the app's
// macOS/Sources/App/Explainer/ExplainerView.swift). Or, in card mode, a
// paragraph at a time beside its figure (ExplainerCards.swift). Scroll back and
// it all goes back.
import {
  Bird, Motion, Palette, Polyline, bezier, clamp01, curve, drawBird, drawTemari, drawThread, easeInOut, ellipse, faded, fillRound, glow, hex, lin,
  mixP, pt, rect, smooth, span, stroke, text,
} from './art.js'
import { Figures, Wall, figurePath, scene } from './scenes.js'

const READING_LINE = 0.5
const still = matchMedia('(prefers-reduced-motion: reduce)').matches
const docEl = document.querySelector('.doc')
const mainEl = document.querySelector('.main')

// ---- The page's blocks ----

function pageBlocks() {
  const blocks = []
  for (const el of docEl.children) {
    if (el.matches('figure.ex-fig')) {
      const spec = Figures[el.dataset.fig]
      const space = el.querySelector('.ex-fig-space')
      space.style.aspectRatio = `${spec.crop.w} / ${spec.crop.h}`
      blocks.push({ el: space, kind: 'figure', spec, caption: el.querySelector('figcaption').textContent, id: el.dataset.fig })
    } else if (el.matches('ul, ol')) {
      blocks.push({ el, kind: 'list', items: [...el.children] })
    } else if (el.matches('h1, h2, h3')) {
      blocks.push({ el, kind: el.tagName.toLowerCase() })
    } else {
      blocks.push({ el, kind: 'other' })
    }
  }
  return blocks
}
const blocks = pageBlocks()

// ---- Layers ----

function makeLayer(cls) {
  const c = document.createElement('canvas')
  c.className = `ex-layer ${cls}`
  c.setAttribute('aria-hidden', 'true')
  document.body.append(c)
  return c
}
const back = makeLayer('back'), front = makeLayer('front')
const off = document.createElement('canvas')
let layout = null

function measure() {
  const main = mainEl.getBoundingClientRect()
  const dpr = Math.min(2, window.devicePixelRatio || 1)
  const width = main.width, height = window.innerHeight
  for (const c of [back, front]) {
    c.style.left = `${main.left}px`
    c.style.width = `${width}px`
    c.style.height = `${height}px`
    c.width = Math.round(width * dpr)
    c.height = Math.round(height * dpr)
  }
  const box = (el) => {
    const r = el.getBoundingClientRect()
    return rect(r.left - main.left, r.top + window.scrollY, r.width, r.height)
  }
  const column = box(docEl)
  const placed = blocks.map((b) => ({ ...b, rect: box(b.el), itemRects: b.items?.map(box), plain: b.items?.map((i) => i.textContent) }))
  layout = { width, height, dpr, column, blocks: placed, thread: makeThread(placed, column), figures: placed.map((b, i) => ({ ...b, i })).filter((b) => b.kind === 'figure') }
}

const mapTo = (f, r) => (q) => { const s = r.w / f.crop.w; return pt(r.x + (q.x - f.crop.x) * s, r.y + (q.y - f.crop.y) * s) }

// ---- The thread: legs one after another, each with the stretch of the reading line it belongs to ----

function makeThread(placed, column) {
  const legs = [], holes = []
  const colMin = column.x
  const gutter = colMin - 22
  const first = placed[0]
  if (!first) return { legs, holes, start: pt(0, 0) }
  const start = pt(gutter + 2, first.rect.y + first.rect.h / 2 - 6)
  let cursor = start
  const gutterLeg = (h) => {
    if (h.y <= cursor.y + 1) { cursor = h; return }
    const a = cursor, d = h
    const k = Math.min(14, (d.y - a.y) * 0.3)
    const pts = curve(a, pt(gutter, a.y + k), pt(gutter, d.y - k), d, 18)
    legs.push({ points: pts, times: pts.map((q) => (q.y - a.y) / (d.y - a.y)), y0: a.y, y1: d.y, figure: null })
    holes.push([h, d.y])
    cursor = h
  }
  placed.forEach((b, i) => {
    if (i === 0) return
    const r = b.rect
    if (b.kind === 'figure') {
      const map = mapTo(b.spec, r)
      const e = map(b.spec.route[0][0])
      if (e.y > cursor.y + 1) {
        const c0 = cursor
        const pts = curve(c0, pt(gutter, c0.y + (e.y - c0.y) * 0.5), pt(e.x, e.y - 30), e, 14)
        legs.push({ points: pts, times: pts.map((q) => (q.y - c0.y) / Math.max(1, e.y - c0.y)), y0: c0.y, y1: e.y, figure: null })
      }
      const path = figurePath(b.spec)
      const pts = path.points.map(map)
      legs.push({ points: pts, times: path.times, y0: r.y, y1: r.y + r.h, figure: i })
      cursor = pts[pts.length - 1] ?? cursor
    } else if (b.kind === 'list') {
      b.itemRects.forEach((ir) => gutterLeg(pt(colMin - 12, ir.y + 13)))
    } else if (b.kind === 'h2' || b.kind === 'h3') {
      gutterLeg(pt(colMin - 12, r.y + 16))
    } else {
      gutterLeg(pt(colMin - 12, r.y + 13))
    }
  })
  return { legs, holes, start }
}

function portions(thread, y) {
  const out = []
  for (const leg of thread.legs) {
    const f = (y - leg.y0) / Math.max(1, leg.y1 - leg.y0)
    if (f <= 0) break
    const pts = [leg.points[0]]
    for (let k = 1; k < leg.points.length; k++) {
      if (leg.times[k] <= f) { pts.push(leg.points[k]); continue }
      const t0 = leg.times[k - 1], t1 = leg.times[k]
      pts.push(mixP(leg.points[k - 1], leg.points[k], (f - t0) / Math.max(0.0001, t1 - t0)))
      break
    }
    out.push({ points: pts, inFigure: leg.figure !== null })
    if (f < 1) break
  }
  return out
}

function readTip(thread, y) {
  const out = [thread.start]
  for (const leg of thread.legs) {
    const f = (y - leg.y0) / Math.max(1, leg.y1 - leg.y0)
    if (f >= 1) { out.push(...leg.points); continue }
    if (f <= 0) break
    let tip = leg.points[0]
    for (let k = 1; k < leg.points.length; k++) {
      if (leg.times[k] <= f) { out.push(leg.points[k]); tip = leg.points[k]; continue }
      const t0 = leg.times[k - 1], t1 = leg.times[k]
      tip = mixP(leg.points[k - 1], leg.points[k], (f - t0) / Math.max(0.0001, t1 - t0))
      out.push(tip)
      break
    }
    const before = out.length > 2 ? out[out.length - 3] : out[0]
    return { tip, heading: pt(tip.x - before.x, tip.y - before.y), leg, f }
  }
  const tip = out[out.length - 1]
  const before = out.length > 2 ? out[out.length - 3] : out[0]
  return { tip, heading: pt(tip.x - before.x, tip.y - before.y), leg: null, f: 1 }
}

// ---- A figure on the page: frameless, its edges fading into the page ----

function paintFigure(g, f, r, p, t, dpr) {
  const w = Math.max(1, Math.round(r.w * dpr)), h = Math.max(1, Math.round(r.h * dpr))
  if (off.width !== w || off.height !== h) { off.width = w; off.height = h }
  const o = off.getContext('2d')
  o.setTransform(1, 0, 0, 1, 0, 0)
  o.globalCompositeOperation = 'source-over'
  o.clearRect(0, 0, w, h)
  const k = (r.w / f.crop.w) * dpr
  o.setTransform(k, 0, 0, k, -f.crop.x * k, -f.crop.y * k)
  f.draw(o, p, t)
  o.setTransform(1, 0, 0, 1, 0, 0)
  o.globalCompositeOperation = 'destination-in'
  const across = o.createLinearGradient(0, 0, w, 0)
  across.addColorStop(0, 'rgba(0,0,0,0)'); across.addColorStop(0.05, '#000'); across.addColorStop(0.95, '#000'); across.addColorStop(1, 'rgba(0,0,0,0)')
  o.fillStyle = across
  o.fillRect(0, 0, w, h)
  const down = o.createLinearGradient(0, 0, 0, h)
  down.addColorStop(0, 'rgba(0,0,0,0)'); down.addColorStop(0.04, '#000'); down.addColorStop(0.9, '#000'); down.addColorStop(1, 'rgba(0,0,0,0)')
  o.fillStyle = down
  o.fillRect(0, 0, w, h)
  glow(g, r.x + r.w / 2, r.y + r.h / 2, r.w * 0.55, r.h * 0.55, Palette.tealTop, 0.4)
  g.drawImage(off, r.x, r.y, r.w, r.h)
}

const triggers = new Map()

function paintBack(g, thread, scrollY, readingY, t) {
  const L = layout
  const visible = [scrollY - 200, scrollY + L.height + 200]
  for (const b of L.figures) {
    const r = b.rect
    b.u = clamp01((readingY - r.y) / r.h)
    b.p = b.spec.from + (b.spec.to - b.spec.from) * b.u
    if (r.y + r.h <= visible[0] || r.y >= visible[1]) continue
    scene.since = -1
    if (b.spec.trigger) {
      if (r.y + (b.spec.trigger.y - b.spec.crop.y) * (r.w / b.spec.crop.w) <= readingY) {
        if (!triggers.has(b.id)) triggers.set(b.id, t)
        scene.since = still ? 10 : t - triggers.get(b.id)
      } else {
        triggers.delete(b.id)
      }
    }
    paintFigure(g, b.spec, r, b.p, t, L.dpr)
  }
  // cues: list items lit while the figure below acts them out
  for (const b of L.figures) {
    for (const cue of b.spec.cues ?? []) {
      const r = itemRect(cue.item)
      if (!r) continue
      const o = span(b.p, cue.from, cue.from + 0.12) * (1 - span(b.p, cue.to - 0.12, cue.to))
      if (o <= 0.01) continue
      const box = rect(r.x - 12, r.y - 7, r.w + 24, r.h + 14)
      fillRound(g, box, 8, hex(Palette.rufous, 0.13 * o))
      fillRound(g, rect(box.x, box.y + 4, 3, box.h - 8), 1.5, hex(Palette.rufous, o))
    }
  }
  // the holes the thread has gone through: barely there
  for (const [h, y] of thread.holes) {
    if (h.y >= visible[0] && h.y <= visible[1] && y <= readingY) ellipse(g, h.x, h.y, 1.6, 1.6, hex(Palette.thread, 0.55))
  }
  // the thread, behind what it passes behind, showing through holes
  g.save()
  const clip = new Path2D()
  clip.rect(-100, scrollY - 400, L.width + 200, L.height + 800)
  for (const b of L.figures) {
    const map = mapTo(b.spec, b.rect), s = b.rect.w / b.spec.crop.w
    for (const o of b.spec.occluders?.(b.p) ?? []) {
      const q = map(pt(o.x, o.y))
      clip.rect(q.x, q.y, o.w * s, o.h * s)
    }
    for (const [c, radius] of b.spec.holes?.(b.p) ?? []) {
      if (radius <= 0.5) continue
      const q = map(c), rr = radius * s
      clip.moveTo(q.x + rr, q.y)
      clip.ellipse(q.x, q.y, rr, rr, 0, 0, Math.PI * 2)
    }
  }
  g.clip(clip, 'evenodd')
  for (const part of portions(thread, readingY)) {
    if (part.inFigure) drawThread(g, part.points, 2.2)
    else stroke(g, part.points, hex(Palette.thread, 0.38), 1.1)
  }
  g.restore()
  // what travels on the thread
  for (const b of L.figures) {
    const beads = b.spec.beads
    if (!beads || b.p < beads.appear) continue
    const leg = thread.legs.find((l) => l.figure === b.i)
    if (!leg) continue
    const k = b.rect.w / b.spec.crop.w
    const local = []
    leg.points.forEach((q, n) => {
      if (leg.times[n] >= beads.from && leg.times[n] <= beads.to) local.push(pt((q.x - b.rect.x) / k + b.spec.crop.x, (q.y - b.rect.y) / k + b.spec.crop.y))
    })
    if (local.length < 2) continue
    g.save()
    g.translate(b.rect.x, b.rect.y)
    g.scale(k, k)
    g.translate(-b.spec.crop.x, -b.spec.crop.y)
    Wall.drawBeads(g, b.p, t, new Polyline(local))
    g.restore()
  }
  drawTemari(g, pt(thread.start.x - 4, thread.start.y + 10), 10, -readingY / 160)
}

function itemRect(textIn) {
  for (const b of layout.blocks) {
    if (b.kind !== 'list') continue
    const n = b.plain.findIndex((s) => s.includes(textIn))
    if (n >= 0) return b.itemRects[n]
  }
  return null
}

function paintFront(g, thread, readingY, t) {
  for (const b of layout.figures) {
    const map = mapTo(b.spec, b.rect)
    for (const fl of b.spec.flights ?? []) {
      const from = itemRect(fl.item)
      if (!from) continue
      const u = lin(b.p, fl.at - 0.32, fl.at - 0.06)
      if (u <= 0 || u >= 1) continue
      const a = pt(from.x + 34, from.y + 12), to = map(fl.to)
      const q = bezier(a, pt(a.x - 120, a.y + 60), pt(to.x - 40, to.y - 160), to, easeInOut(u))
      g.save()
      g.translate(q.x, q.y)
      g.rotate(Math.sin(u * Math.PI) * -0.12)
      g.globalAlpha *= Math.min(1, u * 5) * (1 - smooth((u - 0.9) / 0.1))
      const w = fl.label.length * 21 + 40
      const r = rect(-w / 2, -21, w, 42)
      fillRound(g, rect(r.x, r.y + 4, w, 42), 6, 'rgba(0,0,0,0.3)')
      fillRound(g, r, 6, hex(0x5d4b40))
      text(g, fl.label, pt(0, 0), 19, { weight: 700 })
      g.restore()
    }
  }
  const read = readTip(thread, readingY)
  let hidden = false
  const fig = read.leg?.figure != null ? layout.figures.find((b) => b.i === read.leg.figure) : null
  if (fig) {
    const away = fig.spec.birdAway
    if (away && read.f >= away[0] && read.f <= away[1]) hidden = true
    const s = fig.rect.w / fig.spec.crop.w, map = mapTo(fig.spec, fig.rect)
    for (const o of fig.spec.occluders?.(fig.p) ?? []) {
      const q = map(pt(o.x, o.y))
      if (read.tip.x >= q.x - 6 && read.tip.x <= q.x + o.w * s + 6 && read.tip.y >= q.y && read.tip.y <= q.y + o.h * s) hidden = true
    }
  }
  if (hidden) return
  const inFigure = !!fig
  const motion = still ? 0.3 : t
  const birdPose = inFigure ? Motion.flying(motion, 0.15) : Motion.idle(motion * 0.5)
  if (!inFigure) birdPose.hop = Math.min(birdPose.hop, 3)
  const mirrored = inFigure && read.heading.x < -0.5
  const scale = inFigure ? 0.115 : 0.085
  const gx = (Bird.gape.x - Bird.feet.x) * scale, gy = (Bird.gape.y - Bird.feet.y) * scale
  drawBird(g, pt(read.tip.x - (mirrored ? -gx : gx), read.tip.y - gy), scale, birdPose, mirrored)
}

// ---- Reading mode's frame ----

let deckOpen = false

function frame(ms) {
  requestAnimationFrame(frame)
  if (deckOpen || !layout) { if (deckOpen) drawDeck(ms); return }
  if (document.hidden) return
  const t = still ? 12 : ms / 1000
  const scrollY = window.scrollY
  const readingY = scrollY + layout.height * READING_LINE
  const b = back.getContext('2d'), f = front.getContext('2d')
  for (const g of [b, f]) {
    g.setTransform(layout.dpr, 0, 0, layout.dpr, 0, -scrollY * layout.dpr)
    g.clearRect(-200, scrollY - 10, layout.width + 400, layout.height + 20)
  }
  paintBack(b, layout.thread, scrollY, readingY, t)
  paintFront(f, layout.thread, readingY, t)
}

// ---- Card mode ----

/** The cards of a document: a paragraph, list or table with the headings before it, and the figure it plays. */
function makeCards() {
  const cards = [], section = [], figures = []
  let pending = [], current = 0
  for (const b of blocks) {
    if (b.kind === 'h1' || b.kind === 'h2') {
      if (!pending.length) current++
      pending.push(b.el)
    } else if (b.kind === 'h3') {
      pending.push(b.el)
    } else if (b.kind === 'figure') {
      figures.push({ after: cards.length - 1, section: current, spec: b.spec, caption: b.caption, id: b.id })
    } else {
      cards.push({ els: [...pending, b.el], fig: null, p: 0 })
      section.push(current)
      pending = []
    }
  }
  if (pending.length) { cards.push({ els: pending, fig: null, p: 0 }); section.push(current) }
  cards.forEach((c, i) => {
    const next = figures.find((f) => f.section === section[i] && f.after >= i)
    if (next) {
      const prior = [...figures].reverse().find((f) => f.section === section[i] && f.after < i)
      const start = (prior ? prior.after : section.indexOf(section[i]) - 1) + 1
      const n = next.after - start + 1, k = i - start
      c.fig = next
      c.p = next.spec.from + ((next.spec.to - next.spec.from) * (k + 1)) / Math.max(1, n)
    } else {
      const last = [...figures].reverse().find((f) => f.after < i)
      if (last) { c.fig = last; c.p = last.spec.to }
    }
  })
  return cards
}

const deck = { el: null, cards: [], index: 0, p0: 0, p1: 0, start: 0, duration: 1, canvas: null }
const durationOf = (f, a, b) => (f ? Math.min(5.5, Math.max(0.9, (Math.abs(b - a) / (f.spec.to - f.spec.from)) * 6)) : 0.6)

function openDeck() {
  deck.cards = makeCards()
  deck.index = 0
  const first = deck.cards[0]
  deck.p0 = first?.fig ? first.fig.spec.from : 0
  deck.p1 = first?.p ?? 0
  deck.start = performance.now()
  deck.duration = durationOf(first?.fig, deck.p0, deck.p1)
  const el = document.createElement('section')
  el.className = 'deck'
  el.setAttribute('aria-label', 'カード')
  el.tabIndex = -1
  el.innerHTML = `<div class="deck-close" role="group" aria-label="読み方"><button type="button" data-to="read" aria-pressed="false">読む</button><button type="button" aria-pressed="true">カード</button></div>
    <div class="deck-stage"><div class="deck-figure" hidden><canvas></canvas><p></p></div><div class="deck-text"></div></div>
    <div class="deck-controls"><button type="button" data-go="-1" aria-label="前へ">‹</button><div class="deck-dots"></div><button type="button" data-go="1" aria-label="次へ">›</button></div>`
  mainEl.append(el)
  deck.el = el
  deck.canvas = el.querySelector('canvas')
  el.querySelector('[data-to="read"]').addEventListener('click', () => setMode('read'))
  el.querySelectorAll('[data-go]').forEach((b) => b.addEventListener('click', () => go(Number(b.dataset.go))))
  el.querySelector('.deck-stage').addEventListener('click', (e) => { if (!e.target.closest('a')) go(1) })
  deckOpen = true
  showCard()
  el.focus()
  back.hidden = front.hidden = true
}

function showCard() {
  const card = deck.cards[deck.index]
  const textEl = deck.el.querySelector('.deck-text')
  const copy = textEl.cloneNode(false)
  card?.els.forEach((e) => copy.append(e.cloneNode(true)))
  copy.classList.toggle('wide', !card?.fig)
  textEl.replaceWith(copy)
  const figEl = deck.el.querySelector('.deck-figure')
  figEl.hidden = !card?.fig
  if (card?.fig) { figEl.querySelector('p').textContent = card.fig.caption; sizeDeckCanvas() }
  deck.el.querySelector('.deck-dots').innerHTML = deck.cards.map((_, i) => `<i${i === deck.index ? ' class="on"' : ''}></i>`).join('')
  deck.el.querySelector('[data-go="-1"]').disabled = deck.index === 0
  deck.el.querySelector('[data-go="1"]').disabled = deck.index >= deck.cards.length - 1
}

function sizeDeckCanvas() {
  const card = deck.cards[deck.index]
  if (!card?.fig) return
  const c = card.fig.spec.crop
  const stage = deck.el.querySelector('.deck-stage').getBoundingClientRect()
  const narrow = stage.width < 1100
  const w = Math.max(240, Math.min(narrow ? stage.width - 40 : stage.width - 620, ((narrow ? stage.height * 0.5 : stage.height - 130) * c.w) / c.h))
  const dpr = Math.min(2, window.devicePixelRatio || 1)
  deck.canvas.width = Math.round(w * dpr)
  deck.canvas.height = Math.round(((w * c.h) / c.w) * dpr)
  deck.canvas.style.width = `${w}px`
  deck.canvas.style.height = `${(w * c.h) / c.w}px`
  deck.el.querySelector('.deck-figure').style.width = `${w}px`
}

function go(dir) {
  const j = deck.index + dir
  if (j < 0 || j >= deck.cards.length) return
  const now = performance.now()
  const shown = deck.p0 + (deck.p1 - deck.p0) * easeInOut((now - deck.start) / 1000 / deck.duration)
  const cur = deck.cards[deck.index], next = deck.cards[j]
  const same = cur.fig?.id === next.fig?.id
  const from = same ? shown : dir > 0 ? next.fig?.spec.from ?? 0 : next.p
  deck.p0 = from
  deck.p1 = next.p
  deck.duration = durationOf(next.fig, from, deck.p1)
  deck.start = now
  deck.index = j
  showCard()
}

function drawDeck(ms) {
  const card = deck.cards[deck.index]
  const f = card?.fig
  if (!f || !deck.canvas.width) return
  const dpr = Math.min(2, window.devicePixelRatio || 1)
  const t = still ? 12 : ms / 1000
  const u = still ? 1 : easeInOut((ms - deck.start) / 1000 / deck.duration)
  const p = deck.p0 + (deck.p1 - deck.p0) * u
  const g = deck.canvas.getContext('2d')
  g.setTransform(dpr, 0, 0, dpr, 0, 0)
  const w = deck.canvas.width / dpr, h = deck.canvas.height / dpr
  g.clearRect(0, 0, w, h)
  scene.since = f.spec.triggerAt != null ? (p - f.spec.triggerAt) * 2.5 : -1
  paintFigure(g, f.spec, rect(0, 0, w, h), p, t, dpr)
  const k = w / f.spec.crop.w
  g.save()
  g.scale(k, k)
  g.translate(-f.spec.crop.x, -f.spec.crop.y)
  drawFigureThread(g, f.spec, p, t)
  g.restore()
}

/** The figure's thread in card mode, in the scene's coordinates. */
function drawFigureThread(g, f, p, t) {
  if (f.route.length <= 2) return
  const u = clamp01((p - f.from) / (f.to - f.from))
  const path = figurePath(f)
  const pts = [path.points[0]]
  for (let k = 1; k < path.points.length; k++) {
    if (path.times[k] <= u) { pts.push(path.points[k]); continue }
    const t0 = path.times[k - 1], t1 = path.times[k]
    pts.push(mixP(path.points[k - 1], path.points[k], (u - t0) / Math.max(0.0001, t1 - t0)))
    break
  }
  const tip = pts[pts.length - 1]
  const before = pts.length > 2 ? pts[pts.length - 3] : pts[0]
  const heading = pt(tip.x - before.x, tip.y - before.y)
  g.save()
  const clip = new Path2D()
  clip.rect(-500, -500, 3000, 3000)
  const occluders = f.occluders?.(p) ?? []
  for (const o of occluders) clip.rect(o.x, o.y, o.w, o.h)
  for (const [c, r] of f.holes?.(p) ?? []) {
    if (r <= 0.5) continue
    clip.moveTo(c.x + r, c.y)
    clip.ellipse(c.x, c.y, r, r, 0, 0, Math.PI * 2)
  }
  g.clip(clip, 'evenodd')
  drawThread(g, pts, 2.4)
  g.restore()
  if (f.beads && p >= f.beads.appear) {
    const local = path.points.filter((_, i) => path.times[i] >= f.beads.from && path.times[i] <= f.beads.to)
    if (local.length > 1) Wall.drawBeads(g, p, t, new Polyline(local))
  }
  const hiddenByCard = occluders.some((o) => tip.x >= o.x - 6 && tip.x <= o.x + o.w + 6 && tip.y >= o.y && tip.y <= o.y + o.h)
  const away = f.birdAway && u >= f.birdAway[0] && u <= f.birdAway[1]
  if (u <= 0.001 || u >= 0.97 || away || hiddenByCard) return
  const mirrored = heading.x < -0.5
  const scale = 0.13
  const gx = (Bird.gape.x - Bird.feet.x) * scale, gy = (Bird.gape.y - Bird.feet.y) * scale
  drawBird(g, pt(tip.x - (mirrored ? -gx : gx), tip.y - gy), scale, Motion.flying(still ? 0.3 : t, 0.15), mirrored)
}

// ---- Switching ----

function setMode(mode) {
  document.querySelectorAll('.mode button').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.mode === mode)))
  try { sessionStorage.setItem('tsugumi.mode', mode) } catch { /* the page works without it */ }
  if (mode === 'cards') {
    if (!deckOpen) openDeck()
  } else if (deckOpen) {
    deck.el.remove()
    deckOpen = false
    back.hidden = front.hidden = false
    measure()
  }
}
document.querySelectorAll('.mode button').forEach((b) => b.addEventListener('click', () => setMode(b.dataset.mode)))
document.addEventListener('keydown', (e) => {
  if (!deckOpen || e.metaKey || e.ctrlKey || e.altKey) return
  if (['ArrowRight', 'ArrowDown', ' '].includes(e.key)) { e.preventDefault(); go(1) }
  if (['ArrowLeft', 'ArrowUp'].includes(e.key)) { e.preventDefault(); go(-1) }
  if (e.key === 'Escape') setMode('read')
})

// ---- Start ----

measure()
new ResizeObserver(() => { if (deckOpen) sizeDeckCanvas(); else measure() }).observe(docEl)
addEventListener('resize', () => { if (deckOpen) sizeDeckCanvas(); else measure() })
addEventListener('load', () => measure())
document.fonts?.load('20px "Yusei Magic Subset"').then(() => measure()).catch(() => {})
document.fonts?.ready.then(() => measure())
let saved = null
try { saved = sessionStorage.getItem('tsugumi.mode') } catch { /* the page works without it */ }
if (saved === 'cards') setMode('cards')
requestAnimationFrame(frame)
