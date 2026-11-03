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
/// the edges. The bird scene is the Card's picture; with a fraction the
/// branch grows as the bar, without it the bird hops and pecks.
struct TsugumiLoadingCard: View {
  let content: LoadingContent
  var onCancel: (() -> Void)?
  @Environment(\.colorScheme) private var scheme
  @Environment(\.openWindow) private var openWindow

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
          Group {
            if let f { TsugumiProgressScene(fraction: f) } else { TsugumiIdleScene() }
          }
        }
        .frame(height: 150)
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
        .padding(.horizontal, 22).padding(.vertical, 16)
        .frame(maxWidth: .infinity)
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

/// Indeterminate: the bird hops and pecks on a short perch.
struct TsugumiIdleScene: View {
  var birdScale: CGFloat = 0.22
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
  @Environment(\.accessibilityReduceMotion) private var systemReduceMotion
  private var reduceMotion: Bool { AppPreferences.shared.tsugumiStill(reduceMotion: systemReduceMotion) }

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
