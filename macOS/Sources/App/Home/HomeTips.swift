import SwiftUI

/// What the bird says on home: how to use the app, and where its name comes
/// from (docs/Name.md). English in tools/en_strings.py.
enum HomeTips {
  static let all: [String] = [
    String(localized: "Draft を置いて線でつなぐと、まず処理の流れだけを描けるよ"),
    String(localized: "Draft の実装は、チャットから Agent に頼めるよ"),
    String(localized: "Card は C++ でも Python でも bash でも書けるよ"),
    String(localized: "途中の Card を直したら、保存された入力でそこから試し直せるよ"),
    String(localized: "Card の実行履歴から、前の入力と出力をいつでも見返せるよ"),
    String(localized: "実行環境は Nix で固定。同じ計算をもう一度確かめられるよ"),
    String(localized: "⌥⌘K で、実行環境のコンソールを開けるよ"),
    String(localized: "「デモを見る」から、Collatz などの例をすぐに開けるよ"),
    String(localized: "名前は「継ぐ・積む・組む」から。処理を継ぎ、試行を積み、実験を組むよ"),
    String(localized: "つぐみ → つむぐ。試行錯誤をつないで、研究を「紡ぐ」道具でありたいんだ"),
    String(localized: "くわえている手毬の糸は「紡ぐ」から。この糸が Card をつなぐ配線になるよ"),
    String(localized: "ツグミは冬に日本へ渡ってくる鳥。数歩跳ねては立ち止まって、あたりを確かめるよ"),
    String(localized: "冬はさえずらず口を「噤む」から、ツグミという名になったという説もあるよ"),
  ]
}

/// The bird's words, cut like the key visual's facets: a dark panel with
/// chamfered corners, a faint lighter facet across it, a thin cream edge and
/// a sharp tail down to the bird's head (`tailX` from the leading edge).
/// Every text is laid out and only `index` shown, so the bubble is as tall as
/// the longest in the current language and nothing moves when it changes.
struct TipBubble: View {
  let texts: [String]
  let index: Int
  var tailX: CGFloat = 140

  var body: some View {
    ZStack(alignment: .topLeading) {
      ForEach(texts.indices, id: \.self) { i in
        Text(verbatim: texts[i])
          .font(.system(size: 13, weight: .medium))
          .foregroundStyle(TsugumiPalette.cream)
          .lineSpacing(2)
          .fixedSize(horizontal: false, vertical: true)
          .frame(maxWidth: .infinity, alignment: .leading)
          .opacity(i == index ? 1 : 0)
          .accessibilityHidden(i != index)
      }
    }
    .padding(.horizontal, 14).padding(.vertical, 11)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background {
      let shape = FacetBubble(tailX: tailX)
      ZStack {
        shape.fill(TsugumiPalette.hex(0x10242a, 0.92))
        // one lighter facet across the upper left, as on the bird
        shape.fill(LinearGradient(stops: [.init(color: .white.opacity(0.07), location: 0),
                                          .init(color: .white.opacity(0.07), location: 0.45),
                                          .init(color: .clear, location: 0.45)],
                                  startPoint: .topLeading, endPoint: .bottomTrailing))
        shape.stroke(TsugumiPalette.cream.opacity(0.35), lineWidth: 1)
      }
      .shadow(color: .black.opacity(0.35), radius: 8, y: 4)
    }
  }
}

/// A rectangle with its corners cut off and a sharp tail on the lower edge,
/// as one outline so the edge runs round the tail too.
struct FacetBubble: Shape {
  var tailX: CGFloat
  var cut: CGFloat = 7

  func path(in r: CGRect) -> Path {
    let x = min(max(tailX, cut + 12), r.width - cut - 12)
    var p = Path()
    p.move(to: CGPoint(x: r.minX + cut, y: r.minY))
    p.addLine(to: CGPoint(x: r.maxX - cut, y: r.minY))
    p.addLine(to: CGPoint(x: r.maxX, y: r.minY + cut))
    p.addLine(to: CGPoint(x: r.maxX, y: r.maxY - cut))
    p.addLine(to: CGPoint(x: r.maxX - cut, y: r.maxY))
    p.addLine(to: CGPoint(x: x + 5, y: r.maxY))
    p.addLine(to: CGPoint(x: x + 9, y: r.maxY + 12))   // the tail, leaning toward the bird
    p.addLine(to: CGPoint(x: x - 7, y: r.maxY))
    p.addLine(to: CGPoint(x: r.minX + cut, y: r.maxY))
    p.addLine(to: CGPoint(x: r.minX, y: r.maxY - cut))
    p.addLine(to: CGPoint(x: r.minX, y: r.minY + cut))
    p.closeSubpath()
    return p
  }
}

/// The bubble over the bird on home: a new tip every few seconds, the next
/// one on a click.
struct HomeTipBubble: View {
  var every: Duration = .seconds(7)
  @State private var tip = 0

  var body: some View {
    TipBubble(texts: HomeTips.all, index: tip)
      .contentShape(Rectangle())
      .onTapGesture { next() }
      .help("クリックで次のヒント")
      .task {
        while !Task.isCancelled {
          try? await Task.sleep(for: every)
          if !Task.isCancelled { next() }
        }
      }
  }

  private func next() {
    withAnimation(.easeInOut(duration: 0.35)) { tip = (tip + 1) % HomeTips.all.count }
  }
}

#Preview {
  HomeTipBubble().padding(18).frame(width: 250).background(TsugumiPalette.tealBottom)
}
