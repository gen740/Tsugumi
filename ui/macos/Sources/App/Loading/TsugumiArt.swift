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
  static let thread = hex(0xefe0c3)
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
  /// Where the thread crosses the bill (on the gape line).
  static let gape = CGPoint(x: 752, y: 388)
  private static let upperBill = [CGPoint(x: 694, y: 356), CGPoint(x: 776, y: 394), CGPoint(x: 728, y: 384)]
  private static let billColor = TsugumiPalette.hex(0x2e2117)

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

  /// Where a point of the bird (SVG units) lands when drawn with `draw`.
  static func point(_ p: CGPoint, at: CGPoint, scale: CGFloat, pose: BirdPose) -> CGPoint {
    let x = (p.x - feet.x) * scale * (1 + pose.squash * 0.5), y = (p.y - feet.y) * scale * (1 - pose.squash)
    let c = CGFloat(cos(pose.tilt)), s = CGFloat(sin(pose.tilt))
    return CGPoint(x: at.x + x * c - y * s, y: at.y - pose.hop + x * s + y * c)
  }

  /// Draws with the feet at `at`; `scale` maps SVG units (~616 × 410) to points.
  /// `holding` draws (in the caller's coordinates) over the bird and under
  /// its upper bill, so a thread through `gape` is held in the bill.
  static func draw(_ g: GraphicsContext, at: CGPoint, scale: CGFloat, pose: BirdPose,
                   holding: ((GraphicsContext) -> Void)? = nil) {
    let scene = g
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
    guard let holding else { return }
    holding(scene)
    // the upper bill again, over the thread: held between the bills
    var bill = Path(); bill.addLines(upperBill); bill.closeSubpath()
    g.fill(bill, with: .color(billColor))
    g.stroke(bill, with: .color(billColor), lineWidth: 1.2)
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
  @Environment(\.accessibilityReduceMotion) private var systemReduceMotion
  private var reduceMotion: Bool { AppPreferences.shared.tsugumiStill(reduceMotion: systemReduceMotion) }

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

/// The thread the bird holds: up from its temari below the picture, over the
/// picture's edge, hanging slack across into the bill, with a short end out
/// of the bill. The scattered tries spun into one line, left to right.
enum TsugumiThread {
  /// The turn over the edge between the panel (front) and the picture
  /// (behind): a quarter circle from straight up to straight right whose
  /// middle lies on the edge `y`, so half of it is on each side.
  static let bendRadius: CGFloat = 9
  static func bend(x: CGFloat, edge y: CGFloat) -> (start: CGPoint, end: CGPoint, path: Path) {
    let r = bendRadius, k: CGFloat = 0.5523
    let cy = y + r / 2.squareRoot()
    let start = CGPoint(x: x, y: cy), end = CGPoint(x: x + r, y: cy - r)
    var p = Path()
    p.move(to: start)
    p.addCurve(to: end, control1: CGPoint(x: x, y: cy - k * r), control2: CGPoint(x: x + r - k * r, y: cy - r))
    return (start, end, p)
  }

  /// In front, on the light panel below the picture (the panel's top edge at
  /// y = 0): straight up from the ball `a` into the bend over the edge at `x`,
  /// lying on the panel (a soft shadow beside it).
  static func near(_ g: GraphicsContext, from a: CGPoint, x: CGFloat, size: CGSize) {
    var g = g
    g.clip(to: Path(CGRect(origin: .zero, size: size)))
    let turn = bend(x: x, edge: 0)
    var p = Path()
    p.move(to: a)
    p.addLine(to: turn.start)
    p.addPath(turn.path)
    var shadow = g
    shadow.addFilter(.blur(radius: 1.2))
    shadow.stroke(p.offsetBy(dx: 2.5, dy: 1), with: .color(.black.opacity(0.25)), style: StrokeStyle(lineWidth: 2.4))
    g.stroke(p, with: .color(TsugumiPalette.hex(0x9c8460, 0.85)), style: StrokeStyle(lineWidth: 3.6))
    g.stroke(p, with: .color(TsugumiPalette.thread), style: StrokeStyle(lineWidth: 2.4))
  }

  /// Behind the panel (its top edge at the picture's bottom, `edge`): the
  /// rest of the bend over the edge at `x`, then hanging slack across to
  /// `back`. One width throughout: a stylised thread.
  static func receding(_ g: GraphicsContext, x: CGFloat, edge: CGFloat, to back: CGPoint) {
    let turn = bend(x: x, edge: edge)
    let from = turn.end
    // the slack: the lowest point sits below the straight line between the ends
    let sag = min(26, max(8, hypot(back.x - from.x, back.y - from.y) * 0.12))
    var p = turn.path
    p.addCurve(to: back, control1: CGPoint(x: from.x + (back.x - from.x) * 0.3, y: from.y + (back.y - from.y) * 0.3 + sag * 1.6),
               control2: CGPoint(x: from.x + (back.x - from.x) * 0.75, y: from.y + (back.y - from.y) * 0.75 + sag))
    var shadow = g
    shadow.addFilter(.blur(radius: 1.2))
    shadow.stroke(p.offsetBy(dx: 1.5, dy: 2.5), with: .color(.black.opacity(0.3)), style: StrokeStyle(lineWidth: 2, lineCap: .round))
    g.stroke(p, with: .color(TsugumiPalette.thread), style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
  }

  /// From the ball's top `a` up to the bill `b`, bowing out a little toward
  /// the beak's side (the key visual's thread).
  static func up(_ g: GraphicsContext, from a: CGPoint, to b: CGPoint) {
    var p = Path()
    p.move(to: a)
    p.addCurve(to: b, control1: CGPoint(x: a.x + 3, y: a.y + (b.y - a.y) * 0.3),
               control2: CGPoint(x: b.x + 1, y: a.y + (b.y - a.y) * 0.7))
    g.stroke(p, with: .color(TsugumiPalette.thread), style: StrokeStyle(lineWidth: 2, lineCap: .round))
  }

  /// A let-go thread: from the ball's top `a`, sagging to its end `e`, then a
  /// short tail curling down.
  static func slack(_ g: GraphicsContext, from a: CGPoint, to e: CGPoint, time: Double) {
    let d = hypot(e.x - a.x, e.y - a.y)
    var p = Path()
    p.move(to: a)
    p.addCurve(to: e, control1: CGPoint(x: a.x + (e.x - a.x) * 0.3, y: a.y - min(6, d * 0.2)),
               control2: CGPoint(x: a.x + (e.x - a.x) * 0.8, y: e.y - min(5, d * 0.15)))
    let w = CGFloat(sin(time * 1.3)) * 1.2
    p.addCurve(to: CGPoint(x: e.x + 6 + w, y: e.y + 12), control1: CGPoint(x: e.x + 5, y: e.y - 2),
               control2: CGPoint(x: e.x + 7, y: e.y + 6))
    g.stroke(p, with: .color(TsugumiPalette.thread), style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
  }

  /// A short loose end out of the bill `s`, swaying lightly.
  static func loose(_ g: GraphicsContext, from s: CGPoint, time: Double) {
    let w = CGFloat(sin(time * 1.3)) * 3
    var p = Path()
    p.move(to: s)
    p.addCurve(to: CGPoint(x: s.x + 26, y: s.y - 8 + w), control1: CGPoint(x: s.x + 6, y: s.y - 12),
               control2: CGPoint(x: s.x + 16, y: s.y - 2 + w))
    g.stroke(p, with: .color(TsugumiPalette.thread), style: StrokeStyle(lineWidth: 2, lineCap: .round))
  }

  private static func line(_ a: CGPoint, _ b: CGPoint) -> Path {
    var p = Path()
    p.move(to: a)
    p.addLine(to: b)
    return p
  }
}

/// The temari (kagari-wound) the bird unwinds, low-poly like the bird: a ball
/// of young-grass green thread with four wound bands in two tones, each facet shaded by
/// how it faces the light. The same ball as the key visual
/// (docs/assets/tsugumi-icon.svg #temari).
enum TsugumiTemari {
  struct Facet { let points: [CGPoint]; let color: Color }

  /// Where the thread leaves the ball: its top.
  static func lead(center: CGPoint, radius: CGFloat) -> CGPoint {
    CGPoint(x: center.x, y: center.y - radius)
  }

  /// Draws the ball resting on `center.y + radius`; `spin` (radians) turns
  /// it clockwise as thread comes off its top.
  static func draw(_ g: GraphicsContext, center: CGPoint, radius: CGFloat, spin: Double) {
    var g = g
    g.fill(Path(ellipseIn: CGRect(x: center.x - radius * 0.95, y: center.y + radius * 0.82, width: radius * 1.9, height: radius * 0.3)),
           with: .color(.black.opacity(0.28)))
    g.translateBy(x: center.x, y: center.y)
    g.scaleBy(x: radius, y: radius)
    let shape = build(spin: spin)
    var outline = Path(); outline.addLines(shape.outline); outline.closeSubpath()
    g.clip(to: outline)
    for f in shape.facets {
      var p = Path(); p.addLines(f.points); p.closeSubpath()
      g.fill(p, with: .color(f.color))
      g.stroke(p, with: .color(f.color), lineWidth: 1 / radius)
    }
  }

  private typealias V = SIMD3<Double>

  /// Facets of the unit ball (centre at the origin, y down).
  private static func build(spin: Double) -> (facets: [Facet], outline: [CGPoint]) {
    func norm(_ v: V) -> V { v / (v * v).sum().squareRoot() }
    func cross(_ a: V, _ b: V) -> V { V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x) }
    let ry = 38.0 * .pi / 180, rx = -28.0 * .pi / 180
    func view(_ v: V) -> V {
      let x = v.x * cos(ry) + v.z * sin(ry), z = -v.x * sin(ry) + v.z * cos(ry)
      let y = v.y * cos(rx) - z * sin(rx), z2 = v.y * sin(rx) + z * cos(rx)
      // the spin, clockwise on screen (the top moves toward the bird)
      return V(x * cos(spin) + y * sin(spin), -x * sin(spin) + y * cos(spin), z2)
    }
    func screen(_ v: V) -> CGPoint { CGPoint(x: v.x, y: -v.y) }
    func shade(_ hex: UInt32, _ k: Double) -> Color {
      func c(_ shift: UInt32) -> Double { min(1, Double(hex >> shift & 0xff) / 255 * k) }
      return Color(.sRGB, red: c(16), green: c(8), blue: c(0))
    }
    let light = norm(V(-0.45, 0.6, 0.66))
    var rng = SeededRandom(seed: 5)
    var facets: [Facet] = []
    func tri(_ a: V, _ b: V, _ c: V, _ hex: UInt32) {
      guard (a.z + b.z + c.z) / 3 > 0.02 else { return }  // the far side
      var n = norm(cross(b - a, c - a))
      if n.z < 0 { n = -n }
      let k = 0.62 + 0.5 * max(0, (n * light).sum()) + rng.next(-0.04, 0.04)
      facets.append(Facet(points: [screen(a), screen(b), screen(c)], color: shade(hex, k)))
    }

    // the ball: an icosahedron subdivided once
    let t = (1 + 5.0.squareRoot()) / 2
    var vs: [V] = [V(-1, t, 0), V(1, t, 0), V(-1, -t, 0), V(1, -t, 0), V(0, -1, t), V(0, 1, t),
                   V(0, -1, -t), V(0, 1, -t), V(t, 0, -1), V(t, 0, 1), V(-t, 0, -1), V(-t, 0, 1)].map(norm)
    var faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6),
                 (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11), (6, 2, 10),
                 (8, 6, 7), (9, 8, 1)]
    var mids: [Int: Int] = [:]
    func mid(_ a: Int, _ b: Int) -> Int {
      let key = min(a, b) * 1000 + max(a, b)
      if let m = mids[key] { return m }
      vs.append(norm((vs[a] + vs[b]) / 2))
      mids[key] = vs.count - 1
      return vs.count - 1
    }
    faces = faces.flatMap { a, b, c -> [(Int, Int, Int)] in
      let ab = mid(a, b), bc = mid(b, c), ca = mid(c, a)
      return [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
    }
    let seen = vs.map(view)
    for (a, b, c) in faces { tri(seen[a], seen[b], seen[c], 0x9fb070) }

    // the bands: strips along great circles, two tones (two groups of threads)
    let bands: [(V, UInt32, UInt32)] = [(V(1, 0, 0), 0xf6ecd8, 0xd8c49e), (V(0, 1, 0), 0xd58b49, 0xa35c29),
                                        (V(0, 0, 1), 0xf6ecd8, 0xd8c49e), (V(1, 1, 1), 0xd58b49, 0xa35c29)]
    let steps = 14, spread = 0.15, lift = 1.025
    for (n0, lightHex, darkHex) in bands {
      let n = norm(n0)
      let u = norm(cross(n, abs(n.z) < 0.9 ? V(0, 0, 1) : V(1, 0, 0))), w = cross(n, u)
      func at(_ i: Int, _ d: Double) -> V {
        let a = 2 * Double.pi * Double(i) / Double(steps), r = (1 - d * d).squareRoot() * lift
        return view(n * d * lift + (u * cos(a) + w * sin(a)) * r)
      }
      for i in 0..<steps {
        for (lo, hi, hex) in [(-spread, 0.0, lightHex), (0.0, spread, darkHex)] {
          tri(at(i, lo), at(i + 1, lo), at(i + 1, hi), hex)
          tri(at(i, lo), at(i + 1, hi), at(i, hi), hex)
        }
      }
    }
    return (facets, hull(seen.map(screen)))
  }

  /// Convex hull (monotone chain): the ball's silhouette.
  private static func hull(_ points: [CGPoint]) -> [CGPoint] {
    let ps = points.sorted { ($0.x, $0.y) < ($1.x, $1.y) }
    func half(_ seq: [CGPoint]) -> [CGPoint] {
      var h: [CGPoint] = []
      for p in seq {
        while h.count >= 2 {
          let a = h[h.count - 2], b = h[h.count - 1]
          if (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) > 0 { break }
          h.removeLast()
        }
        h.append(p)
      }
      return h
    }
    return Array(half(ps).dropLast() + half(ps.reversed()).dropLast())
  }
}

/// A faceted branch from x0 to x1; `grown` (0…1) is the part with bark,
/// leaves. The rest is a faint outline.
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
    // leaves sprout once the bark reaches them, scattered: seeded spacing,
    // side, angle, size and shade, sometimes two from one spot
    var rng = SeededRandom(seed: 23)
    var sx = x0 + rng.next(8, 20)
    var i = 0
    while sx < x1 - 6 {
      let count = rng.next(0, 1) < 0.3 ? 2 : 1
      for _ in 0..<count {
        let up = rng.next(0, 1) < 0.55
        let angle = (up ? -1 : 1) * rng.next(0.35, 1.35)
        let size = rng.next(0.7, 1.25)
        let shade = rng.next(0, 1)
        let grow = min(1, max(0, (fx - sx) / 22))
        defer { i += 1 }
        guard grow > 0 else { continue }
        var l = g
        l.translateBy(x: sx, y: yAt(sx) + (up ? -2.5 : 2.5))
        l.rotate(by: .radians(angle + sin(time * 2 + Double(i)) * 0.08))
        l.scaleBy(x: grow * size, y: grow * size)
        let (light, dark) = shade < 0.4 ? (TsugumiPalette.olive, TsugumiPalette.oliveDark)
          : shade < 0.75 ? (TsugumiPalette.hex(0xb6c28c), TsugumiPalette.hex(0x879b52))
          : (TsugumiPalette.hex(0x9fb070), TsugumiPalette.hex(0x6d8240))
        poly(l, [.zero, CGPoint(x: 18, y: 0), CGPoint(x: 9, y: -5)], light)
        poly(l, [.zero, CGPoint(x: 18, y: 0), CGPoint(x: 9, y: 4)], dark)
      }
      sx += rng.next(10, 34)
    }
  }

  private static func poly(_ g: GraphicsContext, _ pts: [CGPoint], _ color: Color) {
    var p = Path(); p.addLines(pts); p.closeSubpath(); g.fill(p, with: .color(color))
  }
}
