import SwiftUI

// Drawing pieces of the loading screen, in the key visual's style
// (docs/assets/tsugumi-icon.svg): low-poly facets on a teal ground.
// Nothing here knows about projects or sessions.

enum TsugumiPalette {
  static func hex(_ v: UInt32, _ a: Double = 1) -> Color {
    Color(.sRGB, red: Double(v >> 16 & 0xff) / 255, green: Double(v >> 8 & 0xff) / 255, blue: Double(v & 0xff) / 255, opacity: a)
  }
  static let tealTop = hex(0x2d5a5c), tealBottom = hex(0x132b33)
  static let cream = hex(0xf6ead2), mist = hex(0xa9c4bf)
  static let rufous = hex(0xc97d3d), olive = hex(0xa9b77a), oliveDark = hex(0x7d9148)
  static let bark = hex(0x7d5834), barkDark = hex(0x5c3e24), barkShade = hex(0x4e341e)
}

/// Fixes the animation clock (snapshots, previews); nil follows real time.
private struct TsugumiClockKey: EnvironmentKey { static let defaultValue: Double? = nil }
extension EnvironmentValues {
  var tsugumiClock: Double? {
    get { self[TsugumiClockKey.self] }
    set { self[TsugumiClockKey.self] = newValue }
  }
}

// MARK: - Bird

struct BirdPose {
  var hop: CGFloat = 0      // points lifted off the perch
  var tilt: Double = 0      // radians around the feet; + leans forward (peck)
  var squash: CGFloat = 0   // + squashes on landing
  var blink: CGFloat = 0    // 0 open … 1 closed
  var flap: Double = 0      // 0 … 1 wing raised
}

/// The key-visual bird, polygon for polygon, in its SVG coordinates (facing
/// right). The wing and the eye are separate so they can move.
enum TsugumiBird {
  enum Part { case body, wing, eye, highlight }
  struct Facet { let part: Part; let points: [CGPoint]; let color: Color }

  static let feet = CGPoint(x: 505, y: 706)
  static let beakTip: CGFloat = 776
  static let tailTip: CGFloat = 160
  static let shoulder = CGPoint(x: 545, y: 402)
  static let eyeCenter = CGPoint(x: 628, y: 368)

  // Copied from tsugumi-icon.svg #tsugumi-bird in draw order: legs, tail,
  // body, spots, head, supercilium, eye, beak, wing.
  private static let source: [(Part, String, UInt32, Double)] = [
    (.body, "468,600 494,604 486,640 470,640", 0xd9c29e, 1), (.body, "474,640 484,640 478,702 472,702", 0xc08d68, 1),
    (.body, "472,702 478,694 512,704 476,708", 0xa8744f, 1), (.body, "472,700 476,708 444,708", 0x8e5f3e, 1),
    (.body, "528,592 554,596 546,636 530,636", 0xcdb48c, 1), (.body, "534,636 544,636 540,700 534,700", 0xb07c58, 1),
    (.body, "534,700 540,692 572,700 538,706", 0xa8744f, 1), (.body, "534,698 538,706 508,706", 0x8e5f3e, 1),
    (.body, "330,480 340,585 175,600", 0x4e3320, 1), (.body, "175,600 340,585 215,655", 0x3a2617, 1),
    (.body, "175,600 215,655 160,640", 0x2f1f13, 1),
    (.body, "520,330 655,452 420,400", 0x6f4a2c, 1), (.body, "655,452 605,545 420,400", 0xefe0c3, 1),
    (.body, "420,400 605,545 330,480", 0xe2cca4, 1), (.body, "330,480 605,545 560,610", 0xf4e9d4, 1),
    (.body, "330,480 560,610 470,628", 0xe9d8b8, 1), (.body, "330,480 470,628 340,585", 0xd4b98d, 1),
    (.body, "655,452 690,500 605,545", 0xe6d3b0, 1), (.body, "605,545 690,500 560,610", 0xf8efdd, 1),
    (.body, "610,470 628,478 612,490", 0x3b2718, 1), (.body, "575,500 594,505 580,520", 0x3b2718, 1),
    (.body, "630,512 648,516 636,532", 0x3b2718, 1), (.body, "548,540 566,546 552,560", 0x3b2718, 1),
    (.body, "596,552 614,556 602,572", 0x3b2718, 1), (.body, "520,575 536,580 524,592", 0x3b2718, 1),
    (.body, "570,590 586,593 576,606", 0x3b2718, 1),
    (.body, "590,298 665,320 628,366", 0x8a5d36, 1), (.body, "520,330 590,298 628,366", 0x7a5232, 1),
    (.body, "665,320 694,356 628,366", 0x9a6a3f, 1), (.body, "520,330 628,366 655,452", 0xc9a77a, 1),
    (.body, "628,366 694,356 688,402", 0x3b2718, 1), (.body, "628,366 688,402 655,452", 0xf3e6cc, 1),
    (.body, "655,452 688,402 690,500", 0xeadbbd, 1),
    (.body, "575,338 662,334 668,346 592,352", 0xf1e2c4, 1),
    (.eye, "628,354 641,361 641,376 628,383 615,376 615,361", 0x16100b, 1),
    (.highlight, "623,360 631,358 628,366", 0xffffff, 0.9),
    (.body, "694,356 776,394 728,384", 0x2e2117, 1), (.body, "688,402 694,356 728,384", 0x3d2c1f, 1),
    (.body, "688,402 728,384 776,394", 0xd9a441, 1),
    (.wing, "545,402 440,418 455,500", 0xb0672f, 1), (.wing, "545,402 455,500 525,522", 0xc97d3d, 1),
    (.wing, "440,418 298,560 455,500", 0x8f4f22, 1), (.wing, "455,500 298,560 525,522", 0xa35c29, 1),
    (.wing, "298,560 525,522 400,560", 0x6f3c1a, 1), (.wing, "545,402 525,522 565,470", 0xd58b49, 1),
    (.wing, "380,470 300,556 410,505", 0x5a3015, 0.6),
  ]

  static let facets: [Facet] = source.map { part, pts, hex, a in
    let points = pts.split(separator: " ").map { pair -> CGPoint in
      let v = pair.split(separator: ",").compactMap { Double($0) }
      return CGPoint(x: v[0], y: v[1])
    }
    return Facet(part: part, points: points, color: TsugumiPalette.hex(hex, a))
  }

  /// Draws with the feet at `at`; `scale` maps SVG units (~616 × 410) to points.
  static func draw(_ g: GraphicsContext, at: CGPoint, scale: CGFloat, pose: BirdPose) {
    var g = g
    g.translateBy(x: at.x, y: at.y - pose.hop)
    g.rotate(by: .radians(pose.tilt))
    g.scaleBy(x: scale * (1 + pose.squash * 0.5), y: scale * (1 - pose.squash))
    g.translateBy(x: -feet.x, y: -feet.y)

    var wing = g
    wing.translateBy(x: shoulder.x, y: shoulder.y)
    wing.rotate(by: .radians(-0.45 * pose.flap))
    wing.translateBy(x: -shoulder.x, y: -shoulder.y)

    let open = max(0.1, 1 - pose.blink)
    var eye = g
    eye.translateBy(x: eyeCenter.x, y: eyeCenter.y)
    eye.scaleBy(x: 1, y: open)
    eye.translateBy(x: -eyeCenter.x, y: -eyeCenter.y)

    for f in facets {
      let ctx: GraphicsContext
      switch f.part {
      case .body: ctx = g
      case .wing: ctx = wing
      case .eye: ctx = eye
      case .highlight: if open < 0.5 { continue }; ctx = eye
      }
      var p = Path(); p.addLines(f.points); p.closeSubpath()
      ctx.fill(p, with: .color(f.color))
      // a same-colour hairline hides the seams between facets
      if f.part != .highlight { ctx.stroke(p, with: .color(f.color), lineWidth: 1.2) }
    }
  }
}

enum BirdMotion {
  /// A hop over `0…1`: lift, a little squash on take-off and landing, wing up.
  static func hop(_ u: Double, height: CGFloat) -> BirdPose {
    var p = BirdPose()
    p.hop = height * CGFloat(sin(u * .pi))
    p.flap = sin(u * .pi)
    if u < 0.12 { p.squash = CGFloat(0.12 - u) * 0.6 }
    if u > 0.88 { p.squash = CGFloat(u - 0.88) * 0.6 }
    return p
  }

  static func blink(_ t: Double, every: Double, offset: Double = 0) -> CGFloat {
    let u = (t + offset).truncatingRemainder(dividingBy: every)
    return u < 0.16 ? CGFloat(sin(u / 0.16 * .pi)) : 0
  }

  /// Indeterminate loop (3.2 s): hop, hop, look up, peck, peck, blink.
  static func idle(_ t: Double) -> BirdPose {
    let u = t.truncatingRemainder(dividingBy: 3.2)
    var p = BirdPose()
    switch u {
    case 0..<0.45: p = hop(u / 0.45, height: 14)
    case 0.45..<0.55: p.squash = 0.03
    case 0.55..<1.0: p = hop((u - 0.55) / 0.45, height: 14)
    case 1.0..<1.6: p.tilt = -0.1 * sin((u - 1.0) / 0.6 * .pi)
    case 1.7..<2.05: p.tilt = 0.38 * sin((u - 1.7) / 0.35 * .pi)
    case 2.1..<2.45: p.tilt = 0.38 * sin((u - 2.1) / 0.35 * .pi)
    default: break
    }
    p.blink = max(p.blink, blink(u, every: 3.2, offset: 0.4))
    return p
  }
}

// MARK: - Scenery

/// Seeded low-poly teal ground, like the key visual's background.
struct LowPolyBackground: View {
  var body: some View {
    Canvas { g, size in
      g.fill(Path(CGRect(origin: .zero, size: size)),
             with: .linearGradient(Gradient(colors: [TsugumiPalette.tealTop, TsugumiPalette.tealBottom]),
                                   startPoint: .zero, endPoint: CGPoint(x: size.width, y: size.height)))
      var rng = SeededRandom(seed: 7)
      let cols = 7, rows = 5
      var pts: [[CGPoint]] = []
      for r in 0...rows {
        pts.append((0...cols).map { col in
          let jx = (col == 0 || col == cols) ? 0 : rng.next(-0.35, 0.35)
          let jy = (r == 0 || r == rows) ? 0 : rng.next(-0.35, 0.35)
          return CGPoint(x: (Double(col) + jx) * size.width / Double(cols), y: (Double(r) + jy) * size.height / Double(rows))
        })
      }
      for r in 0..<rows {
        for col in 0..<cols {
          let a = pts[r][col], b = pts[r][col + 1], c = pts[r + 1][col], d = pts[r + 1][col + 1]
          for tri in [[a, b, d], [a, d, c]] {
            var p = Path(); p.addLines(tri); p.closeSubpath()
            let shade = rng.next(-0.05, 0.07)
            g.fill(p, with: .color(.white.opacity(max(0, shade))))
            g.fill(p, with: .color(.black.opacity(max(0, -shade) + Double(r) * 0.02)))
          }
        }
      }
    }
  }
}

struct SeededRandom {
  var state: UInt64
  init(seed: UInt64) { state = seed &* 0x9E3779B97F4A7C15 }
  mutating func next(_ lo: Double, _ hi: Double) -> Double {
    state = state &* 6364136223846793005 &+ 1442695040888963407
    return lo + (hi - lo) * Double(state >> 11) / Double(1 << 53)
  }
}

/// A faint DAG of Cards behind the bird. With a fraction, Cards complete in
/// order and a signal flows only into the running one; without, signals flow
/// through every wire.
struct WorkflowBackdrop: View {
  var fraction: Double?
  @Environment(\.tsugumiClock) private var clock
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  // unit position (card top-centre) and completion threshold
  private static let nodes: [(x: Double, y: Double, done: Double)] = [
    (0.10, 0.26, 0.0), (0.30, 0.06, 0.22), (0.24, 0.50, 0.40), (0.56, 0.03, 0.62), (0.82, 0.16, 0.82), (0.90, 0.50, 0.97),
  ]
  private static let edges: [(Int, Int)] = [(0, 1), (0, 2), (1, 3), (2, 3), (3, 4), (4, 5)]
  private static let cardSize = CGSize(width: 62, height: 36)

  var body: some View {
    TimelineView(.animation(paused: clock != nil || reduceMotion)) { ctx in
      let t = clock ?? (reduceMotion ? 0.3 : ctx.date.timeIntervalSinceReferenceDate)
      Canvas { g, size in draw(g, size, t) }
    }
    .accessibilityHidden(true)
  }

  private func draw(_ g: GraphicsContext, _ size: CGSize, _ t: Double) {
    let cs = Self.cardSize
    func center(_ i: Int) -> CGPoint { CGPoint(x: Self.nodes[i].x * size.width, y: Self.nodes[i].y * size.height + cs.height / 2) }
    func done(_ i: Int) -> Bool { fraction.map { $0 >= Self.nodes[i].done } ?? false }
    func running(_ i: Int) -> Bool { fraction != nil && !done(i) && Self.edges.filter { $0.1 == i }.allSatisfy { done($0.0) } }
    func wire(_ e: (Int, Int)) -> (CGPoint, CGPoint, CGPoint, CGPoint) {
      let a = CGPoint(x: center(e.0).x + cs.width / 2, y: center(e.0).y)
      let b = CGPoint(x: center(e.1).x - cs.width / 2, y: center(e.1).y)
      let k = max(24, (b.x - a.x) * 0.5)
      return (a, CGPoint(x: a.x + k, y: a.y), CGPoint(x: b.x - k, y: b.y), b)
    }
    func at(_ w: (CGPoint, CGPoint, CGPoint, CGPoint), _ s: Double) -> CGPoint {
      let u = 1 - s
      return CGPoint(x: u * u * u * w.0.x + 3 * u * u * s * w.1.x + 3 * u * s * s * w.2.x + s * s * s * w.3.x,
                     y: u * u * u * w.0.y + 3 * u * u * s * w.1.y + 3 * u * s * s * w.2.y + s * s * s * w.3.y)
    }

    var glow = [Double](repeating: 0, count: Self.nodes.count)
    for (n, e) in Self.edges.enumerated() {
      let w = wire(e)
      var p = Path(); p.move(to: w.0); p.addCurve(to: w.3, control1: w.1, control2: w.2)
      let lit = done(e.0) && done(e.1)
      g.stroke(p, with: .color((lit ? TsugumiPalette.olive : TsugumiPalette.mist).opacity(lit ? 0.5 : 0.25)), lineWidth: 1.4)
      guard fraction == nil || (done(e.0) && running(e.1)) else { continue }
      let s = (t * 0.45 + Double(n) * 0.37).truncatingRemainder(dividingBy: 1)
      let q = at(w, s)
      g.fill(Path(ellipseIn: CGRect(x: q.x - 2.4, y: q.y - 2.4, width: 4.8, height: 4.8)), with: .color(TsugumiPalette.cream.opacity(0.75)))
      if s > 0.8 { glow[e.1] = max(glow[e.1], (s - 0.8) / 0.2) }
    }

    for i in Self.nodes.indices {
      let c = center(i)
      let r = CGRect(x: c.x - cs.width / 2, y: c.y - cs.height / 2, width: cs.width, height: cs.height)
      let card = Path(roundedRect: r, cornerRadius: 7)
      let isDone = done(i), isRunning = running(i)
      let pulse = isRunning ? 0.5 + 0.5 * sin(t * 4) : glow[i]
      g.fill(card, with: .color(TsugumiPalette.tealBottom.opacity(0.5)))
      g.fill(card, with: .color((isDone ? TsugumiPalette.olive : TsugumiPalette.cream).opacity(isDone ? 0.18 : 0.04 + 0.08 * pulse)))
      let stroke = isDone ? TsugumiPalette.olive : isRunning ? TsugumiPalette.rufous : TsugumiPalette.mist
      g.stroke(card, with: .color(stroke.opacity(isDone ? 0.7 : 0.3 + 0.4 * pulse)), lineWidth: 1.1)
      // title and two text bars, like a Card on the canvas
      for (y, w, h, a) in [(8.0, 26.0, 4.0, 0.4), (17.0, 40.0, 3.0, 0.25), (24.0, 30.0, 3.0, 0.25)] {
        g.fill(Path(roundedRect: CGRect(x: r.minX + 8, y: r.minY + y, width: w, height: h), cornerRadius: h / 2),
               with: .color(TsugumiPalette.cream.opacity(a)))
      }
      if isDone {
        var check = Path()
        check.move(to: CGPoint(x: r.maxX - 15, y: r.minY + 10))
        check.addLine(to: CGPoint(x: r.maxX - 11, y: r.minY + 14))
        check.addLine(to: CGPoint(x: r.maxX - 5, y: r.minY + 6))
        g.stroke(check, with: .color(TsugumiPalette.olive), style: StrokeStyle(lineWidth: 1.6, lineCap: .round, lineJoin: .round))
      }
      for px in [r.minX, r.maxX] {
        g.fill(Path(ellipseIn: CGRect(x: px - 2.5, y: c.y - 2.5, width: 5, height: 5)), with: .color(TsugumiPalette.mist.opacity(0.5)))
      }
    }
  }
}

/// A faceted branch from x0 to x1; `grown` (0…1) is the part with bark,
/// leaves and olives. The rest is a faint outline.
enum TsugumiBranch {
  static func draw(_ g: GraphicsContext, x0: CGFloat, x1: CGFloat, y: CGFloat, grown: Double, time: Double) {
    let segs = 14
    let fx = x0 + (x1 - x0) * grown
    func yAt(_ x: CGFloat) -> CGFloat { y + sin(x * 0.045) * 2.5 }
    for i in 0..<segs {
      let a = x0 + (x1 - x0) * CGFloat(i) / CGFloat(segs), b = x0 + (x1 - x0) * CGFloat(i + 1) / CGFloat(segs)
      let top = [CGPoint(x: a, y: yAt(a) - 3.5), CGPoint(x: b, y: yAt(b) - 3.5), CGPoint(x: b, y: yAt(b)), CGPoint(x: a, y: yAt(a))]
      let bot = [CGPoint(x: a, y: yAt(a)), CGPoint(x: b, y: yAt(b)), CGPoint(x: b, y: yAt(b) + 3.5), CGPoint(x: a, y: yAt(a) + 3.5)]
      if b <= fx + 0.5 {
        poly(g, top, i % 2 == 0 ? TsugumiPalette.bark : TsugumiPalette.hex(0x6b4a2b))
        poly(g, bot, i % 2 == 0 ? TsugumiPalette.barkDark : TsugumiPalette.barkShade)
      } else {
        poly(g, top + bot.reversed(), .white.opacity(0.08))
      }
    }
    // leaves sprout at fixed slots once the bark reaches them
    let slots = 11
    for i in 0..<slots {
      let sx = x0 + 18 + (x1 - x0 - 36) * CGFloat(i) / CGFloat(slots - 1)
      let grow = min(1, max(0, (fx - sx) / 22))
      guard grow > 0 else { continue }
      let up = i % 2 == 0
      var l = g
      l.translateBy(x: sx, y: yAt(sx) + (up ? -3 : 3))
      l.rotate(by: .radians((up ? -0.9 : 0.9) + sin(time * 2 + Double(i)) * 0.08))
      l.scaleBy(x: grow, y: grow)
      poly(l, [.zero, CGPoint(x: 18, y: 0), CGPoint(x: 9, y: -5)], TsugumiPalette.olive)
      poly(l, [.zero, CGPoint(x: 18, y: 0), CGPoint(x: 9, y: 4)], TsugumiPalette.oliveDark)
      if i % 3 == 1 {
        var o = g
        o.translateBy(x: sx + 6, y: yAt(sx) + 10)
        o.scaleBy(x: grow, y: grow)
        let hexagon = (0..<6).map { k in
          let a = Double(k) * .pi / 3 + .pi / 6
          return CGPoint(x: 5 * cos(a), y: 5 * sin(a))
        }
        poly(o, hexagon, i % 2 == 0 ? TsugumiPalette.hex(0x55661f) : TsugumiPalette.hex(0x3a2a3e))
        poly(o, [CGPoint(x: -2, y: -3), CGPoint(x: 1, y: -4), CGPoint(x: 0, y: -1)], .white.opacity(0.6))
      }
    }
  }

  private static func poly(_ g: GraphicsContext, _ pts: [CGPoint], _ color: Color) {
    var p = Path(); p.addLines(pts); p.closeSubpath(); g.fill(p, with: .color(color))
  }
}
