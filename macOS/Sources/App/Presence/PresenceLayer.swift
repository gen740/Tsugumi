import AppKit
import SwiftUI

/// Colours of the Agent's thread: unbleached cotton on the dark canvas, deep
/// teal on the light one; `ink` is what settles into a written Card.
enum PresenceColor {
  static let thread = dynamic(dark: NSColor(red: 0.94, green: 0.88, blue: 0.76, alpha: 1),
                              light: NSColor(red: 0.25, green: 0.50, blue: 0.45, alpha: 1))
  static let ink = dynamic(dark: NSColor(red: 0.30, green: 0.55, blue: 0.50, alpha: 1),
                           light: NSColor(red: 0.50, green: 0.71, blue: 0.66, alpha: 1))

  /// The main Agent's bird while it has helpers out: gold, to tell it apart.
  static let lead = dynamic(dark: NSColor(red: 0.98, green: 0.80, blue: 0.35, alpha: 1),
                            light: NSColor(red: 0.72, green: 0.52, blue: 0.05, alpha: 1))

  /// The thread of a bird: the main Agent's (0), then one hue per helper.
  static func of(_ tint: Int) -> Color {
    guard tint > 0 else { return thread }
    let hues: [Double] = [0.08, 0.58, 0.83, 0.13, 0.95, 0.45, 0.70, 0.30]
    let h = hues[(tint - 1) % hues.count]
    return dynamic(dark: NSColor(hue: h, saturation: 0.45, brightness: 0.95, alpha: 1),
                   light: NSColor(hue: h, saturation: 0.75, brightness: 0.62, alpha: 1))
  }

  static func dynamic(dark: NSColor, light: NSColor) -> Color {
    Color(nsColor: NSColor(name: nil) { $0.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? dark : light })
  }
}

/// Where things are on the canvas, for the bird and the threads (world units).
struct PresenceGeometry {
  var card: (CardKey) -> CGRect?  // nil: not in the scope shown
  var wire: (AgentPresence.Wire) -> Path?
  var screen: (CGPoint) -> CGPoint
  var zoom: Double
}

/// The bird's place and doing at one moment (world units).
struct AvatarPose {
  enum Mood { case perched, reading, writing, flying }
  var center: CGPoint
  var mood: Mood
  var facing: CGFloat = 1
}

/// Bird geometry in icon units scaled to world units: from its feet and
/// from its beak tip to the middle of its body (what moves).
enum BirdMetrics {
  static let scale: CGFloat = 0.075
  static let feetToCenter = CGVector(dx: (500 - 508) * scale, dy: (480 - 706) * scale)
  static let beakToCenter = CGVector(dx: (500 - 776) * scale, dy: (480 - 394) * scale)
}

private func + (p: CGPoint, v: CGVector) -> CGPoint { CGPoint(x: p.x + v.dx, y: p.y + v.dy) }
private func lerp(_ a: CGPoint, _ b: CGPoint, _ u: Double) -> CGPoint {
  CGPoint(x: a.x + (b.x - a.x) * CGFloat(u), y: a.y + (b.y - a.y) * CGFloat(u))
}

/// A thread on its way: the wire's route up to `progress`, hanging by `sag`
/// in the middle (world units).
func threadPath(_ route: Path, progress: Double, sag: Double) -> Path {
  var out = Path()
  let n = 40, last = max(1, Int(Double(n) * progress))
  for k in 0...last {
    let u = Double(k) / Double(n)
    guard let p = route.trimmedPath(from: 0, to: max(0.0001, u)).currentPoint else { continue }
    let q = CGPoint(x: p.x, y: p.y + CGFloat(26 * sag * sin(.pi * u)))
    if out.isEmpty { out.move(to: q) } else { out.addLine(to: q) }
  }
  return out
}

/// Where the bird is during each scene: on a Card's top edge while the Agent
/// reads it, along the writing line while a Card is written out, at the head
/// of the thread while a wire is threaded; it flies between them unhurried.
@MainActor
struct Choreography {
  let presence: AgentPresence
  let g: PresenceGeometry

  func perch(_ k: CardKey) -> CGPoint? {
    g.card(k).map { CGPoint(x: $0.minX + 26, y: $0.minY) + BirdMetrics.feetToCenter }
  }

  func writing(_ k: CardKey, _ u: Double) -> CGPoint? {
    g.card(k).map { CGPoint(x: $0.minX + $0.width * CGFloat(u), y: $0.midY) + BirdMetrics.beakToCenter }
  }

  func head(_ w: AgentPresence.Wire, _ p: Double, sag: Double) -> CGPoint? {
    g.wire(w).flatMap { threadPath($0, progress: max(0.02, p), sag: sag).currentPoint }.map { $0 + BirdMetrics.beakToCenter }
  }

  /// Where a scene leaves the bird.
  func end(of s: AgentPresence.Scheduled) -> CGPoint? {
    switch s.beat {
    case .visit(let k), .run(let k), .write(let k, false): perch(k)
    case .write(let k, true): writing(k, 1)
    case .thread(let w): head(w, 1, sag: 0)
    }
  }

  /// Without motion: at each scene's place at once, no flight.
  func still(at t: Date) -> AvatarPose? {
    guard let (cur, _) = presence.current(at: t) else { return nil }
    let done = t.timeIntervalSince(cur.start) > cur.length + 0.2
    guard let at = done ? perch(cur.beat.card) : end(of: cur) else { return nil }
    return AvatarPose(center: at, mood: .perched)
  }

  /// The pose, facing the way it is going.
  func posed(at t: Date) -> AvatarPose? {
    guard var p = pose(at: t) else { return nil }
    let ahead = pose(at: t.addingTimeInterval(0.15))?.center ?? p.center
    p.facing = ahead.x < p.center.x - 2 ? -1 : 1
    return p
  }

  func pose(at t: Date) -> AvatarPose? {
    guard let (cur, prev) = presence.current(at: t) else { return nil }
    let local = t.timeIntervalSince(cur.start)
    // Nothing follows: it settles onto the top edge of the Card it was at.
    if local > cur.length + 0.2, let from = end(of: cur), let to = perch(cur.beat.card) {
      let u = (local - cur.length - 0.2) / 1.0
      if u < 1 && from != to {
        let e = AgentPresence.rise(u)
        let p = lerp(from, to, e)
        return AvatarPose(center: CGPoint(x: p.x, y: p.y - CGFloat(20 * sin(.pi * e))), mood: .flying)
      }
      return AvatarPose(center: to, mood: .perched)
    }
    let fly = { (a: CGPoint, b: CGPoint, u: Double) -> CGPoint in
      let e = AgentPresence.rise(u)
      let p = lerp(a, b, e)
      return CGPoint(x: p.x, y: p.y - CGFloat(20 * sin(.pi * e)))
    }
    typealias T = AgentPresence.Timing
    switch cur.beat {
    case .visit(let k), .run(let k), .write(let k, false):
      guard let to = perch(k) else { return nil }
      let from = prev.flatMap(end) ?? to
      if local < 1.0 && from != to { return AvatarPose(center: fly(from, to, local / 1.0), mood: .flying) }
      if case .write = cur.beat { return AvatarPose(center: to, mood: .writing) }
      return AvatarPose(center: to, mood: .reading)
    case .write(let k, true):
      guard let start = writing(k, 0) else { return nil }
      let from = prev.flatMap(end) ?? start
      if local < T.revealStart { return AvatarPose(center: lerp(from, start, AgentPresence.rise(local / T.revealStart)), mood: .flying) }
      let u = AgentPresence.ease((local - T.revealStart) / T.revealLength)
      return writing(k, u).map { AvatarPose(center: $0, mood: .writing) }
    case .thread(let w):
      guard case .drawing(let p, let sag) = presence.thread(w, at: t), let h = head(w, p, sag: sag) else {
        // Before the thread starts: to its beginning.
        guard let h0 = head(w, 0, sag: 1) else { return nil }
        let from = prev.flatMap(end) ?? h0
        return AvatarPose(center: lerp(from, h0, AgentPresence.rise(local / T.threadStart)), mood: .flying)
      }
      return AvatarPose(center: h, mood: .flying)
    }
  }
}

/// The Agent on the canvas: threads being drawn, and (unless switched off in
/// the settings) the tsugumi or a dot with its trail and a bubble with what
/// it is doing. Never takes a click.
struct PresenceLayer: View {
  let presence: AgentPresence
  let geometry: PresenceGeometry
  let avatar: AgentAvatar
  /// The main Agent while helpers work: larger, gold, named "Agent".
  var lead = false
  @Environment(\.accessibilityReduceMotion) private var systemReduceMotion
  /// Still: the app's "Tsugumi のアニメーション" (or the system's reduce motion).
  private var reduceMotion: Bool { AppPreferences.shared.tsugumiStill(reduceMotion: systemReduceMotion) }

  var body: some View {
    let _ = presence.revision
    let now = Date()
    let live = presence.animating(at: now)
    GeometryReader { geo in
      SwiftUI.TimelineView(.animation(paused: !live)) { ctx in
        scene(at: ctx.date, size: geo.size)
      }
    }
    .allowsHitTesting(false)
  }

  @ViewBuilder
  private func scene(at t: Date, size: CGSize) -> some View {
    let ch = Choreography(presence: presence, g: geometry)
    // The main Agent stands out among its helpers: larger and gold; the
    // helpers are a little smaller, each in its colour.
    let scale = lead ? 1.4 : presence.label != nil ? 0.8 : 1.0
    let z = CGFloat(geometry.zoom * scale)
    let color = lead ? PresenceColor.lead : PresenceColor.of(presence.tint)
    let label = lead ? "Agent" : presence.label
    ZStack(alignment: .topLeading) {
      threads(at: t)
      if avatar != .none, presence.visible(at: t), let pose = reduceMotion ? ch.still(at: t) : ch.posed(at: t) {
        let fade = presence.fade(at: t)
        // Fades in as it comes; at the end it rises a little as it fades away.
        let p = geometry.screen(pose.center).applying(CGAffineTransform(translationX: 0, y: CGFloat(-16 * fade.lift)))
        let inside = CGRect(origin: .zero, size: size).insetBy(dx: 8, dy: 8).contains(p)
        if !reduceMotion {
          Trail(points: (0...18).compactMap { k in ch.pose(at: t.addingTimeInterval(-0.045 * Double(k))).map { geometry.screen($0.center) } },
                strong: avatar == .dot, zoom: z, color: color)
            .opacity(fade.opacity)
        }
        // Where the birds stand when there are several: a halo in its colour.
        if label != nil, avatar == .bird {
          Ellipse().fill(color.opacity(lead ? 0.45 : 0.32))
            .frame(width: 46 * z, height: 12 * z)
            .position(x: p.x, y: p.y + 24 * z)
            .opacity(fade.opacity)
        }
        figure(pose, at: t, zoom: z, color: color).position(p).opacity(fade.opacity)
        // Its name beside the bird (clear of the Card's title), in its colour.
        if let label {
          let w = label.reduce(CGFloat(12)) { $0 + ($1.isASCII ? 6.2 : 10.5) }
          Text(label).font(.system(size: lead ? 11 : 10, weight: lead ? .bold : .semibold)).foregroundStyle(color)
            .padding(.horizontal, 5).padding(.vertical, 1)
            .background(Capsule().fill(.regularMaterial))
            .fixedSize()
            .position(x: p.x + (avatar == .bird ? 30 * min(1.3, max(0.8, z)) : 10) + w / 2, y: p.y - (avatar == .bird ? 14 : 0))
            .opacity(fade.opacity)
        }
        if let line = presence.line(at: t) {
          let since = t.timeIntervalSince(line.since)
          let width = line.text.reduce(CGFloat(22)) { $0 + ($1.isASCII ? 6.9 : 11.8) }
          // A done line goes quietly before the bird does.
          let leaving = presence.working ? 1 : AgentPresence.rise((AgentPresence.Timing.doneLine - since) / 0.8)
          PresenceBubble(text: line.text, color: color)
            .opacity(min(1, since / 0.45) * leaving * fade.opacity)
            .offset(x: min(size.width - 12 - width, max(12, p.x - 36)),
                    y: p.y - (avatar == .bird ? 54 * min(1.3, max(0.8, z)) : 34) + CGFloat(5 * (1 - AgentPresence.rise(since / 0.45))))
        }
        if !inside { EdgeMark(at: p, in: size, color: color) }
      }
    }
    .frame(width: size.width, height: size.height, alignment: .topLeading)
  }

  @ViewBuilder
  private func figure(_ pose: AvatarPose, at t: Date, zoom: CGFloat, color: Color) -> some View {
    switch avatar {
    case .bird: BirdFigure(mood: pose.mood, t: t.timeIntervalSinceReferenceDate, facing: pose.facing, still: reduceMotion, zoom: zoom)
    case .dot: Circle().fill(color).frame(width: 8, height: 8).shadow(color: color.opacity(0.6), radius: 3)
    case .none: EmptyView()
    }
  }

  /// Wires being threaded, hanging and then pulled taut.
  private func threads(at t: Date) -> some View {
    Canvas { ctx, _ in
      let w = max(1, 1.6 * min(1.4, geometry.zoom))
      for s in presence.beats {
        guard case .thread(let wire) = s.beat, case .drawing(let p, let sag) = presence.thread(wire, at: t),
              let route = geometry.wire(wire) else { continue }
        let path = threadPath(route, progress: p, sag: sag).applying(CGAffineTransform(
          a: CGFloat(geometry.zoom), b: 0, c: 0, d: CGFloat(geometry.zoom),
          tx: geometry.screen(.zero).x, ty: geometry.screen(.zero).y))
        // The thread's colour while it is drawn, the wire's once it is taut.
        let color = p < 1 ? PresenceColor.of(presence.tint) : Color.secondary.opacity(0.75)
        ctx.stroke(path, with: .color(color), style: StrokeStyle(lineWidth: w, lineCap: .round, lineJoin: .round))
      }
    }
  }
}

/// Where the bird was over the last moment: a thread that thins and fades.
struct Trail: View {
  let points: [CGPoint]
  let strong: Bool
  let zoom: CGFloat
  var color = PresenceColor.thread
  var body: some View {
    Canvas { ctx, _ in
      let n = points.count
      guard n > 1 else { return }
      for k in 1..<n {
        let a = points[k - 1], b = points[k]
        guard hypot(a.x - b.x, a.y - b.y) > 0.2 else { continue }
        let f = 1 - Double(k) / Double(n)
        var seg = Path()
        seg.move(to: a)
        seg.addLine(to: b)
        ctx.stroke(seg, with: .color(color.opacity((strong ? 0.85 : 0.4) * f)),
                   style: StrokeStyle(lineWidth: ((strong ? 3 : 2) * f + 0.4) * min(1.3, max(0.7, zoom)), lineCap: .round))
      }
    }
  }
}

struct PresenceBubble: View {
  let text: String
  var color = PresenceColor.thread
  var body: some View {
    Text(text).font(.system(size: 11.5)).lineLimit(1)
      .padding(.horizontal, 10).padding(.vertical, 6)
      .background(RoundedRectangle(cornerRadius: 9).fill(.regularMaterial))
      .overlay(RoundedRectangle(cornerRadius: 9).strokeBorder(color.opacity(0.45), lineWidth: 1))
      .fixedSize()
  }
}

/// The bird is out of view: a small mark at the nearest edge, pointing to it.
struct EdgeMark: View {
  let at: CGPoint
  let size: CGSize
  let color: Color
  init(at: CGPoint, in size: CGSize, color: Color = PresenceColor.thread) { self.at = at; self.size = size; self.color = color }
  var body: some View {
    let x = min(size.width - 22, max(22, at.x)), y = min(size.height - 16, max(16, at.y))
    let symbol = at.x > size.width ? "chevron.right" : at.x < 0 ? "chevron.left" : at.y < 0 ? "chevron.up" : "chevron.down"
    HStack(spacing: 2) {
      Circle().fill(color).frame(width: 6, height: 6)
      Image(systemName: symbol).font(.system(size: 9, weight: .bold)).foregroundStyle(color)
    }
    .padding(.horizontal, 6).padding(.vertical, 4)
    .background(Capsule().fill(.regularMaterial))
    .position(x: x, y: y)
  }
}

/// The tsugumi of the app icon: wings beat in full strokes when it flies,
/// flutter now and then when it stands, the head pecks while it reads.
struct BirdFigure: View {
  let mood: AvatarPose.Mood
  let t: Double
  let facing: CGFloat
  let still: Bool
  let zoom: CGFloat

  var body: some View {
    Canvas { ctx, size in
      let flying = mood == .flying || mood == .writing
      let stroke = flying ? sin(t * (mood == .writing ? 19 : 23)) : 0
      let reach = mood == .writing ? 0.85 : 1.0
      let burst = t.truncatingRemainder(dividingBy: 2.0)
      let flutter = burst < 0.6 ? sin(.pi * burst / 0.6) : 0
      // Wing angle about the shoulder (positive raises it).
      let flap = still ? 0 : (flying ? (0.425 + 0.775 * stroke) * reach : (0.3 + 0.45 * sin(t * 34)) * flutter)
      let peck = !still && mood == .reading ? pow(max(0, sin(t * 3.2)), 2) * 12 : 0
      let tail = still ? 0 : sin(t * 3.2) * 0.09 + (flying ? stroke * 0.12 : 0)
      let lift = still ? 0 : (flying ? -(0.5 - 0.5 * stroke) * 26 : sin(t * 1.8) * 4)
      ctx.translateBy(x: size.width / 2, y: size.height / 2)
      ctx.scaleBy(x: BirdMetrics.scale * zoom * facing, y: BirdMetrics.scale * zoom)
      ctx.translateBy(x: -500, y: -480 + lift)

      func draw(_ list: [BirdPolygon], _ transform: CGAffineTransform = .identity) {
        for p in list {
          var path = Path()
          path.addLines(p.points.map { $0.applying(transform) })
          path.closeSubpath()
          ctx.fill(path, with: .color(Color(red: p.color.0, green: p.color.1, blue: p.color.2).opacity(p.opacity)))
        }
      }
      func rotate(about c: CGPoint, _ a: Double) -> CGAffineTransform {
        CGAffineTransform(translationX: c.x, y: c.y).rotated(by: a).translatedBy(x: -c.x, y: -c.y)
      }
      if !flying { draw(BirdArt.legs) }
      draw(BirdArt.tail, rotate(about: CGPoint(x: 330, y: 480), tail))
      draw(BirdArt.body)
      draw(BirdArt.spots)
      let head = CGAffineTransform(translationX: peck * 0.4, y: peck)
      draw(BirdArt.head, head)
      draw(BirdArt.eye, head)
      draw(BirdArt.beak, head)
      draw(BirdArt.wing, rotate(about: CGPoint(x: 545, y: 402), flap))
    }
    .frame(width: 76 * zoom, height: 64 * zoom)
  }
}
