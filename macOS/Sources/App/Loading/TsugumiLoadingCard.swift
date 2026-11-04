import SwiftUI

/// What a loading screen says. `fraction == nil` is indeterminate.
struct LoadingContent: Equatable {
  var title: String
  var message: String = ""
  var fraction: Double?
  var detail: String?
  /// Unsaved edits exist and are being kept.
  var dirty = false
  /// Offers the execution environment's console (its start log).
  var showsConsole = false
}

/// The loading Card, drawn as one Card of a workflow inside the window: an
/// input (square) above and an output (circle) below with wires running off
/// the edges. The bird scene is the Card's picture: the bird holds a thread
/// unwound from the temari beside the words below; with a fraction the
/// branch grows as the bar and the temari unwinds, without it the bird hops
/// and pecks.
struct TsugumiLoadingCard: View {
  let content: LoadingContent
  var onCancel: (() -> Void)?
  /// The temari's column left of the words, and its centre (from the card's
  /// left edge): its thread runs straight up and hooks over the panel's top
  /// edge there, into the picture behind.
  static let spoolWidth: CGFloat = 56
  static let spoolX: CGFloat = 22 + spoolWidth / 2
  static let catchX: CGFloat = spoolX

  /// Where the scene's bird stands (window coordinates) and its size, for the
  /// guide's bird to land on: the idle bird over its perch, or the progress
  /// bird at its step along the branch.
  static func birdFeet(in r: CGRect, fraction: Double?) -> (feet: CGPoint, scale: CGFloat) {
    let y = r.maxY - 25
    guard let fraction else {
      let s: CGFloat = 0.22
      return (CGPoint(x: r.midX + (TsugumiBird.feet.x - (TsugumiBird.tailTip + TsugumiBird.beakTip) / 2) * s, y: y), s)
    }
    let s: CGFloat = 0.2, x0 = r.minX + 24, x1 = r.maxX - 24
    let tip = x0 + (x1 - x0) * fraction
    let lo = x0 + (TsugumiBird.feet.x - TsugumiBird.tailTip) * s - 8
    let want = max(lo, tip - (TsugumiBird.beakTip - TsugumiBird.feet.x) * s + 6)
    return (CGPoint(x: lo + ((want - lo) / 18).rounded(.down) * 18, y: y), s)
  }
  @Environment(\.colorScheme) private var scheme
  @Environment(\.openWindow) private var openWindow
  /// Tsugumi's flights in this window: the guide's bird lands on this card's bird.
  @Environment(GuideStage.self) private var stage: GuideStage?

  var body: some View {
    let f = content.fraction.map { min(1, max(0, $0)) }
    VStack(spacing: 0) {
      wire
      port(input: true)
      VStack(spacing: 0) {
        ZStack(alignment: .top) {
          LowPolyBackground()
          WorkflowBackdrop(fraction: f)
            .opacity(0.5)
            .mask(LinearGradient(stops: [.init(color: .black, location: 0), .init(color: .black, location: 0.5),
                                         .init(color: .clear, location: 1)], startPoint: .top, endPoint: .bottom))
          // the picture sits behind the panel below: the panel's edge shades it
          LinearGradient(colors: [.black.opacity(0), .black.opacity(0.45)], startPoint: .top, endPoint: .bottom)
            .frame(height: 14).frame(maxHeight: .infinity, alignment: .bottom)
          Group {
            if let f { TsugumiProgressScene(fraction: f, catchX: Self.catchX, hidesBird: stage?.handoff ?? false) }
            else { TsugumiIdleScene(catchX: Self.catchX, hidesBird: stage?.handoff ?? false) }
          }
        }
        .frame(height: 150)
        .onGeometryChange(for: CGRect.self) { $0.frame(in: .global) } action: { r in stage?.loadingBird = Self.birdFeet(in: r, fraction: f) }
        .onDisappear { stage?.loadingBird = nil }
        .environment(\.colorScheme, .dark)
        .clipped()
        VStack(spacing: 8) {
          if let f {
            Text(verbatim: "\(Int((f * 100).rounded(.down)))%")
              .font(.system(size: 26, weight: .bold, design: .rounded).monospacedDigit())
              .contentTransition(.numericText(value: f))
          }
          Text(content.title).font(.title3.weight(.semibold))
          if !content.message.isEmpty {
            Text(content.message).font(.callout).foregroundStyle(.secondary)
              .multilineTextAlignment(.center).lineLimit(3)
              .contentTransition(.opacity)
          }
          if let d = content.detail {
            Text(d).font(.caption.monospaced()).foregroundStyle(.tertiary).lineLimit(1).truncationMode(.middle)
          }
          if content.dirty {
            Label("未保存の変更があります（保持しています）", systemImage: "pencil.circle")
              .font(.callout).foregroundStyle(TsugumiPalette.rufous)
          }
          if content.showsConsole {
            Button { openWindow(id: "console") } label: { Label("コンソールで詳細を見る", systemImage: "apple.terminal") }
              .buttonStyle(.link).font(.callout)
          }
          if let onCancel {
            Button("キャンセル", action: onCancel).controlSize(.regular).padding(.top, 2)
          }
        }
        // room for the temari on the left, the same on the right: the words stay centred
        .padding(.horizontal, Self.spoolWidth - 8)
        .padding(.horizontal, 22).padding(.vertical, 16)
        .frame(maxWidth: .infinity)
        // the temari beside the words; its thread catches on the picture's edge
        .overlay {
          TsugumiSpool(fraction: f, x: Self.spoolX, catchX: Self.catchX).allowsHitTesting(false)
        }
      }
      .frame(width: 380)
      .background(Color(nsColor: .controlBackgroundColor))
      .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
      .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(Color.secondary.opacity(0.3)))
      .shadow(color: .black.opacity(scheme == .dark ? 0.25 : 0.06), radius: 3, y: 1)
      port(input: false)
      wire
    }
    .accessibilityElement(children: .contain)
    .accessibilityLabel(content.title)
    .onChange(of: f) { stage?.loadingBird = nil }
    .accessibilityValue(f.map { "\(Int($0 * 100))%" } ?? content.message)
  }

  private var wire: some View {
    Rectangle().fill(Color.secondary.opacity(0.45)).frame(width: 1.7, height: 54)
      .accessibilityHidden(true)
  }

  @ViewBuilder private func port(input: Bool) -> some View {
    Group {
      if input {
        RoundedRectangle(cornerRadius: 2).fill(Color(nsColor: .controlBackgroundColor))
          .overlay(RoundedRectangle(cornerRadius: 2).strokeBorder(Color.secondary, lineWidth: 1.5))
      } else {
        Circle().fill(Color.accentColor)
      }
    }
    .frame(width: 10, height: 10)
    .padding(.vertical, -5)
    .zIndex(1)
    .accessibilityHidden(true)
  }
}

/// Indeterminate: the bird hops and pecks on a short perch, holding the
/// thread that catches on the lower edge at `catchX` (nil: only the loose
/// end).
struct TsugumiIdleScene: View {
  var catchX: CGFloat? = nil
  var birdScale: CGFloat = 0.22
  /// The guide's bird is flying in to be this one: none drawn until it lands.
  var hidesBird = false
  @Environment(\.tsugumiClock) private var clock
  @Environment(\.accessibilityReduceMotion) private var systemReduceMotion
  private var reduceMotion: Bool { AppPreferences.shared.tsugumiStill(reduceMotion: systemReduceMotion) }

  var body: some View {
    TimelineView(.animation(paused: clock != nil || reduceMotion)) { ctx in
      let t = clock ?? (reduceMotion ? 2.8 : ctx.date.timeIntervalSinceReferenceDate)
      Canvas { g, size in
        let y = size.height - 22, mid = size.width / 2
        TsugumiBranch.draw(g, x0: mid - 110, x1: mid + 110, y: y, grown: 1, time: t)
        let pose = BirdMotion.idle(t)
        // centre the body (tail…beak) over the perch; the shadow shrinks in the air
        let fx = mid + (TsugumiBird.feet.x - (TsugumiBird.tailTip + TsugumiBird.beakTip) / 2) * birdScale
        let s = 1 - pose.hop / 40
        g.fill(Path(ellipseIn: CGRect(x: fx - 30 * s, y: y - 6, width: 60 * s, height: 6)), with: .color(.black.opacity(0.2)))
        if hidesBird { return }
        let feet = CGPoint(x: fx, y: y - 3)
        let bill = TsugumiBird.point(TsugumiBird.gape, at: feet, scale: birdScale, pose: pose)
        // over the lower edge and away into the picture, in front of the bird
        TsugumiBird.draw(g, at: feet, scale: birdScale, pose: pose) { g in
          if let catchX { TsugumiThread.receding(g, x: catchX, edge: size.height, to: bill) }
          TsugumiThread.loose(g, from: bill, time: t)
        }
      }
    }
    .accessibilityHidden(true)
  }
}

/// Determinate: the branch is the bar; the bird hops along the growing tip
/// (it moves only while hopping, a step at a time), drawing out the thread
/// that catches on the lower edge at `catchX`.
struct TsugumiProgressScene: View {
  let fraction: Double
  var catchX: CGFloat
  var birdScale: CGFloat = 0.2
  var hidesBird = false
  /// How far one hop goes along the branch.
  var stride: CGFloat = 18
  @State private var walker = HopWalker()
  @Environment(\.tsugumiClock) private var clock
  @Environment(\.accessibilityReduceMotion) private var systemReduceMotion
  private var reduceMotion: Bool { AppPreferences.shared.tsugumiStill(reduceMotion: systemReduceMotion) }

  var body: some View {
    TimelineView(.animation(paused: clock != nil || reduceMotion)) { ctx in
      let t = clock ?? (reduceMotion ? 0.6 : ctx.date.timeIntervalSinceReferenceDate)
      Canvas { g, size in
        let x0: CGFloat = 24, x1 = size.width - 24, y = size.height - 22
        TsugumiBranch.draw(g, x0: x0, x1: x1, y: y, grown: fraction, time: t)
        // beak at the growing tip, but keep the tail inside the card; the bird
        // keeps to steps along the branch and hops from one to the next
        let tip = x0 + (x1 - x0) * fraction
        let lo = x0 + (TsugumiBird.feet.x - TsugumiBird.tailTip) * birdScale - 8
        let want = max(lo, tip - (TsugumiBird.beakTip - TsugumiBird.feet.x) * birdScale + 6)
        let step = lo + (((want - lo) / stride).rounded(.down)) * stride
        let moving = clock == nil && !reduceMotion
        var (fx, pose) = walker.position(step: step, time: t, animated: moving)
        if fraction >= 1 {
          pose = BirdMotion.hop(t.truncatingRemainder(dividingBy: 0.7) / 0.7, height: 18)
          pose.flap = 1
          fx = step
        }
        pose.blink = max(pose.blink, BirdMotion.blink(t, every: 2.7))
        if hidesBird { return }
        let feet = CGPoint(x: fx, y: y - 3)
        let bill = TsugumiBird.point(TsugumiBird.gape, at: feet, scale: birdScale, pose: pose)
        // over the lower edge and away into the picture, in front of the bird
        TsugumiBird.draw(g, at: feet, scale: birdScale, pose: pose) { g in
          TsugumiThread.receding(g, x: catchX, edge: size.height, to: bill)
          TsugumiThread.loose(g, from: bill, time: t)
        }
      }
    }
    .accessibilityHidden(true)
  }
}

/// Where the bird of the progress scene stands: it stays on its step and,
/// when the step moves on, hops there (a hop longer for a longer way). A
/// class so the drawing can move it on without invalidating the view.
final class HopWalker {
  private var from: CGFloat?
  private var to: CGFloat = 0
  private var start: Double = 0
  private var length: Double = 0.36

  func position(step: CGFloat, time t: Double, animated: Bool) -> (CGFloat, BirdPose) {
    guard animated, let from else {
      self.from = step
      to = step
      return (step, BirdPose())
    }
    if step != to {
      // from wherever it is now (mid-hop included) to the new step
      let here = current(t, from: from)
      self.from = here
      to = step
      start = t
      length = min(0.6, 0.3 + Double(abs(step - here)) / 400)
    }
    let u = (t - start) / length
    guard u < 1 else {
      self.from = to
      return (to, BirdPose())
    }
    let height = min(16, 7 + abs(to - (self.from ?? to)) * 0.12)
    return (current(t, from: self.from ?? to), BirdMotion.hop(u, height: height))
  }

  private func current(_ t: Double, from: CGFloat) -> CGFloat {
    let u = min(1, max(0, (t - start) / length))
    let eased = CGFloat(u * u * (3 - 2 * u))
    return from + (to - from) * eased
  }
}

/// The temari beside the loading words, over the whole lower part of the
/// card: its thread runs straight up from the top of the ball to `catchX` on
/// the top edge, where it hooks over the edge into the picture behind. With a
/// fraction the ball turns and shrinks as the bird draws the thread out;
/// without, it rocks a little with each hop.
struct TsugumiSpool: View {
  let fraction: Double?
  /// The ball's centre and the catch, from the left edge.
  var x: CGFloat
  var catchX: CGFloat
  var radius: CGFloat = 15
  /// The ball's centre from the top (beside the first lines).
  var centerY: CGFloat = 46
  @Environment(\.tsugumiClock) private var clock
  @Environment(\.accessibilityReduceMotion) private var systemReduceMotion
  private var reduceMotion: Bool { AppPreferences.shared.tsugumiStill(reduceMotion: systemReduceMotion) }

  var body: some View {
    TimelineView(.animation(paused: clock != nil || reduceMotion || fraction != nil)) { ctx in
      let t = clock ?? (reduceMotion ? 2.8 : ctx.date.timeIntervalSinceReferenceDate)
      Canvas { g, size in
        let r = radius * CGFloat(1 - 0.35 * (fraction ?? 0))
        let ball = CGPoint(x: x, y: centerY)
        let spin = fraction.map { $0 * 5 * .pi } ?? 0.25 * sin(t * 2 * .pi / 3.2)
        TsugumiTemari.draw(g, center: ball, radius: r, spin: spin)
        TsugumiThread.near(g, from: TsugumiTemari.lead(center: ball, radius: r), x: catchX, size: size)
      }
    }
    .accessibilityHidden(true)
  }
}

#Preview("割合あり") {
  TsugumiLoadingCard(content: LoadingContent(title: "初回の準備をしています", message: "実行環境イメージを読み込んでいます",
                                             fraction: 0.62, detail: "508 MB / 820 MB"), onCancel: {})
    .padding(40)
}

#Preview("割合なし") {
  TsugumiLoadingCard(content: LoadingContent(title: "起動しています", message: "コンテナに接続しています…"))
    .padding(40)
}
