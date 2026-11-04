import SwiftUI

extension Explainer {
  // Card mode: the same documents a paragraph at a time, the figure beside
  // it. Moving to the next card plays the figure on in time (not with the
  // scroll), so it runs at the display's pace: smooth when recorded.

  struct Card {
    /// A paragraph, list or table, with the headings before it.
    var blocks: [MDBlock]
    var figure: FigureSpec?
    /// Where the figure's story is once this card is shown.
    var p: Double
    var caption: String { figure?.caption ?? "" }
  }

  /// The cards of a document. A figure plays across the cards of its section
  /// up to where it stands in the text; after it, it stays as it ended.
  static func cards(of doc: Doc) -> [Card] {
    var cards: [Card] = []
    var section: [Int] = []          // section number per card
    var figures: [(after: Int, section: Int, spec: FigureSpec)] = []
    var pending: [MDBlock] = []
    var current = 0
    for b in doc.blocks {
      switch b {
      case .h1, .h2:
        if pending.isEmpty { current += 1 }
        pending.append(b)
      case .h3:
        pending.append(b)
      case .figure(let f):
        figures.append((cards.count - 1, current, f))
      default:
        cards.append(Card(blocks: pending + [b], figure: nil, p: 0))
        section.append(current)
        pending = []
      }
    }
    if !pending.isEmpty { cards.append(Card(blocks: pending, figure: nil, p: 0)); section.append(current) }
    for c in cards.indices {
      if let next = figures.first(where: { $0.section == section[c] && $0.after >= c }) {
        // the cards from the section's previous figure (or its start) up to this one share its story
        let start = (figures.last { $0.section == section[c] && $0.after < c }?.after ?? (section.firstIndex(of: section[c]) ?? 0) - 1) + 1
        let n = next.after - start + 1, k = c - start
        let f = next.spec
        cards[c].figure = f
        cards[c].p = f.from + (f.to - f.from) * Double(k + 1) / Double(max(1, n))
      } else if let last = figures.last(where: { $0.after < c }) {
        cards[c].figure = last.spec
        cards[c].p = last.spec.to
      }
    }
    return cards
  }

  /// The figure's thread in card mode, drawn in the scene's coordinates: the
  /// route up to where the story is, behind what hides it, what travels on
  /// it, and the bird at its end.
  static func drawFigureThread(_ g: GraphicsContext, _ f: FigureSpec, p: Double, t: Double) {
    guard f.threadActs else { return }
    let u = f.u(ofStory: p)
    let path = f.path
    var pts = [path.points[0]]
    var tip = pts[0], heading = CGPoint(x: 1, y: 0)
    for k in 1..<path.points.count {
      if path.times[k] <= u { pts.append(path.points[k]); continue }
      let t0 = path.times[k - 1], t1 = path.times[k]
      pts.append(mix(path.points[k - 1], path.points[k], (u - t0) / max(0.0001, t1 - t0)))
      break
    }
    tip = pts.last ?? tip
    if pts.count > 2 { heading = pt(tip.x - pts[pts.count - 3].x, tip.y - pts[pts.count - 3].y) }
    var clip = Path(CGRect(x: -500, y: -500, width: 3000, height: 3000))
    for o in f.occluders(p) { clip.addRect(o) }
    for (c, r) in f.holes(p) where r > 0.5 { clip.addEllipse(in: CGRect(x: c.x - r, y: c.y - r, width: r * 2, height: r * 2)) }
    var tg = g
    tg.clip(to: clip, style: FillStyle(eoFill: true))
    drawThread(tg, linePath(pts), width: 2.4)
    if let beads = f.beads, p >= beads.appear {
      let local = zip(path.points, path.times).filter { $0.1 >= beads.from && $0.1 <= beads.to }.map(\.0)
      if local.count > 1 { WallScene.drawBeads(g, p, t, on: Polyline(local)) }
    }
    // the bird at the end, unless the figure has its own or the end is hidden
    guard u > 0.001, u < 0.97, !(f.birdAway?.contains(u) ?? false),
          !f.occluders(p).contains(where: { $0.insetBy(dx: -6, dy: 0).contains(tip) }) else { return }
    let mirrored = heading.x < -0.5
    let scale: CGFloat = 0.13
    let gx = (TsugumiBird.gape.x - TsugumiBird.feet.x) * scale, gy = (TsugumiBird.gape.y - TsugumiBird.feet.y) * scale
    drawBird(g, at: pt(tip.x - (mirrored ? -gx : gx), tip.y - gy), scale: scale, pose: flying(t, lean: 0.15), mirrored: mirrored)
  }

  /// The cards of one document, with ← → (or Space, or a click) to move.
  struct CardDeck: View {
    let doc: Doc
    private let cards: [Card]
    @State private var index = 0
    /// The figure's story animating from p0 to p1 since `start`.
    @State private var p0 = 0.0
    @State private var p1 = 0.0
    @State private var start = Date.distantPast
    @State private var duration = 1.0
    @FocusState private var focused: Bool

    init(doc: Doc) {
      self.doc = doc
      cards = Explainer.cards(of: doc)
      let first = cards.first?.figure.map { $0.from } ?? 0
      _p0 = State(initialValue: first)
      _p1 = State(initialValue: cards.first?.p ?? 0)
      _start = State(initialValue: Date())
      _duration = State(initialValue: Self.duration(cards.first?.figure, first, cards.first?.p ?? 0))
    }

    static func duration(_ f: FigureSpec?, _ a: Double, _ b: Double) -> Double {
      guard let f else { return 0.6 }
      return min(5.5, max(0.9, abs(b - a) / (f.to - f.from) * 6))
    }

    var body: some View {
      GeometryReader { geo in
        let card = cards.isEmpty ? nil : cards[index]
        VStack(spacing: 0) {
          HStack(alignment: .center, spacing: 44) {
            if let f = card?.figure {
              let w = min(geo.size.width - 620, (geo.size.height - 170) * f.crop.width / f.crop.height)
              VStack(spacing: 10) {
                TimelineView(.animation) { ctx in
                  let now = ctx.date
                  Canvas { g, size in
                    let u = easeInOut(now.timeIntervalSince(start) / duration)
                    let p = p0 + (p1 - p0) * u
                    let t = now.timeIntervalSinceReferenceDate
                    SceneOptions.since = f.triggerAt.map { (p - $0) * 2.5 } ?? -1
                    let r = CGRect(origin: .zero, size: size)
                    Explainer.drawFigure(g, f, in: r, p: p, t: t)
                    var s = g
                    let k = r.width / f.crop.width
                    s.scaleBy(x: k, y: k)
                    s.translateBy(x: -f.crop.minX, y: -f.crop.minY)
                    drawFigureThread(s, f, p: p, t: t)
                  }
                }
                .frame(width: w, height: w * f.crop.height / f.crop.width)
                Text(verbatim: f.caption).font(.system(size: 13)).foregroundStyle(DocInk.secondary)
                  .multilineTextAlignment(.center).frame(width: w)
              }
            }
            if let card {
              VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(card.blocks.enumerated()), id: \.offset) { i, b in
                  BlockView(block: b, index: i, width: card.figure == nil ? DocMetrics.column : 480, scale: 1.18, selectable: false)
                }
              }
              .id(index)
              .transition(.asymmetric(insertion: .opacity.combined(with: .offset(y: 14)), removal: .opacity))
              .frame(width: card.figure == nil ? DocMetrics.column : 480, alignment: .leading)
            }
          }
          .frame(maxWidth: .infinity, maxHeight: .infinity)
          // a click anywhere on the card moves on
          .overlay { Button { go(+1) } label: { Color.white.opacity(0.001) }.buttonStyle(.plain) }
          controls
        }
        .padding(.horizontal, 40).padding(.top, 40)
      }
      .focusable()
      .focused($focused)
      .focusEffectDisabled()
      .onAppear { focused = true }
      .onKeyPress(.rightArrow) { go(+1); return .handled }
      .onKeyPress(.space) { go(+1); return .handled }
      .onKeyPress(.downArrow) { go(+1); return .handled }
      .onKeyPress(.leftArrow) { go(-1); return .handled }
      .onKeyPress(.upArrow) { go(-1); return .handled }
    }

    private var controls: some View {
      HStack(spacing: 16) {
        Button { go(-1) } label: { Image(systemName: "chevron.left") }.disabled(index == 0)
        HStack(spacing: 5) {
          ForEach(cards.indices, id: \.self) { i in
            Circle().fill(i == index ? TsugumiPalette.thread : DocInk.secondary.opacity(0.35)).frame(width: 5, height: 5)
          }
        }
        Button { go(+1) } label: { Image(systemName: "chevron.right") }.disabled(index >= cards.count - 1)
      }
      .buttonStyle(.plain).foregroundStyle(DocInk.secondary)
      .frame(height: 56)
    }

    /// To the next or previous card; the figure plays on from where it is.
    private func go(_ dir: Int) {
      let j = index + dir
      guard cards.indices.contains(j) else { return }
      let now = Date()
      let shown = p0 + (p1 - p0) * easeInOut(now.timeIntervalSince(start) / duration)
      let same = cards[j].figure?.caption == cards[index].figure?.caption
      let from = same ? shown : (dir > 0 ? cards[j].figure?.from ?? 0 : cards[j].p)
      p0 = from
      p1 = cards[j].p
      duration = Self.duration(cards[j].figure, from, p1)
      start = now
      withAnimation(.easeInOut(duration: 0.45)) { index = j }
    }
  }
}
