// Reorder named ports without changing a connection or moving a Card. Each Card's
// proposal follows the opposite ends of its wires and is kept, Card by Card, only
// when the actual router then draws fewer crossing pairs: one Card that would get
// worse does not hold back another. Arrange runs the pass after placing Cards, and
// the end of a drag runs it for the Cards around what moved.
import { Pt, Rect, WireEnds, WireRouter } from './canvasKit'
import { ArrangeCard, ArrangeWire } from './arrange'
import { FlowDirection } from './prefs'

export interface UntangleCard extends ArrangeCard { x: number; y: number; movable: boolean }
export interface PortOrder { id: string; inputs: string[]; outputs: string[] }

/** Ignore vanished names, retain every new port, and never duplicate a port. */
export function orderedPorts<T extends { name: string }>(ports: T[], saved: string[]): T[] {
  const rank = new Map([...new Set(saved)].map((name, i) => [name, i]))
  return ports.map((port, i) => ({ port, i })).sort((a, b) =>
    (rank.get(a.port.name) ?? saved.length + a.i) - (rank.get(b.port.name) ?? saved.length + b.i)).map(p => p.port)
}

/** A crossing pair counts once, including a crossing exactly on a sampled point. */
export function crossingPairs(paths: Pt[][], ends: WireEnds[]): number {
  const boxes = paths.map(ps => ({ x: Math.min(...ps.map(p => p.x)), y: Math.min(...ps.map(p => p.y)),
    right: Math.max(...ps.map(p => p.x)), bottom: Math.max(...ps.map(p => p.y)) }))
  const equal = (a: Pt, b: Pt) => a.x === b.x && a.y === b.y
  const hit = (p: Pt, q: Pt, r: Pt, s: Pt) => {
    const dx = q.x - p.x, dy = q.y - p.y, ex = s.x - r.x, ey = s.y - r.y
    const d = dx * ey - dy * ex
    if (Math.abs(d) < 1e-9) return false
    const t = ((r.x - p.x) * ey - (r.y - p.y) * ex) / d
    const u = ((r.x - p.x) * dy - (r.y - p.y) * dx) / d
    return t >= -1e-9 && t < 1 - 1e-9 && u >= -1e-9 && u < 1 - 1e-9
  }
  let count = 0
  for (let i = 0; i < paths.length; i++) for (let j = i + 1; j < paths.length; j++) {
    if (equal(ends[i].a, ends[j].a) || equal(ends[i].b, ends[j].b)) continue
    const a = boxes[i], b = boxes[j]
    if (a.right < b.x || b.right < a.x || a.bottom < b.y || b.bottom < a.y) continue
    let crossed = false
    for (let x = 1; x < paths[i].length && !crossed; x++) for (let y = 1; y < paths[j].length; y++) {
      if (hit(paths[i][x - 1], paths[i][x], paths[j][y - 1], paths[j][y])) { crossed = true; break }
    }
    if (crossed) count++
  }
  return count
}

/** Every trial routes all the wires given, so a call makes at most this many. */
export const UNTANGLE_TRIALS = 96

export function untanglePorts(flow: FlowDirection, cards: UntangleCard[], wires: ArrangeWire[], extra: Rect[] = []): PortOrder[] {
  let current = cards.map(c => ({ ...c, inputs: c.inputs.map(p => ({ ...p })), outputs: c.outputs.map(p => ({ ...p })) }))
  const endpoints = (cs: UntangleCard[]) => wires.flatMap(w => {
    const f = cs.find(c => c.id === w.from), t = cs.find(c => c.id === w.to)
    const a = f?.outputs.find(p => p.name === w.fromPort), b = t?.inputs.find(p => p.name === w.toPort)
    return f && t && a && b ? [{ a: { x: f.x + a.x, y: f.y + a.y }, b: { x: t.x + b.x, y: t.y + b.y } }] : []
  })
  const score = (cs: UntangleCard[]) => {
    const ends = endpoints(cs)
    const routes = new WireRouter(flow, [...cs, ...extra]).routeAll(ends)
    return crossingPairs(routes.map(r => r.pts), ends)
  }
  let best = score(current), trials = 0
  search: for (let pass = 0; pass < 4 && best > 0; pass++) {
    let improved = false
    for (const output of [true, false]) for (let i = 0; i < current.length; i++) {
      const c = current[i]
      if (!c.movable) continue
      const ports = output ? c.outputs : c.inputs
      const attached = ports.map((p, slot) => {
        const values = wires.flatMap(w => {
          if ((output ? w.from !== c.id || w.fromPort !== p.name : w.to !== c.id || w.toPort !== p.name)) return []
          const other = current.find(n => n.id === (output ? w.to : w.from))
          const port = (output ? other?.inputs : other?.outputs)?.find(n => n.name === (output ? w.toPort : w.fromPort))
          return other && port ? [flow === 'topToBottom' ? other.x + port.x : other.y + port.y] : []
        })
        return { name: p.name, slot, value: values.length ? values.reduce((a, b) => a + b, 0) / values.length : undefined }
      }).filter(p => p.value !== undefined)
      const sorted = [...attached].sort((a, b) => a.value! - b.value! || a.slot - b.slot)
      if (attached.every((p, j) => p.name === sorted[j].name)) continue
      if (trials++ >= UNTANGLE_TRIALS) break search
      const moved = ports.map(p => ({ ...p }))
      attached.forEach((p, j) => { moved[p.slot].name = sorted[j].name })
      const proposed = current.map((n, j) => j !== i ? n : output ? { ...n, outputs: moved } : { ...n, inputs: moved })
      const count = score(proposed)
      if (count >= best) continue
      current = proposed; best = count; improved = true
      if (best === 0) break search
    }
    if (!improved) break
  }
  return current.flatMap((c, i) => {
    const inputs = c.inputs.map(p => p.name), outputs = c.outputs.map(p => p.name)
    return inputs.some((p, j) => p !== cards[i].inputs[j].name) || outputs.some((p, j) => p !== cards[i].outputs[j].name)
      ? [{ id: c.id, inputs, outputs }] : []
  })
}
