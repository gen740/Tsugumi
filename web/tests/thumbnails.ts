// Home's thumbnails and the gallery draw every demo with the Canvas's own
// geometry: no two of a demo's Cards may lie on each other. The Mac app checks
// the same with its geometry (macOS/Tests/TsugumiAppTests/DemoThumbnailTests.swift).
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import type { CardKind, CardNodeData, FlowDirection } from '../src/model/canvasKit'

const { CardGeometry } = await import('../src/model/canvasKit')
const root = process.argv[2]
const data = path.join(root, 'docs', 'site', 'player-data')
const ids = JSON.parse(fs.readFileSync(path.join(root, 'demos', 'ja', 'index.json'), 'utf8')).demos.map((d: { id: string }) => d.id) as string[]
assert.ok(ids.length > 0)

const node = (kind: CardKind, inputs = [] as { name: string }[], outputs = [] as { name: string }[]) => ({
  title: 'x', kind, inputs, outputs, params: [], invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false, hasDisplay: false,
}) as unknown as CardNodeData

for (const id of ids) {
  const d = JSON.parse(fs.readFileSync(path.join(data, `${id}.json`), 'utf8'))
  const rects = d.cards.map((c: CardNodeData & { id: string; x: number; y: number }) => {
    const g = new CardGeometry({ ...c, title: c.id, invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false, hasDisplay: !!d.outputs[c.id] }, d.flow)
    return { id: c.id, x: c.x, y: c.y, ...g.size }
  })
  for (const [i, a] of rects.entries()) {
    for (const b of rects.slice(i + 1)) {
      const apart = a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y || b.y + b.height <= a.y
      assert.ok(apart, `${id}: ${a.id} (${a.x}, ${a.y}, ${a.width}×${a.height}) lies on ${b.id} (${b.x}, ${b.y}, ${b.width}×${b.height})`)
    }
  }
}
for (const flow of ['topToBottom', 'leftToRight'] as FlowDirection[]) {
  // The start Card is the small pill of the Mac app's CardGeometry, its output on its downstream edge.
  const start = new CardGeometry(node('trigger', [], [{ name: 'out' }]), flow)
  assert.deepEqual(start.size, { width: 116, height: 34 })
  assert.deepEqual(start.port('out', true), flow === 'topToBottom' ? { x: 58, y: 34 } : { x: 116, y: 17 })
}

console.log('thumbnails: passed')
