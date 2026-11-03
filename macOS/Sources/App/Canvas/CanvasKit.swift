import AppKit
import SwiftUI
import TsugumiRPC

// The Card canvas building blocks shared by the main canvas, the appearance
// preview in Settings and the preview in Card settings: Card data, geometry,
// the Card view, wires and a small self-contained canvas (CanvasSurface).
//
// Cards are drawn at the screen scale (sizes and fonts multiplied by the
// zoom) instead of being scaled as a bitmap, and placed on whole device
// pixels, so text stays sharp at any zoom. Ports, wires, the selection and
// hit-testing all use the same geometry and the same world → screen mapping.

/// Direction the work flows in: inputs enter on the top (or left) edge and
/// outputs leave on the bottom (or right) edge.
enum FlowDirection: String, CaseIterable, Identifiable {
  case topToBottom, leftToRight
  var id: String { rawValue }
  var label: String { self == .topToBottom ? String(localized: "上から下") : String(localized: "左から右") }
}

/// One execution stage of an invocation, in the order the core runs them.
enum Stage: Int, CaseIterable, Identifiable {
  case waiting, prepare, evaluate, build, execute, validate, save, done
  var id: Int { rawValue }

  var label: String {
    switch self {
    case .waiting: return String(localized: "入力待ち")
    case .prepare: return String(localized: "SDK 準備")
    case .evaluate: return String(localized: "Nix 評価")
    case .build: return "build"
    case .execute: return "run"
    case .validate: return "validate"
    case .save: return String(localized: "保存")
    case .done: return String(localized: "完了")
    }
  }

  /// Short "in progress" wording for the status row.
  var activeLabel: String {
    switch self {
    case .waiting: return String(localized: "入力待ち")
    case .prepare: return String(localized: "SDK 準備中")
    case .evaluate: return String(localized: "Nix 評価中")
    case .build: return String(localized: "build 中")
    case .execute: return String(localized: "実行中")
    case .validate: return String(localized: "出力を検証中")
    case .save: return String(localized: "保存中")
    case .done: return String(localized: "完了")
    }
  }

  /// The stage's own icon (kept in every state; the state is shown by color
  /// and a small badge). Only symbols that exist are named here.
  var symbol: String {
    switch self {
    case .waiting: return "tray.and.arrow.down"
    case .prepare: return "shippingbox"
    case .evaluate: return "snowflake"
    case .build: return "hammer"
    case .execute: return "play"
    case .validate: return "checkmark.shield"
    case .save: return "externaldrive"
    case .done: return "flag.checkered"
    }
  }

  /// The filled variant shown while the stage runs (where one exists).
  var activeSymbol: String {
    switch self {
    case .prepare: return "shippingbox.fill"
    case .build: return "hammer.fill"
    case .execute: return "play.fill"
    case .validate: return "checkmark.shield.fill"
    case .save: return "externaldrive.fill"
    case .waiting, .evaluate, .done: return symbol
    }
  }

  /// The log stream that holds what this stage printed.
  var logStream: Tsugumi_Data_LogStream {
    switch self {
    case .prepare, .evaluate, .build: return .build
    case .execute: return .stdout
    case .waiting, .validate, .save, .done: return .journal
    }
  }

  init?(phase: Tsugumi_Data_Phase) {
    switch phase {
    case .prepare: self = .prepare
    case .evaluate: self = .evaluate
    case .build: self = .build
    case .execute: self = .execute
    case .validate: self = .validate
    case .save: self = .save
    default: return nil
    }
  }

  /// Stages shown for a Card, in order: build (SDK preparation, Nix
  /// evaluation and build), run, validate (output check and saving).
  /// Builtin Cards do not build.
  static func applicable(code: Bool) -> [Stage] {
    code ? [.build, .execute, .validate] : [.execute, .validate]
  }

  /// The core's stages a shown stage stands for.
  var parts: [Stage] {
    switch self {
    case .build: return [.prepare, .evaluate, .build]
    case .validate: return [.validate, .save]
    default: return [self]
    }
  }
}

/// Progress of the latest invocation as the stage strip shows it, from the
/// stages the core recorded (never inferred from the final state).
struct StageProgress: Equatable {
  enum Status: Equatable {
    case pending     // not reached (yet)
    case running
    case done
    case cached      // the Nix store had the build: no work
    case reused      // kept from an earlier invocation of the run
    case failed
    case cancelled
    case notRun      // the invocation ended before this stage

    var label: String {
      switch self {
      case .pending: return String(localized: "未開始")
      case .running: return String(localized: "進行中")
      case .done: return String(localized: "完了")
      case .cached: return String(localized: "キャッシュを利用（処理なし）")
      case .reused: return String(localized: "この Run で準備済みのものを再利用")
      case .failed: return String(localized: "失敗")
      case .cancelled: return String(localized: "停止")
      case .notRun: return String(localized: "実行されませんでした")
      }
    }
  }

  struct Item: Equatable, Identifiable {
    var stage: Stage
    var status: Status
    var seconds: Double?
    var id: Int { stage.rawValue }
  }

  var items: [Item]
  var workerReused = false

  var current: Item? { items.first { $0.status == .running } }

  /// The status of a shown stage from the core's stages it covers.
  static func merge(_ parts: [Status], ended: Bool) -> Status {
    if parts.contains(.failed) { return .failed }
    if parts.contains(.cancelled) { return .cancelled }
    if parts.contains(.running) { return .running }
    let reached = parts.filter { $0 != .pending && $0 != .notRun }
    if reached.isEmpty { return parts.first ?? .pending }
    // Part of it done, the rest still to come: in progress while the call runs.
    if reached.count < parts.count { return ended ? (reached.contains(.done) ? .done : reached[0]) : .running }
    if reached.contains(.done) { return .done }
    return reached.allSatisfy { $0 == .reused } ? .reused : .cached
  }
  var failed: Item? { items.first { $0.status == .failed || $0.status == .cancelled } }
}

struct PortData: Hashable {
  var name: String
  var typeName: String?
  var optional = false
  var note: String?
}

struct ParamData: Hashable {
  var name: String
  var type: String
  var value: String
}

/// Everything a Card view needs, with no dependency on a session.
struct CardNodeData: Equatable {
  var title: String
  var kind: CardKind
  var symbol: String?
  /// Short description (one line on the Card; the whole text on hover).
  var summary: String?
  var inputs: [PortData]
  var outputs: [PortData]
  var params: [ParamData] = []
  var result: String?
  var resultFailed = false
  var state: Tsugumi_Data_CardState?
  var stage: StageProgress?
  /// Invocation number in the current run (0: not run in this run).
  var invocation: UInt64 = 0
  /// The run is still going on (a succeeded Card may run again).
  var runContinues = false
  var failure: String?
  var errors: [String] = []
  var warnings: [String] = []
  var collapsed = false
  /// The Card's result has a frame on the canvas (display anchor).
  var hasDisplay = false

  var isDraft: Bool { kind == .draft }
  var isRouting: Bool { kind == .fork || kind == .branch || kind == .merge }

  /// "実行 #3 · 成功" and the like; nil before the first invocation.
  var runText: String? {
    guard let state, invocation > 0 else { return nil }
    var s = String(localized: "実行 #\(invocation)")
    if let cur = stage?.current, state == .running || state == .preparing {
      s += " · " + cur.stage.activeLabel
    } else {
      s += " · " + state.label
      if state == .succeeded && runContinues { s += String(localized: "（次の入力を待てます）") }
    }
    return s
  }

  /// Plain status for a Card that did not run in this run.
  var idleText: String {
    if isDraft { return String(localized: "未実装（Draft）") }
    if let state, state != .notStarted { return state.label }
    return String(localized: "未実行（0 回）")
  }
}

/// Outline of a Card on the canvas.
enum CardShape {
  case card, branch, fork, merge, start

  init(_ kind: CardKind) {
    switch kind {
    case .trigger: self = .start
    case .branch: self = .branch
    case .fork: self = .fork
    case .merge: self = .merge
    default: self = .card
    }
  }
}

/// Size, outline and port positions of a Card (world units, relative to its
/// top-left corner). Shared by drawing, wires, the selection and hit-tests.
struct CardGeometry {
  static let width: CGFloat = 220
  static let header: CGFloat = 32
  static let summaryRow: CGFloat = 15
  static let portRow: CGFloat = 15
  static let paramRow: CGFloat = 26
  static let resultRow: CGFloat = 18
  static let stageRow: CGFloat = 22
  /// Labels narrower than this are hidden (shown on hover instead).
  static let minLabel: CGFloat = 26
  /// Share of a triangle's wide edge taken by its narrow end (its apex).
  static let narrow: CGFloat = 0

  let inputs: [String]
  let outputs: [String]
  let collapsed: Bool
  let summary: Bool
  let params: Int
  let result: Bool
  let stages: Bool
  let flow: FlowDirection
  let shape: CardShape

  init(_ d: CardNodeData, flow: FlowDirection) {
    inputs = d.inputs.map(\.name)
    outputs = d.outputs.map(\.name)
    collapsed = d.collapsed
    summary = d.summary != nil && !d.isRouting
    params = d.params.count
    // Rows are reserved by kind, so a run never makes a Card grow or shrink.
    result = d.kind == .logger || d.hasDisplay
    stages = d.kind == .code || d.kind == .draft
    self.flow = flow
    shape = CardShape(d.kind)
  }

  // MARK: Outline

  /// Width across the flow and length along it of a routing Card's outline.
  private var routingSize: CGSize {
    switch shape {
    // Small marks: what they do is plain from the shape (details on hover).
    case .branch: return CGSize(width: 60, height: 48)
    case .fork: return CGSize(width: max(56, CGFloat(outputs.count) * 30 + 12), height: 40)
    case .merge: return CGSize(width: max(56, CGFloat(inputs.count) * 30 + 12), height: 40)
    // The start Card: its name and a run button, nothing more.
    case .start: return CGSize(width: 116, height: 34)
    case .card: return .zero
    }
  }

  /// Labels of a routing Card sit outside its outline, in these bands.
  private var bandIn: CGFloat { shape != .card && flow == .topToBottom && showLabels(input: true) ? Self.portRow : 0 }
  private var bandOut: CGFloat { shape != .card && flow == .topToBottom && showLabels(input: false) ? Self.portRow : 0 }

  var portBand: CGFloat {
    flow == .leftToRight && shape == .card ? CGFloat(max(inputs.count, outputs.count, 1)) * Self.portRow : 0
  }
  var topLabels: CGFloat { shape == .card && flow == .topToBottom && showLabels(input: true) ? Self.portRow : 0 }
  var bottomLabels: CGFloat { shape == .card && flow == .topToBottom && showLabels(input: false) ? Self.portRow : 0 }

  var size: CGSize {
    if shape == .start { return routingSize }
    if shape != .card {
      let r = routingSize
      return flow == .topToBottom ? CGSize(width: r.width, height: bandIn + r.height + bandOut)
                                  : CGSize(width: r.height + 60, height: r.width)
    }
    if collapsed { return CGSize(width: Self.width, height: Self.header) }
    let h = topLabels + Self.header + (summary ? Self.summaryRow : 0) + portBand + CGFloat(params) * Self.paramRow +
      (result ? Self.resultRow : 0) + (stages ? Self.stageRow : 0) + bottomLabels + 4
    return CGSize(width: Self.width, height: h)
  }
  var height: CGFloat { size.height }

  /// The outline's frame inside the Card's box.
  var outlineRect: CGRect {
    let s = size
    guard shape != .card, shape != .start else { return CGRect(origin: .zero, size: s) }
    if flow == .topToBottom { return CGRect(x: 0, y: bandIn, width: s.width, height: s.height - bandIn - bandOut) }
    return CGRect(x: 30, y: 0, width: s.width - 60, height: s.height)
  }

  /// The outline itself (Card box, diamond, fan or funnel) inside `r`.
  func outline(in r: CGRect) -> Path {
    switch shape {
    case .card:
      return Path(roundedRect: r, cornerRadius: 9)
    case .start:
      return Path(roundedRect: r, cornerRadius: r.height / 2)
    case .branch:
      return roundedPolygon([CGPoint(x: r.midX, y: r.minY), CGPoint(x: r.maxX, y: r.midY),
                             CGPoint(x: r.midX, y: r.maxY), CGPoint(x: r.minX, y: r.midY)], radius: 6)
    case .fork, .merge:
      // Triangle: apex upstream (fork) or downstream (merge).
      if flow == .topToBottom {
        return roundedPolygon(shape == .fork
          ? [CGPoint(x: r.midX, y: r.minY), CGPoint(x: r.maxX, y: r.maxY), CGPoint(x: r.minX, y: r.maxY)]
          : [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.midX, y: r.maxY)], radius: 4)
      }
      let pts: [CGPoint]
      if flow == .topToBottom {
        let inset = r.width * (1 - Self.narrow) / 2
        pts = shape == .fork
          ? [CGPoint(x: r.minX + inset, y: r.minY), CGPoint(x: r.maxX - inset, y: r.minY), CGPoint(x: r.maxX, y: r.maxY), CGPoint(x: r.minX, y: r.maxY)]
          : [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.maxX - inset, y: r.maxY), CGPoint(x: r.minX + inset, y: r.maxY)]
      } else {
        let inset = r.height * (1 - Self.narrow) / 2
        pts = shape == .fork
          ? [CGPoint(x: r.minX, y: r.minY + inset), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.maxX, y: r.maxY), CGPoint(x: r.minX, y: r.maxY - inset)]
          : [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY + inset), CGPoint(x: r.maxX, y: r.maxY - inset), CGPoint(x: r.minX, y: r.maxY)]
      }
      return roundedPolygon(pts, radius: 7)
    }
  }

  // MARK: Ports

  /// The segment ports of one side are spread along (top-to-bottom: an x
  /// range at a fixed y).
  private func edgeSpan(input: Bool) -> (start: CGFloat, width: CGFloat, at: CGFloat) {
    switch shape {
    case .card, .branch, .start:
      let s = shape == .card ? CGSize(width: Self.width, height: collapsed ? Self.header : 0) : routingSize
      return (0, s.width, 0)
    case .fork, .merge:
      let w = routingSize.width
      let narrowSide = (shape == .fork) == input
      let span = narrowSide ? w * Self.narrow : w
      return ((w - span) / 2, span, 0)
    }
  }

  /// Room for one port's label on its edge.
  func spacing(input: Bool) -> CGFloat {
    let list = input ? inputs : outputs
    guard !list.isEmpty else { return 0 }
    if shape == .branch { return routingSize.width / 2 - 8 }
    let span = edgeSpan(input: input)
    // A single label may be as wide as the Card; several share the edge
    // without overlapping their neighbours.
    let pitch = span.width / CGFloat(list.count + 1)
    return list.count == 1 ? max(pitch * 2, 90) : pitch - 4
  }

  /// Labels are shown when there is room for them; otherwise the whole name
  /// and type show on hover.
  func showLabels(input: Bool) -> Bool {
    let list = input ? inputs : outputs
    guard !list.isEmpty else { return false }
    if shape == .card && collapsed { return false }
    if shape == .start { return false }
    // Routing marks name their ports on hover; a branch marks only T / F.
    if shape == .fork || shape == .merge { return false }
    if shape == .branch { return true }
    return spacing(input: input) >= Self.minLabel
  }

  func port(_ name: String, output: Bool) -> CGPoint {
    let list = output ? outputs : inputs
    let index = list.firstIndex(of: name) ?? 0
    let i = CGFloat(index)
    let n = CGFloat(max(list.count, 1))
    let r = outlineRect
    if shape == .start {
      // Its one output, in the middle of the downstream edge.
      return flow == .topToBottom ? CGPoint(x: r.midX, y: r.maxY) : CGPoint(x: r.maxX, y: r.midY)
    }
    if shape == .branch {
      // value / condition on the two upper edges, a / b on the two lower edges.
      let first = index == 0
      if flow == .topToBottom {
        return CGPoint(x: r.minX + r.width * (first ? 0.25 : 0.75), y: r.minY + r.height * (output ? 0.75 : 0.25))
      }
      return CGPoint(x: r.minX + r.width * (output ? 0.75 : 0.25), y: r.minY + r.height * (first ? 0.25 : 0.75))
    }
    switch flow {
    case .topToBottom:
      let span = edgeSpan(input: !output)
      return CGPoint(x: span.start + span.width * (i + 1) / (n + 1), y: output ? r.maxY : r.minY)
    case .leftToRight:
      if shape != .card {
        let narrowSide = (shape == .fork) == !output
        let h = narrowSide ? r.height * Self.narrow : r.height
        return CGPoint(x: output ? r.maxX : r.minX, y: r.midY - h / 2 + h * (i + 1) / (n + 1))
      }
      let x = output ? Self.width : 0
      if collapsed { return CGPoint(x: x, y: Self.header / 2) }
      let top = Self.header + (summary ? Self.summaryRow : 0)
      return CGPoint(x: x, y: top + i * Self.portRow + Self.portRow / 2)
    }
  }

  /// Anchor of the line to the Card's output frame (on the side, across the flow).
  var displayAnchor: CGPoint {
    let r = outlineRect
    return flow == .topToBottom ? CGPoint(x: r.maxX, y: r.minY + min(Self.header / 2, r.height / 2))
                                : CGPoint(x: r.midX, y: r.maxY)
  }
}

/// A closed polygon with rounded corners.
func roundedPolygon(_ pts: [CGPoint], radius: CGFloat) -> Path {
  Path { p in
    let n = pts.count
    guard n > 2 else { return }
    p.move(to: CGPoint(x: (pts[n - 1].x + pts[0].x) / 2, y: (pts[n - 1].y + pts[0].y) / 2))
    for i in 0..<n {
      p.addArc(tangent1End: pts[i], tangent2End: pts[(i + 1) % n], radius: radius)
    }
    p.closeSubpath()
  }
}

// MARK: - Camera

struct CanvasCamera: Equatable {
  var x: Double = 40
  var y: Double = 40
  var zoom: Double = 1

  func screen(_ w: CGPoint) -> CGPoint { CGPoint(x: w.x * zoom + x, y: w.y * zoom + y) }
  func world(_ s: CGPoint) -> CGPoint { CGPoint(x: (s.x - x) / zoom, y: (s.y - y) / zoom) }
  var transform: CGAffineTransform { CGAffineTransform(a: zoom, b: 0, c: 0, d: zoom, tx: x, ty: y) }

  /// Zooms keeping `anchor` (a screen point) fixed.
  mutating func zoom(to z: Double, around anchor: CGPoint) {
    let nz = max(0.2, min(3, z))
    x = anchor.x - (anchor.x - x) * nz / zoom
    y = anchor.y - (anchor.y - y) * nz / zoom
    zoom = nz
  }

  /// Frames `rect` (world units) in the part of a view of `size` that is not
  /// covered by floating controls (`insets`).
  mutating func fit(_ rect: CGRect, in size: CGSize, insets: EdgeInsets = EdgeInsets(), maxZoom: Double = 1.5,
                    margin: CGFloat = 40) {
    guard !rect.isNull, size.width > 0, size.height > 0 else { return }
    let r = rect.insetBy(dx: -margin, dy: -margin)
    let w = max(80, size.width - insets.leading - insets.trailing)
    let h = max(80, size.height - insets.top - insets.bottom)
    let z = max(0.2, min(maxZoom, min(w / r.width, h / r.height)))
    zoom = z
    x = insets.leading + w / 2 - r.midX * z
    y = insets.top + h / 2 - r.midY * z
  }
}

extension EnvironmentValues {
  /// The canvas zoom a view inside the canvas draws itself at (1 elsewhere).
  @Entry var canvasZoom: CGFloat = 1
}

extension View {
  /// Places a view drawn at screen scale (`size` × `zoom`) with its top-left
  /// corner at `topLeft` (screen units), on whole device pixels.
  func placeScaled(at topLeft: CGPoint, size: CGSize, zoom: Double, pixel: CGFloat = 2) -> some View {
    let w = size.width * zoom, h = size.height * zoom
    let x = (topLeft.x * pixel).rounded() / pixel, y = (topLeft.y * pixel).rounded() / pixel
    return frame(width: w, height: h).position(x: x + w / 2, y: y + h / 2)
  }

  /// Places a view of `size` world units (frames, notes, groups) laid out at
  /// screen size: the view draws itself at `canvasZoom` (fonts and spacing
  /// times the zoom), so text stays sharp at any zoom (nothing is scaled up
  /// as a bitmap).
  func place(at topLeft: CGPoint, size: CGSize, zoom: Double) -> some View {
    frame(width: size.width * zoom, height: size.height * zoom, alignment: .topLeading)
      .environment(\.canvasZoom, CGFloat(zoom))
      .position(x: topLeft.x + size.width * zoom / 2, y: topLeft.y + size.height * zoom / 2)
  }
}

// MARK: - Card view

/// A Card as drawn on every canvas, at the screen scale `zoom`.
struct CardNodeView: View {
  let data: CardNodeData
  let flow: FlowDirection
  var zoom: Double = 1
  var selected = false
  /// Committing an edited param of an input Card (nil: read-only).
  var onParamCommit: ((String, String) -> Void)?
  var paramsEditable = true
  var onPortDrag: ((String, CGPoint) -> Void)?
  var onPortDrop: ((String, CGPoint) -> Void)?
  /// Dragging from an input port (a wire drawn back to an output).
  var onInputDrag: ((String, CGPoint) -> Void)?
  var onInputDrop: ((String, CGPoint) -> Void)?
  var onStageTap: ((Stage) -> Void)?
  var onToggleCollapse: (() -> Void)?
  /// The start Card's button: runs the workflow from it (nil: not offered).
  var onRun: (() -> Void)?

  private var z: CGFloat { CGFloat(zoom) }
  private func s(_ v: CGFloat) -> CGFloat { v * z }
  private func font(_ size: CGFloat, _ weight: Font.Weight = .regular, mono: Bool = false) -> Font {
    .system(size: max(6, size * z), weight: weight, design: mono ? .monospaced : .default)
  }

  var body: some View {
    let g = CardGeometry(data, flow: flow)
    let size = CGSize(width: g.size.width * z, height: g.size.height * z)
    let o = g.outlineRect
    let outline = CGRect(x: o.minX * z, y: o.minY * z, width: o.width * z, height: o.height * z)
    let path = g.outline(in: outline)
    ZStack(alignment: .topLeading) {
      path.fill(fillColor)
        .shadow(color: .black.opacity(selected ? 0.2 : 0.07), radius: selected ? 4 : 1.5, y: 1)
      if g.shape == .card { cardBody(g) } else if g.shape == .start { startBody(outline: outline) } else { routingBody(g, outline: outline) }
      path.stroke(borderColor, style: StrokeStyle(lineWidth: selected ? 2.4 : 1, dash: data.isDraft ? [5 * z, 3 * z] : []))
      portLabels(g)
      portMarks(g)
    }
    .frame(width: size.width, height: size.height, alignment: .topLeading)
    // The outline and the ports: a port sits on the edge, and the half of it
    // outside the outline must still start a wire.
    .contentShape(hitShape(g, outline: path))
    .accessibilityElement(children: .contain)
    .accessibilityLabel(accessibilityText)
  }

  private func hitShape(_ g: CardGeometry, outline: Path) -> Path {
    var p = outline
    let hit = max(16, s(18))
    for (list, output) in [(data.inputs, false), (data.outputs, true)] {
      for port in list {
        let pt = g.port(port.name, output: output)
        p.addRect(CGRect(x: s(pt.x) - hit / 2, y: s(pt.y) - hit / 2, width: hit, height: hit))
      }
    }
    return p
  }

  private var accessibilityText: String {
    var parts = ["Card \(data.title)", data.kind.label, data.runText ?? data.idleText]
    if let s = data.summary { parts.append(s) }
    if !data.errors.isEmpty { parts.append(data.errors.joined(separator: " ")) }
    return parts.joined(separator: ", ")
  }

  private var fillColor: Color {
    data.isRouting ? Color(nsColor: .underPageBackgroundColor) : Color(nsColor: .controlBackgroundColor)
  }

  // MARK: Ordinary Card

  private func cardBody(_ g: CardGeometry) -> some View {
    VStack(alignment: .leading, spacing: 0) {
      Color.clear.frame(height: s(g.topLabels))
      header.frame(height: s(CardGeometry.header))
      if !g.collapsed {
        if g.summary, let summary = data.summary {
          Text(summary).font(font(10)).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail)
            .padding(.horizontal, s(10)).frame(height: s(CardGeometry.summaryRow), alignment: .top)
        }
        if flow == .leftToRight { Color.clear.frame(height: s(g.portBand)) }
        ForEach(data.params, id: \.name) { p in
          ParamFieldView(param: p, editable: onParamCommit != nil && paramsEditable, zoom: zoom) { onParamCommit?(p.name, $0) }
            .frame(height: s(CardGeometry.paramRow))
            .padding(.horizontal, s(10))
        }
        if g.result {
          HStack(spacing: s(4)) {
            Image(systemName: "text.below.photo").font(font(9))
            Text(data.result ?? String(localized: "結果はまだありません")).font(font(10, mono: data.result != nil)).lineLimit(1)
          }
          .foregroundStyle(.secondary)
          .padding(.horizontal, s(10))
          .frame(height: s(CardGeometry.resultRow))
        }
        if g.stages { statusRow.frame(height: s(CardGeometry.stageRow)).padding(.horizontal, s(8)) }
      }
    }
    .frame(width: s(CardGeometry.width), alignment: .leading)
  }

  private var header: some View {
    HStack(spacing: s(6)) {
      Image(systemName: data.symbol ?? data.kind.symbol).font(font(12)).foregroundStyle(.secondary).frame(width: s(15))
      Text(data.title).font(font(12, .semibold)).lineLimit(1).truncationMode(.tail)
      if data.isDraft {
        Text("未実装").font(font(9, .semibold)).padding(.horizontal, s(4))
          .background(Color.orange.opacity(0.2), in: Capsule())
      }
      Spacer(minLength: s(2))
      if !data.errors.isEmpty {
        Image(systemName: "xmark.octagon.fill").font(font(11)).foregroundStyle(.red)
      } else if !data.warnings.isEmpty {
        Image(systemName: "exclamationmark.triangle.fill").font(font(11)).foregroundStyle(.yellow)
      }
      stateBadge
      if let toggle = onToggleCollapse, data.kind != .trigger {
        Button(action: toggle) {
          Image(systemName: data.collapsed ? "chevron.down" : "chevron.up").font(font(9, .semibold))
        }
        .buttonStyle(.borderless).foregroundStyle(.secondary)
        .help(data.collapsed ? "展開" : "折りたたむ（実行には影響しません）")
      }
    }
    .padding(.horizontal, s(9))
  }

  /// State icon and invocation number.
  @ViewBuilder private var stateBadge: some View {
    if let st = data.state, st != .notStarted {
      HStack(spacing: s(2)) {
        Image(systemName: st.symbol).foregroundStyle(st.color)
        if data.invocation > 0 { Text("#\(data.invocation)").monospacedDigit().foregroundStyle(.secondary) }
      }
      .font(font(10))
      .help(data.runText ?? st.label)
    }
  }

  /// Stage strip (or the plain status) of a code Card.
  @ViewBuilder private var statusRow: some View {
    if let p = data.stage, data.invocation > 0 {
      StageStrip(progress: p, zoom: zoom, onTap: onStageTap, caption: data.runText ?? "")
    } else {
      Text(data.idleText).font(font(10)).foregroundStyle(.secondary).lineLimit(1)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
  }

  // MARK: Start Card

  /// Its name and a small run button; the run's state beside the name.
  private func startBody(outline: CGRect) -> some View {
    HStack(spacing: s(6)) {
      Button { onRun?() } label: {
        Image(systemName: "play.fill").font(font(9, .bold)).foregroundStyle(.white)
          .frame(width: s(20), height: s(20))
          .background(Circle().fill(onRun == nil ? Color.secondary : Color.accentColor))
      }
      .buttonStyle(.plain)
      .disabled(onRun == nil)
      .help(String(localized: "ここから Workflow を実行"))
      Text(data.title).font(font(12, .semibold)).lineLimit(1).truncationMode(.tail)
      Spacer(minLength: 0)
      if let st = data.state, st != .notStarted { Image(systemName: st.symbol).font(font(10)).foregroundStyle(st.color) }
    }
    .padding(.horizontal, s(7))
    .frame(width: outline.width, height: outline.height)
  }

  // MARK: Routing Card (branch, fork, merge)

  /// Only the kind's icon, coloured by the state; name and details on hover.
  private func routingBody(_ g: CardGeometry, outline: CGRect) -> some View {
    let y = g.shape == .fork ? outline.minY + outline.height * 0.62 : g.shape == .merge ? outline.minY + outline.height * 0.38 : outline.midY
    return Image(systemName: data.kind.symbol)
      .font(font(10, .semibold))
      .foregroundStyle(data.state.map { $0 == .notStarted ? Color.secondary : $0.color } ?? .secondary)
      .position(x: outline.midX, y: y)
      .help(data.title + " · " + data.kind.label + (data.runText.map { " · " + $0 } ?? ""))
  }

  // MARK: Ports

  /// Port names centred on their ports; hidden when there is no room (the
  /// whole name and type then show on hover).
  @ViewBuilder private func portLabels(_ g: CardGeometry) -> some View {
    if flow == .topToBottom {
      if g.showLabels(input: true) { ForEach(data.inputs, id: \.name) { p in label(p, g, output: false) } }
      if g.showLabels(input: false) { ForEach(data.outputs, id: \.name) { p in label(p, g, output: true) } }
    } else if g.shape == .card, !g.collapsed {
      let w = CardGeometry.width / 2 - 14
      ForEach(data.inputs, id: \.name) { p in
        let pt = g.port(p.name, output: false)
        Text(p.name).font(font(10)).foregroundStyle(.secondary).lineLimit(1)
          .frame(width: s(w), alignment: .leading)
          .position(x: s(pt.x + 10 + w / 2), y: s(pt.y))
      }
      ForEach(data.outputs, id: \.name) { p in
        let pt = g.port(p.name, output: true)
        Text(p.name).font(font(10)).foregroundStyle(.secondary).lineLimit(1)
          .frame(width: s(w), alignment: .trailing)
          .position(x: s(pt.x - 10 - w / 2), y: s(pt.y))
      }
    }
  }

  private func label(_ p: PortData, _ g: CardGeometry, output: Bool) -> some View {
    let pt = g.port(p.name, output: output)
    let w = g.spacing(input: !output)
    var text = p.name
    if data.kind == .branch {
      // Which output is true / false, and which input is the boolean condition.
      if output { text = p.name == "a" ? "a (T)" : p.name == "b" ? "b (F)" : p.name }
      else if p.name == "condition" { text = "bool" }
    }
    let label = Text(text).font(font(9.5)).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
    if g.shape == .card {
      // Inside an ordinary Card: below its input, above its output.
      let half = CardGeometry.portRow / 2 + 0.5
      return AnyView(label.frame(width: s(w)).position(x: s(pt.x), y: s(pt.y + (output ? -half : half))).allowsHitTesting(false))
    }
    // Beside the port of a routing Card, so the wire (vertical at the port)
    // never runs through the name: right of it, or left on the left half.
    let left = pt.x < g.outlineRect.midX - 1 && (data.kind == .branch || g.spacing(input: !output) < 70)
    let lw: CGFloat = data.kind == .branch ? 40 : max(CardGeometry.minLabel, min(w, 80))
    let dy: CGFloat = output ? CardGeometry.portRow / 2 : -CardGeometry.portRow / 2
    let x = left ? pt.x - 6 - lw / 2 : pt.x + 6 + lw / 2
    return AnyView(label.frame(width: s(lw), alignment: left ? .trailing : .leading)
      .position(x: s(x), y: s(pt.y + dy)).allowsHitTesting(false))
  }

  /// Inputs are small squares, outputs small circles; dragging from either
  /// draws a wire to connect (it takes priority over moving the Card). The
  /// hit area is larger than the mark.
  private func portMarks(_ g: CardGeometry) -> some View {
    let mark = max(6, s(8))
    let hit = max(16, s(18))
    return ZStack(alignment: .topLeading) {
      ForEach(data.inputs, id: \.name) { p in
        let pt = g.port(p.name, output: false)
        // A branch's condition is a boolean: a small diamond instead of a square.
        let condition = data.kind == .branch && p.name == "condition"
        RoundedRectangle(cornerRadius: max(1, s(1.5)))
          .fill(condition ? Color.green.opacity(0.35) : Color(nsColor: .controlBackgroundColor))
          .overlay(RoundedRectangle(cornerRadius: max(1, s(1.5))).strokeBorder(condition ? Color.green : Color.secondary, lineWidth: 1.4))
          .frame(width: mark, height: mark)
          .rotationEffect(.degrees(condition ? 45 : 0))
          .frame(width: hit, height: hit)
          .contentShape(Rectangle())
          .position(x: s(pt.x), y: s(pt.y))
          .highPriorityGesture(DragGesture(minimumDistance: 1, coordinateSpace: .named("canvas"))
            .onChanged { onInputDrag?(p.name, $0.location) }
            .onEnded { onInputDrop?(p.name, $0.location) })
          .help(portHelp(p, output: false) + (onInputDrag != nil ? String(localized: "（ドラッグして出力ポートへ接続）") : ""))
      }
      ForEach(data.outputs, id: \.name) { p in
        let pt = g.port(p.name, output: true)
        Circle().fill(Color.accentColor)
          .frame(width: mark, height: mark)
          .frame(width: hit, height: hit)
          .contentShape(Circle())
          .position(x: s(pt.x), y: s(pt.y))
          .highPriorityGesture(DragGesture(minimumDistance: 1, coordinateSpace: .named("canvas"))
            .onChanged { onPortDrag?(p.name, $0.location) }
            .onEnded { onPortDrop?(p.name, $0.location) })
          .help(portHelp(p, output: true) + (onPortDrag != nil ? String(localized: "（ドラッグして入力ポートへ接続）") : ""))
      }
    }
  }

  private func portHelp(_ p: PortData, output: Bool) -> String {
    var s = (output ? String(localized: "出力 ") : String(localized: "入力 ")) + p.name
    if data.kind == .branch {
      if p.name == "condition" { s += String(localized: "（bool: true なら a、false なら b へ value を送る）") }
      if p.name == "a" { s += String(localized: "（condition が true のとき）") }
      if p.name == "b" { s += String(localized: "（condition が false のとき）") }
    }
    if let t = p.typeName { s += " : \(t)" }
    s += p.optional ? String(localized: "（省略可）") : String(localized: "（必須）")
    if let n = p.note, !n.isEmpty { s += "\n" + n }
    return s
  }

  private var borderColor: Color {
    if selected { return .accentColor }
    if let s = data.state, s != .notStarted { return s.color.opacity(0.7) }
    return .secondary.opacity(data.isDraft ? 0.7 : 0.4)
  }
}

/// Stages of the latest invocation in their order. Every stage keeps its own
/// icon; color and a small badge say whether it is pending, running, done,
/// cached, reused or failed. The running stage is also named in words.
struct StageStrip: View {
  let progress: StageProgress
  var zoom: Double = 1
  var onTap: ((Stage) -> Void)?
  var caption = ""

  private func s(_ v: CGFloat) -> CGFloat { v * CGFloat(zoom) }

  var body: some View {
    HStack(spacing: s(2)) {
      // The stages in order, joined by arrows: build → run → validate.
      ForEach(Array(progress.items.enumerated()), id: \.element.id) { i, item in
        if i > 0 {
          Image(systemName: "arrow.right").font(.system(size: max(4, s(6)), weight: .semibold))
            .foregroundStyle(.secondary.opacity(0.6))
            .accessibilityHidden(true)
        }
        Button { onTap?(item.stage) } label: { icon(item) }
          .buttonStyle(.plain)
          .disabled(onTap == nil)
          .help(help(item))
          .accessibilityLabel(item.stage.label + "：" + item.status.label)
      }
      if progress.workerReused {
        Image(systemName: "arrow.triangle.2.circlepath").font(.system(size: max(6, s(8)))).foregroundStyle(.teal)
          .help("前回の worker を再利用して実行しました")
      }
      Text(caption).font(.system(size: max(6, s(9.5)))).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
    .accessibilityElement(children: .contain)
  }

  private func color(_ st: StageProgress.Status) -> Color {
    switch st {
    case .pending, .notRun: return .secondary.opacity(0.45)
    case .running: return .orange
    case .done: return .green
    case .cached: return .blue
    case .reused: return .teal
    case .failed: return .red
    case .cancelled: return .brown
    }
  }

  private func badge(_ st: StageProgress.Status) -> String? {
    switch st {
    case .done: return "checkmark"
    case .cached: return "bolt.fill"
    case .reused: return "arrow.triangle.2.circlepath"
    case .failed: return "xmark"
    case .cancelled: return "stop.fill"
    case .pending, .notRun, .running: return nil
    }
  }

  private func icon(_ item: StageProgress.Item) -> some View {
    let c = color(item.status)
    return Image(systemName: item.status == .running ? item.stage.activeSymbol : item.stage.symbol)
      .font(.system(size: max(6, s(10))))
      .foregroundStyle(c)
      .frame(width: s(15), height: s(15))
      .background {
        if item.status == .running { Circle().fill(Color.orange.opacity(0.15)) }
      }
      .overlay(alignment: .bottomTrailing) {
        if let b = badge(item.status) {
          Image(systemName: b).font(.system(size: max(4, s(5)), weight: .heavy))
            .foregroundStyle(.white)
            .padding(max(0.5, s(1)))
            .background(Circle().fill(c))
            .offset(x: s(2), y: s(2))
        }
      }
  }

  private func help(_ item: StageProgress.Item) -> String {
    var t = item.stage.label + "：" + item.status.label
    if let sec = item.seconds { t += " · " + formatDuration(sec) }
    if item.status == .failed || item.status == .cancelled { t += String(localized: "（クリックでこの段階のログ）") }
    else if onTap != nil, item.status != .pending { t += String(localized: "（クリックでログ）") }
    return t
  }
}

/// A param shown on an input Card; committed on Return or focus loss.
struct ParamFieldView: View {
  let param: ParamData
  let editable: Bool
  var zoom: Double = 1
  let commit: (String) -> Void
  @State private var text = ""
  @State private var editing = false
  @FocusState private var focused: Bool

  var body: some View {
    let z = CGFloat(zoom)
    HStack(spacing: 6 * z) {
      Text(param.name).font(.system(size: max(6, 10 * z))).foregroundStyle(.secondary).lineLimit(1)
      if param.type == "bool" {
        Toggle("", isOn: Binding(get: { param.value == "true" }, set: { commit($0 ? "true" : "false") }))
          .toggleStyle(.switch).controlSize(.mini).labelsHidden()
          .disabled(!editable)
      } else {
        // The value is drawn as text at the zoom; the field (an AppKit text
        // field, which keeps its own layout) exists only while editing, so a
        // zoom never leaves the text out of place.
        Group {
          if editing {
            TextField("", text: $text)
              .textFieldStyle(.plain)
              .font(.system(size: max(6, 11 * z), design: .monospaced))
              .focused($focused)
              .onSubmit { finish() }
              .onChange(of: focused) { if !focused { finish() } }
              .onAppear { focused = true }
          } else {
            Text(text.isEmpty ? " " : text)
              .font(.system(size: max(6, 11 * z), design: .monospaced))
              .lineLimit(1)
              .frame(maxWidth: .infinity, alignment: .leading)
              .contentShape(Rectangle())
              .onTapGesture { if editable { editing = true } }
          }
        }
        .padding(.horizontal, 4 * z)
        .frame(height: 19 * z)
        .background(RoundedRectangle(cornerRadius: 4 * z).fill(Color(nsColor: .textBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 4 * z).strokeBorder(focused ? Color.accentColor : .secondary.opacity(0.35)))
        .help(editable ? "\(param.type)・クリックして編集、Return で反映" : String(localized: "実行中は変更できません"))
      }
    }
    .onAppear { text = param.value }
    .onChange(of: param.value) { if !editing { text = param.value } }
  }

  private func finish() {
    guard editing else { return }
    editing = false
    if text != param.value { commit(text) }
  }
}

// MARK: - Wires

/// A wire as drawn (screen units).
struct WireDraw {
  enum Style { case normal, carried, active, selected, display }
  var path: Path
  var style: Style
}

/// Routes wires in world units around the Cards. Forward wires (downstream)
/// are curves along the flow; wires going back upstream (feedback loops)
/// leave the graph sideways and run in their own lane outside it, so they
/// never cross Cards or each other. The same path draws and hit-tests a wire.
struct WireRouter {
  let flow: FlowDirection
  let obstacles: [CGRect]
  let bounds: CGRect
  private var lanes: [Bool: Int] = [:]

  init(flow: FlowDirection, obstacles: [CGRect]) {
    self.flow = flow
    self.obstacles = obstacles
    bounds = obstacles.reduce(CGRect.null) { $0.union($1) }
  }

  /// A forward wire goes along the flow by at least a little.
  func isForward(_ a: CGPoint, _ b: CGPoint) -> Bool {
    flow == .topToBottom ? b.y >= a.y + 24 : b.x >= a.x + 24
  }

  mutating func route(_ a: CGPoint, _ b: CGPoint) -> Path {
    if isForward(a, b) || bounds.isNull { return curve(a, b) }
    // Computed top-to-bottom; left-to-right swaps x and y.
    let swap = flow == .leftToRight
    func t(_ p: CGPoint) -> CGPoint { swap ? CGPoint(x: p.y, y: p.x) : p }
    func tr(_ r: CGRect) -> CGRect { swap ? CGRect(x: r.minY, y: r.minX, width: r.height, height: r.width) : r }
    let A = t(a), B = t(b)
    let box = tr(bounds)
    let obs = obstacles.map(tr)
    let left = (A.x + B.x) / 2 < box.midX
    let lane = lanes[left, default: 0]
    lanes[left] = lane + 1
    let laneX = left ? box.minX - 34 - CGFloat(lane) * 16 : box.maxX + 34 + CGFloat(lane) * 16
    let stagger = CGFloat(lane) * 5
    let y1 = freeY(from: A, toX: laneX, start: A.y + 16 + stagger, step: 9, obstacles: obs)
    let y2 = freeY(from: B, toX: laneX, start: B.y - 16 - stagger, step: -9, obstacles: obs)
    let pts = [A, CGPoint(x: A.x, y: y1), CGPoint(x: laneX, y: y1), CGPoint(x: laneX, y: y2), CGPoint(x: B.x, y: y2), B]
    return orthogonal(pts.map(t), radius: 10)
  }

  /// First y (from `start`, moving by `step`) where the run from the port
  /// to y and across to the lane crosses no obstacle.
  private func freeY(from p: CGPoint, toX laneX: CGFloat, start: CGFloat, step: CGFloat, obstacles: [CGRect]) -> CGFloat {
    var y = start
    for _ in 0..<40 {
      let horiz = CGRect(x: min(p.x, laneX), y: y - 3, width: abs(laneX - p.x), height: 6)
      let vert = CGRect(x: p.x - 2, y: min(p.y, y) + 6, width: 4, height: max(0, abs(y - p.y) - 12))
      let blocked = obstacles.contains { o in
        let r = o.insetBy(dx: -8, dy: -6)
        return r.intersects(horiz) || (vert.height > 0 && r.intersects(vert))
      }
      if !blocked { return y }
      y += step
    }
    return start
  }

  func curve(_ a: CGPoint, _ b: CGPoint) -> Path {
    var path = Path()
    path.move(to: a)
    switch flow {
    case .topToBottom:
      let dy = max(30, abs(b.y - a.y) / 2)
      path.addCurve(to: b, control1: CGPoint(x: a.x, y: a.y + dy), control2: CGPoint(x: b.x, y: b.y - dy))
    case .leftToRight:
      let dx = max(30, abs(b.x - a.x) / 2)
      path.addCurve(to: b, control1: CGPoint(x: a.x + dx, y: a.y), control2: CGPoint(x: b.x - dx, y: b.y))
    }
    return path
  }

  /// A polyline with rounded corners.
  func orthogonal(_ pts: [CGPoint], radius: CGFloat) -> Path {
    Path { p in
      guard let first = pts.first else { return }
      p.move(to: first)
      for i in 1..<pts.count {
        if i == pts.count - 1 { p.addLine(to: pts[i]); continue }
        let a = pts[i - 1], c = pts[i], n = pts[i + 1]
        let r = min(radius, hypot(c.x - a.x, c.y - a.y) / 2, hypot(n.x - c.x, n.y - c.y) / 2)
        p.addArc(tangent1End: c, tangent2End: n, radius: max(0.1, r))
      }
    }
  }
}

/// Wires (screen paths). Carried ones are colored; active ones are animated.
struct WireLayer: View {
  let wires: [WireDraw]
  var animate = false
  var zoom: Double = 1

  /// The active wires' dashes, and how fast they flow (points per second).
  static let dash: [CGFloat] = [8, 6]
  static let flow: Double = 30

  var body: some View {
    let anyActive = animate && wires.contains { $0.style == .active }
    // At the display's frame rate, the phase wrapping at exactly one dash
    // period, so the dashes flow without a jump.
    SwiftUI.TimelineView(.animation(paused: !anyActive)) { t in
      Canvas { ctx, _ in
        let period = Self.dash.reduce(0, +)
        let phase = anyActive
          ? -CGFloat((t.date.timeIntervalSinceReferenceDate * Self.flow).truncatingRemainder(dividingBy: Double(period))) : 0
        let w = max(1, 1.6 * min(1.4, zoom))
        for wire in wires {
          let round = StrokeStyle(lineWidth: w, lineCap: .round, lineJoin: .round)
          switch wire.style {
          case .normal:
            ctx.stroke(wire.path, with: .color(.secondary.opacity(0.75)), style: round)
          case .carried:
            ctx.stroke(wire.path, with: .color(.orange.opacity(0.8)), style: StrokeStyle(lineWidth: w + 0.4, lineCap: .round, lineJoin: .round))
          case .active:
            ctx.stroke(wire.path, with: .color(.orange), style: StrokeStyle(lineWidth: w + 0.6, lineCap: .round, lineJoin: .round,
                                                                             dash: anyActive ? Self.dash : [], dashPhase: phase))
          case .selected:
            ctx.stroke(wire.path, with: .color(.accentColor), style: StrokeStyle(lineWidth: w + 1.4, lineCap: .round, lineJoin: .round))
          case .display:
            ctx.stroke(wire.path, with: .color(.purple.opacity(0.75)), style: StrokeStyle(lineWidth: max(1, w - 0.2), lineCap: .round, dash: [5, 4]))
          }
        }
      }
    }
    .allowsHitTesting(false)
  }
}

/// Path (world units) from a Card's display anchor to its output frame.
/// The ends of the link between a Card and its result frame: on the edges
/// that face each other (where the two are closest), each in the middle of
/// the stretch the two share, or toward the other when they share none.
/// Returns the points and whether the link runs across (left/right).
func displayLinkEnds(card: CGRect, frame: CGRect) -> (a: CGPoint, b: CGPoint, horizontal: Bool) {
  let gx = max(frame.minX - card.maxX, card.minX - frame.maxX)
  let gy = max(frame.minY - card.maxY, card.minY - frame.maxY)
  func clamp(_ v: CGFloat, _ lo: CGFloat, _ hi: CGFloat) -> CGFloat { lo > hi ? (lo + hi) / 2 : min(max(v, lo), hi) }
  if gx >= gy {
    let right = frame.midX >= card.midX
    let lo = max(card.minY, frame.minY), hi = min(card.maxY, frame.maxY)
    let ay = lo <= hi ? (lo + hi) / 2 : clamp(frame.midY, card.minY + 10, card.maxY - 10)
    let by = lo <= hi ? (lo + hi) / 2 : clamp(card.midY, frame.minY + 10, frame.maxY - 10)
    return (CGPoint(x: right ? card.maxX : card.minX, y: ay), CGPoint(x: right ? frame.minX : frame.maxX, y: by), true)
  }
  let down = frame.midY >= card.midY
  let lo = max(card.minX, frame.minX), hi = min(card.maxX, frame.maxX)
  let ax = lo <= hi ? (lo + hi) / 2 : clamp(frame.midX, card.minX + 10, card.maxX - 10)
  let bx = lo <= hi ? (lo + hi) / 2 : clamp(card.midX, frame.minX + 10, frame.maxX - 10)
  return (CGPoint(x: ax, y: down ? card.maxY : card.minY), CGPoint(x: bx, y: down ? frame.minY : frame.maxY), false)
}

/// The link itself: a short curve leaving and entering straight.
func displayLink(card: CGRect, frame: CGRect) -> Path {
  let (a, b, horizontal) = displayLinkEnds(card: card, frame: frame)
  var p = Path()
  p.move(to: a)
  if horizontal {
    let dx = (b.x - a.x) / 2
    p.addCurve(to: b, control1: CGPoint(x: a.x + dx, y: a.y), control2: CGPoint(x: b.x - dx, y: b.y))
  } else {
    let dy = (b.y - a.y) / 2
    p.addCurve(to: b, control1: CGPoint(x: a.x, y: a.y + dy), control2: CGPoint(x: b.x, y: b.y - dy))
  }
  return p
}

// MARK: - Self-contained canvas (previews)

struct CanvasItem: Identifiable, Equatable {
  var id: String
  var data: CardNodeData
  var origin: CGPoint
}

struct CanvasWire: Equatable {
  var from: String
  var fromPort: String
  var to: String
  var toPort: String
  var active = false
}

/// A small canvas with the main canvas' Card view, wires, selection, drag,
/// collapse and zoom, over local state only (it never edits a project).
/// It frames its Cards once they are there and again when `fitKey` or its
/// size changes, until the user moves or zooms it.
struct CanvasSurface: View {
  @Binding var items: [CanvasItem]
  var wires: [CanvasWire]
  var flow: FlowDirection
  var grid = true
  var gridSize: Double = 20
  var animateWires = false
  var fitKey: String = ""
  var maxZoom: Double = 1
  @Environment(\.displayScale) private var displayScale
  @State private var camera = CanvasCamera()
  @State private var selection: Set<String> = []
  @State private var drag: (id: String, offset: CGSize)?
  @State private var zoomStart: Double?
  @State private var userAdjusted = false
  @State private var lastPan: CGSize?

  var body: some View {
    GeometryReader { geo in
      ZStack(alignment: .topLeading) {
        CanvasGrid(camera: camera, show: grid, size: gridSize)
          .contentShape(Rectangle())
          .onTapGesture { selection = [] }
          .gesture(DragGesture(minimumDistance: 2).onChanged { v in
            userAdjusted = true
            camera.x += v.translation.width - (lastPan?.width ?? 0)
            camera.y += v.translation.height - (lastPan?.height ?? 0)
            lastPan = v.translation
          }.onEnded { _ in lastPan = nil })
        WireLayer(wires: wireDraws, animate: animateWires, zoom: camera.zoom)
        ForEach(items) { item in
          CardNodeView(data: item.data, flow: flow, zoom: camera.zoom, selected: selection.contains(item.id),
                       onToggleCollapse: { toggle(item.id) })
            .placeScaled(at: camera.screen(origin(item)), size: CardGeometry(item.data, flow: flow).size,
                         zoom: camera.zoom, pixel: displayScale)
            .gesture(DragGesture(minimumDistance: 2, coordinateSpace: .named("canvas"))
              .onChanged { v in
                userAdjusted = true
                if !selection.contains(item.id) { selection = [item.id] }
                drag = (item.id, v.translation)
              }
              .onEnded { v in
                for i in items.indices where selection.contains(items[i].id) {
                  items[i].origin.x += v.translation.width / camera.zoom
                  items[i].origin.y += v.translation.height / camera.zoom
                }
                drag = nil
              })
            .onTapGesture { selection = NSEvent.modifierFlags.contains(.shift) ? selection.symmetricDifference([item.id]) : [item.id] }
        }
        HStack(spacing: 4) {
          Button { userAdjusted = true; camera.zoom(to: camera.zoom / 1.2, around: CGPoint(x: geo.size.width / 2, y: geo.size.height / 2)) } label: {
            Image(systemName: "minus.magnifyingglass")
          }
          .help("縮小")
          Text("\(Int((camera.zoom * 100).rounded()))%").font(.caption.monospacedDigit()).frame(minWidth: 36)
          Button { userAdjusted = true; camera.zoom(to: camera.zoom * 1.2, around: CGPoint(x: geo.size.width / 2, y: geo.size.height / 2)) } label: {
            Image(systemName: "plus.magnifyingglass")
          }
          .help("拡大")
          Button { userAdjusted = false; fit(geo.size) } label: { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }
            .help("全体表示")
        }
        .buttonStyle(.borderless)
        .controlSize(.small)
        .padding(3)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 6))
        .padding(6)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
      }
      .coordinateSpace(name: "canvas")
      .clipped()
      .gesture(MagnifyGesture().onChanged { v in
        userAdjusted = true
        if zoomStart == nil { zoomStart = camera.zoom }
        camera.zoom(to: (zoomStart ?? 1) * v.magnification, around: CGPoint(x: geo.size.width / 2, y: geo.size.height / 2))
      }.onEnded { _ in zoomStart = nil })
      // Framing waits for the Cards; later content updates do not re-centre
      // a view the user moved or zoomed.
      .onAppear { fit(geo.size) }
      .onChange(of: itemsBox) { if !userAdjusted { fit(geo.size) } }
      .onChange(of: geo.size) { if !userAdjusted { fit(geo.size) } }
      .onChange(of: fitKey) { userAdjusted = false; fit(geo.size) }
      .onChange(of: flow) { userAdjusted = false; fit(geo.size) }
    }
  }

  /// Bounding box of the Cards (re-framed when a Card appears or changes size).
  private var itemsBox: CGRect {
    items.reduce(CGRect.null) { $0.union(CGRect(origin: $1.origin, size: CardGeometry($1.data, flow: flow).size)) }
  }

  private var wireDraws: [WireDraw] {
    var router = WireRouter(flow: flow, obstacles: items.map { CGRect(origin: origin($0), size: CardGeometry($0.data, flow: flow).size) })
    return wires.map { w in
      let world = router.route(point(w.from, w.fromPort, output: true), point(w.to, w.toPort, output: false))
      return WireDraw(path: world.applying(camera.transform), style: w.active ? .active : .normal)
    }
  }

  private func origin(_ item: CanvasItem) -> CGPoint {
    var o = item.origin
    if let d = drag, selection.contains(item.id) {
      o.x += d.offset.width / camera.zoom
      o.y += d.offset.height / camera.zoom
    }
    return o
  }

  /// A port in world units.
  private func point(_ id: String, _ port: String, output: Bool) -> CGPoint {
    guard let item = items.first(where: { $0.id == id }) else { return .zero }
    let o = origin(item), p = CardGeometry(item.data, flow: flow).port(port, output: output)
    return CGPoint(x: o.x + p.x, y: o.y + p.y)
  }

  private func toggle(_ id: String) {
    guard let i = items.firstIndex(where: { $0.id == id }) else { return }
    items[i].data.collapsed.toggle()
  }

  private func fit(_ size: CGSize) {
    guard !items.isEmpty, size.width > 0 else { return }
    camera.fit(itemsBox, in: size, insets: EdgeInsets(top: 4, leading: 4, bottom: 30, trailing: 4), maxZoom: maxZoom, margin: 24)
  }
}

/// Dot grid behind a canvas.
struct CanvasGrid: View {
  let camera: CanvasCamera
  let show: Bool
  let size: Double

  var body: some View {
    Canvas { ctx, sz in
      guard show else { return }
      let step = size * camera.zoom
      guard step >= 6 else { return }
      var x = camera.x.truncatingRemainder(dividingBy: step)
      let dot = Path(ellipseIn: CGRect(x: 0, y: 0, width: 1.6, height: 1.6))
      while x < sz.width {
        var y = camera.y.truncatingRemainder(dividingBy: step)
        while y < sz.height {
          ctx.fill(dot.offsetBy(dx: x, dy: y), with: .color(.secondary.opacity(0.35)))
          y += step
        }
        x += step
      }
    }
    .background(Color(nsColor: .textBackgroundColor))
  }
}

/// Sample Cards for previews of appearance settings.
enum PreviewSamples {
  static func items() -> [CanvasItem] {
    func port(_ n: String) -> PortData { PortData(name: n, typeName: "t.Number") }
    let input = CardNodeData(title: String(localized: "初期値"), kind: .input, summary: nil,
                             inputs: [port("start")], outputs: [port("value")],
                             params: [ParamData(name: "value", type: "uint64", value: "27")],
                             state: .succeeded, invocation: 1)
    var step = CardNodeData(title: String(localized: "n / 2"), kind: .code,
                            summary: String(localized: "偶数を半分にする"), inputs: [port("number")], outputs: [port("next")],
                            state: .running, invocation: 3, runContinues: true)
    step.stage = StageProgress(items: [
      .init(stage: .build, status: .reused), .init(stage: .execute, status: .running), .init(stage: .validate, status: .pending),
    ], workerReused: true)
    let gate = CardNodeData(title: String(localized: "偶数？"), kind: .branch, summary: nil,
                            inputs: [port("value"), port("condition")], outputs: [port("a"), port("b")])
    let out = CardNodeData(title: String(localized: "結果"), kind: .logger, summary: nil,
                           inputs: [port("in")], outputs: [], result: "[27, 82, 41, …]", state: .succeeded, invocation: 1)
    return [
      CanvasItem(id: "input", data: input, origin: CGPoint(x: 0, y: 0)),
      CanvasItem(id: "step", data: step, origin: CGPoint(x: 0, y: 150)),
      CanvasItem(id: "gate", data: gate, origin: CGPoint(x: 26, y: 300)),
      CanvasItem(id: "out", data: out, origin: CGPoint(x: 0, y: 460)),
    ]
  }

  static let wires = [
    CanvasWire(from: "input", fromPort: "value", to: "step", toPort: "number", active: true),
    CanvasWire(from: "step", fromPort: "next", to: "gate", toPort: "value"),
    CanvasWire(from: "gate", fromPort: "a", to: "out", toPort: "in"),
    CanvasWire(from: "gate", fromPort: "b", to: "step", toPort: "number"),
  ]
}
