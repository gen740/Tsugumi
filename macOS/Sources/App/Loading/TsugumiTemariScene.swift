import SwiftUI

/// Home's bird with the key visual's temari. One cycle: the ball rolls away
/// along the perch, paying out thread; the bird lets go and the end curls up
/// over the ball; it hops about, pecks the end up again and winds the ball
/// back in, leaning back. The ball turns as it rolls, without slipping.
struct TsugumiTemariScene: View {
  var birdScale: CGFloat = 0.19
  /// false: the perch and the temari only (the welcome flies the bird in).
  var showsBird = true
  /// How far the ball rolls out, in points.
  var distance: CGFloat = 30
  /// One cycle, in seconds.
  var period: Double = 8
  @Environment(\.tsugumiClock) private var clock
  @Environment(\.accessibilityReduceMotion) private var systemReduceMotion
  private var reduceMotion: Bool { AppPreferences.shared.tsugumiStill(reduceMotion: systemReduceMotion) }

  var body: some View {
    TimelineView(.animation(paused: clock != nil || reduceMotion)) { ctx in
      // held still: the ball close, the thread in the bill
      let t = clock ?? (reduceMotion ? 0 : ctx.date.timeIntervalSinceReferenceDate)
      Canvas { g, size in draw(g, size: size, t: t) }
    }
    .accessibilityHidden(true)
  }

  private func smooth(_ x: Double) -> Double { let x = min(1, max(0, x)); return x * x * (3 - 2 * x) }
  private func span(_ u: Double, _ a: Double, _ b: Double) -> Double { min(1, max(0, (u - a) / (b - a))) }

  private func draw(_ g: GraphicsContext, size: CGSize, t: Double) {
    let s = birdScale
    let y = size.height - 22, mid = size.width / 2
    TsugumiBranch.draw(g, x0: mid - 110, x1: mid + 110, y: y, grown: 1, time: t)
    // centre the body (tail…beak) over the perch
    let fx = mid + (TsugumiBird.feet.x - (TsugumiBird.tailTip + TsugumiBird.beakTip) / 2) * s
    let feet = CGPoint(x: fx, y: y - 3)
    let u = (t / period).truncatingRemainder(dividingBy: 1)

    // the ball: out while the end is let go, back while winding in
    let out = distance * (u < 0.30 ? smooth(u / 0.30) : u < 0.76 ? 1 : 1 - smooth((u - 0.76) / 0.24))
    let r = 70 * s
    let ball = CGPoint(x: fx + 199 * s + out, y: y - 3 - r)
    let lead = TsugumiTemari.lead(center: ball, radius: r)
    // wound thread keeps its curl: the let-go end stands in a small loop over
    // the top of the ball, a little forward, within reach of the bill
    let rest = CGPoint(x: lead.x + r * 0.35, y: lead.y - r * 0.5)
    // the tilt (with a little crouch) that brings the bill to that end
    func billAt(_ tilt: Double) -> CGPoint {
      var p = BirdPose(); p.tilt = tilt; p.squash = 0.1
      return TsugumiBird.point(TsugumiBird.gape, at: feet, scale: s, pose: p)
    }
    let reach = stride(from: 0.0, through: 0.9, by: 0.01).min { a, b in
      let pa = billAt(a), pb = billAt(b)
      return hypot(pa.x - rest.x, pa.y - rest.y) < hypot(pb.x - rest.x, pb.y - rest.y)
    } ?? 0.5

    var pose = BirdPose()
    var holding = true
    switch u {
    case ..<0.30:                         // paying out, a small hop midway
      pose = BirdMotion.hop(span(u, 0.12, 0.22), height: 8)
    case 0.30..<0.37:                     // let go: a little toss of the head
      pose.tilt = -0.14 * sin(span(u, 0.30, 0.37) * .pi)
      holding = false
    case 0.37..<0.58:                     // free: two hops, a look about
      holding = false
      if u < 0.45 { pose = BirdMotion.hop(span(u, 0.37, 0.45), height: 12) }
      else if u < 0.53 { pose = BirdMotion.hop(span(u, 0.45, 0.53), height: 12) }
      else { pose.tilt = -0.08 * sin(span(u, 0.53, 0.58) * .pi) }
    case 0.58..<0.66:                     // peck down to the end
      holding = false
      let k = smooth(span(u, 0.58, 0.66))
      pose.tilt = reach * k; pose.squash = 0.1 * k
    case 0.66..<0.70:                     // got it
      pose.tilt = reach; pose.squash = 0.1
    case 0.70..<0.76:                     // head up with it
      let k = 1 - smooth(span(u, 0.70, 0.76))
      pose.tilt = reach * k; pose.squash = 0.1 * k
    default:                              // winding in, leaning back
      pose.tilt = -0.10 * sin(span(u, 0.76, 1) * .pi)
    }
    pose.blink = max(pose.blink, BirdMotion.blink(t, every: period / 2, offset: 0.4))
    let bill = TsugumiBird.point(TsugumiBird.gape, at: feet, scale: s, pose: pose)
    // the let-go end falls from where the bill let it go to its curl
    let dropped: CGPoint = {
      let from = TsugumiBird.point(TsugumiBird.gape, at: feet, scale: s, pose: BirdPose())
      let f = CGFloat(smooth(span(u, 0.30, 0.36)))
      return CGPoint(x: from.x + (rest.x - from.x) * f, y: from.y + (rest.y - from.y) * f)
    }()

    let k = 1 - pose.hop / 40   // the shadow shrinks in the air
    if showsBird {
      g.fill(Path(ellipseIn: CGRect(x: fx - 30 * k, y: y - 6, width: 60 * k, height: 6)), with: .color(.black.opacity(0.2)))
      TsugumiBird.draw(g, at: feet, scale: s, pose: pose) { g in
        if holding {
          TsugumiThread.up(g, from: lead, to: bill)
          TsugumiThread.loose(g, from: bill, time: t)
        }
      }
    }
    TsugumiTemari.draw(g, center: ball, radius: r, spin: Double(out / r))
    if !holding || !showsBird { TsugumiThread.slack(g, from: lead, to: dropped, time: t) }
  }
}

#Preview {
  TsugumiTemariScene().frame(width: 250, height: 150).background(LowPolyBackground())
}
