import AppKit
import SwiftUI

// The documents as they are in the repository (README.md, docs/*.md), with
// figures set in at anchors: after the first block whose text contains a
// phrase. The Markdown files stay the single source; GitHub shows the same
// words without the figures.

extension Explainer {
  // MARK: - Blocks

  indirect enum MDBlock {
    case h1(String), h2(String), h3(String)
    case paragraph(String)
    case list([String], ordered: Bool)
    case table([[String]])
    case rule
    case figure(FigureSpec)

    var plain: String {
      switch self {
      case .h1(let s), .h2(let s), .h3(let s), .paragraph(let s): return s
      case .list(let items, _): return items.joined(separator: "\n")
      case .table(let rows): return rows.map { $0.joined(separator: " ") }.joined(separator: "\n")
      default: return ""
      }
    }
  }

  enum MarkdownText {
    /// Block-level Markdown, enough for these documents: headings, paragraphs,
    /// lists, tables, rules. Raw HTML and image lines are skipped.
    static func parse(_ text: String) -> [MDBlock] {
      var out: [MDBlock] = []
      var para: [String] = []
      var list: [String] = [], ordered = false
      var table: [[String]] = []

      func flush() {
        if !para.isEmpty { out.append(.paragraph(join(para))); para = [] }
        if !list.isEmpty { out.append(.list(list, ordered: ordered)); list = [] }
        if !table.isEmpty { out.append(.table(table)); table = [] }
      }

      for raw in text.components(separatedBy: "\n") {
        let line = raw.trimmingCharacters(in: .whitespaces)
        if line.isEmpty { flush(); continue }
        if line.hasPrefix("<") { flush(); continue }
        if line.hasPrefix("### ") { flush(); out.append(.h3(String(line.dropFirst(4)))); continue }
        if line.hasPrefix("## ") { flush(); out.append(.h2(String(line.dropFirst(3)))); continue }
        if line.hasPrefix("# ") { flush(); out.append(.h1(String(line.dropFirst(2)))); continue }
        if line == "---" { flush(); out.append(.rule); continue }
        if line.hasPrefix("![") { flush(); continue }  // images: not in these pages
        if line.hasPrefix("|") {
          if !para.isEmpty || !list.isEmpty { flush() }
          let cells = line.trimmingCharacters(in: CharacterSet(charactersIn: "|")).components(separatedBy: "|")
            .map { $0.trimmingCharacters(in: .whitespaces) }
          if cells.allSatisfy({ $0.allSatisfy { "-: ".contains($0) } }) { continue }   // the separator row
          table.append(cells)
          continue
        }
        if line.hasPrefix("- ") {
          if !para.isEmpty || !table.isEmpty { flush() }
          ordered = false; list.append(String(line.dropFirst(2))); continue
        }
        if let dot = line.firstIndex(of: "."), line[..<dot].allSatisfy(\.isNumber), !line[..<dot].isEmpty,
           line[line.index(after: dot)...].hasPrefix(" ") {
          if !para.isEmpty || !table.isEmpty { flush() }
          ordered = true; list.append(String(line[line.index(dot, offsetBy: 2)...])); continue
        }
        if !list.isEmpty { flush() }
        para.append(line)
      }
      flush()
      return out
    }

    /// Soft line breaks: Japanese lines join directly; a space only between Latin text.
    static func join(_ lines: [String]) -> String {
      lines.dropFirst().reduce(lines[0]) { acc, next in
        let a = acc.last, b = next.first
        let latin = { (c: Character?) in c.map { $0.isASCII && ($0.isLetter || $0.isNumber || ".,)".contains($0)) } ?? false }
        // a closing ** needs the space after it to count as closing (as on GitHub)
        return acc + ((latin(a) && latin(b)) || acc.hasSuffix("**") ? " " : "") + next
      }
    }

    /// The `## ` sections of a file with these titles, in this order.
    static func sections(_ blocks: [MDBlock], titles: [String]) -> [MDBlock] {
      titles.flatMap { title -> [MDBlock] in
        guard let start = blocks.firstIndex(where: { if case .h2(let s) = $0 { return s == title }; return false }) else { return [] }
        var end = start + 1
        while end < blocks.count { if case .h2 = blocks[end] { break }; end += 1 }
        return Array(blocks[start..<end])
      }
    }

    /// Inline Markdown (bold, code, links) for SwiftUI Text, in the document's colours.
    static func inline(_ s: String, accent: Color = DocInk.strong) -> AttributedString {
      guard var a = try? AttributedString(markdown: s, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)) else {
        return AttributedString(s)
      }
      for run in a.runs {
        if let i = run.inlinePresentationIntent {
          if i.contains(.stronglyEmphasized) { a[run.range].foregroundColor = accent }
          if i.contains(.code) { a[run.range].font = .system(size: 15, design: .monospaced) }
        }
        if run.link != nil {
          a[run.range].foregroundColor = DocInk.link
          a[run.range].underlineStyle = .single
        }
      }
      return a
    }
  }

  // MARK: - Figures

  /// An illustration woven into a document: a scene drawn without a frame on
  /// the page, over a stretch of its story (`from`…`to`, in the scene's units)
  /// as the reading line passes it. The document's thread runs through it along
  /// `route` (scene coordinates; with the share of the figure, 0…1, at which
  /// the thread gets to each point).
  struct FigureSpec {
    /// The scene: drawn at story position p and clock t, in its 1000-wide coordinates.
    let draw: (GraphicsContext, Double, Double) -> Void
    let from: Double, to: Double
    /// The part of the scene's 1000-wide drawing to show.
    let crop: CGRect
    let caption: String
    var route: [(CGPoint, Double)] = []
    /// What the thread passes behind at story position p, and the holes it shows through.
    var occluders: (Double) -> [CGRect] = { _ in [] }
    var holes: (Double) -> [(CGPoint, CGFloat)] = { _ in [] }
    /// The document's bird hides while the thread is in this stretch (the scene has its own).
    var birdAway: ClosedRange<Double>? = nil
    /// Words of a list above that fly into the figure: (text of the item, label, where it lands, story time it lands).
    var flights: [(item: String, label: String, to: CGPoint, at: Double)] = []
    /// List items lit while the story is in a stretch: (text of the item, from, to).
    var cues: [(item: String, from: Double, to: Double)] = []
    /// Things travelling both ways on the stretch of the route `from`…`to`, once the story is at `appear`.
    var beads: (from: Double, to: Double, appear: Double)? = nil
    /// A point of the scene (scene coordinates) whose crossing of the reading line starts a moment that plays in time.
    var trigger: CGPoint? = nil

    /// In card mode (no reading line): the story position that starts the moment instead.
    var triggerAt: Double? = nil

    func u(ofStory p: Double) -> Double { clamp01((p - from) / (to - from)) }

    /// The route as a smooth line (scene coordinates), each point with the
    /// share of the figure at which the thread gets there.
    var path: (points: [CGPoint], times: [Double]) {
      var pts: [CGPoint] = [], times: [Double] = []
      for k in 0..<max(0, route.count - 1) {
        let (a, ta) = route[k], (d, td) = route[k + 1]
        let prev = k > 0 ? route[k - 1].0 : a, next = k + 2 < route.count ? route[k + 2].0 : d
        let b = pt(a.x + (d.x - prev.x) / 6, a.y + (d.y - prev.y) / 6), c = pt(d.x - (next.x - a.x) / 6, d.y - (next.y - a.y) / 6)
        for (j, q) in Polyline.curve(a, b, c, d, n: 16).enumerated() where k == 0 || j > 0 {
          pts.append(q); times.append(ta + (td - ta) * Double(j) / 16)
        }
      }
      return (pts, times)
    }
    /// Whether the thread does something here (not only passing down the edge).
    var threadActs: Bool { route.count > 2 }
  }

  enum Figures {
    static let author = FigureSpec(
      draw: AuthorScene.draw, from: 0, to: 1, crop: CGRect(x: 0, y: 90, width: 1000, height: 620),
      caption: "東京大学・安田講堂。星をつなぐように、AI で宇宙を読み解く研究をしています。",
      route: [(pt(40, 90), 0), (pt(40, 710), 1)])
    static let wallCrop = CGRect(x: 0, y: 150, width: 1000, height: 610)
    static let wall = FigureSpec(
      draw: WallScene.draw, from: 0.15, to: 3.3, crop: wallCrop,
      caption: "AI の出力が積み上がるほど、人と AI の間に壁ができていく。",
      route: [(pt(40, 150), 0), (pt(40, 760), 1)],
      flights: [(item: "再現性を担保できない", label: "再現できない", to: pt(500, 628), at: 1.45),
                (item: "結論に至るプロセスが見えない", label: "過程が見えない", to: pt(500, 522), at: 2.05),
                (item: "データを AI に渡すのが怖い", label: "データが怖い", to: pt(500, 342), at: 2.65)])
    static let thread = FigureSpec(
      draw: WallScene.draw, from: 3.45, to: 5.4, crop: wallCrop,
      caption: "Tsugumi は、その壁に一本の糸を通す。",
      route: [(pt(40, 150), 0), (pt(46, 690), 0.13), (pt(236, 696), 0.19), (WallScene.temariTop, 0.24),
              (WallScene.hole, 0.45), (WallScene.tie, 0.64), (pt(770, 712), 0.8), (pt(160, 758), 1)],
      occluders: { _ in [CGRect(x: WallScene.wallX.lowerBound, y: WallScene.wallTop, width: WallScene.wallX.upperBound - WallScene.wallX.lowerBound,
                                height: WallScene.groundY - WallScene.wallTop)] },
      holes: { p in [(WallScene.hole, 15 * CGFloat(pop(lin(p, 4.27, 4.37))))] },
      beads: (from: 0.24, to: 0.64, appear: 4.7))
    static let casaCrop = CGRect(x: 0, y: 95, width: 1000, height: 720)
    static let sketch = FigureSpec(
      draw: CasAScene.draw, from: 0.1, to: 2.5, crop: casaCrop,
      caption: "もし、書いた通りのものが、そのまま形になったら？　紙に描いた三つの箱が、そのまま Draft になる。",
      route: [(pt(10, 95), 0), (pt(10, 815), 1)],
      trigger: pt(792, 224), triggerAt: 0.6)
    static let run = FigureSpec(
      draw: CasAScene.draw, from: 2.6, to: 5.5, crop: casaCrop,
      caption: "Agent が Draft を実装し、実行すると、帯域ごとの光子数と RGB 画像がそのまま Canvas に並ぶ（同梱の Cas A · Chandra RGB デモ）。",
      // the thread becomes the wiring: through each Card while the Agent (the bird) fills it in
      route: {
        let c = CasAScene.self
        func inP(_ k: Int) -> CGPoint { c.inPort(c.cardRect(k)) }
        func outP(_ k: Int) -> CGPoint { c.outPort(c.cardRect(k)) }
        return [(pt(10, 95), 0), (pt(14, 330), 0.06), (pt(c.startRect.minX, c.startRect.midY), 0.09), (c.startPort, 0.105),
                (inP(0), 0.12), (outP(0), 0.155), (inP(1), 0.176), (outP(1), 0.21), (inP(2), 0.232), (outP(2), 0.27),
                (pt(915, 430), 0.31), (pt(918, 700), 0.6), (pt(160, 800), 1)]
      }(),
      occluders: { _ in [CasAScene.startRect] + (0..<3).map(CasAScene.cardRect) },
      cues: [(item: "**実装する。**", from: 2.75, to: 3.45), (item: "**確かめる。**", from: 3.5, to: 4.6)])
    static let name: FigureSpec = {
      func card(_ k: Int) -> CGRect { NameScene.card(k) }
      let gape = birdGape(at: NameScene.feet, scale: NameScene.scale, pose: BirdPose())
      return FigureSpec(
        draw: NameScene.draw, from: 0.4, to: 2.7, crop: CGRect(x: 0, y: 120, width: 1000, height: 610),
        caption: "手毬から引き出した一本の糸が、Card と Card をつなぐ配線になる。",
        route: [(pt(60, 120), 0), (pt(110, 690), 0.12), (pt(170, 698), 0.17), (TsugumiTemari.lead(center: NameScene.temari.center, radius: NameScene.temari.radius), 0.22),
                (gape, 0.36), (pt(card(0).minX, card(0).midY), 0.46), (pt(card(0).maxX, card(0).midY), 0.5),
                (pt(card(1).minX, card(1).midY), 0.57), (pt(card(1).maxX, card(1).midY), 0.62),
                (pt(card(2).minX, card(2).midY), 0.68), (pt(card(2).maxX, card(2).midY), 0.73),
                (pt(985, 480), 0.77), (pt(975, 700), 0.88), (pt(160, 728), 1)],
        occluders: { _ in (0..<3).map(card) },
        birdAway: 0...1)  // the figure has its own bird, perched with the thread in its bill
    }()
  }

  // MARK: - Documents

  struct Doc: Identifiable {
    let id: String
    let title: String
    let source: String
    let blocks: [MDBlock]
    /// What the page expected but did not find: a file, a `##` section, or the
    /// phrase a figure is set after (an edit to the document can drop one).
    let missing: [String]
  }

  enum Docs {
    /// Builds a page from the sections of a file, figures set in after phrases.
    static func page(id: String, title: String, source: String, sections: [String]? = nil, heading: String? = nil,
                     figures: [(String, FigureSpec)] = []) -> Doc {
      var missing: [String] = []
      var blocks: [MDBlock] = []
      if let url = file((source as NSString).lastPathComponent, repo: source), let text = try? String(contentsOf: url, encoding: .utf8) {
        blocks = MarkdownText.parse(text)
      } else {
        missing.append(source)
      }
      if let sections {
        let found = MarkdownText.sections(blocks, titles: sections)
        missing += sections.filter { t in !found.contains { if case .h2(let s) = $0 { return s == t }; return false } }
        blocks = found
      }
      // the "README に戻る" links at the ends of docs/*.md lead nowhere here
      blocks = blocks.filter { if case .paragraph(let s) = $0 { return !s.hasPrefix("[README に戻る]") }; return true }
      if let heading { blocks.insert(.h1(heading), at: 0) }
      for (phrase, f) in figures {
        if let i = blocks.firstIndex(where: { $0.plain.contains(phrase) }) { blocks.insert(.figure(f), at: i + 1) } else { missing.append(phrase) }
      }
      return Doc(id: id, title: title, source: source, blocks: blocks, missing: missing)
    }

    static let all: [Doc] = [
      page(id: "author", title: "この道具を作った人", source: "docs/Author.md",
           figures: [("AI for astrophysics", Figures.author)]),
      page(id: "problem", title: "Tsugumi が解決しようとしている課題", source: "README.md",
           sections: ["AI を研究に積極的に活用できていますか？",
                      "処理の流れそのものを、人と AI の共通言語にする",
                      "処理の形を描く。AI と実装する。実行して確かめる。"],
           heading: "Tsugumi が解決しようとしている課題",
           figures: [("これらに共通しているのは", Figures.wall),
                     ("この三つを同じ Workflow", Figures.thread),
                     ("組み立てる macOS アプリ", Figures.sketch),
                     ("**確かめる。** Nix", Figures.run)]),
      page(id: "philosophy", title: "AI に使われないために、何を作るか", source: "docs/Philosophy.md"),
      page(id: "name", title: "Tsugumi という名前に込めた意味", source: "docs/Name.md",
           figures: [("Card と Card をつなぐ配線", Figures.name)]),
    ]
  }

  enum DocInk {
    static let page = TsugumiPalette.hex(0x1b1d1f), side = TsugumiPalette.hex(0x141618)
    static let text = TsugumiPalette.hex(0xe2dfd8), secondary = TsugumiPalette.hex(0x9b9d9f)
    static let heading = TsugumiPalette.hex(0xf3ede2), strong = TsugumiPalette.hex(0xf0b57a)
    static let link = TsugumiPalette.hex(0xd9a06a), rule = Color.white.opacity(0.08)
  }
}
