import AppKit
import SwiftUI

// The explainer as documents: a list of them on the left, the selected one as
// a page on the right. One thread runs down each page: out of the temari by
// the title, through a needle hole at the start of each paragraph, and through
// the figures, which sit on the page without frames. It is drawn down to the
// reading line, with the tsugumi at its end. In the figures the same thread
// goes through the hole in the wall and becomes the wires between Cards;
// words of the list above fly into the wall; list items light up as the
// figure below acts them out. Scroll back and all of it goes back.

extension Explainer {
  /// When each figure's trigger crossed the reading line (moments that play once, then stay).
  final class TriggerClock {
    var times: [String: Double] = [:]
  }
  struct MomentsKey: EnvironmentKey { nonisolated(unsafe) static let defaultValue = TriggerClock() }

  enum DocMetrics {
    static let sidebar: CGFloat = 260
    static let column: CGFloat = 680
    static let figureMax: CGFloat = 1000
    static let margin: CGFloat = 64
    /// The reading line, as a share of the window's height.
    static let readingLine: CGFloat = 0.5
  }

  /// Where each block (and each list item) is on the page.
  struct BlockID: Hashable { let index: Int; var item: Int = -1 }
  struct BlockFrames: PreferenceKey {
    static let defaultValue: [BlockID: Anchor<CGRect>] = [:]
    static func reduce(value: inout [BlockID: Anchor<CGRect>], nextValue: () -> [BlockID: Anchor<CGRect>]) {
      value.merge(nextValue()) { $1 }
    }
  }

  // MARK: - The page

  struct DocContent: View {
    let doc: Doc
    let pageWidth: CGFloat
    let scrollY: CGFloat
    let viewport: CGFloat

    var body: some View {
      VStack(alignment: .center, spacing: 0) {
        HStack(spacing: 8) {
          Image(systemName: "doc.text")
          Text("\(Text(doc.source).font(.system(size: 12, design: .monospaced))) と同じ文書です").font(.system(size: 12))
        }
        .foregroundStyle(DocInk.secondary)
        .frame(width: DocMetrics.column, alignment: .leading)
        .padding(.bottom, 18)
        ForEach(Array(doc.blocks.enumerated()), id: \.offset) { i, b in
          block(b, i)
        }
        Color.clear.frame(height: viewport * 0.45)  // room to read the last lines at the reading line
      }
      .padding(.top, DocMetrics.margin)
      .frame(width: pageWidth)
      .backgroundPreferenceValue(BlockFrames.self) { anchors in
        GeometryReader { proxy in
          PageLayer(doc: doc, rects: anchors.mapValues { proxy[$0] }, pageWidth: pageWidth, scrollY: scrollY, viewport: viewport, front: false)
        }
      }
      .overlayPreferenceValue(BlockFrames.self) { anchors in
        GeometryReader { proxy in
          PageLayer(doc: doc, rects: anchors.mapValues { proxy[$0] }, pageWidth: pageWidth, scrollY: scrollY, viewport: viewport, front: true)
        }
        .allowsHitTesting(false)
      }
    }

    private var figureWidth: CGFloat { min(pageWidth - 40, DocMetrics.figureMax) }

    @ViewBuilder private func block(_ b: MDBlock, _ i: Int) -> some View {
      if case .figure(let f) = b {
        // the figure is drawn by the page's layers; here is only its place, and its caption
        VStack(spacing: 6) {
          Color.clear.frame(width: figureWidth, height: figureWidth * f.crop.height / f.crop.width).anchored(i)
          Text(f.caption).font(.system(size: 13.5)).foregroundStyle(DocInk.secondary).lineSpacing(5)
            .multilineTextAlignment(.center).frame(width: DocMetrics.column)
        }
        .padding(.top, 4).padding(.bottom, 34)
      } else {
        BlockView(block: b, index: i)
      }
    }
  }

  /// One block of a document as text (headings, paragraphs, lists, tables,
  /// rules), at a width and a size (cards set it larger).
  struct BlockView: View {
    let block: MDBlock
    let index: Int
    var width: CGFloat = DocMetrics.column
    var scale: CGFloat = 1
    /// Text can be selected (not in cards, where a click moves on).
    var selectable = true

    private func size(_ v: CGFloat) -> CGFloat { v * scale }

    var body: some View {
      let i = index
      switch block {
      case .h1(let s):
        Text(MarkdownText.inline(s, accent: DocInk.heading)).font(.system(size: size(34), weight: .bold)).foregroundStyle(DocInk.heading)
          .lineSpacing(6).column(width).anchored(i).padding(.bottom, 26)
      case .h2(let s):
        Text(MarkdownText.inline(s, accent: DocInk.heading)).font(.system(size: size(25), weight: .bold)).foregroundStyle(DocInk.heading)
          .lineSpacing(5).column(width).anchored(i).padding(.top, 46 * scale).padding(.bottom, 16)
      case .h3(let s):
        Text(MarkdownText.inline(s)).font(.system(size: size(20), weight: .semibold)).foregroundStyle(DocInk.heading)
          .column(width).anchored(i).padding(.top, 28).padding(.bottom, 10)
      case .paragraph(let s):
        Text(MarkdownText.inline(s)).font(.system(size: size(17))).foregroundStyle(DocInk.text).lineSpacing(10 * scale)
          .selectable(selectable).column(width).anchored(i).padding(.bottom, 18)
      case .list(let items, let ordered):
        VStack(alignment: .leading, spacing: 12) {
          ForEach(Array(items.enumerated()), id: \.offset) { n, item in
            HStack(alignment: .firstTextBaseline, spacing: 12) {
              Text(ordered ? "\(n + 1)." : "•").font(.system(size: size(17), weight: .semibold)).foregroundStyle(TsugumiPalette.rufous)
                .frame(width: 22 * scale, alignment: .trailing)
              Text(MarkdownText.inline(item)).font(.system(size: size(17))).foregroundStyle(DocInk.text).lineSpacing(9 * scale)
                .selectable(selectable).fixedSize(horizontal: false, vertical: true)
            }
            .transformAnchorPreference(key: BlockFrames.self, value: .bounds) { $0[BlockID(index: i, item: n)] = $1 }
          }
        }
        .column(width).anchored(i).padding(.bottom, 20)
      case .table(let rows):
        Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 0) {
          ForEach(Array(rows.enumerated()), id: \.offset) { r, row in
            GridRow {
              ForEach(Array(row.enumerated()), id: \.offset) { _, cell in
                Text(MarkdownText.inline(cell)).font(.system(size: size(15), weight: r == 0 ? .semibold : .regular))
                  .foregroundStyle(r == 0 ? DocInk.heading : DocInk.text).lineSpacing(5)
                  .padding(.vertical, 9)
              }
            }
            if r < rows.count - 1 { Divider().overlay(DocInk.rule) }
          }
        }
        .column(width).anchored(i).padding(.bottom, 22)
      case .rule:
        Rectangle().fill(DocInk.rule).frame(height: 1).column(width).padding(.vertical, 24)
      case .figure:
        EmptyView()
      }
    }
  }

  // MARK: - The thread

  /// The page's thread: legs one after another, each with the stretch of the
  /// reading line it belongs to (`y0`…`y1`) and, per point, how far through
  /// that stretch the thread has to be read to get there.
  struct PageThread {
    struct Leg { var points: [CGPoint]; var times: [Double]; var y0: CGFloat; var y1: CGFloat; var figure: Int? }
    var legs: [Leg] = []
    /// Needle holes at the paragraphs, with the reading position that reaches them.
    var holes: [(CGPoint, CGFloat)] = []
    var start: CGPoint = .zero

    init(doc: Doc, rects: [BlockID: CGRect], pageWidth: CGFloat) {
      let colMin = (pageWidth - DocMetrics.column) / 2
      let gutter = colMin - 22
      guard let first = doc.blocks.indices.first(where: { rects[BlockID(index: $0)] != nil }), let top = rects[BlockID(index: first)] else { return }
      start = pt(gutter + 2, top.midY - 6)
      var cursor = start

      func gutterLeg(to h: CGPoint) {
        guard h.y > cursor.y + 1 else { cursor = h; return }
        let a = cursor, d = h
        let b = pt(gutter, a.y + min(14, (d.y - a.y) * 0.3)), c = pt(gutter, d.y - min(14, (d.y - a.y) * 0.3))
        let pts = Polyline.curve(a, b, c, d, n: 18)
        legs.append(Leg(points: pts, times: pts.map { Double(($0.y - a.y) / (d.y - a.y)) }, y0: a.y, y1: d.y, figure: nil))
        holes.append((h, d.y))
        cursor = h
      }

      for (i, b) in doc.blocks.enumerated() where i != first {
        guard let r = rects[BlockID(index: i)] else { continue }
        switch b {
        case .figure(let f):
          let s = r.width / f.crop.width
          func map(_ q: CGPoint) -> CGPoint { pt(r.minX + (q.x - f.crop.minX) * s, r.minY + (q.y - f.crop.minY) * s) }
          guard let entry = f.route.first else { continue }
          let e = map(entry.0)
          // into the figure from above
          if e.y > cursor.y + 1 {
            let c0 = cursor
            let pts = Polyline.curve(c0, pt(gutter, c0.y + (e.y - c0.y) * 0.5), pt(e.x, e.y - 30), e, n: 14)
            legs.append(Leg(points: pts, times: pts.map { Double(($0.y - c0.y) / max(1, e.y - c0.y)) }, y0: c0.y, y1: e.y, figure: nil))
          }
          // through it, smoothly, each point at its time
          let path = f.path
          let pts = path.points.map(map)
          legs.append(Leg(points: pts, times: path.times, y0: r.minY, y1: r.maxY, figure: i))
          cursor = pts.last ?? cursor
        case .list:
          var n = 0
          while let ir = rects[BlockID(index: i, item: n)] {
            gutterLeg(to: pt(colMin - 12, ir.minY + 13))
            n += 1
          }
        case .h2, .h3:
          gutterLeg(to: pt(colMin - 12, r.minY + 16))
        default:
          gutterLeg(to: pt(colMin - 12, r.minY + 13))
        }
      }
    }

    /// The thread read up to `y`, leg by leg (in a figure or beside the text).
    func portions(to y: CGFloat) -> [(points: [CGPoint], inFigure: Bool)] {
      var out: [(points: [CGPoint], inFigure: Bool)] = []
      for leg in legs {
        let f = Double((y - leg.y0) / max(1, leg.y1 - leg.y0))
        if f <= 0 { break }
        var pts = [leg.points[0]]
        for k in 1..<leg.points.count {
          if leg.times[k] <= f { pts.append(leg.points[k]); continue }
          let t0 = leg.times[k - 1], t1 = leg.times[k]
          pts.append(mix(leg.points[k - 1], leg.points[k], (f - t0) / max(0.0001, t1 - t0)))
          break
        }
        out.append((pts, leg.figure != nil))
        if f < 1 { break }
      }
      return out
    }

    /// The thread read up to `y`: its points, the tip, the direction there, and the leg it ends in.
    func read(to y: CGFloat) -> (points: [CGPoint], tip: CGPoint, heading: CGPoint, leg: Leg?, f: Double) {
      var out: [CGPoint] = [start]
      for leg in legs {
        let f = Double((y - leg.y0) / max(1, leg.y1 - leg.y0))
        if f >= 1 { out += leg.points; continue }
        if f <= 0 { break }
        var tip = leg.points[0]
        for k in 1..<leg.points.count {
          if leg.times[k] <= f { out.append(leg.points[k]); tip = leg.points[k]; continue }
          let t0 = leg.times[k - 1], t1 = leg.times[k]
          tip = mix(leg.points[k - 1], leg.points[k], (f - t0) / max(0.0001, t1 - t0))
          out.append(tip)
          break
        }
        let before = out.count > 2 ? out[out.count - 3] : out[0]
        return (out, tip, pt(tip.x - before.x, tip.y - before.y), leg, f)
      }
      let tip = out.last ?? start
      let before = out.count > 2 ? out[out.count - 3] : out[0]
      return (out, tip, pt(tip.x - before.x, tip.y - before.y), nil, 1)
    }
  }

  // MARK: - The page's drawing

  struct PageLayer: View {
    let doc: Doc
    let rects: [BlockID: CGRect]
    let pageWidth: CGFloat
    let scrollY: CGFloat
    let viewport: CGFloat
    let front: Bool
    @Environment(\.explainerMoments) private var triggers

    var body: some View {
      let thread = PageThread(doc: doc, rects: rects, pageWidth: pageWidth)
      TimelineView(.animation) { ctx in
        let t = ctx.date.timeIntervalSinceReferenceDate
        Canvas { g, _ in
          var g = g
          g.translateBy(x: 0, y: -scrollY)
          if front { drawFront(g, thread, t) } else { drawBack(g, thread, t) }
        }
        .frame(width: pageWidth, height: viewport)
        .offset(y: scrollY)
      }
    }

    private var readingY: CGFloat { scrollY + viewport * DocMetrics.readingLine }
    private var visible: ClosedRange<CGFloat> { (scrollY - 200)...(scrollY + viewport + 200) }

    /// Each figure on the page: its block, rect, spec and story position.
    private var figures: [(i: Int, rect: CGRect, spec: FigureSpec, p: Double, u: Double)] {
      doc.blocks.enumerated().compactMap { i, b in
        guard case .figure(let f) = b, let r = rects[BlockID(index: i)] else { return nil }
        let u = clamp01(Double((readingY - r.minY) / r.height))
        return (i, r, f, f.from + (f.to - f.from) * u, u)
      }
    }

    private func map(_ q: CGPoint, _ f: FigureSpec, _ r: CGRect) -> CGPoint {
      let s = r.width / f.crop.width
      return pt(r.minX + (q.x - f.crop.minX) * s, r.minY + (q.y - f.crop.minY) * s)
    }
    private func map(_ q: CGRect, _ f: FigureSpec, _ r: CGRect) -> CGRect {
      let a = map(q.origin, f, r), s = r.width / f.crop.width
      return CGRect(x: a.x, y: a.y, width: q.width * s, height: q.height * s)
    }

    /// The list item containing `text`, anywhere on the page.
    private func item(containing text: String) -> CGRect? {
      for (i, b) in doc.blocks.enumerated() {
        guard case .list(let items, _) = b, let n = items.firstIndex(where: { $0.contains(text) }) else { continue }
        return rects[BlockID(index: i, item: n)]
      }
      return nil
    }

    // Behind the text: figures, list cues, the thread and its holes, the temari.
    private func drawBack(_ g: GraphicsContext, _ thread: PageThread, _ t: Double) {
      for fig in figures where fig.rect.maxY > visible.lowerBound && fig.rect.minY < visible.upperBound {
        drawFigure(g, fig.spec, fig.rect, fig.p, t)
      }
      for fig in figures {
        for cue in fig.spec.cues {
          guard let r = item(containing: cue.item) else { continue }
          let o = span(fig.p, cue.from, cue.from + 0.12) * (1 - span(fig.p, cue.to - 0.12, cue.to))
          guard o > 0.01 else { continue }
          let box = r.insetBy(dx: -12, dy: -7)
          g.fill(Path(roundedRect: box, cornerRadius: 8), with: .color(TsugumiPalette.rufous.opacity(0.13 * o)))
          g.fill(Path(roundedRect: CGRect(x: box.minX, y: box.minY + 4, width: 3, height: box.height - 8), cornerRadius: 1.5),
                 with: .color(TsugumiPalette.rufous.opacity(o)))
        }
      }

      let read = thread.read(to: readingY)
      // the holes the thread has gone through: barely there
      for (h, y) in thread.holes where visible.contains(h.y) && y <= readingY {
        g.fill(Path(ellipseIn: CGRect(x: h.x - 1.6, y: h.y - 1.6, width: 3.2, height: 3.2)), with: .color(TsugumiPalette.thread.opacity(0.55)))
      }
      // the thread, behind what it passes behind, showing through holes
      var tg = g
      var clip = Path(CGRect(x: -100, y: scrollY - 400, width: pageWidth + 200, height: viewport + 800))
      for fig in figures {
        for o in fig.spec.occluders(fig.p) { clip.addRect(map(o, fig.spec, fig.rect)) }
        for (c, r) in fig.spec.holes(fig.p) where r > 0.5 {
          let q = map(c, fig.spec, fig.rect), rr = r * fig.rect.width / fig.spec.crop.width
          clip.addEllipse(in: CGRect(x: q.x - rr, y: q.y - rr, width: rr * 2, height: rr * 2))
        }
      }
      tg.clip(to: clip, style: FillStyle(eoFill: true))
      // beside the text it is a fine, quiet line; in the figures, the thread proper
      for part in thread.portions(to: readingY) {
        if part.inFigure {
          drawThread(tg, linePath(part.points), width: 2.2)
        } else {
          tg.stroke(linePath(part.points), with: .color(TsugumiPalette.thread.opacity(0.38)),
                    style: StrokeStyle(lineWidth: 1.1, lineCap: .round, lineJoin: .round))
        }
      }
      _ = read

      // what travels on the thread, drawn in the figure's own terms (it knows what hides them)
      for fig in figures {
        guard let beads = fig.spec.beads, fig.p >= beads.appear,
              let leg = thread.legs.first(where: { $0.figure == fig.i }) else { continue }
        let k = fig.rect.width / fig.spec.crop.width
        var local: [CGPoint] = []
        for (q, time) in zip(leg.points, leg.times) where time >= beads.from && time <= beads.to {
          let x: CGFloat = (q.x - fig.rect.minX) / k + fig.spec.crop.minX
          let y: CGFloat = (q.y - fig.rect.minY) / k + fig.spec.crop.minY
          local.append(pt(x, y))
        }
        guard local.count > 1 else { continue }
        var s = g
        s.translateBy(x: fig.rect.minX, y: fig.rect.minY)
        s.scaleBy(x: k, y: k)
        s.translateBy(x: -fig.spec.crop.minX, y: -fig.spec.crop.minY)
        WallScene.drawBeads(s, fig.p, t, on: Polyline(local))
      }

      // the temari it comes from
      TsugumiTemari.draw(g, center: pt(thread.start.x - 4, thread.start.y + 10), radius: 10, spin: -Double(readingY) / 160)
    }

    // Over the text: words flying into a figure, and the bird at the thread's end.
    private func drawFront(_ g: GraphicsContext, _ thread: PageThread, _ t: Double) {
      for fig in figures {
        for fl in fig.spec.flights {
          guard let from = item(containing: fl.item) else { continue }
          let u = lin(fig.p, fl.at - 0.32, fl.at - 0.06)
          guard u > 0, u < 1 else { continue }
          let a = pt(from.minX + 34, from.minY + 12), b = map(fl.to, fig.spec, fig.rect)
          let e = easeInOut(u)
          let q = bezier(a, pt(a.x - 120, a.y + 60), pt(b.x - 40, b.y - 160), b, e)
          var c = g
          c.translateBy(x: q.x, y: q.y)
          c.rotate(by: .radians(sin(u * .pi) * -0.12))
          let w = CGFloat(fl.label.count) * 21 + 40
          let r = CGRect(x: -w / 2, y: -21, width: w, height: 42)
          c.opacity *= min(1, u * 5) * (1 - smooth((u - 0.9) / 0.1))
          c.fill(Path(roundedRect: r.offsetBy(dx: 0, dy: 4), cornerRadius: 6), with: .color(.black.opacity(0.3)))
          c.fill(Path(roundedRect: r, cornerRadius: 6), with: .color(TsugumiPalette.hex(0x5d4b40)))
          c.text(fl.label, at: .zero, size: 19, weight: .bold, color: Ink.cream)
        }
      }

      let read = thread.read(to: readingY)
      var hidden = false
      if let leg = read.leg, let i = leg.figure, let fig = figures.first(where: { $0.i == i }) {
        if let away = fig.spec.birdAway, away.contains(read.f) { hidden = true }
        for o in fig.spec.occluders(fig.p) where map(o, fig.spec, fig.rect).insetBy(dx: -6, dy: 0).contains(read.tip) { hidden = true }
      }
      guard !hidden else { return }
      let inFigure = read.leg?.figure != nil
      var pose = inFigure ? flying(t, lean: 0.15) : BirdMotion.idle(t * 0.5)
      if !inFigure { pose.hop = min(pose.hop, 3) }
      let mirrored = inFigure && read.heading.x < -0.5
      // the bird holds the thread's end in its bill: small beside the text
      let scale: CGFloat = inFigure ? 0.115 : 0.085
      let gx = (TsugumiBird.gape.x - TsugumiBird.feet.x) * scale, gy = (TsugumiBird.gape.y - TsugumiBird.feet.y) * scale
      let feet = pt(read.tip.x - (mirrored ? -gx : gx), read.tip.y - gy)
      drawBird(g, at: feet, scale: scale, pose: pose, mirrored: mirrored)
    }

    private func drawFigure(_ g: GraphicsContext, _ f: FigureSpec, _ r: CGRect, _ p: Double, _ t: Double) {
      // a figure's moment: time since its trigger point crossed the reading line
      SceneOptions.since = -1
      if let trig = f.trigger {
        let key = "\(doc.id)-\(r.minY)"
        if r.minY + (trig.y - f.crop.minY) * (r.width / f.crop.width) <= readingY {
          let t0 = triggers.times[key] ?? t
          triggers.times[key] = t0
          SceneOptions.since = t - t0
        } else {
          triggers.times[key] = nil
        }
      }
      Explainer.drawFigure(g, f, in: r, p: p, t: t)
    }
  }

  /// A figure on the page, frameless: a faint teal ground fading into the
  /// page, the scene with soft edges, at story position p.
  static func drawFigure(_ g: GraphicsContext, _ f: FigureSpec, in r: CGRect, p: Double, t: Double) {
    // a faint ground of the key visual's teal, fading into the page
    var glow = g
    glow.addFilter(.blur(radius: 60))
    glow.fill(Path(ellipseIn: r.insetBy(dx: r.width * 0.08, dy: r.height * 0.12)), with: .color(TsugumiPalette.tealTop.opacity(0.45)))
    var s = g
    s.clip(to: Path(r))
    // soft edges all round: no frame, the drawing fades into the page
    s.clipToLayer { m in
      m.fill(Path(r), with: .linearGradient(Gradient(stops: [.init(color: .clear, location: 0), .init(color: .white, location: 0.05),
                                                            .init(color: .white, location: 0.95), .init(color: .clear, location: 1)]),
                                            startPoint: pt(r.minX, r.midY), endPoint: pt(r.maxX, r.midY)))
    }
    s.clipToLayer { m in
      m.fill(Path(r), with: .linearGradient(Gradient(stops: [.init(color: .clear, location: 0), .init(color: .white, location: 0.04),
                                                            .init(color: .white, location: 0.9), .init(color: .clear, location: 1)]),
                                            startPoint: pt(r.midX, r.minY), endPoint: pt(r.midX, r.maxY)))
    }
    let k = r.width / f.crop.width
    s.translateBy(x: r.minX, y: r.minY)
    s.scaleBy(x: k, y: k)
    s.translateBy(x: -f.crop.minX, y: -f.crop.minY)
    f.draw(s, p, t)
  }

  // MARK: - The list of documents

  struct DocSidebar: View {
    let selected: String
    /// Read as a page, or a card at a time.
    @Binding var cards: Bool
    var onPick: (String) -> Void = { _ in }

    var body: some View {
      VStack(alignment: .leading, spacing: 4) {
        HStack(spacing: 8) {
          Image(systemName: "book.closed").foregroundStyle(TsugumiPalette.rufous)
          Text("Tsugumi を知る").font(.system(size: 15, weight: .bold)).foregroundStyle(DocInk.heading)
        }
        .padding(.horizontal, 14).padding(.top, 44).padding(.bottom, 12)
        Picker("", selection: $cards) {
          Text("読む").tag(false)
          Text("カード").tag(true)
        }
        .pickerStyle(.segmented).labelsHidden()
        .frame(width: 160)
        .padding(.leading, 12).padding(.bottom, 14)
        ForEach(Docs.all) { d in
          let on = d.id == selected
          Button { onPick(d.id) } label: {
            HStack(alignment: .firstTextBaseline, spacing: 10) {
              // the page you are on: a short length of the thread
              Capsule().fill(TsugumiPalette.thread.opacity(on ? 0.8 : 0)).frame(width: 2, height: 14)
              VStack(alignment: .leading, spacing: 3) {
                Text(d.title).font(.system(size: 13.5, weight: on ? .semibold : .regular))
                  .foregroundStyle(on ? DocInk.heading : DocInk.secondary).multilineTextAlignment(.leading)
                Text(d.source).font(.system(size: 10.5, design: .monospaced)).foregroundStyle(DocInk.secondary.opacity(on ? 0.8 : 0.5))
              }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, 8)
            .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
        }
        Spacer()
      }
      .padding(.leading, 18).padding(.trailing, 8)
      .frame(width: DocMetrics.sidebar)
      .frame(maxHeight: .infinity, alignment: .top)
      .background(DocInk.page)
    }
  }

  // MARK: - The window

  /// The window "Tsugumi を知る".
  struct ExplainerWindow: View {
    @State private var selected = Docs.all[0].id
    @State private var offset: CGFloat = 0
    @State private var moments = TriggerClock()
    @State private var cards = false

    var body: some View {
      HStack(spacing: 0) {
        DocSidebar(selected: selected, cards: $cards) { selected = $0; offset = 0 }
        GeometryReader { geo in
          if let d = Docs.all.first(where: { $0.id == selected }) {
            if cards {
              CardDeck(doc: d).id(selected)
            } else {
              ScrollView {
                DocContent(doc: d, pageWidth: geo.size.width, scrollY: offset, viewport: geo.size.height)
              }
              .id(selected)  // a fresh page (scrolled to the top) per document
              .onScrollGeometryChange(for: CGFloat.self) { $0.contentOffset.y } action: { _, y in offset = y }
              .environment(\.explainerMoments, moments)
            }
          }
        }
        .background(DocInk.page)
      }
      .ignoresSafeArea()
    }
  }
}

extension EnvironmentValues {
  /// When figures' moments began, per window.
  var explainerMoments: Explainer.TriggerClock {
    get { self[Explainer.MomentsKey.self] }
    set { self[Explainer.MomentsKey.self] = newValue }
  }
}

private extension View {
  @ViewBuilder func selectable(_ on: Bool) -> some View {
    if on { textSelection(.enabled) } else { self }
  }
  func column(_ width: CGFloat) -> some View { fixedSize(horizontal: false, vertical: true).frame(width: width, alignment: .leading) }
  func anchored(_ i: Int) -> some View { transformAnchorPreference(key: Explainer.BlockFrames.self, value: .bounds) { $0[Explainer.BlockID(index: i)] = $1 } }
}
