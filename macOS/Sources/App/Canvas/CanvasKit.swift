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
  /// Short description (one line on the Card, a Draft's content in a few;
  /// the whole text on hover).
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
  /// What it may do beyond computing (network, no sandbox, Mac folders).
  var permissions: [CardPermission] = []
  /// A Draft being written on the canvas: the Card opens up for it.
  var draftEditing = false
  /// A Draft without a name of its own: its content is all it shows.
  var untitled = false

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
  /// A Draft's content on the Card (three lines), and while it is being written.
  static let draftRow: CGFloat = 50
  static let draftEditRow: CGFloat = 120
  /// What is left of the header over the content of a Draft without a name.
  static let untitledTop: CGFloat = 8
  /// What a wire names to end at a Draft's next port (`connect` makes the port).
  static let newPort = "+"
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
  let draft: Bool
  let draftEditing: Bool
  let untitled: Bool

  init(_ d: CardNodeData, flow: FlowDirection) {
    inputs = d.inputs.map(\.name)
    outputs = d.outputs.map(\.name)
    collapsed = d.collapsed
    draft = d.isDraft
    draftEditing = d.isDraft && d.draftEditing
    untitled = d.isDraft && d.untitled && !d.draftEditing && !d.collapsed
    summary = d.summary != nil && !d.isRouting && !d.isDraft
    params = d.params.count
    // Rows are reserved by kind, so a run never makes a Card grow or shrink.
    result = d.kind == .logger || d.hasDisplay
    stages = d.kind == .code
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
    // A Draft has no port names to give a row: its ports are at the middle of its sides.
    guard flow == .leftToRight, shape == .card, !draft else { return 0 }
    return CGFloat(max(inputs.count, outputs.count, 1)) * Self.portRow
  }
  /// The header, or what is left of it over the content of a Draft without a name.
  var headerRow: CGFloat { untitled ? Self.untitledTop : Self.header }
  /// The room for a Draft's content under its header.
  var draftRow: CGFloat { !draft ? 0 : draftEditing ? Self.draftEditRow : Self.draftRow }
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
    let h = topLabels + headerRow + (summary ? Self.summaryRow : 0) + draftRow + portBand + CGFloat(params) * Self.paramRow +
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
    // A Draft's ports come from its wires: their names say nothing yet (shown on hover).
    if draft { return false }
    if shape == .card && collapsed { return false }
    if shape == .start { return false }
    // Routing marks name their ports on hover; a branch marks only T / F.
    if shape == .fork || shape == .merge { return false }
    if shape == .branch { return true }
    return spacing(input: input) >= Self.minLabel
  }

  /// Where a wire starts from a side of a Draft that has no port yet (the
  /// middle of the side, where the port will be). A side with ports starts from them.
  func newPortPoint(output: Bool) -> CGPoint? {
    guard draft, !collapsed, (output ? outputs : inputs).isEmpty else { return nil }
    let r = outlineRect
    return flow == .topToBottom ? CGPoint(x: Self.width / 2, y: output ? r.maxY : r.minY)
                                : CGPoint(x: output ? Self.width : 0, y: r.midY)
  }

  /// A Draft's ports are around the middle of its side, closer together when there are many.
  private func draftPortY(_ index: Int, of count: Int) -> CGFloat {
    let r = outlineRect
    let pitch = min(Self.portRow, (r.height - 8) / CGFloat(count + 1))
    return r.midY + (CGFloat(index) - CGFloat(count - 1) / 2) * pitch
  }

  func port(_ name: String, output: Bool) -> CGPoint {
    if name == Self.newPort, let p = newPortPoint(output: output) { return p }
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
      if draft { return CGPoint(x: x, y: draftPortY(index, of: list.count)) }
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

/// Writing a Draft on its Card: what is typed so far, where it goes, and how it ends.
struct DraftEdit {
  let name: Binding<String>
  let content: Binding<String>
  /// Why the Card cannot take the name typed so far.
  let nameProblem: String?
  /// Done, the focus having moved elsewhere (false) or by the keyboard (true).
  let onDone: (_ byKey: Bool) -> Void
}

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
  var draftEdit: DraftEdit?
  private enum DraftField { case name, content }
  @FocusState private var draftField: DraftField?

  private var edit: DraftEdit? { data.draftEditing ? draftEdit : nil }
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
      if onPortDrag != nil, paramsEditable, let pt = g.newPortPoint(output: output) {
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
      if g.untitled { Color.clear.frame(height: s(g.headerRow)) } else { header.frame(height: s(CardGeometry.header)) }
      if !g.collapsed {
        if g.draft { draftBody(g) }
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
    // Done when the focus has left both of a Draft's fields (not on the way
    // from one to the other).
    .onChange(of: draftField) { _, field in
      guard field == nil, edit != nil else { return }
      Task { @MainActor in
        await Task.yield()
        if draftField == nil { edit?.onDone(false) }
      }
    }
    // The content is what is written first: a name is not needed.
    .task(id: edit != nil) { if edit != nil { draftField = .content } }
  }

  /// A Draft's content, written on the Card while it is being edited.
  @ViewBuilder private func draftBody(_ g: CardGeometry) -> some View {
    if let edit {
      // No box: the text stays where it is on the Card (the editor insets its text by 5).
      VStack(alignment: .leading, spacing: s(2)) {
        if let problem = edit.nameProblem {
          Text(problem).font(font(9.5)).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, 5)
        }
        ZStack(alignment: .topLeading) {
          if edit.content.wrappedValue.isEmpty {
            Text("何を受け取り、何を出し、どう処理するかを書きます").font(font(11)).foregroundStyle(.tertiary)
              .padding(.horizontal, 5).allowsHitTesting(false)
          }
          TextEditor(text: edit.content)
            .font(font(11)).foregroundStyle(.secondary).scrollContentBackground(.hidden)
            // A scroll bar only when the text does not fit.
            .scrollIndicators(draftTextFits(edit.content.wrappedValue, g) ? .never : .automatic)
            .focused($draftField, equals: .content)
            .onExitCommand { edit.onDone(true) }
            .onKeyPress(.return, phases: .down) { press in
              guard press.modifiers.contains(.command) else { return .ignored }
              edit.onDone(true)
              return .handled
            }
            .accessibilityLabel("Draft の内容")
        }
      }
      .padding(.horizontal, max(0, s(10) - 5)).padding(.bottom, s(8))
      .frame(height: s(g.draftRow), alignment: .top)
    } else {
      Group { if let text = data.summary { Text(verbatim: text) } else { Text("ダブルクリックで内容を書く") } }
        .font(font(11)).foregroundStyle(.secondary).opacity(data.summary == nil ? 0.6 : 1)
        .lineLimit(3).truncationMode(.tail)
        .frame(maxWidth: .infinity, alignment: .topLeading)
        .padding(.horizontal, s(10))
        .frame(height: s(g.draftRow), alignment: .top)
    }
  }

  /// Whether a Draft's content fits in the room it has while it is being written.
  private func draftTextFits(_ text: String, _ g: CardGeometry) -> Bool {
    let width = s(CardGeometry.width - 20)
    let box = (text as NSString).boundingRect(with: CGSize(width: width, height: .greatestFiniteMagnitude),
                                              options: [.usesLineFragmentOrigin, .usesFontLeading],
                                              attributes: [.font: NSFont.systemFont(ofSize: max(6, 11 * z))])
    return box.height <= s(g.draftRow - 8)
  }

  private var header: some View {
    HStack(spacing: s(6)) {
      Image(systemName: data.symbol ?? data.kind.symbol).font(font(12)).foregroundStyle(.secondary).frame(width: s(15))
      if let edit {
        TextField("名前", text: edit.name)
          .textFieldStyle(.plain).font(font(12, .semibold))
          .focused($draftField, equals: .name)
          .onSubmit { if edit.nameProblem == nil { draftField = .content } }
          .onExitCommand { edit.onDone(true) }
          .accessibilityLabel("Draft の名前")
      } else {
        Text(verbatim: data.untitled ? data.summary?.split(separator: "\n").first.map(String.init) ?? data.title : data.title)
          .font(font(12, .semibold)).lineLimit(1).truncationMode(.tail)
      }
      if edit == nil { Spacer(minLength: s(2)) }
      ForEach(data.permissions) { p in
        Image(systemName: p.symbol).font(font(10)).foregroundStyle(p == .sandboxOff ? .orange : .secondary).help(p.label)
      }
      if !data.errors.isEmpty {
        Image(systemName: "xmark.octagon.fill").font(font(11)).foregroundStyle(.red)
      } else if !data.warnings.isEmpty {
        Image(systemName: "exclamationmark.triangle.fill").font(font(11)).foregroundStyle(.yellow)
      }
      stateBadge
      if let toggle = onToggleCollapse, data.kind != .trigger, edit == nil {
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
    } else if g.shape == .card, !g.collapsed, !g.draft {
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
      if onPortDrag != nil, paramsEditable {
        newPortMark(g, output: false)
        newPortMark(g, output: true)
      }
    }
  }

  /// Dragging from here draws a wire that makes a Draft's first port on this
  /// side (nothing is drawn until the pointer is over it).
  @ViewBuilder private func newPortMark(_ g: CardGeometry, output: Bool) -> some View {
    if let pt = g.newPortPoint(output: output) {
      NewPortMark(mark: max(6, s(8)), hit: max(16, s(18)))
        .position(x: s(pt.x), y: s(pt.y))
        .highPriorityGesture(DragGesture(minimumDistance: 1, coordinateSpace: .named("canvas"))
          .onChanged { (output ? onPortDrag : onInputDrag)?(CardGeometry.newPort, $0.location) }
          .onEnded { (output ? onPortDrop : onInputDrop)?(CardGeometry.newPort, $0.location) })
        .help(output ? "ドラッグして次の Card へ接続（出力は線に合わせて作られます）" : "ドラッグして前の Card から接続（入力は線に合わせて作られます）")
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

/// A place to start a wire from: shows only under the pointer.
private struct NewPortMark: View {
  let mark: CGFloat
  let hit: CGFloat
  @State private var over = false

  var body: some View {
    Circle().fill(Color.accentColor).opacity(over ? 1 : 0)
      .frame(width: mark, height: mark)
      .frame(width: hit, height: hit)
      .contentShape(Circle())
      .onHover { over = $0 }
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
  /// The connection, when the Agent may be threading it (drawn by the
  /// presence layer meanwhile).
  var thread: AgentPresence.Wire? = nil
}

/// Where a searched wire runs (world units) and what the search pays for.
private enum WireCost {
  /// Distance kept from Cards, and the length of the run straight out of a port.
  static let clearance: CGFloat = 20
  /// Distance between wires that would otherwise share a line.
  static let lane: CGFloat = 12
  /// One bend is worth this much length, so a path does not become a staircase.
  static let turn: CGFloat = 30
  /// One crossing of another wire is worth this much length.
  static let crossing: CGFloat = 80
  /// Extra cost per unit along another wire, and per unit closer to a Card than `clearance`.
  static let overlap: CGFloat = 3
  static let near: CGFloat = 0.25
  /// How deep a routing Card's box holds its own port. A port deeper than this in
  /// a box is under a Card laid over it, which the wire cannot get out of along the flow.
  static let portDepth: CGFloat = 44
}

/// Routes wires in world units around the Cards. A wire going along the flow
/// whose curve touches no Card stays that curve. Any other wire takes the
/// cheapest path on a grid of lines beside the Cards: by length, bends and
/// crossings of other wires, leaving and entering its ports along the flow.
/// The same path draws and hit-tests a wire. The same algorithm as `WireRouter`
/// in web/src/model/canvasKit.ts: tests/fixtures/wire-routes.json holds both to it.
struct WireRouter {
  private struct Routed {
    var a: CGPoint
    var pts: [CGPoint]
    var box: CGRect
    var curved: Bool
  }
  private struct Near {
    var cards: [CGRect]
    var wires: [Routed]
  }

  let flow: FlowDirection
  let obstacles: [CGRect]
  /// The Cards as the search sees them: computed top-to-bottom (left-to-right swaps x and y).
  private let frame: [CGRect]
  private var cardLines: (xs: [CGFloat], ys: [CGFloat])?
  /// The wires routed so far, for the search to keep off them (in the search's frame).
  private var routed: [Routed] = []

  init(flow: FlowDirection, obstacles: [CGRect]) {
    self.flow = flow
    self.obstacles = obstacles
    frame = flow == .leftToRight ? obstacles.map { CGRect(x: $0.minY, y: $0.minX, width: $0.height, height: $0.width) } : obstacles
  }

  private func toFrame(_ p: CGPoint) -> CGPoint {
    flow == .leftToRight ? CGPoint(x: p.y, y: p.x) : p
  }

  /// A wire that goes along the flow, however little, and crosses no Card on the short way.
  func isForward(_ a: CGPoint, _ b: CGPoint) -> Bool {
    let along = flow == .topToBottom ? b.y - a.y : b.x - a.x
    return along >= 0 && !crossesCard(a, b)
  }

  /// The cheap first check: most wires end here, and only Cards near the curve are looked at.
  private func crossesCard(_ a: CGPoint, _ b: CGPoint) -> Bool {
    let (c1, c2) = controls(a, b)
    let x0 = min(a.x, b.x, c1.x, c2.x), x1 = max(a.x, b.x, c1.x, c2.x)
    let y0 = min(a.y, b.y, c1.y, c2.y), y1 = max(a.y, b.y, c1.y, c2.y)
    // A port sits on its Card's edge: only the inside counts. A branch's port is
    // inside its box, and the wire has to get out of there anyway.
    func inside(_ o: CGRect, _ p: CGPoint) -> Bool { p.x > o.minX + 2 && p.x < o.maxX - 2 && p.y > o.minY + 2 && p.y < o.maxY - 2 }
    let near = obstacles.filter { x0 < $0.maxX && x1 > $0.minX && y0 < $0.maxY && y1 > $0.minY && !inside($0, a) && !inside($0, b) }
    if near.isEmpty { return false }
    var p = a
    for i in 1...32 {
      let q = Self.bezier(a, c1, c2, b, CGFloat(i) / 32)
      if near.contains(where: { Self.segmentEnters(p, q, $0) }) { return true }
      p = q
    }
    return false
  }

  /// Cards laid over each other, each covering the other's port: the wire is under them.
  private func underCards(_ a: CGPoint, _ b: CGPoint) -> Bool {
    func inside(_ o: CGRect, _ p: CGPoint) -> Bool { p.x > o.minX && p.x < o.maxX && p.y > o.minY && p.y < o.maxY }
    func touches(_ o: CGRect, _ p: CGPoint) -> Bool { p.x >= o.minX && p.x <= o.maxX && p.y >= o.minY && p.y <= o.maxY }
    return obstacles.contains { inside($0, a) && touches($0, b) } && obstacles.contains { inside($0, b) && touches($0, a) }
  }

  /// Leaves and enters along the flow: by half the distance, and by more only
  /// as far as the ports are apart across it, so close ports are joined straight.
  private func controls(_ a: CGPoint, _ b: CGPoint) -> (CGPoint, CGPoint) {
    switch flow {
    case .topToBottom:
      let dy = max(abs(b.y - a.y) / 2, min(30, abs(b.x - a.x) / 2))
      return (CGPoint(x: a.x, y: a.y + dy), CGPoint(x: b.x, y: b.y - dy))
    case .leftToRight:
      let dx = max(abs(b.x - a.x) / 2, min(30, abs(b.y - a.y) / 2))
      return (CGPoint(x: a.x + dx, y: a.y), CGPoint(x: b.x - dx, y: b.y))
    }
  }

  /// One wire, keeping off those already routed.
  mutating func route(_ a: CGPoint, _ b: CGPoint) -> Path {
    routeAll([(a: a, b: b)])[0]
  }

  /// The wires of a frame, in the order given.
  mutating func routeAll(_ wires: [(a: CGPoint, b: CGPoint)]) -> [Path] {
    let corners = plan(wires)
    return wires.indices.map { i in corners[i].map { orthogonal($0, radius: 10) } ?? curve(wires[i].a, wires[i].b) }
  }

  /// The corners of each wire's path (nil: it is a curve). Curves depend on the
  /// Cards alone; the rest go shortest first, in an order of their own, so that
  /// the order of the connections changes nothing and long wires settle around short ones.
  mutating func plan(_ wires: [(a: CGPoint, b: CGPoint)]) -> [[CGPoint]?] {
    var out = [[CGPoint]?](repeating: nil, count: wires.count)
    var searched: [Int] = []
    for (i, w) in wires.enumerated() {
      if isForward(w.a, w.b) || underCards(w.a, w.b) {
        // Enough of a curve to tell whether something crosses it.
        let (c1, c2) = controls(w.a, w.b)
        remember(w.a, stride(from: 0, through: 32, by: 4).map { toFrame(Self.bezier(w.a, c1, c2, w.b, CGFloat($0) / 32)) }, curved: true)
      } else {
        searched.append(i)
      }
    }
    func span(_ w: (a: CGPoint, b: CGPoint)) -> CGFloat { abs(w.b.x - w.a.x) + abs(w.b.y - w.a.y) }
    searched.sort { i, j in
      let u = wires[i], v = wires[j]
      return (span(u), u.a.x, u.a.y, u.b.x, u.b.y, i) < (span(v), v.a.x, v.a.y, v.b.x, v.b.y, j)
    }
    for i in searched {
      // Shut in by Cards touching all around, nothing is free to take: it stays a curve.
      guard let pts = search(toFrame(wires[i].a), toFrame(wires[i].b)) else { continue }
      remember(wires[i].a, pts, curved: false)
      out[i] = pts.map(toFrame)
    }
    return out
  }

  private mutating func remember(_ a: CGPoint, _ pts: [CGPoint], curved: Bool) {
    let xs = pts.map(\.x), ys = pts.map(\.y)
    let x = xs.min() ?? 0, y = ys.min() ?? 0
    routed.append(Routed(a: a, pts: pts, box: CGRect(x: x, y: y, width: (xs.max() ?? 0) - x, height: (ys.max() ?? 0) - y), curved: curved))
  }

  /// How far along the flow a port's wire has to run to get out of a Card's box (0: not in it).
  private func depth(_ o: CGRect, _ p: CGPoint, _ dir: CGFloat) -> CGFloat {
    if p.x <= o.minX || p.x >= o.maxX { return 0 }
    if dir > 0 { return o.minY <= p.y && p.y < o.maxY ? o.maxY - p.y : 0 }
    return o.minY < p.y && p.y <= o.maxY ? p.y - o.minY : 0
  }

  /// Where a port's wire is out of the boxes it starts in (a Card's port is on the edge already).
  private func clear(_ p: CGPoint, _ dir: CGFloat, _ cards: [CGRect]) -> CGPoint {
    var y = p.y
    var moved = true
    while moved {
      moved = false
      for o in cards {
        let d = depth(o, CGPoint(x: p.x, y: y), dir)
        if d > 0 {
          y += dir * d
          moved = true
        }
      }
    }
    return CGPoint(x: p.x, y: y)
  }

  /// The run straight out of a port: shorter where the next Card or the other port is close.
  private func stub(_ p: CGPoint, _ dir: CGFloat, along: CGFloat, _ cards: [CGRect]) -> CGFloat {
    var s = along > 0 ? min(WireCost.clearance, along / 2) : WireCost.clearance
    for o in cards {
      if p.x <= o.minX || p.x >= o.maxX { continue }
      let d = dir > 0 ? o.minY - p.y : p.y - o.minY - o.height
      if d >= 0 { s = min(s, d / 2) }
    }
    return s
  }

  /// The lines beside the Cards, and through gaps too narrow to keep clear of both sides.
  private func linesOfCards() -> (xs: [CGFloat], ys: [CGFloat]) {
    func gaps(_ edges: [CGFloat]) -> [CGFloat] {
      let sorted = edges.sorted()
      return sorted.indices.dropFirst().compactMap { i in
        sorted[i] > sorted[i - 1] && sorted[i] - sorted[i - 1] < 2 * WireCost.clearance ? (sorted[i] + sorted[i - 1]) / 2 : nil
      }
    }
    let c = WireCost.clearance
    return (frame.flatMap { [$0.minX - c, $0.minX + $0.width + c] } + gaps(frame.flatMap { [$0.minX, $0.minX + $0.width] }),
            frame.flatMap { [$0.minY - c, $0.minY + $0.height + c] } + gaps(frame.flatMap { [$0.minY, $0.minY + $0.height] }))
  }

  /// The cheapest path from port to port (A* over grid node and direction of travel),
  /// in the search's frame. Ties go to the lowest state, so nothing depends on chance.
  private mutating func search(_ a: CGPoint, _ b: CGPoint) -> [CGPoint]? {
    if cardLines == nil { cardLines = linesOfCards() }
    return search(a, b, lines: cardLines!)
  }

  private func search(_ a: CGPoint, _ b: CGPoint, lines: (xs: [CGFloat], ys: [CGFloat])) -> [CGPoint]? {
    // A Card laid over a port is no wall for its wire: the wire runs under it.
    let cards = frame.filter { depth($0, a, 1) <= WireCost.portDepth && depth($0, b, -1) <= WireCost.portDepth }
    let out = clear(a, 1, cards), into = clear(b, -1, cards)
    let along = into.y - out.y
    let first = CGPoint(x: out.x, y: out.y + stub(out, 1, along: along, cards))
    let last = CGPoint(x: into.x, y: into.y - stub(into, -1, along: along, cards))

    var xLines = lines.xs + [first.x, last.x, (first.x + last.x) / 2]
    var yLines = lines.ys + [first.y, last.y, (first.y + last.y) / 2]
    let others = routed.filter { $0.a.x != a.x || $0.a.y != a.y }
    for w in others where !w.curved {
      // A lane beside every run of another wire, for this one to take instead of sharing it.
      for i in w.pts.indices.dropFirst() {
        if w.pts[i].x == w.pts[i - 1].x { xLines += [w.pts[i].x - WireCost.lane, w.pts[i].x + WireCost.lane] }
        else { yLines += [w.pts[i].y - WireCost.lane, w.pts[i].y + WireCost.lane] }
      }
    }
    let xs = Self.sortedUnique(xLines), ys = Self.sortedUnique(yLines)
    let nx = xs.count, ny = ys.count
    guard let startX = xs.firstIndex(of: first.x), let startY = ys.firstIndex(of: first.y),
          let goalX = xs.firstIndex(of: last.x), let goalY = ys.firstIndex(of: last.y) else { return nil }
    let start = startX + startY * nx, goal = goalX + goalY * nx

    // Directions: 0 along the flow, 1 +x, 2 against the flow, 3 -x.
    var best = [CGFloat](repeating: .infinity, count: nx * ny * 4)
    var from = [Int](repeating: -1, count: nx * ny * 4)
    // What a stretch between neighbouring nodes costs, worked out once (-1: not yet).
    var stretch = [CGFloat](repeating: -1, count: nx * ny * 2)
    // What lies near each line, also once: most lines pass few Cards and wires.
    var nearY = [Near?](repeating: nil, count: ny), nearX = [Near?](repeating: nil, count: nx)
    func cost(_ n: Int, horizontal: Bool) -> CGFloat {
      let k = n * 2 + (horizontal ? 1 : 0)
      if stretch[k] < 0 {
        let ix = n % nx, iy = n / nx
        if horizontal {
          if nearY[iy] == nil { nearY[iy] = Self.nearLine(ys[iy], horizontal: true, cards, others) }
          stretch[k] = Self.runCost(xs[ix], xs[ix + 1], ys[iy], horizontal: true, nearY[iy]!)
        } else {
          if nearX[ix] == nil { nearX[ix] = Self.nearLine(xs[ix], horizontal: false, cards, others) }
          stretch[k] = Self.runCost(ys[iy], ys[iy + 1], xs[ix], horizontal: false, nearX[ix]!)
        }
      }
      return stretch[k]
    }
    // The length still to go, and the bend that still takes if the wire is not heading there.
    func h(_ n: Int, _ dir: Int) -> CGFloat {
      let dx = abs(xs[n % nx] - last.x), dy = abs(ys[n / nx] - last.y)
      return dx + dy + ((dir & 1 == 1 ? dy : dx) > 0 ? WireCost.turn : 0)
    }

    var open = WireHeap()
    best[start * 4] = 0
    open.push(h(start, 0), start * 4)
    var end = -1
    while let (f, id) = open.pop() {
      let n = id >> 2, dir = id & 3
      let g = best[id]
      if f > g + h(n, dir) { continue }
      if n == goal {
        end = id
        break
      }
      let ix = n % nx, iy = n / nx
      for d in 0..<4 {
        if d == (dir + 2) & 3 { continue }
        if d == 0 ? iy + 1 >= ny : d == 1 ? ix + 1 >= nx : d == 2 ? iy == 0 : ix == 0 { continue }
        let m = d == 0 ? n + nx : d == 1 ? n + 1 : d == 2 ? n - nx : n - 1
        var g2 = g + cost(d == 0 || d == 1 ? n : m, horizontal: d == 1 || d == 3)
        if g2 == .infinity { continue }
        if d != dir { g2 += WireCost.turn }
        if m == goal {
          // It goes on into the port along the flow: never back along the way in.
          if d == 2 { continue }
          if d != 0 { g2 += WireCost.turn }
        }
        let next = m * 4 + d
        if g2 < best[next] {
          best[next] = g2
          from[next] = id
          open.push(g2 + h(m, d), next)
        }
      }
    }
    if end < 0 { return nil }

    var corners = [b, into, last]
    var id = from[end]
    while id >= 0 {
      let n = id >> 2
      corners.append(CGPoint(x: xs[n % nx], y: ys[n / nx]))
      id = from[id]
    }
    corners += [first, out, a]
    return Self.straightened(corners.reversed())
  }

  /// The Cards closer to a grid line than `clearance` and the wires that reach it.
  private static func nearLine(_ v: CGFloat, horizontal: Bool, _ cards: [CGRect], _ wires: [Routed]) -> Near {
    let c = WireCost.clearance, l = WireCost.lane
    return horizontal
      ? Near(cards: cards.filter { v > $0.minY - c && v < $0.minY + $0.height + c }, wires: wires.filter { v >= $0.box.minY - l && v <= $0.box.minY + $0.box.height + l })
      : Near(cards: cards.filter { v > $0.minX - c && v < $0.minX + $0.width + c }, wires: wires.filter { v >= $0.box.minX - l && v <= $0.box.minX + $0.box.width + l })
  }

  /// The cost of a stretch along a grid line, from `u0` to `u1` at `v`; infinity through a Card.
  private static func runCost(_ u0: CGFloat, _ u1: CGFloat, _ v: CGFloat, horizontal: Bool, _ near: Near) -> CGFloat {
    var cost = u1 - u0
    for o in near.cards {
      let lo = horizontal ? o.minX : o.minY, hi = lo + (horizontal ? o.width : o.height)
      let vlo = horizontal ? o.minY : o.minX, vhi = vlo + (horizontal ? o.height : o.width)
      if u0 < hi && u1 > lo && v > vlo && v < vhi { return .infinity }
      let c = WireCost.clearance
      cost += WireCost.near * max(0, min(u1, hi + c) - max(u0, lo - c))
    }
    for w in near.wires {
      let lo = horizontal ? w.box.minX : w.box.minY, hi = lo + (horizontal ? w.box.width : w.box.height)
      if u1 < lo || u0 > hi { continue }
      for i in w.pts.indices.dropFirst() {
        let su = horizontal ? w.pts[i - 1].x : w.pts[i - 1].y, sv = horizontal ? w.pts[i - 1].y : w.pts[i - 1].x
        let tu = horizontal ? w.pts[i].x : w.pts[i].y, tv = horizontal ? w.pts[i].y : w.pts[i].x
        // Half open on both sides: a crossing at a node or a corner counts once.
        if (sv <= v) != (tv <= v) {
          let at = su + ((v - sv) * (tu - su)) / (tv - sv)
          if at >= u0 && at < u1 { cost += WireCost.crossing }
        } else if abs(sv - v) < WireCost.lane / 2 && abs(tv - v) < WireCost.lane / 2 {
          cost += WireCost.overlap * max(0, min(u1, max(su, tu)) - max(u0, min(su, tu)))
        }
      }
    }
    return cost
  }

  private static func bezier(_ a: CGPoint, _ c1: CGPoint, _ c2: CGPoint, _ b: CGPoint, _ t: CGFloat) -> CGPoint {
    let u = 1 - t
    return CGPoint(x: u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x,
                   y: u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y)
  }

  /// Whether a segment goes through the inside of a Card (its edge is where ports sit).
  private static func segmentEnters(_ p: CGPoint, _ q: CGPoint, _ o: CGRect) -> Bool {
    var t0: CGFloat = 0, t1: CGFloat = 1
    func axis(_ from: CGFloat, _ to: CGFloat, _ lo: CGFloat, _ hi: CGFloat) -> Bool {
      let d = to - from
      if d == 0 { return from > lo && from < hi }
      let ta = (lo - from) / d, tb = (hi - from) / d
      t0 = max(t0, min(ta, tb))
      t1 = min(t1, max(ta, tb))
      return t0 <= t1
    }
    return axis(p.x, q.x, o.minX + 2, o.maxX - 2) && axis(p.y, q.y, o.minY + 2, o.maxY - 2)
  }

  private static func sortedUnique(_ values: [CGFloat]) -> [CGFloat] {
    let sorted = values.sorted()
    return sorted.indices.filter { $0 == 0 || sorted[$0] != sorted[$0 - 1] }.map { sorted[$0] }
  }

  /// A path without repeated points or corners that are not corners.
  private static func straightened(_ pts: [CGPoint]) -> [CGPoint] {
    var out: [CGPoint] = []
    for p in pts {
      if let e = out.last, e == p { continue }
      let n = out.count
      if n >= 2 && ((out[n - 2].x == out[n - 1].x && out[n - 1].x == p.x) || (out[n - 2].y == out[n - 1].y && out[n - 1].y == p.y)) { out[n - 1] = p }
      else { out.append(p) }
    }
    return out
  }

  func curve(_ a: CGPoint, _ b: CGPoint) -> Path {
    let (c1, c2) = controls(a, b)
    var path = Path()
    path.move(to: a)
    path.addCurve(to: b, control1: c1, control2: c2)
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

/// Binary heap on (f, id): a total order, so ties come out the same on Mac and web.
private struct WireHeap {
  private var items: [(f: CGFloat, id: Int)] = []

  private func less(_ i: Int, _ j: Int) -> Bool {
    items[i].f < items[j].f || (items[i].f == items[j].f && items[i].id < items[j].id)
  }

  mutating func push(_ f: CGFloat, _ id: Int) {
    items.append((f, id))
    var i = items.count - 1
    while i > 0 {
      let up = (i - 1) >> 1
      if !less(i, up) { break }
      items.swapAt(i, up)
      i = up
    }
  }

  mutating func pop() -> (CGFloat, Int)? {
    guard let top = items.first, let tail = items.popLast() else { return nil }
    if !items.isEmpty {
      items[0] = tail
      var i = 0
      while true {
        let l = 2 * i + 1, r = l + 1
        var m = i
        if l < items.count && less(l, m) { m = l }
        if r < items.count && less(r, m) { m = r }
        if m == i { break }
        items.swapAt(i, m)
        i = m
      }
    }
    return (top.f, top.id)
  }
}

/// Wires (screen paths). Carried ones are colored; active ones are animated.
struct WireLayer: View {
  let wires: [WireDraw]
  var animate = false
  var zoom: Double = 1
  /// The birds (the Agent's and its helpers'): a wire one of them threads is
  /// drawn by its presence layer meanwhile.
  var presences: [AgentPresence] = []

  /// The active wires' dashes, and how fast they flow (points per second).
  static let dash: [CGFloat] = [8, 6]
  static let flow: Double = 30

  var body: some View {
    let anyActive = animate && wires.contains { $0.style == .active }
    let _ = presences.map(\.revision)
    let threading = presences.contains { $0.animating(at: Date()) }
    // At the display's frame rate, the phase wrapping at exactly one dash
    // period, so the dashes flow without a jump.
    SwiftUI.TimelineView(.animation(paused: !anyActive && !threading)) { t in
      Canvas { ctx, _ in
        let period = Self.dash.reduce(0, +)
        let phase = anyActive
          ? -CGFloat((t.date.timeIntervalSinceReferenceDate * Self.flow).truncatingRemainder(dividingBy: Double(period))) : 0
        let w = max(1, 1.6 * min(1.4, zoom))
        for wire in wires {
          // A wire the Agent is threading is drawn by the presence layer.
          if let th = wire.thread, presences.contains(where: { $0.thread(th, at: t.date) != .plain }) { continue }
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
    let routes = router.routeAll(wires.map { (a: point($0.from, $0.fromPort, output: true), b: point($0.to, $0.toPort, output: false)) })
    return zip(wires, routes).map { w, world in
      WireDraw(path: world.applying(camera.transform), style: w.active ? .active : .normal)
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
