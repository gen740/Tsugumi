// Home: one tsugumi on its branch, the temari, and one thread that goes on from
// the bill through the ways into the documents, as wires between Cards. The
// thread pays out as the page opens; the way in the pointer is on lights up along
// it, from the bill. The demos of the gallery play in place.
import { Motion, Palette, birdGape, curve, drawBird, drawBranch, drawTemari, ellipse, glow, hex, mix, pose, pt, smooth, temariLead } from './art.js'

const still = matchMedia('(prefers-reduced-motion: reduce)').matches
const mainEl = document.querySelector('.main')
const perch = document.querySelector('.home-perch')
const doors = [...document.querySelectorAll('.door')]

const canvas = document.createElement('canvas')
canvas.className = 'home-art'
canvas.setAttribute('aria-hidden', 'true')
document.body.append(canvas)

let L = null

function measure() {
  const main = mainEl.getBoundingClientRect()
  const dpr = Math.min(2, window.devicePixelRatio || 1)
  canvas.style.left = `${main.left}px`
  canvas.style.width = `${main.width}px`
  canvas.style.height = `${window.innerHeight}px`
  canvas.width = Math.round(main.width * dpr)
  canvas.height = Math.round(window.innerHeight * dpr)
  const box = (el) => {
    const r = el.getBoundingClientRect()
    return { x: r.left - main.left, y: r.top + window.scrollY, w: r.width, h: r.height }
  }
  const P = box(perch)
  const scale = Math.min(P.w / 1000, P.h / 640) * 1.02
  const feet = pt(P.x + P.w * 0.5, P.y + P.h * 0.62)
  const thisPose = pose()
  const gape = birdGape(feet, scale, thisPose)
  const temari = { c: pt(feet.x - scale * 270, P.y + P.h * 0.92), r: Math.max(20, scale * 62) }
  // from the temari up into the bill, and on through each way in
  const points = []
  const up = temariLead(temari.c, temari.r)
  points.push(...curve(up, pt(up.x + 8, up.y - 120), pt(gape.x - 30, gape.y + 100), gape, 26))
  const rects = doors.map(box)
  const inPort = (r) => pt(r.x, r.y + 32), outPort = (r) => pt(r.x + r.w, r.y + 32)
  const stops = [points.length]
  let from = gape, leftFrom = true
  rects.forEach((r) => {
    const to = inPort(r)
    if (to.y > from.y + 40) {
      // down the side and along the gap above this row, so that it never crosses the words
      const lane = r.y - 22
      const side = pt(leftFrom ? from.x + 26 : from.x + 26, lane)
      const near = pt(to.x - 28, lane)
      points.push(...curve(from, pt(from.x + 34, from.y + 30), pt(side.x + 10, lane - 60), side, 18).slice(1))
      points.push(...curve(side, pt(side.x - 40, lane), pt(near.x + 40, lane), near, 8).slice(1))
      points.push(...curve(near, pt(near.x - 24, lane), pt(to.x - 30, to.y), to, 12).slice(1))
    } else {
      const reach = Math.max(26, Math.abs(to.x - from.x) * 0.5)
      points.push(...curve(from, pt(from.x + reach, from.y), pt(to.x - reach, to.y), to, 16).slice(1))
    }
    stops.push(points.length)
    from = outPort(r)
    leftFrom = false
    points.push(from)
  })
  const lengths = [0]
  for (let i = 1; i < points.length; i++) lengths.push(lengths[i - 1] + Math.hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y))
  L = { width: main.width, height: window.innerHeight, dpr, feet, scale, gape, temari, points, lengths, stops, doorsAt: rects.map((_, i) => lengths[stops[i] - 1]) }
}

/** Where a distance along the thread is. */
function along(d) {
  const { points, lengths } = L
  let i = 1
  while (i < lengths.length - 1 && lengths[i] < d) i++
  const seg = Math.max(0.0001, lengths[i] - lengths[i - 1])
  const u = Math.min(1, Math.max(0, (d - lengths[i - 1]) / seg))
  return { at: pt(mix(points[i - 1].x, points[i].x, u), mix(points[i - 1].y, points[i].y, u)), dx: points[i].x - points[i - 1].x }
}

let target = null, lit = null, lastMs = 0, startMs = null
doors.forEach((d, i) => {
  const on = () => { target = i }
  const off = () => { if (target === i) target = null }
  d.addEventListener('pointerenter', on)
  d.addEventListener('focusin', on)
  d.addEventListener('pointerleave', off)
  d.addEventListener('focusout', off)
})

function frame(ms) {
  requestAnimationFrame(frame)
  if (!L || document.hidden) return
  if (startMs === null) startMs = ms
  const dt = Math.min(0.5, (ms - lastMs) / 1000)
  lastMs = ms
  const t = still ? 12 : ms / 1000
  const g = canvas.getContext('2d')
  const scrollY = window.scrollY
  g.setTransform(L.dpr, 0, 0, L.dpr, 0, -scrollY * L.dpr)
  g.clearRect(-20, scrollY - 10, L.width + 40, L.height + 20)
  const total = L.lengths[L.lengths.length - 1]
  const pay = still ? 1 : smooth((ms - startMs) / 1000 / 3.4)
  const shown = total * pay

  // the thread
  g.save()
  g.beginPath()
  let lastPoint = L.points[0]
  g.moveTo(lastPoint.x, lastPoint.y)
  for (let i = 1; i < L.points.length; i++) {
    if (L.lengths[i] > shown) { const q = along(shown).at; g.lineTo(q.x, q.y); break }
    g.lineTo(L.points[i].x, L.points[i].y)
  }
  g.lineCap = 'round'
  g.lineJoin = 'round'
  g.strokeStyle = 'rgba(0,0,0,0.25)'
  g.lineWidth = 2.4
  g.save(); g.translate(1.5, 2.5); g.stroke(); g.restore()
  g.strokeStyle = hex(Palette.thread, 0.92)
  g.stroke()
  g.restore()

  // the temari and the tsugumi, large
  glow(g, L.feet.x, L.feet.y - L.scale * 180, L.scale * 520, L.scale * 420, Palette.tealTop ?? 0x1d3a3e, 0.55)
  drawBranch(g, L.feet.x - L.scale * 380, L.feet.x + L.scale * 300, L.feet.y + L.scale * 8, t)
  drawTemari(g, L.temari.c, L.temari.r, -pay * 7 - t * 0.02)
  drawBird(g, L.feet, L.scale, pose({ blink: Motion.blink(t, 3.4), tilt: 0.02 * Math.sin(t * 0.9) }))

  // the way in under the pointer: the thread lights up to it, from the bill
  const from = L.lengths[L.stops[0] - 1]
  const want = target === null ? from : L.doorsAt[target]
  lit = lit === null || still ? want : lit + (want - lit) * (1 - Math.exp(-dt / 0.28))
  if (pay > 0.97 && lit > from + 2) {
    g.beginPath()
    let first = true
    for (let i = L.stops[0] - 1; i < L.points.length; i++) {
      const q = L.lengths[i] > lit ? along(lit).at : L.points[i]
      if (first) g.moveTo(q.x, q.y); else g.lineTo(q.x, q.y)
      first = false
      if (L.lengths[i] > lit) break
    }
    g.lineCap = 'round'
    g.lineJoin = 'round'
    g.strokeStyle = hex(0xf0b57a, 0.95)
    g.lineWidth = 3
    g.stroke()
    const end = along(lit).at
    ellipse(g, end.x, end.y, 4.5, 4.5, hex(0xf6ead2))
  }
}

measure()
new ResizeObserver(measure).observe(document.querySelector('.home'))
new ResizeObserver(measure).observe(mainEl)
addEventListener('resize', measure)
addEventListener('load', measure)
document.fonts?.ready.then(measure)
requestAnimationFrame(frame)

// ---- The gallery: each demo plays in its tile, and opens large (also from a link to #demo-<id>) ----

async function gallery() {
  const tiles = [...document.querySelectorAll('.demo-tile')]
  let player = null
  try {
    player = await import('./player.js')
  } catch {
    return  // the tiles still read as a list
  }
  const dataUrl = (id) => new URL(`../player-data/${id}.json`, import.meta.url).href
  const seen = new IntersectionObserver((entries) => {
    for (const e of entries) {
      if (!e.isIntersecting) continue
      seen.unobserve(e.target)
      const id = e.target.dataset.demo
      player.mountPlayer(e.target.querySelector('.demo-thumb'), { demo: id, data: dataUrl(id), compact: true, autoplay: !still })
    }
  }, { rootMargin: '120px' })
  const open = (id) => {
    if (document.querySelector('dialog.player-dialog')) return
    const dialog = document.createElement('dialog')
    dialog.className = 'player-dialog'
    dialog.innerHTML = '<form method="dialog"><button class="close">Close</button></form><div class="player-host"></div>'
    document.body.append(dialog)
    const mounted = player.mountPlayer(dialog.querySelector('.player-host'), { demo: id, data: dataUrl(id), autoplay: !still })
    dialog.addEventListener('close', () => {
      mounted?.destroy?.()
      dialog.remove()
      // Closed: the address no longer names the demo, so the same link opens it again.
      if (location.hash === `#demo-${id}`) history.replaceState(null, '', `${location.pathname}${location.search}#demos`)
    })
    dialog.showModal()
  }
  tiles.forEach((tile) => {
    seen.observe(tile)
    tile.addEventListener('click', () => open(tile.dataset.demo))
  })
  // A link to #demo-<id> (the README's tiles) opens that demo; without the player it still scrolls to the tile.
  const linked = () => {
    const id = location.hash.match(/^#demo-(.+)$/)?.[1]
    if (id && tiles.some((t) => t.dataset.demo === id)) open(id)
  }
  addEventListener('hashchange', linked)
  linked()
}
gallery()
