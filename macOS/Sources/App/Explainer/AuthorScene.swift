import SwiftUI

extension Explainer {
  // 0. この道具を作った人 (docs/Author.md): the University of Tokyo's Yasuda
  // Auditorium, low-poly like the bird, assembling itself from the ground up,
  // ginkgo either side and a student before it; above, the night sky, whose
  // stars get joined into a network (AI, for astrophysics).
  enum AuthorScene {
    static let groundY: CGFloat = 640
    static let brick: [UInt32] = [0x9b5b3e, 0x8c5035, 0xa86448, 0x7d4630, 0xb26c4a]

    static func draw(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      drawSky(g, p, t)
      var ground = g
      ground.translateBy(x: 0, y: groundY - WallScene.groundY)
      WallScene.drawGround(ground, span(p, 0, 0.15))
      drawTree(g, x: 150, p: p, t: t, seed: 3)
      drawTree(g, x: 850, p: p, t: t, seed: 8)
      drawAuditorium(g, p, t)
      // the student
      let s = span(p, 0.62, 0.78)
      if s > 0.01 {
        var h = g.faded(s)
        let feet = pt(318, groundY)
        h.translateBy(x: feet.x, y: feet.y); h.scaleBy(x: 0.6, y: 0.6); h.translateBy(x: -feet.x, y: -feet.y)
        WallScene.drawHuman(h, feet: feet, t: t, puzzled: 0, p: 0)
      }
    }

    // MARK: The sky

    static func drawSky(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      let o = span(p, 0, 0.3)
      guard o > 0.01 else { return }
      var rng = SeededRandom(seed: 31)
      for i in 0..<90 {
        let q = pt(CGFloat(rng.next(30, 970)), CGFloat(rng.next(100, 430)))
        let r = CGFloat(rng.next(0.7, 2.2))
        let tw = 0.55 + 0.45 * sin(t * rng.next(0.8, 2.2) + Double(i))
        g.fill(Path(ellipseIn: CGRect(x: q.x - r, y: q.y - r, width: r * 2, height: r * 2)),
               with: .color(Ink.cream.opacity(o * tw * 0.8)))
      }
      // a spiral galaxy, faint
      let gal = span(p, 0.2, 0.5)
      if gal > 0.01 {
        var gx = g
        gx.translateBy(x: 815, y: 175)
        gx.rotate(by: .radians(-0.45))
        gx.scaleBy(x: 1, y: 0.42)
        var glow = gx
        glow.addFilter(.blur(radius: 10))
        glow.fill(Path(ellipseIn: CGRect(x: -26, y: -26, width: 52, height: 52)), with: .color(TsugumiPalette.hex(0xd7c6f0, 0.35 * gal)))
        var r2 = SeededRandom(seed: 5)
        for k in 0..<160 {
          let arm = Double(k % 2) * .pi
          let s = r2.next(0.05, 1)
          let a = arm + s * 4.2 + r2.next(-0.25, 0.25)
          let rad = 8 + s * 62
          let q = pt(CGFloat(cos(a) * rad), CGFloat(sin(a) * rad))
          gx.fill(Path(ellipseIn: CGRect(x: q.x - 1.3, y: q.y - 1.3, width: 2.6, height: 2.6)),
                  with: .color(TsugumiPalette.hex(k % 3 == 0 ? 0xb9c8ff : 0xf1e3ff, gal * (1 - s * 0.6))))
        }
      }
      // the network: bright stars joined one by one, its nodes lit as they join
      let nodes: [CGPoint] = [pt(120, 170), pt(230, 120), pt(300, 210), pt(410, 150), pt(520, 230), pt(600, 135),
                              pt(690, 250), pt(760, 330), pt(880, 300), pt(560, 345), pt(380, 300), pt(250, 330)]
      let edges: [(Int, Int)] = [(0, 1), (1, 2), (2, 3), (3, 5), (3, 4), (4, 5), (4, 6), (5, 6), (6, 7), (7, 8),
                                 (4, 9), (9, 10), (10, 2), (10, 11), (11, 0), (9, 7)]
      let net = lin(p, 0.72, 1)
      for (n, e) in edges.enumerated() {
        let u = clamp01(net * Double(edges.count) - Double(n))
        guard u > 0 else { continue }
        var l = Path(); l.move(to: nodes[e.0]); l.addLine(to: mix(nodes[e.0], nodes[e.1], u))
        g.stroke(l, with: .color(TsugumiPalette.hex(0x9fd3e6, 0.45)), style: StrokeStyle(lineWidth: 1.2, lineCap: .round))
      }
      for (i, q) in nodes.enumerated() {
        let lit = edges.enumerated().contains { n, e in (e.0 == i || e.1 == i) && net * Double(edges.count) > Double(n) }
        let r: CGFloat = lit ? 3.4 : 2.4
        if lit {
          var glow = g
          glow.addFilter(.blur(radius: 5))
          glow.fill(Path(ellipseIn: CGRect(x: q.x - 7, y: q.y - 7, width: 14, height: 14)), with: .color(TsugumiPalette.hex(0x9fd3e6, 0.6)))
        }
        g.fill(Path(ellipseIn: CGRect(x: q.x - r, y: q.y - r, width: r * 2, height: r * 2)), with: .color(Ink.cream.opacity(o)))
      }
    }

    // MARK: Ginkgo

    static func drawTree(_ g: GraphicsContext, x: CGFloat, p: Double, t: Double, seed: UInt64) {
      let u = pop(lin(p, 0.48, 0.68))
      guard u > 0.01 else { return }
      var g = g
      g.translateBy(x: x, y: groundY)
      g.scaleBy(x: CGFloat(u), y: CGFloat(u))
      g.poly([pt(-6, 0), pt(6, 0), pt(3, -70), pt(-3, -70)], TsugumiPalette.hex(0x4e3624))
      var rng = SeededRandom(seed: seed)
      let golds: [UInt32] = [0xe8b931, 0xd9a21e, 0xf2cf4a, 0xc98f17]
      for _ in 0..<26 {
        let c = pt(CGFloat(rng.next(-48, 48)), CGFloat(rng.next(-150, -60)))
        let s = CGFloat(rng.next(14, 26))
        let a = rng.next(0, 2 * .pi)
        let tri = (0..<3).map { k in pt(c.x + s * CGFloat(cos(a + Double(k) * 2.1)), c.y + s * CGFloat(sin(a + Double(k) * 2.1))) }
        g.poly(tri, TsugumiPalette.hex(golds[Int(rng.next(0, 3.99))]))
      }
      // a few leaves on their way down
      for k in 0..<3 {
        let f = (t * 0.12 + Double(k) * 0.33 + Double(seed) * 0.1).truncatingRemainder(dividingBy: 1)
        let q = pt(CGFloat(sin(f * 9 + Double(k)) * 30) + CGFloat(k * 20 - 20), CGFloat(-120 + f * 120))
        var l = g
        l.translateBy(x: q.x, y: q.y)
        l.rotate(by: .radians(f * 8))
        l.opacity *= sin(f * .pi)
        l.poly([pt(0, -5), pt(5, 3), pt(-5, 3)], TsugumiPalette.hex(0xf2cf4a))
      }
    }

    // MARK: Yasuda Auditorium

    /// The auditorium's masses, front on: wings, the central block with the
    /// great arch, the tower with its tall windows and parapet.
    static let masses: [CGRect] = [
      CGRect(x: 255, y: 500, width: 150, height: 140),  // left wing
      CGRect(x: 595, y: 500, width: 150, height: 140),  // right wing
      CGRect(x: 405, y: 452, width: 190, height: 188),  // central block
      CGRect(x: 438, y: 290, width: 124, height: 162),  // tower
      CGRect(x: 430, y: 274, width: 140, height: 16),   // parapet
      CGRect(x: 430, y: 258, width: 16, height: 16),    // corner pinnacles
      CGRect(x: 554, y: 258, width: 16, height: 16),
    ]

    static func drawAuditorium(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      var rng = SeededRandom(seed: 13)
      // facets of brick, built from the ground up
      for (mi, m) in masses.enumerated() {
        let cell: CGFloat = mi >= 4 ? 8 : 24
        var y = m.minY
        while y < m.maxY - 0.5 {
          let h = min(cell, m.maxY - y)
          var x = m.minX
          while x < m.maxX - 0.5 {
            let w = min(cell, m.maxX - x)
            let appear = 0.1 + Double((groundY - (y + h)) / 390) * 0.5
            let u = lin(p, appear, appear + 0.07)
            let shades = (rng.next(0, 4.99), rng.next(0, 4.99))
            if u > 0 {
              var c = g
              c.opacity *= u
              c.translateBy(x: 0, y: CGFloat(1 - easeOut(u)) * -14)
              // light from the left: the left of each mass a little lighter
              let side = Double((x - m.minX) / max(1, m.width))
              let base = brick[Int(shades.0)], other = brick[Int(shades.1)]
              let k = 1.08 - 0.16 * side
              c.poly([pt(x, y), pt(x + w, y), pt(x, y + h)], shade(base, k * 1.04))
              c.poly([pt(x + w, y), pt(x + w, y + h), pt(x, y + h)], shade(other, k * 0.94))
              if mi >= 4 { c.poly([pt(x, y), pt(x + w, y), pt(x + w, y + h), pt(x, y + h)], shade(0x6e3c28, 1)) }
            }
            x += w
          }
          y += h
        }
      }
      let detail = span(p, 0.55, 0.72)
      guard detail > 0.01 else { return }
      let d = g.faded(detail)
      // cornices: darker bands at the tops of the wings and the block
      for r in [CGRect(x: 250, y: 494, width: 160, height: 8), CGRect(x: 590, y: 494, width: 160, height: 8),
                CGRect(x: 400, y: 446, width: 200, height: 8)] {
        d.fill(Path(r), with: .color(shade(0x6e3c28, 1)))
      }
      // the great arch, warm inside
      let arch = archPath(x: 452, width: 96, top: 472, bottom: groundY)
      d.fill(arch, with: .color(TsugumiPalette.hex(0x2b1b15)))
      var warm = d
      warm.addFilter(.blur(radius: 14))
      warm.fill(archPath(x: 466, width: 68, top: 500, bottom: groundY), with: .color(TsugumiPalette.hex(0xf2b56a, 0.32 + 0.06 * sin(t * 1.3))))
      d.stroke(arch, with: .color(shade(0x6e3c28, 1)), lineWidth: 4)
      // the tower's tall windows, and the wings'
      for x in [462.0, 492.0, 522.0] { d.fill(archPath(x: CGFloat(x), width: 16, top: 318, bottom: 428), with: .color(TsugumiPalette.hex(0x2e1d16))) }
      for wing in [255.0, 595.0] {
        for k in 0..<5 {
          let x = CGFloat(wing) + 16 + CGFloat(k) * 26
          d.fill(archPath(x: x, width: 12, top: 540, bottom: 606), with: .color(TsugumiPalette.hex(0x2e1d16)))
          d.fill(Path(CGRect(x: x, y: 516, width: 12, height: 12)), with: .color(TsugumiPalette.hex(0x2e1d16)))
        }
      }
      // the tower's crest: a small round window
      d.fill(Path(ellipseIn: CGRect(x: 489, y: 294, width: 22, height: 22)), with: .color(TsugumiPalette.hex(0x2e1d16)))
    }

    /// A round-headed arch: a rectangle with a semicircle on top.
    static func archPath(x: CGFloat, width w: CGFloat, top: CGFloat, bottom: CGFloat) -> Path {
      var p = Path()
      let r = w / 2
      p.move(to: pt(x, bottom))
      p.addLine(to: pt(x, top + r))
      p.addArc(center: pt(x + r, top + r), radius: r, startAngle: .degrees(180), endAngle: .degrees(0), clockwise: false)
      p.addLine(to: pt(x + w, bottom))
      p.closeSubpath()
      return p
    }

    static func shade(_ hex: UInt32, _ k: Double) -> Color {
      let c = rgb(hex)
      return Color(.sRGB, red: min(1, c.0 * k), green: min(1, c.1 * k), blue: min(1, c.2 * k))
    }
  }
}
