import AppKit
import SwiftUI

// "Tsugumi を知る": the repository's documents (README.md, docs/*.md) read as
// pages, with figures woven in. Everything of it lives in this namespace.
enum Explainer {
  /// A file the explainer reads: Contents/Resources/explainer (build-app.sh),
  /// or the repository in a development build.
  static func file(_ name: String, repo path: String) -> URL? {
    if let dir = Bundle.main.url(forResource: "explainer", withExtension: nil) {
      let url = dir.appendingPathComponent(name)
      if FileManager.default.fileExists(atPath: url.path) { return url }
    }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 { repo.deleteLastPathComponent() }
    let url = repo.appendingPathComponent(path)
    if FileManager.default.fileExists(atPath: url.path) { return url }
    #endif
    return nil
  }
}

// Drawing helpers of the scenes. A scene is a pure function of its story
// position `p` (where the reading line is in the figure) and the clock `t`
// (idle motion only), so scrolling back plays it back.
extension Explainer {
  /// Handwriting for the sketches: Yusei Magic (SIL OFL 1.1, bundled with its
  /// license), registered for this process only; Klee (macOS) otherwise.
  enum HandFont {
    static let name: String? = {
      guard let url = file("YuseiMagic-Regular.ttf", repo: "macOS/Resources/explainer/YuseiMagic-Regular.ttf") as CFURL? else { return nil }
      CTFontManagerRegisterFontsForURL(url, .process, nil)
      guard let ds = CTFontManagerCreateFontDescriptorsFromURL(url) as? [CTFontDescriptor], let d = ds.first else { return nil }
      return CTFontDescriptorCopyAttribute(d, kCTFontNameAttribute) as? String
    }()
  }
  static func handFont(_ size: CGFloat) -> Font {
    if let n = HandFont.name { return .custom(n, size: size) }
    return .custom("Klee-Demibold", size: size)
  }

  /// What a figure is doing now (set while it is drawn).
  enum SceneOptions {
    /// Seconds since the figure being drawn crossed the reading line (< 0: not yet).
    nonisolated(unsafe) static var since: Double = -1
  }

  /// A child's handwriting: each character a little turned, sized and lifted
  /// differently (seeded, so it stays put from frame to frame).
  static func childText(_ g: GraphicsContext, _ s: String, at p: CGPoint, size: CGFloat, color: Color, opacity: Double = 1,
                 seed: UInt64 = 1, leading: Bool = false) {
    guard opacity > 0.002 else { return }
    var g = g
    g.opacity *= opacity
    var rng = SeededRandom(seed: seed)
    let lines = s.split(separator: "\n", omittingEmptySubsequences: false)
    for (li, line) in lines.enumerated() {
      var glyphs: [(String, CGFloat, CGFloat, CGFloat, Double)] = []  // char, scale, advance, lift, turn
      var width: CGFloat = 0
      for ch in line {
        let sc = CGFloat(rng.next(0.94, 1.08))
        let em: CGFloat = ch == " " ? 0.3 : ch.isASCII ? 0.56 : 0.98
        let adv = em * size * sc + CGFloat(rng.next(-1, 2))
        glyphs.append((String(ch), sc, adv, CGFloat(rng.next(-0.05, 0.05)) * size, rng.next(-0.06, 0.06)))
        width += adv
      }
      var x = leading ? p.x : p.x - width / 2
      let y = p.y + (CGFloat(li) - CGFloat(lines.count - 1) / 2) * size * 1.38 + CGFloat(rng.next(-2, 2))
      for (ch, sc, adv, lift, turn) in glyphs {
        var c = g
        c.translateBy(x: x + adv / 2, y: y + lift)
        c.rotate(by: .radians(turn))
        c.draw(Text(ch).font(handFont(size * sc)).foregroundStyle(color), at: .zero)
        x += adv
      }
    }
  }


  /// The scenes' drawing: 1000 points wide for what figures show, the ground running on.
  static let artW: CGFloat = 1600, artH: CGFloat = 900

  static func pt(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x, y: y) }
  static func clamp01(_ x: Double) -> Double { min(1, max(0, x)) }
  static func smooth(_ x: Double) -> Double { let u = clamp01(x); return u * u * (3 - 2 * u) }
  static func easeOut(_ x: Double) -> Double { let u = clamp01(x); return 1 - pow(1 - u, 3) }
  static func easeInOut(_ x: Double) -> Double {
    let u = clamp01(x)
    return u < 0.5 ? 4 * u * u * u : 1 - pow(-2 * u + 2, 3) / 2
  }
  /// Back-out: overshoots a little, then settles (things that pop in).
  static func pop(_ x: Double) -> Double {
    let u = clamp01(x), c1 = 1.70158, c3 = c1 + 1
    return 1 + c3 * pow(u - 1, 3) + c1 * pow(u - 1, 2)
  }
  /// Where `p` is through `a…b`, smoothed.
  static func span(_ p: Double, _ a: Double, _ b: Double) -> Double { smooth((p - a) / (b - a)) }
  static func lin(_ p: Double, _ a: Double, _ b: Double) -> Double { clamp01((p - a) / (b - a)) }
  static func mix(_ a: CGFloat, _ b: CGFloat, _ u: Double) -> CGFloat { a + (b - a) * CGFloat(u) }
  static func mix(_ a: CGPoint, _ b: CGPoint, _ u: Double) -> CGPoint { pt(mix(a.x, b.x, u), mix(a.y, b.y, u)) }
  static func mix(_ a: CGRect, _ b: CGRect, _ u: Double) -> CGRect {
    CGRect(x: mix(a.minX, b.minX, u), y: mix(a.minY, b.minY, u), width: mix(a.width, b.width, u), height: mix(a.height, b.height, u))
  }
  static func mix(_ a: (Double, Double, Double), _ b: (Double, Double, Double), _ u: Double) -> Color {
    Color(.sRGB, red: a.0 + (b.0 - a.0) * u, green: a.1 + (b.1 - a.1) * u, blue: a.2 + (b.2 - a.2) * u)
  }
  static func rgb(_ v: UInt32) -> (Double, Double, Double) {
    (Double(v >> 16 & 0xff) / 255, Double(v >> 8 & 0xff) / 255, Double(v & 0xff) / 255)
  }

  static func bezier(_ a: CGPoint, _ b: CGPoint, _ c: CGPoint, _ d: CGPoint, _ s: Double) -> CGPoint {
    let u = 1 - s
    let w0 = u * u * u, w1 = 3 * u * u * s, w2 = 3 * u * s * s, w3 = s * s * s
    return pt(CGFloat(w0) * a.x + CGFloat(w1) * b.x + CGFloat(w2) * c.x + CGFloat(w3) * d.x,
              CGFloat(w0) * a.y + CGFloat(w1) * b.y + CGFloat(w2) * c.y + CGFloat(w3) * d.y)
  }

  /// A wire as the app draws it: out of a port to the right, into one from the left.
  static func wire(_ a: CGPoint, _ b: CGPoint) -> (CGPoint, CGPoint, CGPoint, CGPoint) {
    let k = max(30, abs(b.x - a.x) * 0.5)
    return (a, pt(a.x + k, a.y), pt(b.x - k, b.y), b)
  }

  /// A polyline sampled from anything, measurable by length (threads, beads on them).
  struct Polyline {
    var points: [CGPoint]
    var lengths: [CGFloat] = []
    init(_ points: [CGPoint]) {
      self.points = points
      var acc: CGFloat = 0
      lengths = [0]
      for i in 1..<max(1, points.count) {
        acc += hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y)
        lengths.append(acc)
      }
    }
    private var total: CGFloat { lengths.last ?? 0 }
    func at(_ f: Double) -> CGPoint {
      guard points.count > 1 else { return points.first ?? .zero }
      let d = CGFloat(clamp01(f)) * total
      var i = 1
      while i < lengths.count - 1 && lengths[i] < d { i += 1 }
      let seg = max(0.0001, lengths[i] - lengths[i - 1])
      return mix(points[i - 1], points[i], Double((d - lengths[i - 1]) / seg))
    }
    func path(upTo f: Double = 1) -> Path {
      var p = Path()
      guard let first = points.first else { return p }
      p.move(to: first)
      let d = CGFloat(clamp01(f)) * total
      for i in 1..<points.count {
        if lengths[i] <= d { p.addLine(to: points[i]) } else { p.addLine(to: at(f)); break }
      }
      return p
    }
    /// Samples a cubic.
    static func curve(_ a: CGPoint, _ b: CGPoint, _ c: CGPoint, _ d: CGPoint, n: Int = 24) -> [CGPoint] {
      (0...n).map { bezier(a, b, c, d, Double($0) / Double(n)) }
    }
  }

  /// Hand-drawn outlines: a rounded rectangle traced with a wobble that can
  /// fade to zero (the sketch becoming a Card).
  static func sketchRect(_ r: CGRect, radius: CGFloat, wobble: CGFloat, seed: Double, n: Int = 120) -> [CGPoint] {
    let rr = min(radius, min(r.width, r.height) / 2)
    // perimeter, clockwise from the top-left corner's end
    let straightW = r.width - 2 * rr, straightH = r.height - 2 * rr
    let arc = CGFloat.pi / 2 * rr
    let per = 2 * straightW + 2 * straightH + 4 * arc
    func point(_ d0: CGFloat) -> (CGPoint, CGPoint) {  // point, outward normal
      var d = d0.truncatingRemainder(dividingBy: per)
      let segs: [(CGFloat, (CGFloat) -> (CGPoint, CGPoint))] = [
        (straightW, { s in (pt(r.minX + rr + s, r.minY), pt(0, -1)) }),
        (arc, { s in let a = -CGFloat.pi / 2 + s / max(rr, 0.001); return (pt(r.maxX - rr + rr * cos(a), r.minY + rr + rr * sin(a)), pt(cos(a), sin(a))) }),
        (straightH, { s in (pt(r.maxX, r.minY + rr + s), pt(1, 0)) }),
        (arc, { s in let a = s / max(rr, 0.001); return (pt(r.maxX - rr + rr * cos(a), r.maxY - rr + rr * sin(a)), pt(cos(a), sin(a))) }),
        (straightW, { s in (pt(r.maxX - rr - s, r.maxY), pt(0, 1)) }),
        (arc, { s in let a = CGFloat.pi / 2 + s / max(rr, 0.001); return (pt(r.minX + rr + rr * cos(a), r.maxY - rr + rr * sin(a)), pt(cos(a), sin(a))) }),
        (straightH, { s in (pt(r.minX, r.maxY - rr - s), pt(-1, 0)) }),
        (arc, { s in let a = CGFloat.pi + s / max(rr, 0.001); return (pt(r.minX + rr + rr * cos(a), r.minY + rr + rr * sin(a)), pt(cos(a), sin(a))) }),
      ]
      for (len, f) in segs {
        if d <= len { return f(d) }
        d -= len
      }
      return segs[0].1(0)
    }
    // a little overshoot past the start, like a marker that doesn't quite close
    let overshoot: CGFloat = wobble > 0.01 ? 0.04 : 0
    return (0...n).map { i in
      let f = CGFloat(i) / CGFloat(n) * (1 + overshoot)
      let (q, nrm) = point(f * per)
      let w = wobble * CGFloat(sin(Double(f) * 13 + seed) * 0.6 + sin(Double(f) * 29 + seed * 1.7) * 0.4)
        + wobble * 1.4 * CGFloat(Double(f) > 1 ? (Double(f) - 1) * 25 : 0)
      return pt(q.x + nrm.x * w, q.y + nrm.y * w)
    }
  }

  static func linePath(_ pts: [CGPoint]) -> Path {
    var p = Path()
    guard let f = pts.first else { return p }
    p.move(to: f)
    for q in pts.dropFirst() { p.addLine(to: q) }
    return p
  }

  enum Ink {
    static let cream = TsugumiPalette.cream, mist = TsugumiPalette.mist
    static let paper = rgb(0xf4ecdc), canvas = rgb(0x1d1e21)
    static let penC = TsugumiPalette.hex(0x2b3a40)
    static let pen = rgb(0x2b3a40), cardFill = rgb(0x2b2c30), cardStroke = rgb(0x6d7177)
    static let wire = TsugumiPalette.hex(0xe0893a)
    static let done = TsugumiPalette.hex(0x5fbf6a)
    static let badge = TsugumiPalette.hex(0xf0a050)
    static let ai = rgb(0x8fc6d6)
  }



  /// The bird (facing right, feet at `at`) and, optionally, mirrored.
  static func drawBird(_ g: GraphicsContext, at: CGPoint, scale: CGFloat, pose: BirdPose, mirrored: Bool = false,
                holding: ((GraphicsContext) -> Void)? = nil) {
    var g = g
    if mirrored {
      g.translateBy(x: at.x, y: 0); g.scaleBy(x: -1, y: 1); g.translateBy(x: -at.x, y: 0)
    }
    TsugumiBird.draw(g, at: at, scale: scale, pose: pose, holding: holding)
  }

  /// Where the bill holds the thread, in scene coordinates.
  static func birdGape(at: CGPoint, scale: CGFloat, pose: BirdPose, mirrored: Bool = false) -> CGPoint {
    let q = TsugumiBird.point(TsugumiBird.gape, at: at, scale: scale, pose: pose)
    return mirrored ? pt(2 * at.x - q.x, q.y) : q
  }

  /// A flying pose: wings beating, leaning into the flight.
  static func flying(_ t: Double, lean: Double = 0.12) -> BirdPose {
    var p = BirdPose()
    p.flap = 0.5 + 0.5 * sin(t * 22)
    p.tilt = lean
    return p
  }

  /// Threads in the key visual's colour (with a soft shadow).
  static func drawThread(_ g: GraphicsContext, _ path: Path, width: CGFloat = 2.4, glow: Double = 0) {
    var shadow = g
    shadow.addFilter(.blur(radius: 1.4))
    shadow.stroke(path.offsetBy(dx: 1.5, dy: 2.5), with: .color(.black.opacity(0.28)), style: StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round))
    if glow > 0 {
      var gl = g
      gl.addFilter(.blur(radius: 6))
      gl.stroke(path, with: .color(TsugumiPalette.thread.opacity(0.7 * glow)), style: StrokeStyle(lineWidth: width * 3, lineCap: .round))
    }
    g.stroke(path, with: .color(TsugumiPalette.thread), style: StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round))
  }
}

// Drawing shorthands of the explainer's scenes.
extension GraphicsContext {
  func text(_ s: String, at p: CGPoint, size: CGFloat, weight: Font.Weight = .regular, color: Color = Explainer.Ink.cream,
            anchor: UnitPoint = .center, opacity: Double = 1, font: Font? = nil, mono: Bool = false) {
    guard opacity > 0.002 else { return }
    var g = self
    g.opacity *= opacity
    let f = font ?? .system(size: size, weight: weight, design: mono ? .monospaced : .default)
    // lines one by one, so several lines centre on each other
    let lines = s.split(separator: "\n", omittingEmptySubsequences: false)
    let lh = size * 1.3
    for (i, line) in lines.enumerated() {
      let y = p.y + (CGFloat(i) - CGFloat(lines.count - 1) / 2) * lh
      g.draw(Text(String(line)).font(f).foregroundStyle(color), at: CGPoint(x: p.x, y: y), anchor: anchor)
    }
  }
  func poly(_ pts: [CGPoint], _ color: Color) {
    var p = Path(); p.addLines(pts); p.closeSubpath()
    fill(p, with: .color(color))
    stroke(p, with: .color(color), lineWidth: 0.8)
  }
  /// A copy faded to `opacity` (multiplies).
  func faded(_ opacity: Double) -> GraphicsContext { var g = self; g.opacity *= opacity; return g }
}
