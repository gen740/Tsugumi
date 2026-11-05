import AppKit
import SwiftUI

extension Explainer {
  // 2. もし、書いた通りのものがそのまま形になったら？
  // Three boxes sketched on paper (download → split by energy → draw) become
  // the Draft Cards of the Cas A demo on the app's canvas; the page's thread
  // runs through them as the Agent implements each, the run lights them, and
  // the band table and the RGB image come out under them. The numbers and the
  // image are those of a run of the demo (ObsID 25628).

  enum CasAScene {
    struct Box {
      let hand: String          // the sketch's words
      let name: String          // the Card
      let summary: String
      let input: String, output: String
      let stack: String
    }
    static let boxes: [Box] = [
      Box(hand: "データを\nダウンロード", name: "load", summary: "観測データを Nix で固定する",
          input: "start", output: "events", stack: "Nix · FITS"),
      Box(hand: "エネルギーで\n区切る", name: "bands", summary: "エネルギー帯で分ける",
          input: "input", output: "images", stack: "Python · numpy · astropy"),
      Box(hand: "描画する", name: "rgb", summary: "RGB画像にする",
          input: "input", output: "rgb", stack: "Python · pillow"),
    ]

    static let paper = CGRect(x: 70, y: 150, width: 860, height: 590)
    static let canvas = CGRect(x: 24, y: 110, width: 952, height: 690)
    static func sketchRect(_ k: Int) -> CGRect { CGRect(x: 137 + CGFloat(k) * 265, y: 400, width: 196, height: 140) }
    static func cardRect(_ k: Int) -> CGRect { CGRect(x: 195 + CGFloat(k) * 240, y: 312, width: 210, height: 92) }
    static let startRect = CGRect(x: 52, y: 339, width: 104, height: 38)
    static var startPort: CGPoint { pt(startRect.maxX, startRect.midY) }
    static func inPort(_ r: CGRect) -> CGPoint { pt(r.minX, r.minY + 64) }
    static func outPort(_ r: CGRect) -> CGPoint { pt(r.maxX, r.minY + 64) }
    static let runButton = CGRect(x: 396, y: 130, width: 92, height: 28)

    // When things happen, in story positions (where the reading line is in the figure).
    static func drawn(_ k: Int) -> (Double, Double) { (0.8 + Double(k) * 0.18, 0.98 + Double(k) * 0.18) }
    static let morph = (1.85, 2.4)
    static func implement(_ k: Int) -> (Double, Double) { (2.95 + Double(k) * 0.16, 3.07 + Double(k) * 0.16) }
    static let press = 3.5
    static func run(_ k: Int) -> (Double, Double) { (3.56 + Double(k) * 0.14, 3.7 + Double(k) * 0.14) }

    enum RunState { case idle, running, done }
    static func state(_ k: Int, _ p: Double) -> RunState {
      let r = run(k)
      return p < r.0 ? .idle : p < r.1 ? .running : .done
    }

    static func draw(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      let enter = span(p, -0.42, 0.1)
      let u = span(p, morph.0, morph.1)
      let g0 = g.faded(enter)
      drawSheet(g0, p, u, rise: CGFloat(1 - enter) * 40)
      drawToolbar(g0, p, t, u)

      // the sketch stays on the paper's side of the wipe
      var sketch = g0
      sketch.clip(to: side(u, dark: false))
      drawTitle(sketch, p)
      drawCasDoodle(sketch, p, t)

      // each box turns into its Card as the wipe passes it
      let us = (0..<3).map { boxU($0, u) }
      let su = span(us[0], 0.5, 1)
      if su > 0.01 { drawStart(g0.faded(su), p, t) }
      for k in 0..<3 { drawArrow(g0, k, p, k == 0 ? us[0] : (us[k - 1] + us[k]) / 2, t) }
      for k in 0..<3 { drawBox(g0, k, p, us[k], t) }

      drawResults(g0, p, t)
      drawHandLabels(g0, p)
    }

    // MARK: Paper → canvas

    /// The wipe: a slanted edge sweeping left to right; the canvas behind it.
    static let slant: CGFloat = 0.4
    static func wipeX(_ u: Double, _ y: CGFloat) -> CGFloat {
      paper.minX - 300 + (paper.width + 700) * CGFloat(u) - (y - paper.minY) * slant
    }
    static func side(_ u: Double, dark: Bool) -> Path {
      let top: CGFloat = -100, bottom = artH + 100, far: CGFloat = dark ? -3000 : 4000
      var p = Path()
      p.addLines([pt(far, top), pt(wipeX(u, top), top), pt(wipeX(u, bottom), bottom), pt(far, bottom)])
      p.closeSubpath()
      return p
    }
    static func boxU(_ k: Int, _ u: Double) -> Double {
      let c = pt(sketchRect(k).midX, sketchRect(k).midY)
      let at = Double((c.x + (c.y - paper.minY) * slant - paper.minX + 300) / (paper.width + 700))
      return span(u, at - 0.03, at + 0.17)
    }


    static func drawSheet(_ g: GraphicsContext, _ p: Double, _ u: Double, rise: CGFloat) {
      let r = mix(paper, canvas, smooth(u)).offsetBy(dx: 0, dy: rise)
      let corner = mix(6, 14, u)
      let shape = Path(roundedRect: r, cornerRadius: corner)
      var sh = g
      sh.addFilter(.blur(radius: 14))
      sh.fill(Path(roundedRect: r.offsetBy(dx: 0, dy: 10), cornerRadius: corner), with: .color(.black.opacity(0.35 - 0.15 * u)))

      // paper, with ruled lines
      var paperSide = g
      paperSide.clip(to: side(u, dark: false))
      paperSide.fill(shape, with: .color(mix(Ink.paper, Ink.paper, 0)))
      var y = r.minY + 46
      while y < r.maxY - 10 {
        var l = Path(); l.move(to: pt(r.minX + 14, y)); l.addLine(to: pt(r.maxX - 14, y))
        paperSide.stroke(l, with: .color(TsugumiPalette.hex(0x7aa0c0, 0.16)), lineWidth: 1)
        y += 36
      }
      var m = Path(); m.move(to: pt(r.minX + 46, r.minY + 8)); m.addLine(to: pt(r.minX + 46, r.maxY - 8))
      paperSide.stroke(m, with: .color(TsugumiPalette.hex(0xd77b6a, 0.25)), lineWidth: 1)

      // the canvas, with its dot grid
      guard u > 0.001 else { return }
      var canvasSide = g
      canvasSide.clip(to: side(u, dark: true))
      canvasSide.fill(shape, with: .color(mix(Ink.canvas, Ink.canvas, 0)))
      var d = Path()
      var yy = r.minY + 12
      while yy < r.maxY - 6 {
        var x = r.minX + 12
        while x < r.maxX - 6 { d.addEllipse(in: CGRect(x: x - 0.9, y: yy - 0.9, width: 1.8, height: 1.8)); x += 22 }
        yy += 22
      }
      canvasSide.fill(d, with: .color(.white.opacity(0.13)))

      // the wipe's edge, glowing
      if u < 0.999 {
        var edge = Path()
        edge.move(to: pt(wipeX(u, r.minY), r.minY)); edge.addLine(to: pt(wipeX(u, r.maxY), r.maxY))
        var e = g
        e.clip(to: shape)
        var glow = e
        glow.addFilter(.blur(radius: 10))
        glow.stroke(edge, with: .color(TsugumiPalette.hex(0xf6d79a, 0.8)), lineWidth: 14)
        e.stroke(edge, with: .color(TsugumiPalette.hex(0xfff3d6, 0.9)), lineWidth: 2)
      }
    }

    static func drawToolbar(_ g: GraphicsContext, _ p: Double, _ t: Double, _ u: Double) {
      let o = span(u, 0.7, 1)
      guard o > 0.01 else { return }
      let g = g.faded(o)
      let bar = CGRect(x: canvas.minX + 16, y: canvas.minY + 16, width: 250, height: 36)
      g.fill(Path(roundedRect: bar, cornerRadius: 18), with: .color(.white.opacity(0.07)))
      g.text("Cas A · Chandra RGB", at: pt(bar.minX + 18, bar.midY), size: 14, weight: .semibold, color: TsugumiPalette.hex(0xe8e8ea), anchor: .leading)

      let mid = CGRect(x: runButton.minX - 8, y: canvas.minY + 16, width: 330, height: 36)
      g.fill(Path(roundedRect: mid, cornerRadius: 18), with: .color(.white.opacity(0.07)))
      let pressed = 1 - smooth(abs(p - press) / 0.05)
      var b = g
      b.translateBy(x: runButton.midX, y: runButton.midY)
      b.scaleBy(x: 1 - 0.06 * pressed, y: 1 - 0.06 * pressed)
      let br = CGRect(x: -runButton.width / 2, y: -runButton.height / 2, width: runButton.width, height: runButton.height)
      b.fill(Path(roundedRect: br, cornerRadius: 7), with: .color(TsugumiPalette.hex(pressed > 0.1 ? 0x5a9cff : 0x2f7cf6)))
      b.draw(Text("\(Image(systemName: "play.fill"))  実行").font(.system(size: 14, weight: .semibold)).foregroundStyle(.white), at: .zero)

      let sx = runButton.maxX + 14, sy = runButton.midY
      if p >= run(0).0 {
        let doneAll = state(2, p) == .done
        let elapsed = doneAll ? 2.41 : (p - run(0).0) / (run(2).1 - run(0).0) * 2.41
        let icon = doneAll ? "checkmark.circle.fill" : "circle.dotted"
        let color = doneAll ? Ink.done : Ink.badge
        g.draw(Text("\(Image(systemName: icon)) \(doneAll ? "成功" : "実行中")").font(.system(size: 13, weight: .semibold)).foregroundStyle(color),
               at: pt(sx, sy), anchor: .leading)
        g.text(String(format: "00:00:%05.2f", elapsed), at: pt(sx + 72, sy), size: 13, color: TsugumiPalette.hex(0xc8c8cc), anchor: .leading, mono: true)
      }
    }

    // MARK: The sketch

    static func drawTitle(_ g: GraphicsContext, _ p: Double) {
      let w = lin(p, 0.15, 0.75)
      guard w > 0 else { return }
      var c = g
      c.clip(to: Path(CGRect(x: 0, y: 0, width: 140 + 520 * CGFloat(w), height: artH)))
      childText(c, "Cas A を 3 色で見たい！", at: pt(140, 222), size: 38, color: Ink.penC, seed: 3, leading: true)
      let under = Polyline((0...30).map { i in
        let s = Double(i) / 30
        return pt(136 + CGFloat(s) * 450, 258 + CGFloat(sin(s * 9) * 3.5 + s * 6))
      })
      c.stroke(under.path(upTo: lin(p, 0.55, 0.9)), with: .color(TsugumiPalette.hex(0xd9844a, 0.85)), style: StrokeStyle(lineWidth: 5, lineCap: .round))
    }

    /// Cas A in crayon: a knotty shell of red, green and blue filaments, the
    /// north-east jet and the dot in the middle. It draws itself once it
    /// crosses the reading line, then its knots keep glinting.
    static func drawCasDoodle(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      let since = SceneOptions.since
      guard since > 0 else { return }
      let d = clamp01(since / 2.2)
      let c = pt(790, 226), R = 50.0
      var rng = SeededRandom(seed: 9)
      func arcPath(_ a0: Double, _ len: Double, _ rr: Double, _ seed: Double) -> Path {
        var arc = Path()
        for k in 0...10 {
          let a = a0 + len * Double(k) / 10
          let r = rr * (1 + 0.06 * sin(a * 7 + seed)) + 1.5 * sin(Double(k) * 1.7 + seed)
          let q = pt(c.x + CGFloat(cos(a) * r), c.y + CGFloat(sin(a) * r * 0.94))
          if k == 0 { arc.move(to: q) } else { arc.addLine(to: q) }
        }
        return arc
      }
      // the night behind it, shaded in with a dark crayon
      let shade = clamp01(d / 0.3)
      for i in 0..<26 {
        let y = c.y - CGFloat(R) * 1.05 + CGFloat(i) * CGFloat(R) * 2.1 / 26
        let half = sqrt(max(0, pow(R * 1.08, 2) - pow(Double(y - c.y), 2)))
        guard half > 2, Double(i) / 26 < shade else { continue }
        var h = Path()
        h.move(to: pt(c.x - CGFloat(half) + CGFloat(rng.next(-3, 3)), y + 3)); h.addLine(to: pt(c.x + CGFloat(half) + CGFloat(rng.next(-3, 3)), y - 3))
        g.stroke(h, with: .color(TsugumiPalette.hex(0x262038, 0.82)), style: StrokeStyle(lineWidth: 5, lineCap: .round))
      }
      // the shell: knotty filaments, red and green with some purple, brighter on one side
      let n = 40
      for i in 0..<n {
        let a0 = rng.next(0, 2 * .pi), len = rng.next(0.35, 1.0)
        let rr = R * rng.next(0.5, 0.95)
        let pick = rng.next(0, 1)
        let col: UInt32 = pick < 0.42 ? 0xe2533f : pick < 0.78 ? 0x5ec25a : 0xc25ad0
        let w = CGFloat(rng.next(2.6, 5.0))
        let start = 0.25 + Double(i) / Double(n) * 0.5
        let u = clamp01((d - start) / 0.22)
        guard u > 0 else { continue }
        let bright = 0.6 + 0.4 * max(0, cos(a0 - 3.4))
        g.stroke(arcPath(a0, len, rr, Double(i)).trimmedPath(from: 0, to: u), with: .color(TsugumiPalette.hex(col, 0.9 * bright)),
                 style: StrokeStyle(lineWidth: w, lineCap: .round, lineJoin: .round))
      }
      // the hard rim, thin and blue
      for i in 0..<7 {
        let u = clamp01((d - 0.72 - Double(i) * 0.02) / 0.15)
        guard u > 0 else { continue }
        g.stroke(arcPath(Double(i) * 0.95, 1.1, R * 1.04, Double(i) + 40).trimmedPath(from: 0, to: u), with: .color(TsugumiPalette.hex(0x5b8bf0, 0.85)),
                 style: StrokeStyle(lineWidth: 1.8, lineCap: .round))
      }
      // the jet, north-east, and the compact object
      let j = clamp01((d - 0.7) / 0.2)
      if j > 0 {
        var jet = Path()
        jet.move(to: pt(c.x + CGFloat(cos(-0.95) * R * 0.95), c.y + CGFloat(sin(-0.95) * R * 0.95)))
        jet.addLine(to: pt(c.x + CGFloat(cos(-0.9) * R * 1.5), c.y + CGFloat(sin(-0.9) * R * 1.5)))
        g.stroke(jet.trimmedPath(from: 0, to: j), with: .color(TsugumiPalette.hex(0x5ec25a, 0.9)), style: StrokeStyle(lineWidth: 2.4, lineCap: .round))
        g.fill(Path(ellipseIn: CGRect(x: c.x - 2.5, y: c.y - 2.5, width: 5, height: 5)), with: .color(Color.white.opacity(0.9 * j)))
      }
      // knots glinting once it is drawn
      let glint = clamp01((since - 2.0) / 0.6)
      if glint > 0 {
        for k in 0..<5 {
          let a = Double(k) * 1.3 + 0.4, r = R * (0.7 + 0.06 * Double(k % 3))
          let q = pt(c.x + CGFloat(cos(a) * r), c.y + CGFloat(sin(a) * r))
          let o = glint * max(0, sin(t * 2.2 + Double(k) * 1.9))
          let s = CGFloat(3 + 3 * o)
          var star = Path()
          star.move(to: pt(q.x - s, q.y)); star.addLine(to: pt(q.x + s, q.y))
          star.move(to: pt(q.x, q.y - s)); star.addLine(to: pt(q.x, q.y + s))
          g.stroke(star, with: .color(TsugumiPalette.hex(0xf2c14e, o)), style: StrokeStyle(lineWidth: 1.8, lineCap: .round))
        }
      }
      childText(g, "Cas A", at: pt(c.x + 62, c.y + 46), size: 20, color: Ink.penC, opacity: clamp01((d - 0.8) / 0.2), seed: 21, leading: true)
    }

    static func drawArrow(_ g: GraphicsContext, _ k: Int, _ p: Double, _ u: Double, _ t: Double) {
      // sketch arrows join the boxes; the start Card's wire comes with the canvas
      let n = 28
      let a: CGPoint, b: CGPoint
      let target: [CGPoint]
      var drawnF = 1.0
      if k == 0 {
        // start → load: no sketch counterpart
        // the start Card is a pill: its port is on its right end, at its middle
        let w = wire(startPort, inPort(cardRect(0)))
        let o = span(u, 0.6, 1)
        guard o > 0.01 else { return }
        drawWire(g.faded(o), Polyline(Polyline.curve(w.0, w.1, w.2, w.3, n: n)), p: p, t: t, into: 0, ink: 1)
        return
      }
      let from = sketchRect(k - 1), to = sketchRect(k)
      a = pt(from.maxX + 10, from.midY); b = pt(to.minX - 12, to.midY)
      let (d0, d1) = drawn(k - 1)
      drawnF = lin(p, d1 - 0.05, d1 + 0.12)
      guard drawnF > 0 else { return }
      let sketch = (0...n).map { i -> CGPoint in
        let s = Double(i) / Double(n)
        return pt(mix(a.x, b.x, s), a.y + CGFloat(sin(s * 7 + Double(k)) * 5 * (1 - u)) - CGFloat(sin(s * .pi)) * 10 * CGFloat(1 - u))
      }
      _ = d0
      let w = wire(outPort(cardRect(k - 1)), inPort(cardRect(k)))
      target = Polyline.curve(w.0, w.1, w.2, w.3, n: n)
      let line = Polyline(zip(sketch, target).map { mix($0, $1, u) })
      drawWire(g, line, p: p, t: t, into: k, ink: u, drawn: drawnF)
      // the arrowhead, gone once it is a wire
      let head = (1 - span(u, 0, 0.35)) * span(drawnF, 0.8, 1)
      if head > 0.01 {
        var h = Path()
        h.move(to: pt(b.x - 14, b.y - 10)); h.addLine(to: b); h.addLine(to: pt(b.x - 13, b.y + 11))
        g.stroke(h, with: .color(Ink.penC.opacity(head)), style: StrokeStyle(lineWidth: 3, lineCap: .round, lineJoin: .round))
      }
    }

    /// A wire into Card `into`: pen ink on paper, orange on the canvas, its dashes
    /// flowing while that Card runs.
    static func drawWire(_ g: GraphicsContext, _ line: Polyline, p: Double, t: Double, into: Int, ink: Double, drawn: Double = 1) {
      let color = mix(Ink.pen, rgb(0xe0893a), ink)
      let width = mix(3.2, 2, ink)
      let running = ink > 0.99 && state(into, p) == .running
      if running {
        g.stroke(line.path(), with: .color(Ink.wire.opacity(0.35)), style: StrokeStyle(lineWidth: width))
        g.stroke(line.path(), with: .color(TsugumiPalette.hex(0xffb36b)),
                 style: StrokeStyle(lineWidth: width + 0.6, lineCap: .round, dash: [8, 6], dashPhase: CGFloat(-t * 40)))
      } else {
        g.stroke(line.path(upTo: drawn), with: .color(color), style: StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round))
      }
    }

    static func drawStart(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      let r = startRect
      let path = Path(roundedRect: r, cornerRadius: r.height / 2)
      g.fill(path, with: .color(mix(Ink.cardFill, Ink.cardFill, 0)))
      let done = p >= run(0).0
      g.stroke(path, with: .color(done ? Ink.done : mix(Ink.cardStroke, Ink.cardStroke, 0)), lineWidth: 1.4)
      let c = pt(r.minX + 19, r.midY)
      g.fill(Path(ellipseIn: CGRect(x: c.x - 9, y: c.y - 9, width: 18, height: 18)), with: .color(TsugumiPalette.hex(0x2f7cf6)))
      g.draw(Text(Image(systemName: "play.fill")).font(.system(size: 8, weight: .bold)).foregroundStyle(.white), at: pt(c.x + 1, c.y))
      g.text("start", at: pt(r.minX + 36, r.midY), size: 13, weight: .semibold, color: TsugumiPalette.hex(0xe8e8ea), anchor: .leading)
      if done {
        g.draw(Text(Image(systemName: "checkmark.circle.fill")).font(.system(size: 11)).foregroundStyle(Ink.done), at: pt(r.maxX - 14, r.midY))
      }
      g.fill(Path(ellipseIn: CGRect(x: r.maxX - 4, y: r.midY - 4, width: 8, height: 8)), with: .color(TsugumiPalette.hex(0x3b82f6)))
    }

    static func drawBox(_ g: GraphicsContext, _ k: Int, _ p: Double, _ u: Double, _ t: Double) {
      let box = boxes[k]
      let (d0, d1) = drawn(k)
      let df = lin(p, d0, d1)
      guard df > 0 else { return }
      let r = mix(sketchRect(k), cardRect(k), u)
      let impl = span(p, implement(k).0, implement(k).1)
      let st = state(k, p)
      let wobble = 7 * CGFloat(1 - smooth(u * 1.25))
      let outline = Polyline(Explainer.sketchRect(r, radius: mix(18, 10, u), wobble: wobble, seed: Double(k) * 2.3))
      let shape = outline.path(upTo: u > 0.01 ? 1 : df)

      // fill (the Card's body)
      if u > 0.01 {
        var f = g
        f.opacity *= u
        f.fill(Path(roundedRect: r, cornerRadius: 10), with: .color(mix(Ink.cardFill, Ink.cardFill, 0)))
      }
      // border: pen → grey dashes (a Draft) → solid (implemented) → running / done
      let draftDash = span(u, 0.5, 0.85) * (1 - impl)
      var stroke = mix(Ink.pen, Ink.cardStroke, u)
      var width = mix(3.2, 1.4, u)
      if st == .running {
        let pulse = 0.5 + 0.5 * sin(t * 7)
        stroke = Color(.sRGB, red: 0.9, green: 0.55 + 0.1 * pulse, blue: 0.25, opacity: 1)
        width = 2
      } else if st == .done {
        stroke = Ink.done
        width = 1.8
      }
      g.faded(1 - draftDash).stroke(shape, with: .color(stroke), style: StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round))
      if draftDash > 0.01 {
        g.faded(draftDash).stroke(Path(roundedRect: r, cornerRadius: 10), with: .color(TsugumiPalette.hex(0x9aa0a8)),
                                 style: StrokeStyle(lineWidth: 1.4, dash: [6, 4]))
      }
      if st == .running {
        var glow = g
        glow.addFilter(.blur(radius: 8))
        glow.stroke(Path(roundedRect: r, cornerRadius: 10), with: .color(Ink.wire.opacity(0.35 + 0.2 * sin(t * 7))), lineWidth: 4)
      }

      // the sketch's words and doodle, written after the box
      let hand = span(p, d1 - 0.08, d1 + 0.12) * (1 - span(u, 0, 0.45))
      if hand > 0.01 {
        childText(g, box.hand, at: pt(r.midX, k == 0 ? r.minY + 50 : r.midY), size: 27, color: Ink.penC, opacity: hand, seed: UInt64(11 + k))
        if k == 0 {
          // just an arrow down: download
          let a = lin(p, d1, d1 + 0.18)
          var down = Path()
          down.move(to: pt(r.midX + 2, r.minY + 86)); down.addQuadCurve(to: pt(r.midX - 1, r.minY + 122), control: pt(r.midX + 6, r.minY + 104))
          down.move(to: pt(r.midX - 13, r.minY + 108)); down.addLine(to: pt(r.midX - 1, r.minY + 123)); down.addLine(to: pt(r.midX + 11, r.minY + 106))
          g.faded(hand).stroke(down.trimmedPath(from: 0, to: a), with: .color(TsugumiPalette.hex(0xd9844a)),
                               style: StrokeStyle(lineWidth: 4, lineCap: .round, lineJoin: .round))
        }
      }

      // the Card's face
      let face = span(u, 0.55, 1)
      guard face > 0.01 else { return }
      let f = g.faded(face)
      let titleColor = TsugumiPalette.hex(0xe8e8ea), sub = TsugumiPalette.hex(0x9a9ca2)
      let icon = impl > 0.5 ? "chevron.left.forwardslash.chevron.right" : "pencil.and.outline"
      f.draw(Text(Image(systemName: icon)).font(.system(size: 11, weight: .semibold)).foregroundStyle(sub), at: pt(r.minX + 18, r.minY + 17))
      f.text(box.name, at: pt(r.minX + 34, r.minY + 17), size: 14, weight: .semibold, color: titleColor, anchor: .leading)
      // run state at the top right
      let sr = pt(r.maxX - 16, r.minY + 17)
      if st == .running {
        var arc = Path()
        let a0 = t * 6
        arc.addArc(center: sr, radius: 6, startAngle: .radians(a0), endAngle: .radians(a0 + 4.2), clockwise: false)
        f.stroke(arc, with: .color(Ink.badge), style: StrokeStyle(lineWidth: 2, lineCap: .round))
      } else if st == .done {
        let pu = pop(lin(p, run(k).1, run(k).1 + 0.08))
        var c = f
        c.translateBy(x: sr.x, y: sr.y); c.scaleBy(x: CGFloat(pu), y: CGFloat(pu))
        c.draw(Text(Image(systemName: "checkmark.circle.fill")).font(.system(size: 13)).foregroundStyle(Ink.done), at: .zero)
      }
      f.text(box.summary, at: pt(r.minX + 14, r.minY + 41), size: 11.5, color: sub, anchor: .leading)
      f.text(box.input, at: pt(r.minX + 12, r.minY + 64), size: 10, color: sub, anchor: .leading)
      f.text(box.output, at: pt(r.maxX - 12, r.minY + 64), size: 10, color: sub, anchor: .trailing)
      // ports
      let ip = inPort(r), op = outPort(r)
      f.fill(Path(CGRect(x: ip.x - 3.5, y: ip.y - 3.5, width: 7, height: 7)), with: .color(TsugumiPalette.hex(0xd8d8dc)))
      f.fill(Path(ellipseIn: CGRect(x: op.x - 4, y: op.y - 4, width: 8, height: 8)), with: .color(TsugumiPalette.hex(0x3b82f6)))
      // what it is now: nothing for a Draft; its stack; then its run
      let row = st == .done ? "実行 #1 · 成功" : impl > 0.5 ? box.stack : ""
      if !row.isEmpty {
        f.text(row, at: pt(r.minX + 14, r.minY + 81), size: 9.5, color: st == .done ? Ink.done.opacity(0.9) : sub, anchor: .leading,
               opacity: span(p, implement(k).1 - 0.05, implement(k).1 + 0.05))
      }
      // sparkles when the Agent fills it in
      let sp = lin(p, implement(k).0, implement(k).1 + 0.08)
      if sp > 0, sp < 1 {
        for i in 0..<7 {
          let a = Double(i) / 7 * 2 * .pi + Double(k)
          let d = 30 + 70 * easeOut(sp)
          let q = pt(r.midX + CGFloat(cos(a) * d * 1.3), r.midY + CGFloat(sin(a) * d * 0.6))
          let s = CGFloat(5 * sin(sp * .pi))
          var star = Path()
          star.move(to: pt(q.x, q.y - s)); star.addLine(to: pt(q.x + s * 0.3, q.y - s * 0.3)); star.addLine(to: pt(q.x + s, q.y))
          star.addLine(to: pt(q.x + s * 0.3, q.y + s * 0.3)); star.addLine(to: pt(q.x, q.y + s)); star.addLine(to: pt(q.x - s * 0.3, q.y + s * 0.3))
          star.addLine(to: pt(q.x - s, q.y)); star.addLine(to: pt(q.x - s * 0.3, q.y - s * 0.3)); star.closeSubpath()
          g.fill(star, with: .color(TsugumiPalette.hex(0xf6d79a, sin(sp * .pi))))
        }
      }
    }

    // MARK: Results

    static func drawResults(_ g: GraphicsContext, _ p: Double, _ t: Double) {
      // bands: the photon counts per band
      let tb = span(p, run(1).1, run(1).1 + 0.18)
      if tb > 0.01 {
        let r = CGRect(x: 435, y: 452 - CGFloat(1 - tb) * 12, width: 210, height: 118)
        let f = g.faded(tb)
        connector(f, from: pt(cardRect(1).midX, cardRect(1).maxY), to: pt(r.midX, r.minY))
        panel(f, r, title: "bands", sub: "実行 #1")
        let cols: [CGFloat] = [r.minX + 12, r.minX + 76, r.maxX - 12]
        f.text("帯域", at: pt(cols[0], r.minY + 36), size: 9.5, weight: .semibold, color: TsugumiPalette.hex(0xc8c8cc), anchor: .leading)
        f.text("エネルギー (eV)", at: pt(cols[1], r.minY + 36), size: 9.5, weight: .semibold, color: TsugumiPalette.hex(0xc8c8cc), anchor: .leading)
        f.text("光子数", at: pt(cols[2], r.minY + 36), size: 9.5, weight: .semibold, color: TsugumiPalette.hex(0xc8c8cc), anchor: .trailing)
        let rows: [(String, String, String, UInt32)] = [("soft", "600–1650", "101,187", 0xe0574f), ("medium", "1650–2250", "157,471", 0x5fb35a),
                                                        ("hard", "2250–7500", "110,852", 0x4a7fd6)]
        for (i, row) in rows.enumerated() {
          let y = r.minY + 56 + CGFloat(i) * 18
          let o = span(p, run(1).1 + 0.05 + Double(i) * 0.04, run(1).1 + 0.12 + Double(i) * 0.04)
          f.fill(Path(ellipseIn: CGRect(x: cols[0], y: y - 3, width: 6, height: 6)), with: .color(TsugumiPalette.hex(row.3, o)))
          f.text(row.0, at: pt(cols[0] + 10, y), size: 10, color: TsugumiPalette.hex(0xe0e0e4), anchor: .leading, opacity: o, mono: true)
          f.text(row.1, at: pt(cols[1], y), size: 10, color: TsugumiPalette.hex(0xe0e0e4), anchor: .leading, opacity: o, mono: true)
          f.text(row.2, at: pt(cols[2], y), size: 10, color: TsugumiPalette.hex(0xe0e0e4), anchor: .trailing, opacity: o, mono: true)
        }
      }
      // rgb: the image
      let ti = span(p, run(2).1, run(2).1 + 0.15)
      if ti > 0.01 {
        let r = CGRect(x: 675, y: 452 - CGFloat(1 - ti) * 12, width: 210, height: 244)
        let f = g.faded(ti)
        connector(f, from: pt(cardRect(2).midX, cardRect(2).maxY), to: pt(r.midX, r.minY))
        panel(f, r, title: "rgb", sub: "実行 #1")
        let img = CGRect(x: r.minX + 6, y: r.minY + 34, width: r.width - 12, height: r.width - 12)
        f.fill(Path(img), with: .color(.black))
        let develop = span(p, run(2).1 + 0.05, run(2).1 + 0.45)
        if let real = realImage {
          // the rgb Card's own output (ObsID 25628), developing from the middle
          var d = f
          let rr = img.width * 0.75 * CGFloat(easeOut(develop))
          d.clip(to: Path(ellipseIn: CGRect(x: img.midX - rr, y: img.midY - rr, width: rr * 2, height: rr * 2)))
          d.opacity *= develop
          d.draw(Image(nsImage: real).interpolation(.high), in: img)
        }
      }
    }

    /// The rgb Card's output from a real run of the demo (ObsID 25628, the counts in the table).
    static let realImage: NSImage? = file("casa-rgb-25628.png", repo: "macOS/Resources/explainer/casa-rgb-25628.png").flatMap(NSImage.init(contentsOf:))

    static func panel(_ g: GraphicsContext, _ r: CGRect, title: String, sub: String) {
      var sh = g
      sh.addFilter(.blur(radius: 8))
      sh.fill(Path(roundedRect: r.offsetBy(dx: 0, dy: 5), cornerRadius: 8), with: .color(.black.opacity(0.4)))
      g.fill(Path(roundedRect: r, cornerRadius: 8), with: .color(TsugumiPalette.hex(0x232427)))
      g.stroke(Path(roundedRect: r, cornerRadius: 8), with: .color(TsugumiPalette.hex(0xa45ad8, 0.75)), lineWidth: 1.2)
      g.draw(Text("\(Image(systemName: "chart.xyaxis.line"))  ").font(.system(size: 9)).foregroundStyle(TsugumiPalette.hex(0xa45ad8)),
             at: pt(r.minX + 10, r.minY + 15), anchor: .leading)
      g.text(title, at: pt(r.minX + 26, r.minY + 15), size: 10.5, weight: .semibold, color: TsugumiPalette.hex(0xe8e8ea), anchor: .leading)
      g.text(sub, at: pt(r.minX + 30 + CGFloat(title.count) * 6.5, r.minY + 15), size: 9, color: TsugumiPalette.hex(0x9a9ca2), anchor: .leading)
    }

    static func connector(_ g: GraphicsContext, from a: CGPoint, to b: CGPoint) {
      var l = Path(); l.move(to: a); l.addLine(to: b)
      g.stroke(l, with: .color(TsugumiPalette.hex(0xa45ad8, 0.75)), style: StrokeStyle(lineWidth: 1.2, dash: [5, 4]))
      g.fill(Path(ellipseIn: CGRect(x: a.x - 3.5, y: a.y - 3.5, width: 7, height: 7)), with: .color(TsugumiPalette.hex(0xc06cf0)))
    }

    /// The sketch's words come back over the Cards: the same three boxes.
    static func drawHandLabels(_ g: GraphicsContext, _ p: Double) {
      let o = span(p, 5.1, 5.45)
      guard o > 0.01 else { return }
      let words = ["データをダウンロード", "エネルギーで区切る", "描画する"]
      for k in 0..<3 {
        let r = cardRect(k)
        let ok = o * span(p, 5.1 + Double(k) * 0.08, 5.3 + Double(k) * 0.08)
        g.text(words[k], at: pt(r.midX, r.minY - 30), size: 20, color: Ink.cream, opacity: ok, font: handFont(20))
        var tick = Path(); tick.move(to: pt(r.midX, r.minY - 16)); tick.addLine(to: pt(r.midX, r.minY - 4))
        g.stroke(tick, with: .color(Ink.cream.opacity(0.6 * ok)), style: StrokeStyle(lineWidth: 2, lineCap: .round, dash: [3, 3]))
      }
    }


  }
}
