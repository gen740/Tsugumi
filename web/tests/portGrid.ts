// Ports on the canvas grid, against the same numbers as the Mac app
// (tests/fixtures/port-grid-contract.json, UILogicTests.swift).
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { CardGeometry, CardNodeData, snapToGrid } from '../src/model/canvasKit'

const contract = JSON.parse(fs.readFileSync(path.join(process.argv[2], 'tests/fixtures/port-grid-contract.json'), 'utf8'))

for (const c of contract.snap) assert.equal(snapToGrid(c.v, c.grid), c.to, `snap ${c.v} to ${c.grid}`)

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
