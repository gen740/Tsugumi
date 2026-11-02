import SwiftUI

/// What a loading screen says. `fraction == nil` is indeterminate.
struct LoadingContent: Equatable {
  var title: String
  var message: String = ""
  var fraction: Double?
  var detail: String?
  /// Unsaved edits exist and are being kept.
  var dirty = false
}

/// The brand card: with a fraction the branch grows as the bar and the
/// percentage is shown; without, the bird hops and pecks.
struct TsugumiLoadingCard: View {
  let content: LoadingContent
  var onCancel: (() -> Void)?

  var body: some View {
    let f = content.fraction.map { min(1, max(0, $0)) }
    VStack(spacing: 0) {
      Group {
        if let f { TsugumiProgressScene(fraction: f) } else { TsugumiIdleScene() }
      }
      .frame(height: 175)

      VStack(spacing: 10) {
        if let f {
          HStack(alignment: .firstTextBaseline, spacing: 2) {
            Text(verbatim: "\(Int((f * 100).rounded(.down)))")
              .font(.system(size: 40, weight: .bold, design: .rounded).monospacedDigit())
              .contentTransition(.numericText(value: f))
            Text(verbatim: "%").font(.system(size: 18, weight: .semibold, design: .rounded)).foregroundStyle(TsugumiPalette.mist)
          }
          .foregroundStyle(TsugumiPalette.cream)
        }
        Text(content.title).font(.title3.weight(.semibold)).foregroundStyle(TsugumiPalette.cream)
        if !content.message.isEmpty {
          Text(content.message).font(.callout).foregroundStyle(TsugumiPalette.mist)
            .multilineTextAlignment(.center).lineLimit(3)
            .contentTransition(.opacity)
        }
        stripe
        if let d = content.detail {
          Text(d).font(.caption.monospacedDigit()).foregroundStyle(TsugumiPalette.mist)
        }
        if content.dirty {
          Label("未保存の変更があります（保持しています）", systemImage: "pencil.circle")
            .font(.callout).foregroundStyle(TsugumiPalette.rufous)
        }
        if let onCancel {
          Button("キャンセル", action: onCancel).buttonStyle(.plain)
            .font(.callout).foregroundStyle(TsugumiPalette.cream)
            .padding(.horizontal, 14).padding(.vertical, 5)
            .background(.white.opacity(0.12), in: Capsule())
            .padding(.top, 4)
        }
      }
      .padding(.horizontal, 24).padding(.bottom, 24)
    }
    .frame(width: 440)
    .background {
      ZStack(alignment: .top) {
        LowPolyBackground()
        // the workflow runs faintly behind the bird and fades out above the text
        WorkflowBackdrop(fraction: f)
          .opacity(0.6)
          .frame(height: 200)
          .mask(LinearGradient(stops: [.init(color: .black, location: 0), .init(color: .black, location: 0.55),
                                       .init(color: .clear, location: 1)], startPoint: .top, endPoint: .bottom))
          .padding(.top, 10)
      }
    }
    .clipShape(RoundedRectangle(cornerRadius: 22, style: .continuous))
    .shadow(color: .black.opacity(0.3), radius: 24, y: 10)
    .environment(\.colorScheme, .dark)
    .accessibilityElement(children: .contain)
    .accessibilityLabel(content.title)
    .accessibilityValue(f.map { "\(Int($0 * 100))%" } ?? content.message)
  }

  /// The three-colour slanted stripe under the hero title.
  private var stripe: some View {
    HStack(spacing: 4) {
      ForEach([TsugumiPalette.rufous, TsugumiPalette.olive, TsugumiPalette.cream], id: \.self) { c in
        SlantedBar().fill(c).frame(width: 34, height: 5)
      }
    }
    .padding(.vertical, 2)
    .accessibilityHidden(true)
  }
}

private struct SlantedBar: Shape {
  func path(in r: CGRect) -> Path {
    Path { p in
      p.move(to: CGPoint(x: r.minX + 4, y: r.minY)); p.addLine(to: CGPoint(x: r.maxX, y: r.minY))
      p.addLine(to: CGPoint(x: r.maxX - 4, y: r.maxY)); p.addLine(to: CGPoint(x: r.minX, y: r.maxY)); p.closeSubpath()
    }
  }
}

/// Indeterminate: the bird hops and pecks on a short perch.
struct TsugumiIdleScene: View {
  var birdScale: CGFloat = 0.22
  @Environment(\.tsugumiClock) private var clock
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

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
        TsugumiBird.draw(g, at: CGPoint(x: fx, y: y - 3), scale: birdScale, pose: pose)
      }
    }
    .accessibilityHidden(true)
  }
}

/// Determinate: the branch is the bar; the bird hops along the growing tip.
struct TsugumiProgressScene: View {
  let fraction: Double
  var birdScale: CGFloat = 0.2
  @Environment(\.tsugumiClock) private var clock
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    TimelineView(.animation(paused: clock != nil || reduceMotion)) { ctx in
      let t = clock ?? (reduceMotion ? 0.6 : ctx.date.timeIntervalSinceReferenceDate)
      Canvas { g, size in
        let x0: CGFloat = 24, x1 = size.width - 24, y = size.height - 22
        TsugumiBranch.draw(g, x0: x0, x1: x1, y: y, grown: fraction, time: t)
        var pose: BirdPose
        if fraction >= 1 {
          pose = BirdMotion.hop(t.truncatingRemainder(dividingBy: 0.7) / 0.7, height: 18)
          pose.flap = 1
        } else {
          let u = t.truncatingRemainder(dividingBy: 1.1)
          pose = u < 0.4 ? BirdMotion.hop(u / 0.4, height: 9) : BirdPose()
          pose.blink = BirdMotion.blink(t, every: 2.7)
        }
        // beak at the growing tip, but keep the tail inside the card
        let tip = x0 + (x1 - x0) * fraction
        let fx = max(x0 + (TsugumiBird.feet.x - TsugumiBird.tailTip) * birdScale - 8,
                     tip - (TsugumiBird.beakTip - TsugumiBird.feet.x) * birdScale + 6)
        TsugumiBird.draw(g, at: CGPoint(x: fx, y: y - 3), scale: birdScale, pose: pose)
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
