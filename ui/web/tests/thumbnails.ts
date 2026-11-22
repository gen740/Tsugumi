// Home's thumbnails and the gallery draw every demo with the Canvas's own
// geometry: no two of a demo's Cards may lie on each other. The Mac app checks
// the same with its geometry (ui/macos/Tests/TsugumiAppTests/DemoThumbnailTests.swift).
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { createElement } from 'react'
import { renderToStaticMarkup } from 'react-dom/server.browser'
import type { CardKind, CardNodeData, FlowDirection } from '../src/model/canvasKit'
import type { DemoData } from '../src/ui/demoPlayer'

const { CardGeometry } = await import('../src/model/canvasKit')
const { Canvas, IDLE } = await import('../src/ui/demoPlayer')
const root = process.argv[2]
const data = path.join(process.env.TSUGUMI_SITE_PLAYER_DIR!, 'player-data')
const ids = JSON.parse(fs.readFileSync(path.join(root, 'demos', 'ja', 'index.json'), 'utf8')).demos.map((d: { id: string }) => d.id) as string[]
assert.ok(ids.length > 0)

const node = (kind: CardKind, inputs = [] as { name: string }[], outputs = [] as { name: string }[]) => ({
  title: 'x', kind, inputs, outputs, params: [], invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false,
}) as unknown as CardNodeData

for (const id of ids) {
  const d = JSON.parse(fs.readFileSync(path.join(data, `${id}.json`), 'utf8'))
  const cards = new Map((d as DemoData).cards.map(c => [c.id, c]))
  for (const w of (d as DemoData).wires) {
    for (const [id, port, direction] of [[w.from, w.fromPort, 'outputs'], [w.to, w.toPort, 'inputs']] as const) {
      const c = cards.get(id)
      assert.ok(c, `${d.id}: missing Card ${id}`)
      assert.ok(c.kind === 'draft' ? port === '' : port !== '' && c[direction].some(p => p.name === port), `${d.id}: ${id}.${port}`)
    }
  }
  const rects = d.cards.map((c: CardNodeData & { id: string; x: number; y: number }) => {
    const g = new CardGeometry({ ...c, title: c.id, invocation: 0n, runContinues: false, errors: [], warnings: [], collapsed: false }, d.flow)
    return { id: c.id, x: c.x, y: c.y, ...g.size }
  })
  for (const [i, a] of rects.entries()) {
    for (const b of rects.slice(i + 1)) {
      const apart = a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y || b.y + b.height <= a.y
      assert.ok(apart, `${id}: ${a.id} (${a.x}, ${a.y}, ${a.width}×${a.height}) lies on ${b.id} (${b.x}, ${b.y}, ${b.width}×${b.height})`)
    }
  }
  if (d.draftOnly) {
    // Both the gallery thumbnail and the site player must draw actual Draft
    // attachments at the Card's side, in either flow direction.
    for (const flow of ['topToBottom', 'leftToRight'] as const) {
      for (const compact of [true, false]) {
        const html = renderToStaticMarkup(createElement(Canvas, {
          data: { ...d, flow }, frame: IDLE, compact, motion: false, onSelect() {},
          view: { width: 800, height: 600, camera: { x: 0, y: 0, zoom: 1 } },
        }))
        const svg = /<svg class="cv-wires"[\s\S]*?<\/svg>/.exec(html)?.[0]
        assert.ok(svg, `${id}: wire layer is drawn`)
        const paths = [...svg.matchAll(/<path d="([^"]*)"/g)].map(m => m[1])
        assert.equal(paths.length, d.wires.length)
        for (const [i, wire] of (d as DemoData).wires.entries()) {
          const points = [...paths[i].matchAll(/(-?[\d.]+),(-?[\d.]+)/g)].map(m => ({ x: Number(m[1]), y: Number(m[2]) }))
          for (const [id, output, point] of [[wire.from, true, points[0]], [wire.to, false, points.at(-1)]] as const) {
            const c = cards.get(id)!
            if (c.kind !== 'draft') continue
            assert.deepEqual(c.inputs, [])
            assert.deepEqual(c.outputs, [])
            const r = new CardGeometry({ ...node('draft'), title: c.id, summary: c.summary, untitled: c.untitled }, flow).outlineRect
            const expected = flow === 'topToBottom'
              ? { x: c.x + r.x + r.width / 2, y: c.y + r.y + (output ? r.height : 0) }
              : { x: c.x + r.x + (output ? r.width : 0), y: c.y + r.y + r.height / 2 }
            assert.deepEqual(point, expected, `${d.id}: ${id}, ${flow}, compact=${compact}, output=${output}`)
          }
        }
      }
    }
  }
}
for (const flow of ['topToBottom', 'leftToRight'] as FlowDirection[]) {
  // The start Card is the small pill of the Mac app's CardGeometry, its output on its downstream edge.
  const start = new CardGeometry(node('trigger', [], [{ name: 'out' }]), flow)
  assert.ok(start.size.width > 0 && start.size.height > 0)
  assert.deepEqual(start.port('out', true), flow === 'topToBottom' ? { x: start.size.width / 2, y: start.size.height } : { x: start.size.width, y: start.size.height / 2 })
}

console.log('thumbnails: passed')
