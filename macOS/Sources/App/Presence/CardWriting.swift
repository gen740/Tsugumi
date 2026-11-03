import SwiftUI

/// A Card the Agent wrote: a Draft becoming code is written out left to
/// right (what is not written yet still shows as the Draft's dashed
/// outline), and the colour of what was written settles into the Card and
/// lets go again. Nothing glows; nothing else changes on the Card.
struct CardWriting: ViewModifier {
  let presence: AgentPresence
  let card: CardKey
  let title: String
  let zoom: Double
  @Environment(\.accessibilityReduceMotion) private var systemReduceMotion
  /// Still: the app's "Tsugumi のアニメーション" (or the system's reduce motion).
  private var reduceMotion: Bool { AppPreferences.shared.tsugumiStill(reduceMotion: systemReduceMotion) }

  func body(content: Content) -> some View {
    let _ = presence.revision
    let active = presence.writing(card, at: Date()) != nil
    // Always the same view tree, so the Card keeps its state; frames run
    // only while something changes.
    SwiftUI.TimelineView(.animation(paused: !active)) { ctx in
      let w = presence.writing(card, at: ctx.date)
      let z = CGFloat(zoom)
      ZStack {
        if let r = w?.reveal {
          DraftGhost(title: title, zoom: z).opacity(1 - r * r)
        }
        content
          .mask(WritingMask(progress: w?.reveal ?? 1, soft: reduceMotion ? 0 : 26 * z, crossfade: reduceMotion))
          .overlay(alignment: .leading) {
            if let r = w?.reveal, !reduceMotion {
              // The line where it is being written, thin and quiet.
              GeometryReader { geo in
                Rectangle().fill(PresenceColor.thread)
                  .frame(width: 1)
                  .offset(x: geo.size.width * CGFloat(r))
                  .opacity(0.6 * sin(.pi * r))
              }
            }
          }
        RoundedRectangle(cornerRadius: 9 * z)
          .fill(PresenceColor.ink.opacity(0.12 * (w?.settle ?? 0)))
          .allowsHitTesting(false)
      }
    }
  }
}

/// Opaque up to the writing line, with a soft edge (or, with reduced
/// motion, the whole Card fading in).
struct WritingMask: View {
  let progress: Double
  let soft: CGFloat
  let crossfade: Bool
  var body: some View {
    if crossfade || progress >= 1 {
      Rectangle().fill(Color.black.opacity(crossfade ? progress : 1))
    } else {
      Canvas { ctx, size in
        let edge = (size.width + soft) * CGFloat(progress)
        ctx.fill(Path(CGRect(x: 0, y: 0, width: max(0, edge - soft), height: size.height)), with: .color(.black))
        if soft > 0 {
          ctx.fill(Path(CGRect(x: edge - soft, y: 0, width: soft, height: size.height)),
                   with: .linearGradient(Gradient(colors: [.black, .clear]),
                                         startPoint: CGPoint(x: edge - soft, y: 0), endPoint: CGPoint(x: edge, y: 0)))
        }
      }
    }
  }
}

/// What the Card looked like as a Draft: its title in a dashed outline.
struct DraftGhost: View {
  let title: String
  let zoom: CGFloat
  var body: some View {
    RoundedRectangle(cornerRadius: 9 * zoom)
      .strokeBorder(Color.secondary.opacity(0.6), style: StrokeStyle(lineWidth: 1.2, dash: [5 * zoom, 4 * zoom]))
      .overlay(alignment: .topLeading) {
        HStack {
          Text(title).font(.system(size: 13 * zoom, weight: .semibold)).lineLimit(1)
          Spacer(minLength: 4 * zoom)
          Text("Draft").font(.system(size: 10 * zoom, weight: .medium)).foregroundStyle(.secondary)
        }
        .padding(12 * zoom)
      }
      .allowsHitTesting(false)
  }
}
