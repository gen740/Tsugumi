// Run with TSUGUMI_WIRE_BENCH=1 npm --prefix web test; timing is opt-in.
import assert from 'node:assert/strict'
import { WireRouter, type Pt, type Rect } from '../../web/src/model/canvasKit'
import type { FlowDirection } from '../../web/src/model/prefs'
export function wireFrameBudget(big: { name: string; flow: FlowDirection; obstacles: Rect[]; wires: { a: Pt; b: Pt }[] }, Router: typeof WireRouter) {
  const t0 = performance.now()
  const frames = 200
  for (let f = 0; f < frames; f++) {
    // As while dragging: the first Card moves, so nothing can be kept from the frame before.
    const obstacles = big.obstacles.map((r, i) => (i ? r : { ...r, x: r.x + f }))
    new WireRouter(big.flow, obstacles).routeAll(big.wires)
  }
  const perFrame = (performance.now() - t0) / frames
  console.log(`wire routes: ${big.name}, ${big.wires.length} wires, ${perFrame.toFixed(3)} ms per frame`)
  assert.ok(perFrame < 25, `routing keeps up with a frame: ${perFrame} ms`)
}
