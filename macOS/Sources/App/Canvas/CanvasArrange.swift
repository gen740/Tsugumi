import Foundation

// The Arrange button: a layered drawing of a scope's graph, the way a person lays
// one out by hand. Wires that can be straight are, what comes after a fork
// stays in its own column, gaps follow the real size of each Card, and output
// frames and notes go in free room near what they belong to.
//
// Everything here is plain arithmetic on the numbers in `ArrangeInput`, in the
// same order as `web/src/model/arrange.ts`, so the two give the same result
// (tests/fixtures/arrange-contract.json checks it). Nothing iterates a
// dictionary or a set, and sorts are total orders.

struct ArrangePort {
  var name: String
  var x: Double
  var y: Double
}

struct ArrangeCard {
  var id: String
  var width: Double
  var height: Double
  /// Where each port is on the Card (from `CardGeometry`).
  var inputs: [ArrangePort]
  var outputs: [ArrangePort]
}

struct ArrangeWire {
  var from: String
  var fromPort: String
  var to: String
  var toPort: String
}

/// An output frame (`id` is its Card's) or a note.
struct ArrangeBox {
  var id: String
  var width: Double
  var height: Double
}

struct ArrangeBarEnd {
  var card: String
  var port: String
}

struct ArrangeInput {
  var flow: FlowDirection
  /// Where frames, notes and layers go.
  var grid: Double
  /// The grid is on: Cards are drawn on it (`placedPosition`), so they are placed on it.
  var snap: Bool
  var cards: [ArrangeCard]
  var wires: [ArrangeWire]
  var frames: [ArrangeBox]
  var notes: [ArrangeBox]
  /// Inside a Card in Card: the Cards its inputs go to and its outputs come from, by bar.
  var boundaryInputs: [ArrangeBarEnd] = []
  var boundaryOutputs: [ArrangeBarEnd] = []
}

struct ArrangeAt: Equatable {
  var id: String
  var x: Double
  var y: Double
}

struct ArrangeResult: Equatable {
  var cards: [ArrangeAt]
  var frames: [ArrangeAt]
  var notes: [ArrangeAt]
}

private enum Arrange {
  /// Space between Cards side by side, and more than this between layers.
  static let sideGap = 40.0
  static let layerGap = 30.0
  /// Room a wire passing a layer keeps from the Cards and wires beside it.
  static let laneGap = 30.0
  /// Room around frames and notes, and around wires they sit next to.
  static let boxGap = 20.0
  static let wireRoom = 12.0
  /// What a frame on the other side of its Card costs (it may be free and near).
  static let frameOtherSide = 400.0
  /// How much a straight wire counts: a long wire bending is the worst to read.
  static let weightFeedback = 1
  static let weightNear = 4
  static let weightLongEnd = 8
  static let weightLong = 32
  /// Sweeps of the ordering, and the longest search of the coordinates.
  static let orderSweeps = 24
  static let simplexLimit = 4000
  /// A row of unconnected graphs is at least this wide before it wraps.
  static let shelfMin = 1200.0

  static func roundNear(_ v: Double) -> Double { (v + 0.5).rounded(.down) }
  static func ceilTo(_ v: Double, _ g: Double) -> Double { (v / g - 1e-9).rounded(.up) * g }
  static func floorTo(_ v: Double, _ g: Double) -> Double { (v / g + 1e-9).rounded(.down) * g }
  static func nearTo(_ v: Double, _ g: Double) -> Double { roundNear(v / g) * g }
  static func tenth(_ v: Double) -> Double { roundNear(v * 10) / 10 + 0 }

  /// A Card, a bar of a Card in Card, or a point of a wire passing a layer.
  final class Node {
    let card: Int // index in the input, -1 otherwise
    let bar: Bool
    let across: Double // size across the flow
    let along: Double // size along it
    let frameAcross: Double // the output frame that goes with it (0: none)
    let frameAlong: Double
    let inputs: [Double] // where its ports are across the flow
    let outputs: [Double]
    var rank = 0
    var order = 0
    var at = 0.0 // position across the flow
    var layerAt = 0.0 // position along it

    init(card: Int = -1, bar: Bool = false, across: Double = 0, along: Double = 0, frameAcross: Double = 0, frameAlong: Double = 0,
         inputs: [Double] = [], outputs: [Double] = [], rank: Int = 0) {
      self.card = card
      self.bar = bar
      self.across = across
      self.along = along
      self.frameAcross = frameAcross
      self.frameAlong = frameAlong
      self.inputs = inputs
      self.outputs = outputs
      self.rank = rank
    }
  }

  /// A wire in flow direction. `feedback`: it runs against the flow.
  final class Edge {
    let from: Int
    let fromSlot: Int
    let to: Int
    let toSlot: Int
    var feedback = false

    init(from: Int, fromSlot: Int, to: Int, toSlot: Int) {
      self.from = from
      self.fromSlot = fromSlot
      self.to = to
      self.toSlot = toSlot
    }
  }

  /// One piece of a wire, between neighbouring layers.
  struct Piece {
    var from: Int
    var fromAt: Double
    var to: Int
    var toAt: Double
    var weight: Int
  }

  struct Rect {
    var across: Double
    var along: Double
    var acrossSize: Double
    var alongSize: Double
  }

  /// From (a, b) to (c, d): across, along.
  struct Line {
    var a: Double
    var b: Double
    var c: Double
    var d: Double
  }

  static func unionRect(_ r: Rect, _ o: Rect) -> Rect {
    let lo = min(r.across, o.across), top = min(r.along, o.along)
    return Rect(across: lo, along: top, acrossSize: max(r.across + r.acrossSize, o.across + o.acrossSize) - lo,
                alongSize: max(r.along + r.alongSize, o.along + o.alongSize) - top)
  }

  static func hits(_ r: Rect, _ o: Rect, _ gap: Double) -> Bool {
    r.across < o.across + o.acrossSize + gap && o.across < r.across + r.acrossSize + gap &&
      r.along < o.along + o.alongSize + gap && o.along < r.along + r.alongSize + gap
  }

  /// Whether a segment comes within `room` of a rectangle.
  static func lineHits(_ l: Line, _ r: Rect, _ room: Double) -> Bool {
    let lo = r.across - room, hi = r.across + r.acrossSize + room, top = r.along - room, bottom = r.along + r.alongSize + room
    // Clipped to the rectangle along its length (Liang-Barsky).
    var t0 = 0.0, t1 = 1.0
    let dx = l.c - l.a, dy = l.d - l.b
    func clip(_ p: Double, _ q: Double) -> Bool {
      if p == 0 { return q >= 0 }
      let t = q / p
      if p < 0 {
        if t > t1 { return false }
        if t > t0 { t0 = t }
      } else {
        if t < t0 { return false }
        if t < t1 { t1 = t }
      }
      return true
    }
    return clip(-dx, l.a - lo) && clip(dx, hi - l.a) && clip(-dy, l.b - top) && clip(dy, bottom - l.b)
  }

  /// The free place for a box nearest `near` that grows the drawing the least,
  /// on the side frames open to (right of a Card when the flow runs down).
  static func placeBox(acrossSize: Double, alongSize: Double, near: Rect, frame: Bool, obstacles: [Rect], lines: [Line], grid: Double) -> Rect {
    var acrossAt = [ceilTo(near.across + near.acrossSize + boxGap, grid), floorTo(near.across - acrossSize - boxGap, grid)]
    var alongAt = [nearTo(near.along, grid), nearTo(near.along + near.alongSize - alongSize, grid)]
    for o in obstacles {
      acrossAt.append(ceilTo(o.across + o.acrossSize + boxGap, grid))
      acrossAt.append(floorTo(o.across - acrossSize - boxGap, grid))
      alongAt.append(nearTo(o.along, grid))
      alongAt.append(nearTo(o.along + o.alongSize - alongSize, grid))
      alongAt.append(ceilTo(o.along + o.alongSize + boxGap, grid))
      alongAt.append(floorTo(o.along - alongSize - boxGap, grid))
    }
    for l in lines {
      acrossAt.append(ceilTo(max(l.a, l.c) + wireRoom, grid))
      acrossAt.append(floorTo(min(l.a, l.c) - acrossSize - wireRoom, grid))
    }
    func sortedUnique(_ list: [Double]) -> [Double] {
      var out: [Double] = []
      for v in list.sorted() where out.last != v { out.append(v) }
      return out
    }
    let across = sortedUnique(acrossAt), along = sortedUnique(alongAt)
    var box: Rect?
    for o in obstacles { box = box.map { unionRect($0, o) } ?? o }
    var best: Rect?
    var bestCost = Double.infinity
    for a in along {
      for c in across {
        let r = Rect(across: c, along: a, acrossSize: acrossSize, alongSize: alongSize)
        if obstacles.contains(where: { hits(r, $0, boxGap - 1e-6) }) { continue }
        if lines.contains(where: { lineHits($0, r, wireRoom) }) { continue }
        let gapAcross = max(0, near.across - (c + acrossSize), c - (near.across + near.acrossSize))
        let gapAlong = max(0, near.along - (a + alongSize), a - (near.along + near.alongSize))
        var growth = 0.0
        if let box {
          let u = unionRect(box, r)
          growth = u.acrossSize - box.acrossSize + (u.alongSize - box.alongSize)
        }
        // A frame opens to one side of its Card and stays level with it if it can;
        // a note has no Card and goes towards where the flow ends.
        let place = frame
          ? (c + acrossSize / 2 < near.across + near.acrossSize / 2 ? frameOtherSide : 0) + 0.1 * abs(a - near.along)
          : -0.01 * a + 0.001 * c
        let cost = 2 * (gapAcross + gapAlong) + growth + place
        if cost < bestCost - 1e-9 {
          bestCost = cost
          best = r
        }
      }
    }
    // (Past everything there is always room.)
    return best ?? Rect(across: across[across.count - 1], along: along[0], acrossSize: acrossSize, alongSize: alongSize)
  }

  /// One connected graph: layers, the order in them, and the coordinates.
  final class Layered {
    private let n: NodeList
    private let edges: [Edge]
    private(set) var list: [Int]
    private let grid: Double
    private let snap: Bool
    private var layers: [[Int]] = []
    private var pieces: [Piece] = []
    /// For each wire, the nodes it passes (its two ends and the points between).
    private var chains: [(edge: Edge, path: [Int])] = []

    init(nodes: NodeList, edges: [Edge], list: [Int], grid: Double, snap: Bool) {
      n = nodes
      self.edges = edges
      self.list = list
      self.grid = grid
      self.snap = snap
    }

    /// Lays the graph out, and says where its wires run.
    func run() -> [Line] {
      removeCycles()
      assignRanks()
      addLanes()
      orderLayers()
      assignAcross()
      assignAlong()
      return wireLines()
    }

    /// Wires against the flow: those that close a cycle, seen from the sources.
    private func removeCycles() {
      var state = [Int](repeating: 0, count: n.items.count)
      var hasIn = [Bool](repeating: false, count: n.items.count)
      for e in edges { hasIn[e.to] = true }
      func visit(_ root: Int) {
        var stack: [(node: Int, next: Int)] = [(root, 0)]
        state[root] = 1
        while let top = stack.last {
          var next = top.next
          var pushed = false
          while next < edges.count {
            let e = edges[next]
            next += 1
            if e.from != top.node || e.feedback { continue }
            let s = state[e.to]
            if s == 1 {
              e.feedback = true
            } else if s == 0 {
              state[e.to] = 1
              stack[stack.count - 1].next = next
              stack.append((e.to, 0))
              pushed = true
              break
            }
          }
          if !pushed {
            state[top.node] = 2
            stack.removeLast()
          }
        }
      }
      for i in list where !hasIn[i] && state[i] == 0 { visit(i) }
      for i in list where state[i] == 0 { visit(i) }
    }

    /// The layer of each node: after everything before it, and sources (or
    /// nodes that give to more than they take) as late as their wires allow.
    private func assignRanks() {
      let nodes = n.items
      func before(_ e: Edge) -> Int { e.feedback ? e.to : e.from }
      func after(_ e: Edge) -> Int { e.feedback ? e.from : e.to }
      for i in list { nodes[i].rank = 0 }
      var pass = 0
      while pass <= list.count {
        var changed = false
        for e in edges where nodes[after(e)].rank < nodes[before(e)].rank + 1 {
          nodes[after(e)].rank = nodes[before(e)].rank + 1
          changed = true
        }
        if !changed { break }
        pass += 1
      }
      pass = 0
      while pass < list.count {
        var changed = false
        for i in list.reversed() {
          if nodes[i].bar { continue }
          let outs = edges.filter { before($0) == i }, ins = edges.filter { after($0) == i }
          if outs.isEmpty || ins.count >= outs.count { continue }
          var room = Int.max
          for e in outs { room = min(room, nodes[after(e)].rank - 1) }
          if room > nodes[i].rank {
            nodes[i].rank = room
            changed = true
          }
        }
        if !changed { break }
        pass += 1
      }
      // The bars stay outside the inner Cards.
      var lo = Int.max, hi = Int.min
      for i in list where !nodes[i].bar {
        lo = min(lo, nodes[i].rank)
        hi = max(hi, nodes[i].rank)
      }
      for i in list where nodes[i].bar { nodes[i].rank = nodes[i].inputs.isEmpty ? lo - 1 : hi + 1 }
      var first = Int.max
      for i in list { first = min(first, nodes[i].rank) }
      for i in list { nodes[i].rank -= first }
    }

    /// Points on the wires that pass a layer, so they have a lane through it.
    private func addLanes() {
      var last = 0
      for i in list { last = max(last, n.items[i].rank) }
      layers = [[Int]](repeating: [], count: last + 1)
      for i in list { layers[n.items[i].rank].append(i) }
      for e in edges {
        if e.feedback {
          chains.append((e, [e.from, e.to]))
          continue
        }
        var path = [e.from]
        var r = n.items[e.from].rank + 1
        while r < n.items[e.to].rank {
          let d = n.items.count
          n.items.append(Node(rank: r))
          list.append(d)
          layers[r].append(d)
          path.append(d)
          r += 1
        }
        path.append(e.to)
        chains.append((e, path))
      }
      for l in layers { for (k, id) in l.enumerated() { n.items[id].order = k } }
    }

    /// Position in the layer and on the port, at both ends of each piece between two layers.
    private func links(_ rank: Int) -> [(top: Double, bottom: Double)] {
      let nodes = n.items
      var out: [(top: Double, bottom: Double)] = []
      for c in chains where !c.edge.feedback {
        var k = 0
        while k + 1 < c.path.count {
          let a = c.path[k], b = c.path[k + 1]
          if nodes[a].rank == rank {
            let af = k == 0 ? (Double(c.edge.fromSlot) + 0.5) / Double(max(nodes[a].outputs.count, 1)) : 0.5
            let bf = k + 2 == c.path.count ? (Double(c.edge.toSlot) + 0.5) / Double(max(nodes[b].inputs.count, 1)) : 0.5
            out.append((Double(nodes[a].order) + af, Double(nodes[b].order) + bf))
          }
          k += 1
        }
      }
      return out
    }

    private func crossings() -> Int {
      var total = 0
      var r = 0
      while r + 1 < layers.count {
        let l = links(r)
        for i in l.indices {
          var j = i + 1
          while j < l.count {
            let top = l[i].top - l[j].top, bottom = l[i].bottom - l[j].bottom
            if (top < 0 && bottom > 0) || (top > 0 && bottom < 0) { total += 1 }
            j += 1
          }
        }
        r += 1
      }
      return total
    }

    /// Barycentres, sweeping down and up, keeping the order with the fewest crossings.
    private func orderLayers() {
      let nodes = n.items
      var best = layers
      var bestCount = crossings()
      var sweep = 0
      while sweep < orderSweeps && bestCount > 0 {
        let down = sweep % 2 == 0
        let count = layers.count
        var k = 1
        while k < count {
          sortLayer(down ? k : count - 1 - k, down: down)
          k += 1
        }
        let c = crossings()
        if c < bestCount {
          bestCount = c
          best = layers
        }
        sweep += 1
      }
      layers = best
      for l in layers { for (k, id) in l.enumerated() { nodes[id].order = k } }
      // Neighbours that are better the other way round.
      var pass = 0
      while pass < 8 && bestCount > 0 {
        var improved = false
        for li in layers.indices {
          var k = 0
          while k + 1 < layers[li].count {
            let a = layers[li][k], b = layers[li][k + 1]
            layers[li][k] = b
            layers[li][k + 1] = a
            nodes[a].order = k + 1
            nodes[b].order = k
            let c = crossings()
            if c < bestCount {
              bestCount = c
              improved = true
            } else {
              layers[li][k] = a
              layers[li][k + 1] = b
              nodes[a].order = k
              nodes[b].order = k + 1
            }
            k += 1
          }
        }
        if !improved { break }
        pass += 1
      }
    }

    private func sortLayer(_ r: Int, down: Bool) {
      let nodes = n.items
      var keyed: [(id: Int, k: Int, v: Double)] = []
      for (position, id) in layers[r].enumerated() {
        var sum = 0.0, count = 0.0
        for c in chains where !c.edge.feedback {
          var k = 0
          while k + 1 < c.path.count {
            let a = c.path[k], b = c.path[k + 1]
            if down && b == id {
              let f = k == 0 ? (Double(c.edge.fromSlot) + 0.5) / Double(max(nodes[a].outputs.count, 1)) : 0.5
              sum += Double(nodes[a].order) + f
              count += 1
            } else if !down && a == id {
              let f = k + 2 == c.path.count ? (Double(c.edge.toSlot) + 0.5) / Double(max(nodes[b].inputs.count, 1)) : 0.5
              sum += Double(nodes[b].order) + f
              count += 1
            }
            k += 1
          }
        }
        keyed.append((id, position, count > 0 ? sum / count : Double(nodes[id].order) + 0.5))
      }
      keyed.sort { $0.v != $1.v ? $0.v < $1.v : $0.k < $1.k }
      for (k, s) in keyed.enumerated() {
        layers[r][k] = s.id
        nodes[s.id].order = k
      }
    }

    /// Positions across the flow: the least (weighted) bending of the wires that
    /// keeps the order and the room in each layer.
    private func assignAcross() {
      let nodes = n.items
      func port(_ id: Int, _ list: [Double], _ slot: Int) -> Double { slot < list.count ? list[slot] : nodes[id].across / 2 }
      pieces = []
      for c in chains {
        let last = c.path.count - 1
        var k = 0
        while k < last {
          let a = c.path[k], b = c.path[k + 1]
          let fromAt = k == 0 ? port(a, nodes[a].outputs, c.edge.fromSlot) : 0
          let toAt = k + 1 == last ? port(b, nodes[b].inputs, c.edge.toSlot) : 0
          let laneA = k > 0, laneB = k + 1 < last
          let weight = c.edge.feedback ? weightFeedback : laneA && laneB ? weightLong : laneA || laneB ? weightLongEnd : weightNear
          pieces.append(Piece(from: a, fromAt: fromAt, to: b, toAt: toAt, weight: weight))
          k += 1
        }
      }
      // The network: one variable per node, one more per piece of wire, in whole
      // grids when Cards are on it (a wire whose ports are not a whole number of
      // grids apart stays as near as it can).
      let unit = snap ? grid : 1
      let vars = list
      var index = [Int](repeating: -1, count: nodes.count)
      for (i, v) in vars.enumerated() { index[v] = i }
      var arcs: [Arc] = []
      for l in layers {
        var k = 0
        while k + 1 < l.count {
          let a = nodes[l[k]], b = nodes[l[k + 1]]
          let lane = a.across == 0 || b.across == 0
          // A Card keeps the room for its frame beside it.
          let frame = a.frameAcross > 0 ? a.frameAcross + boxGap + grid : 0
          let room = ((a.across + frame + (lane ? laneGap : sideGap)) / unit - 1e-9).rounded(.up)
          arcs.append(Arc(from: index[l[k]], to: index[l[k + 1]], min: Int(room), weight: 0))
          k += 1
        }
      }
      var count = vars.count
      for p in pieces {
        let apart = Int(roundNear((p.fromAt - p.toAt) / unit))
        let aux = count
        count += 1
        arcs.append(Arc(from: aux, to: index[p.from], min: apart >= 0 ? 0 : -apart, weight: p.weight))
        arcs.append(Arc(from: aux, to: index[p.to], min: apart >= 0 ? apart : 0, weight: p.weight))
      }
      let solved = solveNetwork(count: count, arcs: arcs)
      for (i, v) in vars.enumerated() { nodes[v].at = Double(solved[i]) * unit }
      if !snap { settleStraight(index) }
    }

    /// Without the grid: wires that came out straight stay exactly straight (ports
    /// are not whole numbers), and each straight run moves as one, most of its
    /// Cards on the lines of the grid.
    private func settleStraight(_ index: [Int]) {
      let nodes = n.items
      var parent = Array(list.indices)
      func find(_ start: Int) -> Int {
        var i = start
        while parent[i] != i {
          parent[i] = parent[parent[i]]
          i = parent[i]
        }
        return i
      }
      let links = pieces.filter { p in
        p.weight != weightFeedback && nodes[p.to].at - nodes[p.from].at == roundNear(p.fromAt - p.toAt)
      }
      for p in links {
        let a = find(index[p.from]), b = find(index[p.to])
        if a != b { parent[max(a, b)] = min(a, b) }
      }
      var groups = [[Int]](repeating: [], count: list.count)
      for (i, id) in list.enumerated() { groups[find(i)].append(id) }
      for group in groups where !group.isEmpty {
        // Exact positions from the first member, following its straight wires.
        var seen = [Bool](repeating: false, count: nodes.count)
        seen[group[0]] = true
        var queue = [group[0]]
        var head = 0
        while head < queue.count {
          let u = queue[head]
          head += 1
          for p in links {
            if p.from == u && !seen[p.to] {
              nodes[p.to].at = nodes[u].at + p.fromAt - p.toAt
              seen[p.to] = true
              queue.append(p.to)
            } else if p.to == u && !seen[p.from] {
              nodes[p.from].at = nodes[u].at + p.toAt - p.fromAt
              seen[p.from] = true
              queue.append(p.from)
            }
          }
        }
        // The shift that puts the most Cards on the grid, the smallest of those.
        var bestShift = 0.0, bestScore = -1.0
        let cards = group.filter { nodes[$0].card >= 0 }
        for id in cards.isEmpty ? group : cards {
          let shift = nearTo(nodes[id].at, grid) - nodes[id].at
          var score = 0.0
          for o in cards {
            let v = (nodes[o].at + shift) / grid
            if abs(v - roundNear(v)) < 1e-6 { score += nodes[o].across }
          }
          if score > bestScore + 1e-9 || (abs(score - bestScore) <= 1e-9 && abs(shift) < abs(bestShift) - 1e-9) {
            bestScore = score
            bestShift = shift
          }
        }
        for id in group { nodes[id].at += bestShift }
      }
    }

    /// Positions along the flow: each layer just past the one before it, and
    /// past the frames of earlier layers that reach it.
    private func assignAlong() {
      let nodes = n.items
      var at = 0.0
      var reach: [(lo: Double, hi: Double, end: Double)] = []
      for l in layers {
        var size = 0.0
        var any = false
        for id in l where nodes[id].card >= 0 {
          size = max(size, nodes[id].along)
          any = true
        }
        for id in l where nodes[id].card >= 0 {
          let v = nodes[id]
          var spans = [(lo: v.at, hi: v.at + v.across)]
          if v.frameAcross > 0 { spans.append((v.at + v.across + boxGap, v.at + v.across + boxGap + v.frameAcross)) }
          for s in spans {
            for r in reach where s.lo < r.hi + boxGap && r.lo < s.hi + boxGap {
              at = max(at, ceilTo(r.end + boxGap, grid))
            }
          }
        }
        for id in l {
          let v = nodes[id]
          v.layerAt = at
          if v.frameAcross > 0 { reach.append((v.at + v.across + boxGap, v.at + v.across + boxGap + v.frameAcross, at + v.frameAlong)) }
        }
        if any { at = ceilTo(at + size + layerGap, grid) }
      }
    }

    private func wireLines() -> [Line] {
      let nodes = n.items
      var lines: [Line] = []
      func point(_ c: (edge: Edge, path: [Int]), _ k: Int) -> (across: Double, along: Double) {
        let v = nodes[c.path[k]]
        if k == 0 {
          let p = c.edge.fromSlot < v.outputs.count ? v.outputs[c.edge.fromSlot] : v.across / 2
          return (v.at + p, v.layerAt + v.along)
        }
        if k == c.path.count - 1 {
          let p = c.edge.toSlot < v.inputs.count ? v.inputs[c.edge.toSlot] : v.across / 2
          return (v.at + p, v.layerAt)
        }
        return (v.at, v.layerAt)
      }
      for c in chains where !c.edge.feedback {
        var k = 0
        while k + 1 < c.path.count {
          let a = point(c, k), b = point(c, k + 1)
          lines.append(Line(a: a.across, b: a.along, c: b.across, d: b.along))
          k += 1
        }
      }
      return lines
    }
  }

  /// The nodes of a whole scope (each graph adds the points of its wires).
  final class NodeList {
    var items: [Node]
    init(_ items: [Node]) { self.items = items }
  }

  struct Arc {
    var from: Int
    var to: Int
    var min: Int
    var weight: Int
  }

  /// Network simplex: minimises the sum of weight * (x[to] - x[from]) with
  /// x[to] - x[from] >= min, over whole numbers.
  static func solveNetwork(count: Int, arcs: [Arc]) -> [Int] {
    var x = [Int](repeating: 0, count: count)
    // A first solution: every variable as low as its arcs allow.
    var pass = 0
    while pass <= count {
      var changed = false
      for a in arcs where x[a.to] < x[a.from] + a.min {
        x[a.to] = x[a.from] + a.min
        changed = true
      }
      if !changed { break }
      pass += 1
    }
    func slack(_ a: Arc) -> Int { x[a.to] - x[a.from] - a.min }
    // A spanning tree of tight arcs.
    var inTree = [Bool](repeating: false, count: count)
    var tree: [Int] = []
    if count > 0 { inTree[0] = true }
    var size = 1
    while size < count {
      var grew = false
      for (i, a) in arcs.enumerated() where inTree[a.from] != inTree[a.to] && slack(a) == 0 {
        inTree[a.from] = true
        inTree[a.to] = true
        tree.append(i)
        size += 1
        grew = true
      }
      if grew { continue }
      var best = -1, bestSlack = Int.max
      for (i, a) in arcs.enumerated() where inTree[a.from] != inTree[a.to] && slack(a) < bestSlack {
        best = i
        bestSlack = slack(a)
      }
      if best < 0 {
        // Not connected (does not happen for one graph): carry on from the next node.
        inTree[inTree.firstIndex(of: false)!] = true
        size += 1
        continue
      }
      let delta = inTree[arcs[best].from] ? bestSlack : -bestSlack
      for v in 0..<count where inTree[v] { x[v] += delta }
    }
    // Exchanges: an arc of the tree whose cut would gain from growing leaves it.
    var adjacent = [[Int]](repeating: [], count: count)
    func rebuild() {
      for v in 0..<count { adjacent[v].removeAll(keepingCapacity: true) }
      for t in tree {
        adjacent[arcs[t].from].append(arcs[t].to)
        adjacent[arcs[t].to].append(arcs[t].from)
      }
    }
    rebuild()
    var side = [Bool](repeating: false, count: count)
    func flood(_ start: Int, without: Arc) {
      for v in 0..<count { side[v] = false }
      side[start] = true
      var stack = [start]
      while let v = stack.popLast() {
        for w in adjacent[v] {
          if side[w] { continue }
          if (v == without.from && w == without.to) || (v == without.to && w == without.from) { continue }
          side[w] = true
          stack.append(w)
        }
      }
    }
    var step = 0
    while step < simplexLimit {
      var leaving = -1
      var k = 0
      while k < tree.count && leaving < 0 {
        let e = arcs[tree[k]]
        flood(e.from, without: e) // `side`: the part with the arc's start
        var cut = 0
        for a in arcs {
          if side[a.from] && !side[a.to] { cut += a.weight } else if !side[a.from] && side[a.to] { cut -= a.weight }
        }
        if cut < 0 { leaving = k }
        k += 1
      }
      if leaving < 0 { break }
      var entering = -1, bestSlack = Int.max
      for (i, a) in arcs.enumerated() where !side[a.from] && side[a.to] && slack(a) < bestSlack {
        entering = i
        bestSlack = slack(a)
      }
      if entering < 0 { break }
      for v in 0..<count where !side[v] { x[v] += bestSlack }
      tree[leaving] = entering
      rebuild()
      step += 1
    }
    var low = Int.max
    for v in x { low = min(low, v) }
    return x.map { $0 - low }
  }
}

func arrangeCards(_ input: ArrangeInput) -> ArrangeResult {
  let vertical = input.flow == .topToBottom
  let grid = input.grid > 0 ? input.grid : 20
  func cardIndex(_ id: String) -> Int { input.cards.firstIndex { $0.id == id } ?? -1 }
  func slot(_ list: [ArrangePort], _ name: String) -> Int { list.firstIndex { $0.name == name } ?? 0 }

  var items: [Arrange.Node] = input.cards.enumerated().map { i, c in
    let frame = input.frames.first { $0.id == c.id }
    func slots(_ list: [ArrangePort]) -> [Double] { list.map { vertical ? $0.x : $0.y } }
    return Arrange.Node(card: i, across: vertical ? c.width : c.height, along: vertical ? c.height : c.width,
                        frameAcross: frame.map { vertical ? $0.width : $0.height } ?? 0, frameAlong: frame.map { vertical ? $0.height : $0.width } ?? 0,
                        inputs: slots(c.inputs), outputs: slots(c.outputs))
  }

  var edges: [Arrange.Edge] = []
  for w in input.wires {
    let f = cardIndex(w.from), t = cardIndex(w.to)
    if f < 0 || t < 0 || f == t { continue }
    edges.append(Arrange.Edge(from: f, fromSlot: slot(input.cards[f].outputs, w.fromPort), to: t, toSlot: slot(input.cards[t].inputs, w.toPort)))
  }

  // Bars are drawn above and below the inner Cards when the flow runs down:
  // they take part in the order, and wires from them get a lane.
  if vertical {
    func addBar(_ list: [ArrangeBarEnd], top: Bool) {
      if !list.contains(where: { cardIndex($0.card) >= 0 }) { return }
      let width = Double(list.count) * 110 + 40
      let slots = list.indices.map { n in (width * Double(n + 1)) / Double(list.count + 1) }
      let bar = items.count
      items.append(Arrange.Node(bar: true, across: width, inputs: top ? [] : slots, outputs: top ? slots : []))
      for (n, e) in list.enumerated() {
        let c = cardIndex(e.card)
        if c < 0 { continue }
        if top {
          edges.append(Arrange.Edge(from: bar, fromSlot: n, to: c, toSlot: slot(input.cards[c].inputs, e.port)))
        } else {
          edges.append(Arrange.Edge(from: c, fromSlot: slot(input.cards[c].outputs, e.port), to: bar, toSlot: n))
        }
      }
    }
    addBar(input.boundaryInputs, top: true)
    addBar(input.boundaryOutputs, top: false)
  }

  // Graphs that have nothing to do with each other are laid out apart.
  var parent = Array(items.indices)
  func find(_ start: Int) -> Int {
    var i = start
    while parent[i] != i {
      parent[i] = parent[parent[i]]
      i = parent[i]
    }
    return i
  }
  for e in edges {
    let a = find(e.from), b = find(e.to)
    if a != b { parent[max(a, b)] = min(a, b) }
  }
  var roots: [Int] = []
  var members: [[Int]] = []
  for i in items.indices {
    let r = find(i)
    if let k = roots.firstIndex(of: r) {
      members[k].append(i)
    } else {
      roots.append(r)
      members.append([i])
    }
  }

  let shared = Arrange.NodeList(items)
  func flowSize(_ w: Double, _ h: Double) -> (across: Double, along: Double) { vertical ? (w, h) : (h, w) }
  func worldRect(_ x: Double, _ y: Double, _ w: Double, _ h: Double) -> Arrange.Rect {
    vertical ? Arrange.Rect(across: x, along: y, acrossSize: w, alongSize: h) : Arrange.Rect(across: y, along: x, acrossSize: h, alongSize: w)
  }
  /// What a Card in Card draws above and below these Cards (`GraphCanvas.boundary`).
  func barRects(_ cards: [Arrange.Rect]) -> [Arrange.Rect] {
    let inputs = input.boundaryInputs.count, outputs = input.boundaryOutputs.count
    if (inputs == 0 && outputs == 0) || cards.isEmpty { return [] }
    var box = cards[0]
    for r in cards { box = Arrange.unionRect(box, r) }
    let x = vertical ? box.across : box.along, y = vertical ? box.along : box.across
    let w = vertical ? box.acrossSize : box.alongSize, h = vertical ? box.alongSize : box.acrossSize
    let width = max(w, Double(max(inputs, outputs)) * 110 + 40)
    let left = x + w / 2 - width / 2
    return [worldRect(left, y - 96, width, 40), worldRect(left, y + h + 56, width, 40)]
  }
  func cardRect(_ i: Int) -> Arrange.Rect {
    let v = shared.items[i]
    return Arrange.Rect(across: v.at, along: v.layerAt, acrossSize: v.across, alongSize: v.along)
  }

  struct Block {
    var list: [Int]
    var frames: [(id: String, rect: Int)]
    var room: [Arrange.Rect]
    var lines: [Arrange.Line]
    var box: Arrange.Rect
  }

  // Each graph with its output frames, nearest the start of the flow first.
  let blocks: [Block] = members.map { list in
    let graphEdges = edges.filter { list.contains($0.from) }
    let graph = Arrange.Layered(nodes: shared, edges: graphEdges, list: list, grid: grid, snap: input.snap)
    let lines = graph.run()
    let nodes = shared.items
    let cards = graph.list.filter { nodes[$0].card >= 0 }
    let rects = cards.map(cardRect)
    var room = rects + barRects(rects)
    var frames: [(id: String, rect: Int)] = []
    let owners = cards.filter { nodes[$0].frameAcross > 0 }.sorted {
      nodes[$0].rank != nodes[$1].rank ? nodes[$0].rank < nodes[$1].rank : nodes[$0].order < nodes[$1].order
    }
    for i in owners {
      let box = input.frames.first { $0.id == input.cards[nodes[i].card].id }!
      let size = flowSize(box.width, box.height)
      frames.append((box.id, room.count))
      room.append(Arrange.placeBox(acrossSize: size.across, alongSize: size.along, near: cardRect(i), frame: true, obstacles: room, lines: lines, grid: grid))
    }
    var box = room[0]
    for r in room { box = Arrange.unionRect(box, r) }
    return Block(list: graph.list, frames: frames, room: room, lines: lines, box: box)
  }

  // The biggest graph first, the others in rows beside it.
  let blockOrder = blocks.indices.sorted { a, b in
    blocks[a].list.count != blocks[b].list.count ? blocks[a].list.count > blocks[b].list.count : a < b
  }
  var area = 0.0, shelf = Arrange.shelfMin
  for b in blocks {
    area += (b.box.acrossSize + Arrange.sideGap) * (b.box.alongSize + Arrange.layerGap)
    shelf = max(shelf, b.box.acrossSize)
  }
  shelf = max(shelf, area.squareRoot() * 1.4)
  var rowAcross = 0.0, rowAlong = 0.0, rowEnd = 0.0
  var room: [Arrange.Rect] = []
  var lines: [Arrange.Line] = []
  var moved: [[Arrange.Rect]] = blocks.map(\.room)
  for k in blockOrder {
    let b = blocks[k]
    if rowAcross > 0 && rowAcross + b.box.acrossSize > shelf {
      rowAcross = 0
      rowAlong = Arrange.ceilTo(rowEnd + Arrange.layerGap + Arrange.sideGap, grid)
    }
    let dc = rowAcross - Arrange.floorTo(b.box.across, grid), da = rowAlong - Arrange.floorTo(b.box.along, grid)
    for i in b.list {
      shared.items[i].at += dc
      shared.items[i].layerAt += da
    }
    for i in b.room.indices {
      moved[k][i].across += dc
      moved[k][i].along += da
      room.append(moved[k][i])
    }
    for l in b.lines { lines.append(Arrange.Line(a: l.a + dc, b: l.b + da, c: l.c + dc, d: l.d + da)) }
    rowAcross = Arrange.ceilTo(rowAcross + b.box.acrossSize + Arrange.sideGap * 2, grid)
    rowEnd = max(rowEnd, b.box.along + da + b.box.alongSize)
  }

  func at(_ id: String, across: Double, along: Double) -> ArrangeAt {
    ArrangeAt(id: id, x: Arrange.tenth(vertical ? across : along), y: Arrange.tenth(vertical ? along : across))
  }

  // Notes: free room next to everything else.
  var notes: [ArrangeAt] = []
  for n in input.notes {
    var box = room.first ?? Arrange.Rect(across: 0, along: 0, acrossSize: 0, alongSize: 0)
    for r in room { box = Arrange.unionRect(box, r) }
    let size = flowSize(n.width, n.height)
    let r = Arrange.placeBox(acrossSize: size.across, alongSize: size.along, near: box, frame: false, obstacles: room, lines: lines, grid: grid)
    room.append(r)
    notes.append(at(n.id, across: r.across, along: r.along))
  }

  var cards: [ArrangeAt] = []
  for v in shared.items where v.card >= 0 { cards.append(at(input.cards[v.card].id, across: v.at, along: v.layerAt)) }
  var frames: [ArrangeAt] = []
  for (k, b) in blocks.enumerated() {
    for f in b.frames { frames.append(at(f.id, across: moved[k][f.rect].across, along: moved[k][f.rect].along)) }
  }
  return ArrangeResult(cards: cards, frames: frames, notes: notes)
}
