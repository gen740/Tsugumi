import SwiftUI

extension Explainer {
  // 1. Tsugumi が解決しようとしている課題 (README).
  // A researcher and an AI; the AI's output piles up; a wall rises between
  // them, one stone per reason (the three reasons are its labelled stones).
  // Then the page's thread (ExplainerView) comes through the temari, the hole
  // in the wall and on to the AI, and the three things Tsugumi shares travel
  // it both ways (drawBeads).

  enum WallScene {
    // Layout (art coordinates).
    static let groundY: CGFloat = 700
    static let human = pt(165, 700)
    static let wallX: ClosedRange<CGFloat> = 400...600
    static let brickH: CGFloat = 34
    static let rows = 13
    static var wallTop: CGFloat { groundY - CGFloat(rows) * brickH }
    static let hole = pt(500, 404)
    static let temari = (center: pt(268, 678), radius: CGFloat(22))
    static var temariTop: CGPoint { TsugumiTemari.lead(center: temari.center, radius: temari.radius) }
    static let aiCenter = pt(835, 450)
    static let tie = pt(758, 452)

    static func draw(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      let enter = span(p, -0.45, 0.15)
      drawGround(g, enter)

      // the AI and its output
      let ai = aiPoint(t)
      drawSheets(g, p, t, from: ai)
      drawAI(g.faded(enter), center: pt(ai.x, ai.y + (1 - CGFloat(enter)) * 30), t: t, calm: span(p, 4.7, 5.2))

      // the researcher, puzzled until the thread arrives
      drawHuman(g.faded(enter), feet: human, t: t, puzzled: span(p, 0.45, 1.2) * (1 - span(p, 4.6, 5.0)), p: p)

      // the temari at the researcher's feet
      let temariIn = pop(lin(p, 3.55, 3.85))
      if temariIn > 0.01 {
        TsugumiTemari.draw(g, center: temari.center, radius: temari.radius * CGFloat(temariIn), spin: -lin(p, 4.0, 4.7) * 16)
      }

      drawWall(g, p, t)
      drawStones(g, p)
    }

    // MARK: Ground

    static func drawGround(_ g: GraphicsContext, _ o: Double) {
      let g = g.faded(o)
      var rng = SeededRandom(seed: 11)
      var xs: [CGFloat] = [0]
      while xs.last! < artW { xs.append(xs.last! + CGFloat(rng.next(70, 140))) }
      for i in 0..<(xs.count - 1) {
        let a = xs[i], b = xs[i + 1], m = (a + b) / 2
        let top = groundY + CGFloat(rng.next(-3, 3))
        g.poly([pt(a, groundY), pt(b, groundY), pt(m, top + 60)], TsugumiPalette.hex(0x1d3a3e, 0.9))
        g.poly([pt(a, groundY), pt(m, top + 60), pt(a, artH)], TsugumiPalette.hex(0x173136, 0.9))
        g.poly([pt(b, groundY), pt(m, top + 60), pt(b, artH)], TsugumiPalette.hex(0x142b30, 0.9))
        g.poly([pt(a, artH), pt(m, top + 60), pt(b, artH)], TsugumiPalette.hex(0x11262b, 0.9))
      }
      var edge = Path(); edge.move(to: pt(0, groundY)); edge.addLine(to: pt(artW, groundY))
      g.stroke(edge, with: .color(TsugumiPalette.olive.opacity(0.35)), lineWidth: 1.5)
    }

    // MARK: The researcher

    static func drawHuman(_ g: GraphicsContext, feet f: CGPoint, t: Double, puzzled: Double, p: Double) {
      var g = g
      g.translateBy(x: f.x, y: f.y); g.scaleBy(x: 1.22, y: 1.22); g.translateBy(x: -f.x, y: -f.y)
      let x = f.x, y = f.y
      let breathe = CGFloat(sin(t * 1.6)) * 1.2
      // head tilts while puzzled
      let tiltHead = CGFloat(puzzled) * 0.16 * CGFloat(1 + 0.15 * sin(t * 1.3))
      let coat = (TsugumiPalette.hex(0x9fc0bb), TsugumiPalette.hex(0x82a7a3), TsugumiPalette.hex(0x6a908d))
      let legs = TsugumiPalette.hex(0x24464a), legsDark = TsugumiPalette.hex(0x1b3739)
      let skin = TsugumiPalette.hex(0xf0dcc0), skinShade = TsugumiPalette.hex(0xd9bf9e)
      let hair = TsugumiPalette.hex(0x2f2722)

      // legs
      g.poly([pt(x - 20, y - 86), pt(x - 3, y - 86), pt(x - 6, y), pt(x - 19, y)], legs)
      g.poly([pt(x + 3, y - 86), pt(x + 20, y - 86), pt(x + 19, y), pt(x + 6, y)], legsDark)
      g.poly([pt(x - 22, y), pt(x - 4, y), pt(x - 6, y - 7)], TsugumiPalette.hex(0x2a2320))
      g.poly([pt(x + 4, y), pt(x + 25, y), pt(x + 6, y - 7)], TsugumiPalette.hex(0x2a2320))
      // coat
      let sh = y - 172 + breathe
      g.poly([pt(x - 34, sh + 6), pt(x, sh), pt(x - 26, y - 80)], coat.0)
      g.poly([pt(x, sh), pt(x + 34, sh + 6), pt(x + 26, y - 80)], coat.2)
      g.poly([pt(x, sh), pt(x - 26, y - 80), pt(x + 26, y - 80)], coat.1)
      g.poly([pt(x - 6, sh + 2), pt(x + 6, sh + 2), pt(x, sh + 30)], TsugumiPalette.hex(0xf3ead6))
      // arms: the right hand rises to the chin while puzzled
      g.poly([pt(x - 34, sh + 6), pt(x - 24, sh + 14), pt(x - 34, y - 92), pt(x - 42, y - 96)], coat.1)
      g.poly([pt(x - 42, y - 96), pt(x - 34, y - 92), pt(x - 37, y - 82)], skin)
      let hand = mix(pt(x + 40, y - 96), pt(x + 14, sh - 8), puzzled)
      let elbow = mix(pt(x + 40, y - 120), pt(x + 36, sh + 34), puzzled)
      g.poly([pt(x + 24, sh + 6), pt(x + 34, sh + 6), elbow], coat.2)
      g.poly([pt(x + 30, sh + 4), elbow, pt(elbow.x - 9, elbow.y + 4)], coat.1)
      g.poly([elbow, pt(elbow.x - 9, elbow.y + 4), pt(hand.x - 4, hand.y + 4), pt(hand.x + 4, hand.y - 2)], coat.0)
      g.poly([pt(hand.x - 6, hand.y), pt(hand.x + 5, hand.y - 6), pt(hand.x + 6, hand.y + 6)], skin)

      // head
      var head = g
      head.translateBy(x: x, y: sh - 8)
      head.rotate(by: .radians(Double(tiltHead)))
      head.poly([pt(-6, 0), pt(6, 0), pt(5, 10), pt(-5, 10)], skinShade)
      let r: CGFloat = 25, cy: CGFloat = -26
      let hex = (0..<6).map { i -> CGPoint in let a = Double(i) * .pi / 3 + .pi / 6; return pt(r * CGFloat(cos(a)), cy + r * CGFloat(sin(a)) * 1.08) }
      head.poly([pt(0, cy), hex[0], hex[1]], skinShade)
      head.poly([pt(0, cy), hex[1], hex[2]], skin)
      head.poly([pt(0, cy), hex[2], hex[3]], skin)
      head.poly([pt(0, cy), hex[3], hex[4]], hair)
      head.poly([pt(0, cy), hex[4], hex[5]], hair)
      head.poly([pt(0, cy), hex[5], hex[0]], TsugumiPalette.hex(0x3b322b))
      head.poly([hex[3], hex[4], pt(-r - 3, cy + 6)], hair)
      // the eye, toward the wall
      head.fill(Path(ellipseIn: CGRect(x: 9, y: cy + 2, width: 4, height: 5)), with: .color(TsugumiPalette.hex(0x2a2320)))

      // the question marks, then a small light when the thread arrives
      for i in 0..<3 {
        let o = span(p, 0.5 + Double(i) * 0.35, 0.75 + Double(i) * 0.35) * puzzled
        guard o > 0.01 else { continue }
        let bob = CGFloat(sin(t * 2 + Double(i) * 1.7)) * 4
        let q = pt(x + 18 + CGFloat(i) * 26, sh - 92 - CGFloat(i) * 22 + bob)
        g.text("?", at: q, size: 30 + CGFloat(i) * 6, weight: .heavy, color: Ink.cream, opacity: o * 0.85)
      }
      let lit = span(p, 4.75, 5.05)
      if lit > 0.01 {
        var glow = g
        glow.addFilter(.blur(radius: 10))
        glow.fill(Path(ellipseIn: CGRect(x: x + 10, y: sh - 110, width: 40, height: 40)), with: .color(TsugumiPalette.hex(0xf6d79a, 0.6 * lit)))
        g.text("!", at: pt(x + 30, sh - 90), size: 34, weight: .heavy, color: TsugumiPalette.hex(0xf6d79a), opacity: lit)
      }
    }

    // MARK: The AI

    static func aiPoint(_ t: Double) -> CGPoint { pt(aiCenter.x, aiCenter.y + CGFloat(sin(t * 1.4)) * 8) }

    /// A faceted gem turning slowly; it speeds up while producing output.
    static func drawAI(_ g: GraphicsContext, center c: CGPoint, t: Double, calm: Double) {
      // shadow on the ground
      g.fill(Path(ellipseIn: CGRect(x: c.x - 70, y: groundY - 10, width: 140, height: 18)), with: .color(.black.opacity(0.25)))
      var glow = g
      glow.addFilter(.blur(radius: 26))
      glow.fill(Path(ellipseIn: CGRect(x: c.x - 90, y: c.y - 90, width: 180, height: 180)),
                with: .color(Color(.sRGB, red: Ink.ai.0, green: Ink.ai.1, blue: Ink.ai.2, opacity: 0.35 + 0.1 * sin(t * 2))))
      typealias V = SIMD3<Double>
      let r = 78.0
      let verts: [V] = [V(0, -1.25, 0), V(1, 0, 0), V(0, 0, 1), V(-1, 0, 0), V(0, 0, -1), V(0, 1.1, 0)]
      let faces = [(0, 1, 2), (0, 2, 3), (0, 3, 4), (0, 4, 1), (5, 2, 1), (5, 3, 2), (5, 4, 3), (5, 1, 4)]
      let a = t * (0.5 - 0.25 * calm), tilt = 0.35
      func view(_ v: V) -> V {
        let x = v.x * cos(a) + v.z * sin(a), z = -v.x * sin(a) + v.z * cos(a)
        let y = v.y * cos(tilt) - z * sin(tilt), z2 = v.y * sin(tilt) + z * cos(tilt)
        return V(x, y, z2)
      }
      let vs = verts.map(view)
      let light = V(-0.5, -0.6, 0.65) / 1.0
      var drawn: [(Double, [CGPoint], Color)] = []
      for (i, j, k) in faces {
        let A = vs[i], B = vs[j], C = vs[k]
        let e1 = B - A, e2 = C - A
        var n = V(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x)
        n /= (n * n).sum().squareRoot()
        if n.z < 0 { continue }
        let k0 = 0.55 + 0.55 * max(0, (n * light).sum() / 1.0)
        let col = Color(.sRGB, red: min(1, Ink.ai.0 * k0), green: min(1, Ink.ai.1 * k0), blue: min(1, Ink.ai.2 * k0))
        let pts = [A, B, C].map { pt(c.x + CGFloat($0.x * r), c.y + CGFloat($0.y * r)) }
        drawn.append(((A.z + B.z + C.z) / 3, pts, col))
      }
      for d in drawn.sorted(by: { $0.0 < $1.0 }) { g.poly(d.1, d.2) }
      g.text("AI", at: pt(c.x, c.y + 116), size: 18, weight: .semibold, color: Ink.mist)
    }

    /// Code and reports flying out of the AI and piling up beside it.
    static func drawSheets(_ g: GraphicsContext, _ p: Double, _ t: Double, from c: CGPoint) {
      let n = 30
      var rng = SeededRandom(seed: 3)
      for i in 0..<n {
        let start = 0.2 + Double(i) * 0.04
        let u = lin(p, start, start + 0.32)
        let jx = CGFloat(rng.next(-14, 14)), rot = rng.next(-0.25, 0.25), kind = rng.next(0, 1)
        guard u > 0 else { continue }
        // the pile: to the AI's right, then spilling toward the wall
        let col = i % 4
        let land = pt(622 + CGFloat(col) * 27 + jx * 0.4, groundY - 8 - CGFloat(i / 4) * 9)
        let top = pt((c.x + land.x) / 2 + jx * 3, c.y - 170 - CGFloat(i % 5) * 14)
        let e = easeInOut(u)
        let q = bezier(c, pt(c.x + jx, top.y), pt(land.x, top.y), land, e)
        var s = g
        s.translateBy(x: q.x, y: q.y)
        let flutter = u < 1 ? sin(t * 9 + Double(i)) * 0.35 : 0
        s.rotate(by: .radians(rot + flutter + (1 - e) * 2.5))
        s.opacity *= min(1, u * 4)
        let sheet = CGRect(x: -15, y: -10, width: 30, height: 20)
        s.fill(Path(roundedRect: sheet, cornerRadius: 2), with: .color(TsugumiPalette.hex(kind < 0.5 ? 0xe9edf0 : 0xd8e6ea)))
        for l in 0..<3 {
          let w: CGFloat = l == 2 ? 12 : 20
          s.fill(Path(CGRect(x: -11, y: -6 + CGFloat(l) * 5, width: w, height: 1.6)),
                 with: .color(TsugumiPalette.hex(kind < 0.5 ? 0x7c8b92 : 0x5c8fa0)))
        }
      }
    }

    // MARK: The wall

    static func bricks() -> [(rect: CGRect, order: Int)] {
      var out: [(CGRect, Int)] = []
      var k = 0
      for r in 0..<rows {
        let y = groundY - CGFloat(r + 1) * brickH
        let edges: [CGFloat] = r % 2 == 0 ? [wallX.lowerBound, 450, 500, 550, wallX.upperBound] : [wallX.lowerBound, 425, 475, 525, 575, wallX.upperBound]
        for i in 0..<(edges.count - 1) {
          out.append((CGRect(x: edges[i], y: y, width: edges[i + 1] - edges[i], height: brickH), k)); k += 1
        }
      }
      return out
    }

    static func drawWall(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      let all = bricks()
      let holeR = 15 * CGFloat(pop(lin(p, 4.27, 4.37)))
      var g = g
      if holeR > 0.5 {
        var clip = Path(CGRect(x: 0, y: 0, width: artW, height: artH))
        clip.addEllipse(in: CGRect(x: hole.x - holeR, y: hole.y - holeR, width: holeR * 2, height: holeR * 2))
        g.clip(to: clip, style: FillStyle(eoFill: true))
      }
      var rng = SeededRandom(seed: 17)
      for (rect, k) in all {
        let start = 1.05 + Double(k) / Double(all.count) * 2.05
        let u = lin(p, start, start + 0.12)
        let shade = rng.next(-0.06, 0.06)
        guard u > 0 else { continue }
        let drop = CGFloat(1 - easeOut(u)) * -46
        let r = rect.offsetBy(dx: 0, dy: drop).insetBy(dx: 1, dy: 1)
        var b = g
        b.opacity *= min(1, u * 3)
        let base = 0.33 + shade
        let top = Color(.sRGB, red: base * 0.95, green: base * 1.05, blue: base * 1.08)
        let bottom = Color(.sRGB, red: base * 0.72, green: base * 0.8, blue: base * 0.85)
        let mid = pt(r.midX + CGFloat(shade * 60), r.midY)
        b.poly([pt(r.minX, r.minY), pt(r.maxX, r.minY), mid], top)
        b.poly([pt(r.maxX, r.minY), pt(r.maxX, r.maxY), mid], bottom)
        b.poly([pt(r.maxX, r.maxY), pt(r.minX, r.maxY), mid], bottom.opacity(0.92))
        b.poly([pt(r.minX, r.maxY), pt(r.minX, r.minY), mid], top.opacity(0.9))
      }
      // a rim of light round the hole once the thread is through
      if holeR > 0.5 {
        let lit = span(p, 4.4, 5.0)
        var ring = Path(); ring.addEllipse(in: CGRect(x: hole.x - holeR, y: hole.y - holeR, width: holeR * 2, height: holeR * 2))
        var h = g
        h.addFilter(.blur(radius: 4))
        h.stroke(ring, with: .color(TsugumiPalette.hex(0xf6d79a, 0.5 * lit + 0.1 * lit * sin(t * 3))), lineWidth: 6)
        g.stroke(ring, with: .color(.black.opacity(0.4)), lineWidth: 1.5)
      }
    }

    /// The three reasons as stones set into the wall as it rises.
    static func drawStones(_ g: GraphicsContext, _ p: Double) {
      let stones: [(String, CGFloat, Double)] = [("再現できない", 628, 1.45), ("過程が見えない", 522, 2.05), ("データが怖い", 342, 2.65)]
      for (label, y, at) in stones {
        let u = pop(lin(p, at - 0.12, at + 0.12))
        guard u > 0.01 else { continue }
        let w: CGFloat = 250, h: CGFloat = 50
        var s = g
        s.translateBy(x: 500, y: y)
        s.scaleBy(x: CGFloat(u), y: CGFloat(u))
        let r = CGRect(x: -w / 2, y: -h / 2, width: w, height: h)
        s.fill(Path(roundedRect: r.offsetBy(dx: 0, dy: 4), cornerRadius: 6), with: .color(.black.opacity(0.3)))
        s.poly([pt(r.minX, r.minY), pt(r.maxX, r.minY), pt(r.maxX - 20, r.midY), pt(r.minX + 30, r.midY + 4)], TsugumiPalette.hex(0x5d4b40))
        s.poly([pt(r.minX, r.minY), pt(r.minX + 30, r.midY + 4), pt(r.minX, r.maxY)], TsugumiPalette.hex(0x4a3b32))
        s.poly([pt(r.maxX, r.minY), pt(r.maxX, r.maxY), pt(r.maxX - 20, r.midY)], TsugumiPalette.hex(0x433630))
        s.poly([pt(r.minX, r.maxY), pt(r.minX + 30, r.midY + 4), pt(r.maxX - 20, r.midY), pt(r.maxX, r.maxY)], TsugumiPalette.hex(0x3b2f29))
        s.text(label, at: .zero, size: 22, weight: .bold, color: Ink.cream)
      }
    }

    /// What travels on the thread, both ways.
    static func drawBeads(_ g: GraphicsContext, _ p: Double, _ t: Double, on line: Polyline) {
      let o = span(p, 4.7, 5.05)
      guard o > 0.01 else { return }
      let beads: [(String, Bool)] = [("何をしてほしいか", true), ("何を実行したか", false), ("次にどこを変えるか", true)]
      for (i, (label, toAI)) in beads.enumerated() {
        var f = (t * 0.075 + Double(i) / 3).truncatingRemainder(dividingBy: 1)
        if !toAI { f = 1 - f }
        let edge = min(f, 1 - f)
        let q = line.at(f)
        let w = CGFloat(label.count) * 15 + 34, h: CGFloat = 30
        var b = g
        let inWall = smooth((abs(q.x - 500) - 100) / 24)
        b.opacity *= o * smooth(edge / 0.06) * inWall
        let r = CGRect(x: q.x - w / 2, y: q.y - h - 8, width: w, height: h)
        b.fill(Path(roundedRect: r.offsetBy(dx: 0, dy: 3), cornerRadius: 15), with: .color(.black.opacity(0.25)))
        b.fill(Path(roundedRect: r, cornerRadius: 15), with: .color(TsugumiPalette.hex(toAI ? 0xf6ead2 : 0xd7ecef)))
        b.fill(Path(ellipseIn: CGRect(x: q.x - 4, y: q.y - 4, width: 8, height: 8)), with: .color(TsugumiPalette.thread))
        let arrow = toAI ? "→" : "←"
        b.text(toAI ? "\(label) \(arrow)" : "\(arrow) \(label)", at: pt(r.midX, r.midY), size: 14.5, weight: .semibold,
               color: TsugumiPalette.hex(0x24363b))
      }
    }
  }
}
