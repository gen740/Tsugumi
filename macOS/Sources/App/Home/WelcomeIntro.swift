import SwiftUI

// The welcome on the first home of a launch: the bird flies in, pecks the
// end of the temari's thread and tosses it up; the lit end spins "tsugumi"
// by itself; the temari settles on the i; then the name shrinks into its
// place on home and the bird flies to its perch there, while the rest of
// home comes up around them (ProjectChooser). A click skips to that move.

/// Plays once per launch, and not when motion is reduced.
@MainActor enum WelcomeIntro {
  static var played = false
  /// Whether this home should play it (marks it played).
  static func take(reduceMotion: Bool) -> Bool {
    guard !played else { return false }
    played = true
    return !AppPreferences.shared.tsugumiStill(reduceMotion: reduceMotion)
  }
}

/// The welcome drawn over home, in home's coordinate space: `homeWord` and
/// `homeScene` are where home's name and bird scene sit.
struct WelcomeOverlay: View {
  let t: Double
  let homeWord: CGRect
  let homeScene: CGRect
  let onSkip: () -> Void

  var body: some View {
    GeometryReader { geo in
      let L = WelcomeLayout(size: geo.size)
      let e = ease(span(t, WelcomeBeat.toHome))
      ZStack(alignment: .topLeading) {
        LowPolyBackground().opacity(1 - e)
        WelcomeScene(t: t, layout: L, front: false).opacity(1 - e)
        let w = homeWord == .zero ? L.wordFrame : mixRect(L.wordFrame, homeWord, e)
        TsugumiWordmark(weight: 6 + 0.5 * CGFloat(e), progress: ease(span(t, WelcomeBeat.write)),
                        dot: t >= WelcomeBeat.dot.upperBound)
          .frame(width: w.width, height: w.height)
          .offset(x: w.minX, y: w.minY)
        WelcomeScene(t: t, layout: L, front: true, homeFeet: homeFeet)
        Text("クリックでスキップ").font(.caption).foregroundStyle(TsugumiPalette.cream.opacity(0.45 * (1 - e)))
          .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing).padding(14)
          .allowsHitTesting(false)
      }
      .environment(\.colorScheme, .dark)
      .contentShape(Rectangle())
      .onTapGesture { if t < WelcomeBeat.toHome.lowerBound { onSkip() } }
    }
    .accessibilityElement()
    .accessibilityLabel(Text(verbatim: "Tsugumi"))
    .accessibilityHint("クリックでスキップ")
  }

  /// Home's bird stands where TsugumiTemariScene puts its feet.
  private var homeFeet: CGPoint? {
    guard homeScene != .zero else { return nil }
    let fx = homeScene.width / 2 + (TsugumiBird.feet.x - (TsugumiBird.tailTip + TsugumiBird.beakTip) / 2) * 0.19
    return CGPoint(x: homeScene.minX + fx, y: homeScene.maxY - 25)
  }
}

/// When each beat happens (seconds from the start).
enum WelcomeBeat {
  static let flyIn = 0.4...2.3       // flaps in, lands by the temari
  static let peck = 2.5...3.3        // pecks the thread's end up
  static let flick = 3.4...4.1       // tosses it up: the end flies off, alight
  static let write = 4.1...6.3       // the lit end spins the name by itself
  static let tie = 6.3...6.8         // the end is tied off: a burst of light
  static let dot = 6.8...7.7         // the temari flies up onto the i
  static let toHome = 8.2...9.8      // the name and the bird go to their places on home
}

fileprivate func ease(_ x: Double) -> Double { let x = min(1, max(0, x)); return x * x * (3 - 2 * x) }
fileprivate func span(_ t: Double, _ r: ClosedRange<Double>) -> Double { min(1, max(0, (t - r.lowerBound) / (r.upperBound - r.lowerBound))) }
fileprivate func mix(_ a: CGPoint, _ b: CGPoint, _ f: Double) -> CGPoint {
  CGPoint(x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f)
}

/// Where things are, for a window of `size`.
struct WelcomeLayout {
  let size: CGSize
  static let birdScale: CGFloat = 0.2
  var wordWidth: CGFloat { min(520, size.width * 0.54) }
  var k: CGFloat { wordWidth / TsugumiWordmark.bounds.width }
  var wordFrame: CGRect {
    let h = TsugumiWordmark.bounds.height * k
    // the perch (250 to the left of the word) and the word, centred together
    return CGRect(x: (size.width - wordWidth - 250) / 2 + 250, y: size.height * 0.46 - h / 2, width: wordWidth, height: h)
  }
  /// A point of the word (its own units) in the window.
  func w(_ p: CGPoint) -> CGPoint {
    let b = TsugumiWordmark.bounds
    return CGPoint(x: wordFrame.minX + (p.x - b.minX) * k, y: wordFrame.minY + (p.y - b.minY) * k)
  }
  var start: CGPoint { w(CGPoint(x: -44, y: -70)) }
  var end: CGPoint { w(CGPoint(x: 456, y: -6)) }
  var perchY: CGFloat { w(.zero).y + 46 }
  var perch: ClosedRange<CGFloat> { (start.x - 250)...(start.x - 40) }
  var temariRadius: CGFloat { 70 * Self.birdScale }
  var temari: CGPoint { CGPoint(x: start.x - 76, y: perchY - 3 - temariRadius) }
  /// Where the bird lands: its bill over the temari, as in the key visual.
  var landing: CGPoint { CGPoint(x: temari.x - 199 * Self.birdScale, y: perchY - 3) }
  var dot: CGPoint { w(TsugumiWordmark.iDot) }
}

/// The word sampled along its line (its own units), with the furthest x
/// reached so far at each sample: the written part grows to the right as the
/// bird flies over, whatever the line does in its loops.
fileprivate enum WordTrace {
  static let n = 300
  static let points: [CGPoint] = (0...n).map {
    TsugumiWordmark.path.trimmedPath(from: 0, to: Double($0) / Double(n)).currentPoint ?? CGPoint(x: -44, y: -70)
  }
  static let reach: [CGFloat] = {
    var m = -CGFloat.infinity
    return points.map { m = max(m, $0.x); return m }
  }()
  static func tip(_ f: Double) -> CGPoint { points[min(n, max(0, Int((f * Double(n)).rounded())))] }
  static func reachX(_ f: Double) -> CGFloat { reach[min(n, max(0, Int((f * Double(n)).rounded())))] }
}

/// The bill relative to the feet, in a pose.
fileprivate func billOffset(_ pose: BirdPose) -> CGPoint {
  TsugumiBird.point(TsugumiBird.gape, at: .zero, scale: WelcomeLayout.birdScale, pose: pose)
}

/// Flying with strong beats: a slow upstroke raising the wing high, a
/// quick hard downstroke past the body. Only the wing moves; the body keeps
/// steady (moving it with every beat looks jerky).
fileprivate func powerFlight(_ t: Double, rate: Double = 3.0) -> BirdPose {
  var p = BirdPose()
  let u = (t * rate).truncatingRemainder(dividingBy: 1)
  let up = 0.62   // share of a beat spent raising the wing
  if u < up {
    p.flap = -0.35 + 2.55 * ease(u / up)
  } else {
    let d = (u - up) / (1 - up)
    p.flap = 2.2 - 2.55 * d * d * (3 - 2 * d)
  }
  p.tilt = 0.08
  return p
}

/// A flying pose: wings beating, the body tipped a little forward.
fileprivate func flying(_ t: Double, rate: Double = 3.6) -> BirdPose {
  var p = BirdPose()
  p.flap = 0.5 + 0.5 * sin(t * 2 * .pi * rate)
  p.tilt = 0.08
  return p
}

/// Everything but the word: the perch, the temari, the thread, the bird,
/// and the magic (in front of the word: drawn with `front`).
struct WelcomeScene: View {
  let t: Double
  let layout: WelcomeLayout
  let front: Bool
  /// Where home's bird stands (its feet), once known.
  var homeFeet: CGPoint? = nil

  var body: some View {
    Canvas { g, _ in
      if front { drawMagic(g); drawBird(g) } else { drawBack(g) }
    }
  }

  private var writing: Double { ease(span(t, WelcomeBeat.write)) }
  private var flick: Double { span(t, WelcomeBeat.flick) }

  /// The temari: on the perch, unwinding while the name is spun, then up
  /// onto the i.
  private var temari: (center: CGPoint, radius: CGFloat, spin: Double) {
    let L = layout
    let r0 = L.temariRadius, r1 = 10.5 * L.k
    let f = ease(span(t, WelcomeBeat.dot))
    let r = r0 * CGFloat(1 - 0.2 * writing) + (r1 - r0 * 0.8) * CGFloat(f)
    var c = mix(L.temari, L.dot, f)
    c.y -= CGFloat(sin(f * .pi)) * 110     // an arc
    return (c, r, writing * 7 * .pi + f * 3 * .pi)
  }

  private var feet: CGPoint { layout.landing }

  /// The bird's pose: lands, pecks, tosses the end up, then watches; a
  /// happy hop when the end is tied off.
  private var pose: BirdPose {
    var p = BirdPose()
    if t < WelcomeBeat.peck.lowerBound {
      let f = ease(span(t, WelcomeBeat.flyIn))
      if f < 0.92 { p = powerFlight(t) } else { p.squash = CGFloat(1 - f) * 0.8 }
    } else if t < WelcomeBeat.peck.upperBound {
      let u = span(t, WelcomeBeat.peck)
      p.tilt = 0.42 * sin(u * .pi)
      p.squash = 0.08 * sin(u * .pi)
    } else if t < WelcomeBeat.flick.upperBound {
      // a quick toss of the head, up and back
      p.tilt = -0.38 * sin(flick * .pi)
      p.flap = 0.4 * sin(flick * .pi)
    } else if t >= WelcomeBeat.tie.lowerBound && t < WelcomeBeat.tie.upperBound + 0.2 {
      p = BirdMotion.hop(span(t, WelcomeBeat.tie.lowerBound...(WelcomeBeat.tie.upperBound + 0.2)), height: 16)
    } else {
      // watching the light: the head follows it up a little
      p.tilt = -0.06 - 0.05 * sin(t * 1.7)
    }
    p.blink = max(p.blink, BirdMotion.blink(t, every: 2.9, offset: 0.5))
    return p
  }

  /// Where the flying end is: from the bill, tossed in an arc to the t, then
  /// along the name.
  /// The toss: the end leaves the bill where the head comes up, flies a
  /// thrown arc (a parabola, at an even pace: gravity) and comes down on the
  /// t's start.
  private var release: CGPoint {
    TsugumiBird.point(TsugumiBird.gape, at: feet, scale: WelcomeLayout.birdScale, pose: BirdPose())
  }
  private var tossControl: CGPoint {
    let r = release, s = layout.start
    // forward and not too high: the apex of the arc is half this lift
    return CGPoint(x: r.x + (s.x - r.x) * 0.6, y: min(r.y, s.y) - 90)
  }
  private static let tossFrom = 0.3   // share of the flick before the end leaves the bill
  private var tossing: Double? {
    guard WelcomeBeat.flick.contains(t), flick > Self.tossFrom else { return nil }
    return (flick - Self.tossFrom) / (1 - Self.tossFrom)
  }
  private func quad(_ a: CGPoint, _ c: CGPoint, _ b: CGPoint, _ f: Double) -> CGPoint {
    let f = CGFloat(f)
    return CGPoint(x: (1 - f) * (1 - f) * a.x + 2 * (1 - f) * f * c.x + f * f * b.x,
                   y: (1 - f) * (1 - f) * a.y + 2 * (1 - f) * f * c.y + f * f * b.y)
  }

  /// Where the lit end is: in the air on its arc, then along the name.
  private var tip: CGPoint? {
    if let f = tossing { return quad(release, tossControl, layout.start, f) }
    if WelcomeBeat.write.contains(t) { return layout.w(WordTrace.tip(writing)) }
    return nil
  }

  private func drawBack(_ g: GraphicsContext) {
    let L = layout
    var g = g
    g.opacity = ease(span(t, 0...0.4))
    TsugumiBranch.draw(g, x0: L.perch.lowerBound, x1: L.perch.upperBound, y: L.perchY, grown: 1, time: t)
    let lead = TsugumiTemari.lead(center: temari.center, radius: temari.radius)
    let style = StrokeStyle(lineWidth: 6 * L.k, lineCap: .round)
    // the thread: from the temari up to where the bill let go, then along
    // the arc the end flew; once the end is down, the thread's weight pulls
    // that point down and the arc slackens
    if t >= WelcomeBeat.flick.lowerBound + Self.tossFrom * (WelcomeBeat.flick.upperBound - WelcomeBeat.flick.lowerBound) && t < WelcomeBeat.dot.lowerBound {
      let s0 = L.start, r = release, c = tossControl
      let fall = ease(span(t, WelcomeBeat.write.lowerBound...(WelcomeBeat.write.lowerBound + 0.7)))
      let slack = CGPoint(x: (lead.x + s0.x) / 2 - 10, y: max(lead.y, s0.y) + 6)
      let joint = mix(r, slack, fall)
      var p = Path()
      p.move(to: lead)
      p.addQuadCurve(to: joint, control: CGPoint(x: (lead.x + joint.x) / 2, y: max(lead.y, joint.y) + 8))
      if let f = tossing {
        // the part of the arc flown so far
        p.addQuadCurve(to: quad(r, c, s0, f), control: mix(r, c, f))
      } else {
        let restC = CGPoint(x: s0.x - 46, y: s0.y + 2)
        p.addQuadCurve(to: s0, control: mix(c, restC, fall))
      }
      g.stroke(p, with: .color(TsugumiPalette.thread), style: style)
    }
    if t < WelcomeBeat.dot.upperBound {
      TsugumiTemari.draw(g, center: temari.center, radius: temari.radius, spin: temari.spin)
    }
  }

  private func drawBird(_ g: GraphicsContext) {
    let L = layout
    let s = WelcomeLayout.birdScale
    var at = feet
    var p = pose
    var scale = s
    if t >= WelcomeBeat.toHome.lowerBound, let home = homeFeet {
      // to home: strong beats up and over, gliding down onto its place there
      let f = ease(span(t, WelcomeBeat.toHome))
      at = mix(L.landing, home, f)
      at.y -= CGFloat(sin(f * .pi)) * 90
      scale = s + (0.19 - s) * CGFloat(f)
      if f > 0.02 && f < 0.94 { p = powerFlight(t) } else if f >= 0.94 { p = BirdPose(); p.squash = CGFloat(1 - f) * 1.2 }
      TsugumiBird.draw(g, at: at, scale: scale, pose: p)
      return
    }
    if t < WelcomeBeat.peck.lowerBound {
      let f = ease(span(t, WelcomeBeat.flyIn))
      at = mix(CGPoint(x: -60, y: L.size.height * 0.18), L.landing, f)
      at.y -= CGFloat(sin(f * .pi)) * 40
    }
    // the thread in the bill from the peck until it is tossed
    let holding = t > (WelcomeBeat.peck.lowerBound + WelcomeBeat.peck.upperBound) / 2 && tossing == nil && t < WelcomeBeat.flick.lowerBound + Self.tossFrom * (WelcomeBeat.flick.upperBound - WelcomeBeat.flick.lowerBound)
    let bill = TsugumiBird.point(TsugumiBird.gape, at: at, scale: scale, pose: p)
    let lead = TsugumiTemari.lead(center: temari.center, radius: temari.radius)
    TsugumiBird.draw(g, at: at, scale: scale, pose: p) { g in
      guard holding else { return }
      var path = Path()
      path.move(to: lead)
      path.addQuadCurve(to: bill, control: CGPoint(x: (lead.x + bill.x) / 2, y: max(lead.y, bill.y) + 6))
      g.stroke(path, with: .color(TsugumiPalette.thread), style: StrokeStyle(lineWidth: 6 * L.k, lineCap: .round))
    }
  }

  // MARK: the magic

  private static let light = Color(.sRGB, red: 1, green: 0.95, blue: 0.78)

  private func drawMagic(_ g: GraphicsContext) {
    let L = layout
    var g = g
    g.blendMode = .plusLighter
    // the freshly spun part of the name glows, fading behind the end
    if WelcomeBeat.write.contains(t) || (t >= WelcomeBeat.write.upperBound && t < WelcomeBeat.write.upperBound + 0.6) {
      let w = writing
      let fade = t > WelcomeBeat.write.upperBound ? 1 - (t - WelcomeBeat.write.upperBound) / 0.6 : 1
      let xf = CGAffineTransform(translationX: L.wordFrame.minX, y: L.wordFrame.minY).scaledBy(x: L.k, y: L.k)
        .translatedBy(x: -TsugumiWordmark.bounds.minX, y: -TsugumiWordmark.bounds.minY)
      for (back, a) in [(0.10, 0.25), (0.05, 0.4), (0.02, 0.6)] {
        let seg = TsugumiWordmark.path.trimmedPath(from: max(0, w - back), to: w).applying(xf)
        var glow = g
        glow.addFilter(.blur(radius: 6))
        glow.stroke(seg, with: .color(Self.light.opacity(a * fade)), style: StrokeStyle(lineWidth: 12 * L.k, lineCap: .round, lineJoin: .round))
      }
    }
    // the lit end
    if let p = tip { orb(g, at: p, size: 1, time: t) }
    // sparkles shed along the way: born where the end was, drifting up, fading
    let n = 60
    for i in 0..<n {
      let born = WelcomeBeat.write.lowerBound + (WelcomeBeat.write.upperBound - WelcomeBeat.write.lowerBound) * Double(i) / Double(n)
      let age = t - born
      guard age > 0, age < 1.4 else { continue }
      var rng = SeededRandom(seed: UInt64(i + 11))
      let at = L.w(WordTrace.tip(ease(span(born, WelcomeBeat.write))))
      let dx = CGFloat(rng.next(-18, 18)), rise = CGFloat(rng.next(14, 34))
      let q = CGPoint(x: at.x + dx * CGFloat(age), y: at.y + CGFloat(rng.next(-6, 6)) - rise * CGFloat(age))
      star(g, at: q, r: CGFloat(rng.next(2, 4.2)) * CGFloat(1 - age / 1.4), alpha: 1 - age / 1.4, turn: age * 2)
    }
    // the end is tied off: a burst at the swash's end
    burst(g, at: L.end, from: WelcomeBeat.tie.lowerBound, seed: 3)
    // the temari's trail on its way to the i, and a burst where it lands
    if WelcomeBeat.dot.contains(t) {
      for k in 1...8 {
        let back = Double(k) * 0.035
        let tt = t - back
        guard tt > WelcomeBeat.dot.lowerBound else { continue }
        let f = ease(span(tt, WelcomeBeat.dot))
        var c = mix(L.temari, L.dot, f)
        c.y -= CGFloat(sin(f * .pi)) * 110
        star(g, at: c, r: 3.4 - CGFloat(k) * 0.3, alpha: 0.8 - Double(k) * 0.09, turn: tt * 3)
      }
    }
    burst(g, at: L.dot, from: WelcomeBeat.dot.upperBound, seed: 9)
  }

  /// A small glowing ball of light with a turning four-point star.
  private func orb(_ g: GraphicsContext, at p: CGPoint, size: CGFloat, time: Double) {
    let r = 16 * size
    g.fill(Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r)),
           with: .radialGradient(Gradient(colors: [Self.light.opacity(0.9), Self.light.opacity(0.25), .clear]),
                                 center: p, startRadius: 0, endRadius: r))
    star(g, at: p, r: 7 * size, alpha: 1, turn: time * 2.5)
    g.fill(Path(ellipseIn: CGRect(x: p.x - 2.5, y: p.y - 2.5, width: 5, height: 5)), with: .color(.white))
  }

  /// A four-point star.
  private func star(_ g: GraphicsContext, at p: CGPoint, r: CGFloat, alpha: Double, turn: Double) {
    guard r > 0.3, alpha > 0 else { return }
    var path = Path()
    for i in 0..<8 {
      let a = turn + Double(i) * .pi / 4
      let rr = i % 2 == 0 ? r : r * 0.28
      let q = CGPoint(x: p.x + rr * CGFloat(cos(a)), y: p.y + rr * CGFloat(sin(a)))
      if i == 0 { path.move(to: q) } else { path.addLine(to: q) }
    }
    path.closeSubpath()
    g.fill(path, with: .color(Self.light.opacity(alpha)))
  }

  /// A ring of light and stars thrown out from `p`, starting at `from`.
  private func burst(_ g: GraphicsContext, at p: CGPoint, from: Double, seed: UInt64) {
    let age = t - from
    guard age > 0, age < 0.9 else { return }
    let f = age / 0.9
    let ring = CGFloat(8 + 40 * ease(f))
    g.stroke(Path(ellipseIn: CGRect(x: p.x - ring, y: p.y - ring, width: 2 * ring, height: 2 * ring)),
             with: .color(Self.light.opacity(0.5 * (1 - f))), lineWidth: 2)
    var rng = SeededRandom(seed: seed)
    for _ in 0..<12 {
      let a = rng.next(0, 2 * .pi), d = CGFloat(rng.next(20, 52)) * CGFloat(ease(f))
      star(g, at: CGPoint(x: p.x + d * CGFloat(cos(a)), y: p.y + d * CGFloat(sin(a))), r: CGFloat(rng.next(2.5, 4.5)) * CGFloat(1 - f),
           alpha: 1 - f, turn: a + age * 3)
    }
  }
}

fileprivate func mixRect(_ a: CGRect, _ b: CGRect, _ f: Double) -> CGRect {
  let f = CGFloat(f)
  return CGRect(x: a.minX + (b.minX - a.minX) * f, y: a.minY + (b.minY - a.minY) * f,
                width: a.width + (b.width - a.width) * f, height: a.height + (b.height - a.height) * f)
}
