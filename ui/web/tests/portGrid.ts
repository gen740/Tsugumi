// Ports on the canvas grid, against the same numbers as the Mac app
// (tests/fixtures/port-grid-contract.json, UILogicTests.swift).
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { CardGeometry, CardNodeData, snapToGrid } from '../src/model/canvasKit'

const contract = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/port-grid-contract.json'), 'utf8'))

for (const c of contract.snap) assert.equal(snapToGrid(c.v, c.grid), c.to, `snap ${c.v} to ${c.grid}`)

// What is drawn: the outline's path data (M, L, Q) as points along it.
function flatten(d: string) {
  const pts: { x: number; y: number }[] = []
  for (const [, op, args] of d.matchAll(/([MLQ])([^MLQZ]+)/g)) {
    const v = args.trim().split(/[ ,]+/).map(Number)
    if (op !== 'Q') { pts.push({ x: v[0], y: v[1] }); continue }
    const a = pts[pts.length - 1]
    for (let t = 0.05; t <= 1; t += 0.05) {
      pts.push({ x: (1 - t) ** 2 * a.x + 2 * t * (1 - t) * v[0] + t ** 2 * v[2], y: (1 - t) ** 2 * a.y + 2 * t * (1 - t) * v[1] + t ** 2 * v[3] })
    }
  }
  return pts
}
function distanceTo(pts: { x: number; y: number }[], p: { x: number; y: number }) {
  return Math.min(...pts.map((a, i) => {
    const b = pts[(i + 1) % pts.length]
    const dx = b.x - a.x, dy = b.y - a.y
    const t = dx || dy ? Math.max(0, Math.min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy))) : 0
    return Math.hypot(a.x + t * dx - p.x, a.y + t * dy - p.y)
  }))
}

const close = (a: number, b: number, what: string) => assert.ok(Math.abs(a - b) < 1e-5, `${what}: ${a} != ${b}`)

for (const c of contract.ports) {
  const name = JSON.stringify({ ...c, in: undefined, out: undefined })
  const ports = (count: number) => Array.from({ length: count }, (_, i) => ({ name: `p${i}`, type: 'x' })) as unknown as CardNodeData['inputs']
  const g = new CardGeometry({
    title: 'x', kind: c.kind, inputs: ports(c.inputs), outputs: ports(c.outputs), params: [], invocation: 0n, runContinues: false, errors: [], warnings: [],
    collapsed: !!c.collapsed, hasDisplay: false, summary: c.summary ? 's' : undefined, portGrid: c.grid ?? undefined,
  }, c.flow)
  close(g.size.width, c.width, `${name} width`)
  close(g.size.height, c.height, `${name} height`)
  const along = (list: number[], output: boolean) => list.map((v, i) => {
    const p = g.port(`p${i}`, output)
    close(c.flow === 'topToBottom' ? p.x : p.y, v, `${name} port ${i}`)
    return v
  })
  const edges = [along(c.in, false), along(c.out, true)]
  if (c.kind === 'fork' || c.kind === 'merge') {
    const r = g.outlineRect
    for (const z of [1, 0.25]) {
      const pts = flatten(g.outline({ x: r.x * z, y: r.y * z, width: r.width * z, height: r.height * z }))
      for (const [i] of c.in.entries()) {
        const p = g.port(`p${i}`, false)
        assert.ok(distanceTo(pts, { x: p.x * z, y: p.y * z }) < z, `${name} input ${i} on the outline at ${z}`)
      }
      for (const [i] of c.out.entries()) {
        const p = g.port(`p${i}`, true)
        assert.ok(distanceTo(pts, { x: p.x * z, y: p.y * z }) < z, `${name} output ${i} on the outline at ${z}`)
      }
      // Rounding takes only a little off the ends of the wide edge.
      const across = pts.map((p) => (c.flow === 'topToBottom' ? p.x : p.y))
      assert.ok(Math.max(...across) - Math.min(...across) >= (c.flow === 'topToBottom' ? r.width : r.height) * z - 8 * z, `${name} keeps its width`)
    }
    // Many ports make it bigger, not flatter.
    const wide = c.flow === 'topToBottom' ? r.width : r.height, long = c.flow === 'topToBottom' ? r.height : r.width
    assert.ok(long >= wide / 4 - 1e-9, `${name} not flat`)
    const icon = g.iconCenter(r)
    close(c.flow === 'topToBottom' ? icon.x : icon.y, wide / 2 + (c.flow === 'topToBottom' ? r.x : r.y), `${name} icon on the axis`)
  }
  if (!c.grid) continue
  const edge = c.flow === 'topToBottom' ? c.width : c.height
  for (const list of edges) {
    for (const [i, v] of list.entries()) {
      // A Card's corner is on a grid line, so this is its place on the canvas too.
      const lines = (v * c.n) / c.grid
      close(lines, Math.round(lines), `${name} on grid/${c.n}`)
      assert.ok(v >= 0 && v <= edge, `${name} on its edge`)
      // A triangle's apex takes everything on that side; elsewhere marks never touch.
      const apex = (c.kind === 'fork' && list === edges[0]) || (c.kind === 'merge' && list === edges[1])
      if (apex) close(v, edge / 2, `${name} at the apex`)
      else if (i > 0 && !c.collapsed || i > 0 && c.flow === 'topToBottom') assert.ok(v - list[i - 1] >= CardGeometry.minPortPitch, `${name} apart`)
    }
  }
}
console.log('port grid behavior: passed')
