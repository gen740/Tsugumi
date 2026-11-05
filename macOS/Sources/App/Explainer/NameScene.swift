import SwiftUI

extension Explainer {
  // 3. 名前の由来 (docs/Name.md): 継ぐ・積む・組む, then 紡ぐ. The page's
  // thread goes up from the temari into the bill and on out of it, and becomes
  // the wires between Cards: the key visual, built up.

  enum NameScene {
    static let feet = pt(318, 556)
    static let scale: CGFloat = 0.36
    static let temari = (center: pt(196, 664), radius: CGFloat(34))
    static func card(_ k: Int) -> CGRect { CGRect(x: 520 + CGFloat(k) * 160, y: 418, width: 124, height: 56) }
    static let names = ["load", "bands", "rgb"]

    static func draw(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      WallScene.drawGround(g, 1)
      TsugumiBranch.draw(g, x0: 90, x1: 560, y: feet.y + 3, grown: 1, time: t)

      // the words
      let words = [("継ぐ", 250.0, 0.6), ("積む", 470.0, 0.75), ("組む", 690.0, 0.9)]
      let out = 1 - span(p, 1.25, 1.45)
      for (w, x, at) in words {
        let u = pop(lin(p, at - 0.1, at + 0.1))
        guard u > 0.01, out > 0.01 else { continue }
        var c = g
        c.translateBy(x: CGFloat(x), y: 190)
        c.scaleBy(x: CGFloat(u), y: CGFloat(u))
        c.text(w, at: .zero, size: 66, weight: .bold, color: Ink.cream, opacity: 0.92 * out)
      }
      let spin = span(p, 1.35, 1.6) * (1 - span(p, 2.25, 2.5))
      if spin > 0.01 {
        var c = g
        c.translateBy(x: 470, y: 190)
        let s = CGFloat(pop(lin(p, 1.35, 1.6)))
        c.scaleBy(x: s, y: s)
        c.text("紡ぐ", at: .zero, size: 92, weight: .heavy, color: TsugumiPalette.hex(0xf0b57a), opacity: spin)
      }
      let mark = span(p, 2.3, 2.6)
      if mark > 0.01 {
        g.text("Tsugumi", at: pt(500, 190), size: 76, weight: .bold, color: Ink.cream, opacity: mark)
        g.text("つぐみ", at: pt(500, 252), size: 22, weight: .medium, color: Ink.mist, opacity: mark)
      }

      // the bird, the page's thread in its bill (ExplainerView draws the thread)
      var pose = BirdPose()
      pose.blink = BirdMotion.blink(t, every: 3.4)
      pose.tilt = 0.03 * sin(t * 0.9)
      let gape = birdGape(at: feet, scale: scale, pose: pose)
      let drawnF = lin(p, 1.3, 2.15)
      TsugumiTemari.draw(g, center: temari.center, radius: temari.radius, spin: -drawnF * 6)
      drawBird(g, at: feet, scale: scale, pose: pose)

      // the Cards appear as the thread reaches them
      for k in 0..<3 {
        let r = card(k)
        let reach = Double((r.minX - gape.x) / (card(2).maxX - gape.x))
        let u = span(drawnF, reach - 0.02, reach + 0.1)
        guard u > 0.01 else { continue }
        let c = g.faded(u)
        let rr = r.offsetBy(dx: 0, dy: CGFloat(1 - u) * 10)
        c.fill(Path(roundedRect: rr, cornerRadius: 9), with: .color(TsugumiPalette.hex(0x1d3a3e, 0.92)))
        c.stroke(Path(roundedRect: rr, cornerRadius: 9), with: .color(TsugumiPalette.olive.opacity(0.85)), lineWidth: 1.4)
        c.draw(Text(Image(systemName: "chevron.left.forwardslash.chevron.right")).font(.system(size: 10, weight: .semibold)).foregroundStyle(Ink.mist),
               at: pt(rr.minX + 16, rr.minY + 17))
        c.text(names[k], at: pt(rr.minX + 30, rr.minY + 17), size: 13, weight: .semibold, color: Ink.cream, anchor: .leading)
        c.draw(Text(Image(systemName: "checkmark.circle.fill")).font(.system(size: 11)).foregroundStyle(TsugumiPalette.olive),
               at: pt(rr.maxX - 14, rr.minY + 17))
        for (y, w) in [(35.0, 70.0), (44.0, 50.0)] {
          c.fill(Path(roundedRect: CGRect(x: rr.minX + 12, y: rr.minY + CGFloat(y), width: CGFloat(w), height: 3), cornerRadius: 1.5),
                 with: .color(Ink.cream.opacity(0.25)))
        }
      }
    }
  }
}
