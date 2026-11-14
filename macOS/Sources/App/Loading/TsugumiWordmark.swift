import SwiftUI

/// "tsugumi" written by one thread, as in the key visual
/// (docs/assets/tsugumi-hero.svg): the arriving thread is the t's crossbar,
/// the letters run on without a break, and the i's dot is a small temari.
/// It fits its width; the height follows (about 0.37 of the width).
struct TsugumiWordmark: View {
  /// The thread's width at a word 500 units wide (the key visual's 4 pt at
  /// its scale).
  var weight: CGFloat = 4.1
  /// The word's box in its own units: baseline at 0, x-height -70.
  static let bounds = CGRect(x: -44, y: -106, width: 506, height: 172)
  static let iDot = CGPoint(x: 436, y: -92)

  var body: some View {
    Canvas { g, size in
      let k = size.width / Self.bounds.width
      var g = g
      g.scaleBy(x: k, y: k)
      g.translateBy(x: -Self.bounds.minX, y: -Self.bounds.minY)
      let style = StrokeStyle(lineWidth: weight, lineCap: .round, lineJoin: .round)
      let path = Self.path
      var shadow = g
      shadow.addFilter(.blur(radius: 2))
      shadow.stroke(path.offsetBy(dx: 2, dy: 3), with: .color(.black.opacity(0.3)), style: style)
      g.stroke(path, with: .color(TsugumiPalette.thread), style: style)
      TsugumiTemari.draw(g, center: Self.iDot, radius: 10.5, spin: 0)
    }
    .aspectRatio(Self.bounds.width / Self.bounds.height, contentMode: .fit)
    .accessibilityElement()
    .accessibilityLabel(Text(verbatim: "Tsugumi"))
    .accessibilityAddTraits(.isHeader)
  }

  /// The letters: the same curves as the key visual's thread.
  static let path: Path = {
    func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x, y: y) }
    var w = Path()
    w.move(to: p(-44, -70))
    w.addLine(to: p(36, -70))
    w.addCurve(to: p(28, -101), control1: p(50, -70), control2: p(44, -96))
    w.addCurve(to: p(14, -88), control1: p(19, -104), control2: p(14, -98))
    w.addLine(to: p(14, -16))
    w.addCurve(to: p(44, -4), control1: p(14, -2), control2: p(30, 4))
    w.addCurve(to: p(76, -70), control1: p(56, -12), control2: p(66, -48))
    w.addCurve(to: p(98, -24), control1: p(82, -56), control2: p(98, -44))
    w.addCurve(to: p(72, 0), control1: p(98, -6), control2: p(84, 1))
    w.addCurve(to: p(58, -9), control1: p(62, -1), control2: p(56, -5))
    w.addCurve(to: p(106, -12), control1: p(66, 6), control2: p(98, 8))
    w.addCurve(to: p(112, -66), control1: p(110, -24), control2: p(112, -48))
    w.addCurve(to: p(110, -18), control1: p(110, -46), control2: p(108, -30))
    w.addCurve(to: p(134, 0), control1: p(112, -4), control2: p(124, 1))
    w.addCurve(to: p(158, -40), control1: p(150, -1), control2: p(158, -16))
    w.addLine(to: p(158, -66))
    w.addCurve(to: p(204, -68), control1: p(170, -74), control2: p(190, -74))
    w.addCurve(to: p(168, -40), control1: p(182, -74), control2: p(168, -60))
    w.addCurve(to: p(194, -8), control1: p(168, -18), control2: p(180, -6))
    w.addCurve(to: p(216, -50), control1: p(208, -10), control2: p(216, -28))
    w.addLine(to: p(216, -70))
    w.addLine(to: p(216, 28))
    w.addCurve(to: p(184, 58), control1: p(216, 56), control2: p(198, 64))
    w.addCurve(to: p(194, 30), control1: p(168, 51), control2: p(176, 38))
    w.addCurve(to: p(250, -66), control1: p(214, 21), control2: p(240, -14))
    w.addCurve(to: p(250, -18), control1: p(250, -46), control2: p(248, -30))
    w.addCurve(to: p(274, 0), control1: p(252, -4), control2: p(264, 1))
    w.addCurve(to: p(298, -40), control1: p(290, -1), control2: p(298, -16))
    w.addLine(to: p(298, -66))
    w.addCurve(to: p(326, -66), control1: p(306, -72), control2: p(318, -72))
    w.addLine(to: p(326, 0))
    w.addLine(to: p(326, -50))
    w.addCurve(to: p(364, -52), control1: p(326, -78), control2: p(364, -78))
    w.addLine(to: p(364, 0))
    w.addLine(to: p(364, -52))
    w.addCurve(to: p(402, -52), control1: p(364, -78), control2: p(402, -78))
    w.addLine(to: p(402, -14))
    w.addCurve(to: p(420, -4), control1: p(402, -3), control2: p(410, 2))
    w.addCurve(to: p(434, -68), control1: p(428, -14), control2: p(432, -44))
    w.addCurve(to: p(434, -12), control1: p(432, -44), control2: p(432, -24))
    w.addCurve(to: p(456, -6), control1: p(436, -2), control2: p(446, 1))
    return w
  }()
}

#Preview {
  TsugumiWordmark().frame(width: 214).padding(30).background(LowPolyBackground())
}
