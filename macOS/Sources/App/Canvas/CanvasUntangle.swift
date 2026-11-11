import SwiftUI

/// The same bounded search as web/src/model/untangle.ts. Names move between
/// visual slots; the graph's named endpoints and the Cards' places never change.
struct UntangleCard {
  var card: ArrangeCard
  var x: Double
  var y: Double
  var movable: Bool
}

struct PortOrder: Equatable {
  var id: String
  var inputs: [String]
  var outputs: [String]
}

func orderedPorts(_ ports: [PortData], saved: [String]) -> [PortData] {
  var rank: [String: Int] = [:]
  for (i, name) in saved.enumerated() where rank[name] == nil { rank[name] = i }
  return ports.enumerated().sorted {
    (rank[$0.element.name] ?? saved.count + $0.offset) < (rank[$1.element.name] ?? saved.count + $1.offset)
  }.map(\.element)
}

/// Count a pair once. Half-open segments include crossings on sampled points.
func crossingPairs(_ paths: [[CGPoint]], ends: [(a: CGPoint, b: CGPoint)]) -> Int {
  let boxes = paths.map { ps in
    CGRect(x: ps.map(\.x).min() ?? 0, y: ps.map(\.y).min() ?? 0,
           width: (ps.map(\.x).max() ?? 0) - (ps.map(\.x).min() ?? 0),
           height: (ps.map(\.y).max() ?? 0) - (ps.map(\.y).min() ?? 0))
  }
  func hit(_ p: CGPoint, _ q: CGPoint, _ r: CGPoint, _ s: CGPoint) -> Bool {
    let dx = q.x - p.x, dy = q.y - p.y, ex = s.x - r.x, ey = s.y - r.y
    let d = dx * ey - dy * ex
    if abs(d) < 1e-9 { return false }
    let t = ((r.x - p.x) * ey - (r.y - p.y) * ex) / d
    let u = ((r.x - p.x) * dy - (r.y - p.y) * dx) / d
    return t >= -1e-9 && t < 1 - 1e-9 && u >= -1e-9 && u < 1 - 1e-9
  }
  var count = 0
  for i in paths.indices {
    for j in paths.indices where j > i {
      if ends[i].a == ends[j].a || ends[i].b == ends[j].b { continue }
      let a = boxes[i], b = boxes[j]
      if a.maxX < b.minX || b.maxX < a.minX || a.maxY < b.minY || b.maxY < a.minY { continue }
      var crossed = false
      for x in paths[i].indices.dropFirst() where !crossed {
        for y in paths[j].indices.dropFirst() {
          if hit(paths[i][x - 1], paths[i][x], paths[j][y - 1], paths[j][y]) { crossed = true; break }
        }
      }
      if crossed { count += 1 }
    }
  }
  return count
}

func untanglePorts(flow: FlowDirection, cards: [UntangleCard], wires: [ArrangeWire], extra: [CGRect] = []) -> [PortOrder] {
  var current = cards
  func endpoints(_ cs: [UntangleCard]) -> [(a: CGPoint, b: CGPoint)] {
    wires.compactMap { w in
      guard let f = cs.first(where: { $0.card.id == w.from }), let t = cs.first(where: { $0.card.id == w.to }),
            let a = f.card.outputs.first(where: { $0.name == w.fromPort }), let b = t.card.inputs.first(where: { $0.name == w.toPort }) else { return nil }
      return (CGPoint(x: f.x + a.x, y: f.y + a.y), CGPoint(x: t.x + b.x, y: t.y + b.y))
    }
  }
  func score(_ cs: [UntangleCard]) -> Int {
    let ends = endpoints(cs)
    var router = WireRouter(flow: flow, obstacles: cs.map { CGRect(x: $0.x, y: $0.y, width: $0.card.width, height: $0.card.height) } + extra)
    let corners = router.plan(ends)
    let paths = ends.indices.map { corners[$0] ?? router.curvePoints(ends[$0].a, ends[$0].b) }
    return crossingPairs(paths, ends: ends)
  }
  var best = score(current)
  for _ in 0..<4 {
    if best == 0 { break }
    var improved = false
    for output in [true, false] {
      var proposed = current
      var changed = false
      for i in proposed.indices {
        let c = proposed[i]
        if !c.movable { continue }
        var ports = output ? c.card.outputs : c.card.inputs
        let attached: [(name: String, slot: Int, value: Double)] = ports.enumerated().compactMap { slot, p in
          let values: [Double] = wires.compactMap { w in
            if output ? w.from != c.card.id || w.fromPort != p.name : w.to != c.card.id || w.toPort != p.name { return nil }
            guard let other = current.first(where: { $0.card.id == (output ? w.to : w.from) }),
                  let port = (output ? other.card.inputs : other.card.outputs).first(where: { $0.name == (output ? w.toPort : w.fromPort) }) else { return nil }
            return flow == .topToBottom ? other.x + port.x : other.y + port.y
          }
          return values.isEmpty ? nil : (p.name, slot, values.reduce(0, +) / Double(values.count))
        }
        let sorted = attached.sorted { $0.value == $1.value ? $0.slot < $1.slot : $0.value < $1.value }
        for (j, p) in attached.enumerated() {
          if p.name != sorted[j].name { changed = true }
          ports[p.slot].name = sorted[j].name
        }
        if output { proposed[i].card.outputs = ports } else { proposed[i].card.inputs = ports }
      }
      if !changed { continue }
      let count = score(proposed)
      if count < best { current = proposed; best = count; improved = true }
    }
    if !improved { break }
  }
  return current.enumerated().compactMap { i, c in
    let inputs = c.card.inputs.map(\.name), outputs = c.card.outputs.map(\.name)
    return inputs != cards[i].card.inputs.map(\.name) || outputs != cards[i].card.outputs.map(\.name)
      ? PortOrder(id: c.card.id, inputs: inputs, outputs: outputs) : nil
  }
}
