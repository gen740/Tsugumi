import AppKit
import SwiftUI
import TsugumiRPC

// Home's demo thumbnails: the gallery's thumbnail of the documentation site
// (web/src/ui/demoPlayer.tsx, compact) drawn with the app's own Card view,
// geometry and wire router. The data is the site's (docs/site/player-data/,
// built by docs/site/player/build.mjs): a demo's real Cards and resolved ports,
// and the order, times and results of a run that really happened. Nothing runs
// here: a thumbnail plays that record once as it appears, and again on hover.

/// docs/site/player-data/<id>.json, as far as a thumbnail needs it.
struct DemoPlayerData: Decodable {
  struct Port: Decodable {
    var name: String
    var typeName: String?
    var optional: Bool?
    var note: String?
    var data: PortData { PortData(name: name, typeName: typeName, optional: optional ?? false, note: note) }
  }

  struct Param: Decodable {
    var name: String
    var type: String
    var value: String
  }

  struct Card: Decodable {
    var id: String
    var kind: String
    var x: Double
    var y: Double
    var symbol: String?
    var summary: String?
    var inputs: [Port]
    var outputs: [Port]
    var params: [Param]
    var permissions: [String]
    var untitled: Bool?
  }

  struct Wire: Decodable {
    var from: String
    var fromPort: String
    var to: String
    var toPort: String
  }

  struct FrameRect: Decodable {
    var x: Double?
    var y: Double?
    var width: Double?
    var height: Double?
    var hidden: Bool?
  }

  struct Step: Decodable { var cards: [String] }

  struct StageRecord: Decodable {
    var phase: String
    var outcome: String
    var ms: Double?
  }

  struct Final: Decodable {
    var state: String
    var count: Int
    var sent: [String]
    var stages: [StageRecord]
  }

  struct Series: Decodable {
    var label: String
    var x: [Double]
    var y: [Double]
    var points: Bool
  }

  enum Output: Decodable {
    case text(String, markdown: Bool)
    case scalar(Double, label: String)
    case table(columns: [String], rows: [[String]], totalRows: Int)
    case image(src: String, alt: String)
    case points(count: Int)
    case plot(title: String, series: [Series])
    case module(name: String)

    private enum Key: String, CodingKey {
      case type, text, markdown, value, label, columns, rows, totalRows, src, alt, count, title, series, name
    }

    init(from decoder: Decoder) throws {
      let c = try decoder.container(keyedBy: Key.self)
      switch try c.decode(String.self, forKey: .type) {
      case "text": self = .text(try c.decode(String.self, forKey: .text), markdown: try c.decodeIfPresent(Bool.self, forKey: .markdown) ?? false)
      case "scalar": self = .scalar(try c.decode(Double.self, forKey: .value), label: try c.decode(String.self, forKey: .label))
      case "table":
        self = .table(columns: try c.decode([String].self, forKey: .columns), rows: try c.decode([[String]].self, forKey: .rows),
                      totalRows: try c.decode(Int.self, forKey: .totalRows))
      case "image": self = .image(src: try c.decode(String.self, forKey: .src), alt: try c.decodeIfPresent(String.self, forKey: .alt) ?? "")
      case "points": self = .points(count: try c.decode(Int.self, forKey: .count))
      case "plot": self = .plot(title: try c.decode(String.self, forKey: .title), series: try c.decode([Series].self, forKey: .series))
      default: self = .module(name: try c.decodeIfPresent(String.self, forKey: .name) ?? "")
      }
    }

    /// What the Card's result row says (DisplaySnapshot.summary).
    var summary: String {
      switch self {
      case .text(let t, let markdown):
        let value: DisplayValue = markdown ? .markdown(t) : .text(t)
        return DisplaySnapshot(runId: "", invocation: 0, revision: 0, value: value).summary
      case .scalar(let v, let label):
        return DisplaySnapshot(runId: "", invocation: 0, revision: 0, value: .scalar(v, label)).summary
      case .table(let columns, _, let totalRows): return String(localized: "表 \(columns.count) 列 × \(totalRows) 行")
      case .image(_, let alt): return alt.isEmpty ? String(localized: "画像") : alt
      case .points(let count): return String(localized: "点群 \(count) 点")
      case .plot(let title, let series):
        return title.isEmpty ? String(localized: "折れ線 \(series.count) 系列 · \(series.map(\.x.count).max() ?? 0) 点") : title
      case .module(let name): return String(localized: "モジュール \(name)")
      }
    }
  }

  var flow: String
  var draftOnly: Bool
  var cards: [Card]
  var wires: [Wire]
  var frames: [String: FrameRect]
  var steps: [Step]
  var final: [String: Final]
  var outputs: [String: Output]

  /// Where the site's data is: in the app's bundle (build-app.sh) or, in a
  /// development build, the documentation build (`nix run .#build-doc`).
  static let root: URL? = {
    if let dir = Bundle.main.url(forResource: "player-data", withExtension: nil) { return dir }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 { repo.deleteLastPathComponent() }
    let dir = repo.appendingPathComponent("build/docs/site/player-data")
    if FileManager.default.fileExists(atPath: dir.path) { return dir }
    #endif
    return nil
  }()
}

// MARK: - The replay

/// One moment of the replay: a list of these is walked by a timer.
struct DemoReplayFrame: Equatable {
  enum Mode { case idle, playing, done }
  var mode: Mode
  var step = -1
  var sub = 0
  var tick = 0
  var ms = 0

  static let idle = DemoReplayFrame(mode: .idle)
  static let done = DemoReplayFrame(mode: .done, step: .max)
  /// Invocation counts shown while a Card that ran many times (a loop) is replayed.
  static let loopTicks = 8
}

/// A demo ready to draw: what never changes during a replay, worked out once,
/// off the main thread. Rows are reserved by kind, so a run moves nothing.
struct DemoScene {
  enum Phase { case before, running, after }

  struct Placed {
    var card: DemoPlayerData.Card
    var base: CardNodeData
    var rect: CGRect
    var outline: CGRect
  }

  /// A Card's result beside it.
  struct Shown {
    var id: String
    var rect: CGRect
    var link: Path
    var output: DemoPlayerData.Output
    var image: CGImage?
    var plot: DemoPlotPicture?
  }

  struct RoutedWire {
    var wire: DemoPlayerData.Wire
    var path: Path
    var dotted: Bool
  }

  let data: DemoPlayerData
  let flow: FlowDirection
  let placed: [Placed]
  let shown: [Shown]
  let wires: [RoutedWire]
  /// Everything there is to frame, with results that are not there yet.
  let box: CGRect
  private let stepOf: [String: Int]

  /// The result frames' images are read this small: a thumbnail never shows more.
  static let imagePixels = 360

  init(_ data: DemoPlayerData, images: URL? = nil) {
    self.data = data
    let flow = FlowDirection(rawValue: data.flow) ?? .topToBottom
    self.flow = flow
    var stepOf: [String: Int] = [:]
    for (i, s) in data.steps.enumerated() { for c in s.cards { stepOf[c] = i } }
    self.stepOf = stepOf

    let placed: [Placed] = data.cards.map { c in
      let node = CardNodeData(
        title: c.id, kind: CardKind(rawValue: c.kind) ?? .code, symbol: c.symbol, summary: c.summary,
        inputs: c.inputs.map(\.data), outputs: c.outputs.map(\.data),
        params: c.params.map { ParamData(name: $0.name, type: $0.type, value: $0.value) },
        hasDisplay: data.outputs[c.id] != nil,
        permissions: c.permissions.compactMap(CardPermission.init(rawValue:)), untitled: c.untitled ?? false)
      let g = CardGeometry(node, flow: flow)
      let origin = CGPoint(x: c.x, y: c.y)
      return Placed(card: c, base: node, rect: CGRect(origin: origin, size: g.size), outline: g.outlineRect.offsetBy(dx: c.x, dy: c.y))
    }
    self.placed = placed

    // The saved place of a Card's output frame, or beside the Card (displayRect).
    let shown: [Shown] = placed.compactMap { p in
      guard let output = data.outputs[p.card.id] else { return nil }
      let f = data.frames[p.card.id]
      if f?.hidden == true { return nil }
      let w = f?.width ?? 320, h = f?.height ?? 170
      let r = flow == .topToBottom
        ? CGRect(x: f?.x ?? p.rect.maxX + 40, y: f?.y ?? p.rect.minY, width: w, height: h)
        : CGRect(x: f?.x ?? p.rect.minX, y: f?.y ?? p.rect.maxY + 30, width: w, height: h)
      var image: CGImage?
      var plot: DemoPlotPicture?
      if case .image(let src, _) = output, let images { image = Self.smallImage(images.appendingPathComponent(src)) }
      if case .plot(_, let series) = output { plot = DemoPlotPicture(series) }
      return Shown(id: p.card.id, rect: r, link: displayLink(card: p.outline, frame: r), output: output, image: image, plot: plot)
    }
    self.shown = shown

    let byId = Dictionary(placed.map { ($0.card.id, $0) }, uniquingKeysWith: { a, _ in a })
    func port(_ id: String, _ name: String, output: Bool) -> CGPoint {
      guard let p = byId[id] else { return .zero }
      let at = CardGeometry(p.base, flow: flow).port(name, output: output)
      return CGPoint(x: p.rect.minX + at.x, y: p.rect.minY + at.y)
    }
    var router = WireRouter(flow: flow, obstacles: placed.map(\.rect) + shown.map(\.rect))
    let drafts = Set(data.cards.filter { $0.kind == CardKind.draft.rawValue }.map(\.id))
    wires = data.wires.map { w in
      RoutedWire(wire: w, path: router.route(port(w.from, w.fromPort, output: true), port(w.to, w.toPort, output: false)),
                 dotted: drafts.contains(w.from) || drafts.contains(w.to))
    }
    box = (placed.map(\.rect) + shown.map(\.rect)).reduce(CGRect.null) { $0.union($1) }
  }

  /// Reads an image at thumbnail size (decoding the whole of it is what takes the time and memory).
  private static func smallImage(_ url: URL) -> CGImage? {
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
    return CGImageSourceCreateThumbnailAtIndex(source, 0, [
      kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceCreateThumbnailWithTransform: true,
      kCGImageSourceThumbnailMaxPixelSize: imagePixels,
    ] as CFDictionary)
  }

  /// A Draft has no run to play.
  var runnable: Bool { !data.draftOnly && !data.steps.isEmpty }

  /// The frames of the replay in order, `speed` times their length.
  func frames(speed: Double) -> [DemoReplayFrame] {
    let kinds = Dictionary(data.cards.map { ($0.id, $0.kind) }, uniquingKeysWith: { a, _ in a })
    let ms = { (v: Double) in Int(v * speed) }
    var out: [DemoReplayFrame] = []
    for (step, st) in data.steps.enumerated() {
      let code = st.cards.contains { kinds[$0] == "code" || kinds[$0] == "subgraph" }
      let loops = st.cards.contains { (data.final[$0]?.count ?? 0) > 1 }
      if !code {
        out.append(DemoReplayFrame(mode: .playing, step: step, ms: ms(620)))
        continue
      }
      out.append(DemoReplayFrame(mode: .playing, step: step, ms: ms(520)))
      if loops {
        for tick in 0..<DemoReplayFrame.loopTicks { out.append(DemoReplayFrame(mode: .playing, step: step, sub: 1, tick: tick, ms: ms(230))) }
      } else {
        out.append(DemoReplayFrame(mode: .playing, step: step, sub: 1, tick: DemoReplayFrame.loopTicks - 1, ms: ms(900)))
      }
      out.append(DemoReplayFrame(mode: .playing, step: step, sub: 2, tick: DemoReplayFrame.loopTicks - 1, ms: ms(480)))
    }
    return out
  }

  func phase(_ id: String, at f: DemoReplayFrame) -> Phase {
    guard f.mode != .idle, let i = stepOf[id] else { return .before }
    if f.mode == .done || i < f.step { return .after }
    return i == f.step ? .running : .before
  }

  private static func state(_ name: String) -> Tsugumi_Data_CardState {
    switch name {
    case "failed": return .failed
    case "blocked": return .blocked
    default: return .succeeded
    }
  }

  /// The stage strip of a finished invocation, from the stages the core recorded.
  private static func finishedStages(_ f: DemoPlayerData.Final) -> [StageProgress.Item] {
    Stage.applicable(code: true).map { shown in
      var parts: [StageProgress.Status] = []
      var seconds: Double?
      for part in shown.parts {
        guard let r = f.stages.last(where: { Stage.named($0.phase) == part }) else {
          parts.append(.notRun)
          continue
        }
        switch r.outcome {
        case "cached": parts.append(.cached)
        case "reused": parts.append(.reused)
        case "failed": parts.append(.failed)
        case "cancelled": parts.append(.cancelled)
        default: parts.append(.done)
        }
        if let ms = r.ms { seconds = (seconds ?? 0) + ms / 1000 }
      }
      return StageProgress.Item(stage: shown, status: StageProgress.merge(parts, ended: true), seconds: seconds)
    }
  }

  /// A Card as it is at a moment of the replay.
  func node(_ p: Placed, at fr: DemoReplayFrame) -> CardNodeData {
    var node = p.base
    node.runContinues = fr.mode == .playing
    let phase = phase(p.card.id, at: fr)
    guard let f = data.final[p.card.id], phase != .before else { return node }
    let kind = p.base.kind
    let composite = kind == .subgraph
    let result = data.outputs[p.card.id]?.summary
    if phase == .after {
      node.state = Self.state(f.state)
      node.invocation = composite ? 0 : UInt64(f.count)
      if kind == .code { node.stage = StageProgress(items: Self.finishedStages(f)) }
      node.result = result
      return node
    }
    let done = Self.finishedStages(f)
    if kind == .code {
      node.stage = StageProgress(items: done.enumerated().map { i, it in
        if i < fr.sub { return it }
        return StageProgress.Item(stage: it.stage, status: i == fr.sub ? .running : .pending)
      })
    }
    let count = fr.sub == 0 ? 1 : max(1, (f.count * (fr.tick + 1) + DemoReplayFrame.loopTicks - 1) / DemoReplayFrame.loopTicks)
    node.state = kind == .code && fr.sub == 0 ? .preparing : .running
    node.invocation = composite ? 0 : UInt64(count)
    if fr.sub >= 1 { node.result = result }
    return node
  }

  /// A value on its way moves, one that arrived stays coloured.
  func style(_ w: DemoPlayerData.Wire, at fr: DemoReplayFrame) -> WireDraw.Style {
    guard let f = data.final[w.from], f.sent.contains(w.fromPort) else { return .normal }
    let from = phase(w.from, at: fr)
    if from == .before { return .normal }
    if from == .running, !(f.count > 1 && fr.sub >= 1), fr.sub < 2 { return .normal }
    if fr.mode == .playing, phase(w.to, at: fr) != .after { return .active }
    return .carried
  }

  /// A result is there once its Card has got to running its code.
  func isShown(_ s: Shown, at fr: DemoReplayFrame) -> Bool {
    let p = phase(s.id, at: fr)
    return p == .after || (p == .running && fr.sub >= 1)
  }

  /// A thumbnail shows the whole graph, smaller than a canvas ever zooms out.
  func camera(in size: CGSize) -> CanvasCamera {
    guard !box.isNull, size.width > 0, size.height > 0 else { return CanvasCamera() }
    let r = box.insetBy(dx: -34, dy: -34)
    let zoom = min(0.85, size.width / r.width, size.height / r.height)
    return CanvasCamera(x: size.width / 2 - r.midX * zoom, y: size.height / 2 - r.midY * zoom, zoom: zoom)
  }
}

private extension Stage {
  /// The core's name of a stage in a recorded run.
  static func named(_ phase: String) -> Stage? {
    switch phase {
    case "prepare": return .prepare
    case "evaluate": return .evaluate
    case "build": return .build
    case "execute": return .execute
    case "validate": return .validate
    case "save": return .save
    default: return nil
    }
  }
}

/// A figure's own points in unit coordinates, without the plot library (the
/// app draws a result with Plotly, Vega-Lite or the WebGPU line view when a project is open).
struct DemoPlotPicture {
  struct Line {
    var path: Path
    var points: Bool
  }

  static let maxPoints = 240
  let lines: [Line]

  init?(_ series: [DemoPlayerData.Series]) {
    let shown = series.prefix(4)
    let xs = shown.flatMap(\.x), ys = shown.flatMap(\.y)
    guard let x0 = xs.min(), let x1 = xs.max(), let y0 = ys.min(), let y1 = ys.max() else { return nil }
    func at(_ s: DemoPlayerData.Series, _ i: Int) -> CGPoint {
      CGPoint(x: x1 > x0 ? (s.x[i] - x0) / (x1 - x0) : 0, y: y1 > y0 ? 1 - (s.y[i] - y0) / (y1 - y0) : 0)
    }
    lines = shown.map { s in
      let n = min(s.x.count, s.y.count)
      var path = Path()
      for i in stride(from: 0, to: n, by: max(1, n / Self.maxPoints)) {
        let p = at(s, i)
        if s.points {
          path.addEllipse(in: CGRect(x: p.x, y: p.y, width: 0, height: 0))
          path.move(to: p)
        } else if i == 0 { path.move(to: p) } else { path.addLine(to: p) }
      }
      return Line(path: path, points: s.points)
    }
  }
}

// MARK: - Loading

/// Scenes are read once, off the main thread, as thumbnails come near the
/// screen (the list makes them lazily), and kept while the app runs. A demo that
/// has played once shows where the run ended when the list comes back to it.
@MainActor
final class DemoScenes {
  static let shared = DemoScenes()
  private var scenes: [String: DemoScene] = [:]
  private(set) var played: Set<String> = []

  func cached(_ id: String) -> DemoScene? { scenes[id] }
  func markPlayed(_ id: String) { played.insert(id) }

  func scene(_ id: String) async -> DemoScene? {
    if let s = scenes[id] { return s }
    guard let root = DemoPlayerData.root else { return nil }
    let scene = await Task.detached(priority: .userInitiated) { () -> DemoScene? in
      guard let bytes = try? Data(contentsOf: root.appendingPathComponent("\(id).json")),
            let data = try? JSONDecoder().decode(DemoPlayerData.self, from: bytes) else { return nil }
      return DemoScene(data, images: root)
    }.value
    if let scene { scenes[id] = scene }
    return scene
  }
}

// MARK: - The view

/// A demo's top-level Workflow in miniature, with its real Cards, ports, wires
/// and results: it plays once as it appears and again under the pointer.
struct DemoThumbnail: View {
  let id: String
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var scene: DemoScene?
  @State private var frame = DemoReplayFrame.idle
  /// Each new value plays the replay from its start (0: nothing asked yet).
  @State private var play = 0

  /// As the gallery: a little faster than the large player.
  private static let speed = 0.6

  var body: some View {
    GeometryReader { geo in
      ZStack(alignment: .topLeading) {
        Color(nsColor: .textBackgroundColor)
        if let scene { DemoThumbnailCanvas(scene: scene, frame: frame, size: geo.size, animate: !reduceMotion) }
      }
      .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
      .clipped()
      .allowsHitTesting(false)
    }
    .contentShape(Rectangle())
    .onHover { if $0, !reduceMotion, scene?.runnable == true { play += 1 } }
    .accessibilityHidden(true)
    .task {
      guard scene == nil, let s = await DemoScenes.shared.scene(id) else { return }
      scene = s
      guard s.runnable else { return }
      if reduceMotion || DemoScenes.shared.played.contains(id) {
        frame = .done
      } else {
        play += 1
      }
    }
    .task(id: play) {
      guard play > 0, let scene else { return }
      DemoScenes.shared.markPlayed(id)
      for f in scene.frames(speed: Self.speed) {
        frame = f
        try? await Task.sleep(for: .milliseconds(f.ms))
        if Task.isCancelled { return }
      }
      frame = .done
    }
  }
}

/// The canvas of a thumbnail at one moment. The Cards and results are laid out
/// at their real size and scaled down as one, so the Card's minimum font sizes
/// never run over its shape; the wires are drawn at screen scale to stay visible.
struct DemoThumbnailCanvas: View {
  let scene: DemoScene
  let frame: DemoReplayFrame
  let size: CGSize
  let animate: Bool

  var body: some View {
    let camera = scene.camera(in: size)
    let shown = scene.shown.filter { scene.isShown($0, at: frame) }
    let box = scene.box
    ZStack(alignment: .topLeading) {
      WireLayer(wires: wires(camera, shown), animate: animate, zoom: camera.zoom)
      ZStack(alignment: .topLeading) {
        ForEach(shown, id: \.id) { s in
          DemoThumbnailFrame(shown: s)
            .equatable()
            .frame(width: s.rect.width, height: s.rect.height)
            .offset(x: s.rect.minX - box.minX, y: s.rect.minY - box.minY)
        }
        ForEach(scene.placed, id: \.card.id) { p in
          DemoThumbnailCard(data: scene.node(p, at: frame), flow: scene.flow)
            .equatable()
            .frame(width: p.rect.width, height: p.rect.height)
            .offset(x: p.rect.minX - box.minX, y: p.rect.minY - box.minY)
        }
      }
      .frame(width: box.width, height: box.height, alignment: .topLeading)
      .scaleEffect(camera.zoom, anchor: .topLeading)
      .offset(x: camera.x + box.minX * camera.zoom, y: camera.y + box.minY * camera.zoom)
    }
    .frame(width: size.width, height: size.height, alignment: .topLeading)
    .clipped()
    // One bitmap of the thumbnail's size. Without it the Cards keep layers at their full size.
    .drawingGroup()
  }

  private func wires(_ camera: CanvasCamera, _ shown: [DemoScene.Shown]) -> [WireDraw] {
    scene.wires.map { WireDraw(path: $0.path.applying(camera.transform), style: scene.style($0.wire, at: frame), dotted: $0.dotted) }
      + shown.map { WireDraw(path: $0.link.applying(camera.transform), style: .display) }
  }
}

/// A Card of a thumbnail: drawn again only when it has changed, which at any
/// moment of a replay is one or two of them.
private struct DemoThumbnailCard: View, Equatable {
  let data: CardNodeData
  let flow: FlowDirection

  var body: some View { CardNodeView(data: data, flow: flow, zoom: 1, paramsEditable: false) }
}

/// A result frame as on the canvas (OutputFrameView), its content read small.
/// A result does not change during a replay.
private struct DemoThumbnailFrame: View, Equatable {
  let shown: DemoScene.Shown

  static func == (a: Self, b: Self) -> Bool { a.shown.id == b.shown.id }

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack(spacing: 6) {
        Image(systemName: "chart.xyaxis.line").font(.system(size: 10)).foregroundStyle(.purple)
        Text(verbatim: shown.id).font(.system(size: 10, weight: .bold)).lineLimit(1)
      }
      content.frame(maxWidth: .infinity, maxHeight: .infinity).clipped()
    }
    .padding(8)
    .background(RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .windowBackgroundColor)))
    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.purple.opacity(0.45)))
  }

  @ViewBuilder private var content: some View {
    switch shown.output {
    case .text(let t, _):
      Text(verbatim: String(t.prefix(800))).font(.system(size: 11, design: .monospaced))
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    case .scalar(let v, let label):
      VStack {
        Text(verbatim: label).font(.system(size: 10))
        Text(v, format: .number.precision(.significantDigits(6))).font(.system(size: 22).monospacedDigit())
      }
    case .table(let columns, let rows, _):
      Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 3) {
        GridRow { ForEach(Array(columns.enumerated()), id: \.offset) { _, c in Text(verbatim: c).font(.system(size: 10, weight: .bold)) } }
        ForEach(Array(rows.prefix(12).enumerated()), id: \.offset) { _, row in
          GridRow { ForEach(Array(row.enumerated()), id: \.offset) { _, cell in Text(verbatim: cell).font(.system(size: 10, design: .monospaced)) } }
        }
      }
      .lineLimit(1)
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    case .image(_, let alt):
      if let image = shown.image { Image(decorative: image, scale: 1).resizable().scaledToFit() } else { placeholder(alt) }
    case .points(let count):
      placeholder(String(localized: "点群 \(count) 点"))
    case .plot(let title, _):
      if let plot = shown.plot { DemoPlotView(plot: plot) } else { placeholder(title) }
    case .module(let name):
      placeholder(String(localized: "モジュール \(name)"))
    }
  }

  private func placeholder(_ text: String) -> some View {
    Text(verbatim: text).font(.system(size: 11)).foregroundStyle(.secondary)
  }
}

private struct DemoPlotView: View {
  let plot: DemoPlotPicture
  private static let colors: [Color] = [.blue, .orange, .green, .purple]

  var body: some View {
    Canvas { g, size in
      let box = CGRect(origin: .zero, size: size).insetBy(dx: 10, dy: 10)
      let t = CGAffineTransform(a: box.width, b: 0, c: 0, d: box.height, tx: box.minX, ty: box.minY)
      for i in 0...2 {
        let y = box.minY + box.height * CGFloat(i) / 2
        var rule = Path()
        rule.move(to: CGPoint(x: box.minX, y: y))
        rule.addLine(to: CGPoint(x: box.maxX, y: y))
        g.stroke(rule, with: .color(.secondary.opacity(0.25)), lineWidth: 1)
      }
      for (i, line) in plot.lines.enumerated() {
        let color = Self.colors[i % Self.colors.count]
        let path = line.path.applying(t)
        // A dot is a line of no length with a round end.
        g.stroke(path, with: .color(line.points ? color.opacity(0.7) : color), style: StrokeStyle(lineWidth: line.points ? 5 : 2.5, lineCap: .round, lineJoin: .round))
      }
    }
  }
}
