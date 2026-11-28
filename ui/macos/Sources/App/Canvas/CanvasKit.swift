import AppKit
import SwiftUI
import TsugumiRPC
import TsugumiViewPresentation

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
    case .prepare: return String(localized: "スキーマ生成")
    case .evaluate: return String(localized: "Nix 評価")
    case .build: return "build"
    case .execute: return "run"
    case .validate: return "validate"
    case .save: return String(localized: "保存")
    case .done: return String(localized: "完了")
    }
  }

  /// The stage in words on a Card's face (the gist of a failure).
  var name: String {
    switch self {
    case .prepare: return String(localized: "準備")
    default: return label
    }
  }

  /// Short "in progress" wording for the status row.
  var activeLabel: String {
    switch self {
    case .waiting: return String(localized: "入力待ち")
    case .prepare: return String(localized: "スキーマ生成中")
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
    case .execute: return .output
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

  /// Stages shown for a Card, in order: build (schema generation, Nix
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

/// Progress of the latest invocation as the stage circles show it, from the
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
    /// Running, but waiting (for a free worker slot or its Nix store).
    var waiting = false
    var id: Int { stage.rawValue }
  }

  var items: [Item]
  var workerReused = false
  /// The core's own stage that failed or stopped (a shown stage covers several).
  var failedPart: Stage?

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

  /// The latest invocation's stages from the stages the core recorded.
  /// Stages the invocation did not reach are "pending" while it runs and
  /// "not run" once it ended. On a worker, WorkerStages decides the build
  /// and execution circles.
  static func of(_ s: CardRunState, code: Bool) -> StageProgress {
    let ended = [.succeeded, .failed, .cancelled, .skipped, .blocked].contains(s.state)
    let failedPart = s.stages.last(where: { $0.outcome == .failed || $0.outcome == .cancelled })?.stage
    // One shown stage covers several of the core's (build: SDK, Nix
    // evaluation, build; validate: check and save).
    let items = Stage.applicable(code: code).map { shown -> StageProgress.Item in
      var parts: [StageProgress.Status] = []
      var seconds: Double?
      for st in shown.parts {
        guard let r = s.stages.last(where: { $0.stage == st }) else {
          parts.append(ended ? .notRun : .pending)
          continue
        }
        switch r.outcome {
        case .running: parts.append(ended ? .notRun : .running)
        case .cached: parts.append(.cached)
        case .reused: parts.append(.reused)
        case .failed: parts.append(.failed)
        case .cancelled: parts.append(.cancelled)
        default: parts.append(.done)
        }
        if let e = r.end, let b = r.start { seconds = (seconds ?? 0) + Double(max(0, e - b)) / 1000 }
      }
      return .init(stage: shown, status: StageProgress.merge(parts, ended: ended), seconds: seconds)
    }
    return StageProgress(items: items, workerReused: s.workerReused, failedPart: failedPart)
      .onWorker(WorkerStages(records: s.stages, service: s.executionService, live: ended ? nil : s,
                             lastActivity: s.executionLastActivity, buildCached: s.cached ? true : nil))
  }

  /// The build and execution circles as WorkerStages says: the worker's
  /// build runs before the Card does, never both at once.
  func onWorker(_ worker: WorkerStages) -> StageProgress {
    var next = self
    let run = items.first { $0.stage == .execute }
    if worker.endedInBuild { next.failedPart = .build }
    for i in next.items.indices {
      switch next.items[i].stage {
      case .build:
        if let status = worker.build {
          next.items[i].status = status
          next.items[i].waiting = worker.buildWaiting
          if worker.endedInBuild, let s = run?.seconds { next.items[i].seconds = (next.items[i].seconds ?? 0) + s }
        }
      case .execute:
        if let status = worker.execute { next.items[i].status = status }
        if worker.endedInBuild { next.items[i].seconds = nil }
        next.items[i].waiting = worker.executeWaiting && next.items[i].status == .running
      default: break
      }
    }
    return next
  }

  /// A Card's stages before it ran in this run: all of them still to come.
  static func notRun(code: Bool) -> StageProgress {
    StageProgress(items: Stage.applicable(code: code).map { Item(stage: $0, status: .pending) })
  }
}

struct PortData: Hashable {
  var name: String
  var typeName: String?
}

/// What the user and the Agent write about a Card (_comments.json), and the record
/// of who did what ("agent" or "user"; nil when not recorded).
struct CardComment: Equatable, Identifiable {
  var id: String
  var text: String
  var resolved = false
  var finding = false
  /// One line on what was done about it.
  var resolution: String?
  var author: String?
  var editedBy: String?
  var stateBy: String?
  /// The Cards the save that resolved it changed.
  var cards: [String] = []

  static func list(in meta: JSONValue?) -> [CardComment] {
    (meta?["comments"]?.array ?? []).compactMap { c in
      guard let id = c["id"]?.string, let text = c["text"]?.string, !id.isEmpty else { return nil }
      let line = c["resolution"]?.string ?? ""
      return CardComment(id: id, text: text, resolved: c["resolved"]?.bool ?? false, finding: c["finding"]?.bool ?? false, resolution: line.isEmpty ? nil : line,
                         author: c["author"]?.string, editedBy: c["edited_by"]?.string, stateBy: c["state_by"]?.string,
                         cards: c["cards"]?.array?.compactMap(\.string) ?? [])
    }
  }

  /// Who did something to a comment, for the sentences that say so.
  static func actor(_ by: String?) -> String? {
    by == "agent" ? "Agent" : by == "user" ? String(localized: "あなた") : nil
  }

  /// What a version or a save did to a comment, and who did it when that is recorded.
  static func change(_ change: String, by: String?) -> String {
    let what: String
    switch change {
    case "added": what = String(localized: "追加")
    case "edited": what = String(localized: "編集")
    case "resolved": what = String(localized: "解決")
    case "reopened": what = String(localized: "開き直し")
    case "removed": what = String(localized: "削除")
    default: what = change
    }
    return actor(by).map { "\(what)（\($0)）" } ?? what
  }
}

/// Everything a Card view needs, with no dependency on a session.
struct CardNodeData: Equatable {
  var title: String
  var kind: CardKind
  var symbol: String?
  /// Description (on hover; a Draft's content on its Card, a few lines).
  var summary: String?
  var inputs: [PortData]
  var outputs: [PortData]
  /// Why the Card does not run (it waits on a Draft): said in its status line.
  var result: String?
  /// What the invocation is doing outside its stages (a file transfer, the
  /// execution service's queue): told in the status line, which every Card
  /// has, so the Card keeps its size and its wires stay where they are.
  var activity: String?
  var state: Tsugumi_Data_CardState?
  var executionList = false
  var stage: StageProgress?
  /// Invocation number in the current run (0: not run in this run).
  var invocation: UInt64 = 0
  /// The run is still going on (a succeeded Card may run again).
  var runContinues = false
  var runMark: String?
  var failure: String?
  /// Each worker's reason, when the Card failed because no worker could be
  /// chosen and so never started (its face says so, the reasons on hover).
  var notStarted: String?
  /// When the latest invocation ended, and how long it took.
  var endedAt: Date?
  var seconds: Double?
  var errors: [String] = []
  var warnings: [String] = []
  var collapsed = false
  /// What it may do beyond computing (network, no sandbox, Mac folders).
  var permissions: [CardPermission] = []
  /// A Draft being written on the canvas: the Card opens up for it.
  var draftEditing = false
  /// A Draft without a name of its own: its content is all it shows.
  var untitled = false
  /// The canvas grid's spacing while the grid is on: the ports follow it.
  var portGrid: CGFloat?
  /// Comments on the Card not resolved yet.
  var openComments = 0
  /// Whether a running invocation is working (CPU time, last output), on hover.
  var liveness: String?
  /// What a remote execution waits for or does, in full (on hover; the
  /// status line has the short form in `activity`).
  var activityDetail: String?
  /// Where the invocation runs (its execution service), on hover.
  var worker: String?

  var isDraft: Bool { kind == .draft }
  var isRouting: Bool { kind == .merge || kind == .collect }

  /// "Invocation #3 · Succeeded" and the like; nil before the first invocation.
  var runText: String? {
    if executionList { return state.map { $0.label + (runMark.map { " · " + $0 } ?? "") } ?? runMark }
    guard let state, invocation > 0 else { return runMark }
    var s = String(localized: "実行 #\(invocation)")
    if let activity {
      s += " · " + activity
    } else if let cur = stage?.current, state == .running || state == .preparing {
      s += " · " + cur.stage.activeLabel
    } else {
      s += " · " + state.label
      if state == .succeeded && runContinues { s += String(localized: "（次の入力を待てます）") }
    }
    return s + (runMark.map { " · " + $0 } ?? "")
  }

  /// Plain status for a Card that did not run in this run.
  var idleText: String {
    if isDraft { return String(localized: "未実装（Draft）") }
    if kind == .invalid { return String(localized: "定義エラー") }
    if let state, state != .notStarted { return state.label }
    return String(localized: "未実行（0 回）")
  }

  /// The one line under a Card's name: what it is doing now, how its last
  /// invocation ended, or the gist of its failure. Always one line, so the
  /// Card's size never changes with its state.
  func statusLine(now: Date = Date()) -> CardStatusLine {
    let mark = runMark.map { " · " + $0 } ?? ""
    // No worker could be chosen (core failure stage=worker_selection): the
    // Card never started, and the hover says each worker's reason.
    if state == .failed, let reasons = notStarted {
      return .init(text: String(localized: "worker を選べず未開始") + mark, tone: .failed, hover: reasons.isEmpty ? nil : reasons)
    }
    if let state, state == .failed || state == .blocked {
      var text = failure.map(firstMeaningfulLine) ?? state.label
      if text.isEmpty { text = state.label }
      if let stage = stage?.failedPart ?? stage?.failed?.stage, stage.name != text { text = stage.name + ": " + text }
      return .init(text: text + mark, tone: state == .failed ? .failed : .state(state))
    }
    if !errors.isEmpty, state == nil || state == .notStarted {
      return .init(text: firstMeaningfulLine(errors[0]), tone: .failed)
    }
    if executionList { return .init(text: runText ?? idleText, tone: state.map(CardStatusLine.Tone.state) ?? .plain) }
    guard let state, state != .notStarted else {
      return .init(text: (result ?? String(localized: "未実行")) + mark, tone: .plain)
    }
    switch state {
    case .running, .preparing:
      let doing = activity ?? stage?.current?.stage.activeLabel ?? state.label
      return .init(text: doing + mark, tone: .state(state))
    case .succeeded:
      var text = state.label
      if invocation > 1 { text += " ×\(invocation)" }
      if let endedAt { text += " · " + relativeAge(endedAt, now: now) }
      if let seconds { text += " · " + formatShortDuration(seconds) }
      return .init(text: text + mark, tone: .plain)
    case .skipped:
      return .init(text: state.label + mark, tone: .plain)
    default:
      return .init(text: state.label + mark, tone: .state(state))
    }
  }
}

/// A duration as a Card's face says it: "4.0 s", "3:12", "1:02:03".
func formatShortDuration(_ seconds: Double) -> String {
  let t = max(0, seconds)
  if t < 59.95 { return String(format: "%.1f s", t) }
  let s = Int(t.rounded(.down))
  if s < 3600 { return String(format: "%d:%02d", s / 60, s % 60) }
  return String(format: "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60)
}

/// A Card's status line and how it is coloured.
struct CardStatusLine: Equatable {
  enum Tone: Equatable {
    case plain
    case failed
    case state(Tsugumi_Data_CardState)
  }
  var text: String
  var tone: Tone
  /// What the line's hover says, when more than the line.
  var hover: String?

  var color: Color {
    switch tone {
    case .plain: return .secondary
    case .failed: return Tsugumi_Data_CardState.failed.color
    case .state(let s): return s.color
    }
  }
}

/// The first line of a diagnostic that says something: blank lines and a
/// bare "error:" prefix are skipped.
func firstMeaningfulLine(_ text: String) -> String {
  for raw in text.split(whereSeparator: \.isNewline) {
    var line = raw.trimmingCharacters(in: .whitespaces)
    if line.lowercased().hasPrefix("error:") { line = String(line.dropFirst(6)).trimmingCharacters(in: .whitespaces) }
    if !line.isEmpty { return line }
  }
  return ""
}

/// Outline of a Card on the canvas.
enum CardShape {
  case card, merge, collect, start

  init(_ kind: CardKind) {
    switch kind {
    case .trigger: self = .start
    case .merge: self = .merge
    case .collect: self = .collect
    default: self = .card
    }
  }
}

/// The nearest grid line. Not `.rounded()`: that goes away from zero on a tie
/// and the web's `Math.round` goes up, and a Card on a negative half would
/// land one grid apart on the two.
func snapToGrid(_ v: CGFloat, _ grid: CGFloat) -> CGFloat { ((v / grid + 0.5).rounded(.down)) * grid }

// Presentation from the document and the layout: each UI computes the shape
// itself rather than asking hostd (port-grid-contract.json keeps them alike).
/// Size, outline and port positions of a Card (world units, relative to its
/// top-left corner). Shared by drawing, wires, the selection and hit-tests.
///
/// With the grid on, a port's place along its edge is on grid/n of the canvas:
/// a Card's corner is on a grid line, so every offset here is a multiple of
/// grid/n from the corner, and the Card's length across the flow is an even
/// number of grids so the middle of the edge is on a line too.
struct CardGeometry {
  static let width: CGFloat = 220
  static let header: CGFloat = 32
  /// An ordinary Card's face: its name (up to two lines) and status line
  /// beside the stage circles. Fixed: nothing about a run changes it.
  static let face: CGFloat = 46
  static let summaryRow: CGFloat = 15
  static let portRow: CGFloat = 15
  /// A Draft's content on the Card (three lines), and while it is being written.
  static let draftRow: CGFloat = 50
  static let draftEditRow: CGFloat = 120
  /// What is left of the header over the content of a Draft without a name.
  static let untitledTop: CGFloat = 8
  /// What a wire names to end at a Draft Card itself.
  static let newPort = ""
  /// Labels narrower than this are hidden (shown on hover instead).
  static let minLabel: CGFloat = 26
  /// Share of a triangle's wide edge taken by its narrow end (its apex).
  static let narrow: CGFloat = 0
  /// Ports this far apart have room for a whole name between them, while there are few.
  static let labelPitch: CGFloat = 60
  /// Port marks are 8 wide: closer than this they would touch.
  static let minPortPitch: CGFloat = 10
  /// Kept free at the ends of a Card's edge (its rounded corners).
  static let portMargin: CGFloat = 12

  let inputs: [String]
  let outputs: [String]
  let collapsed: Bool
  /// The name, status line and stage circles (code and Card in Card Cards).
  let face: Bool
  let summary: Bool
  let stages: Bool
  let flow: FlowDirection
  let shape: CardShape
  let draft: Bool
  let draftEditing: Bool
  let untitled: Bool
  let grid: CGFloat?

  init(_ d: CardNodeData, flow: FlowDirection) {
    // A Draft and a Card whose definition cannot be read have no ports.
    let portless = d.isDraft || d.kind == .invalid
    inputs = portless ? [] : d.inputs.map(\.name)
    outputs = portless ? [] : d.outputs.map(\.name)
    collapsed = d.collapsed
    draft = d.isDraft
    draftEditing = d.isDraft && d.draftEditing
    untitled = d.isDraft && d.untitled && !d.draftEditing && !d.collapsed
    face = (d.kind == .code || d.kind == .subgraph) && !d.collapsed
    // A description is on hover; a Card whose definition cannot be read says
    // why on a row of its own.
    summary = d.summary != nil && d.kind == .invalid
    // The face is the same for every state, so a run never makes a Card grow or shrink.
    stages = d.kind == .code
    self.flow = flow
    shape = CardShape(d.kind)
    grid = d.portGrid
  }

  // MARK: Outline

  /// Width across the flow and length along it of a routing Card's outline.
  private var routingSize: CGSize {
    switch shape {
    // Small marks: what they do is plain from the shape (details on hover).
    case .merge: return CGSize(width: fanWidth(inputs.count), height: fanLength(inputs.count))
    case .collect: return CGSize(width: grid.map { Self.gridLength(atLeast: 64, $0) } ?? 64, height: 44)
    // The start Card: its name and a run button, nothing more.
    case .start:
      guard let grid else { return CGSize(width: 116, height: 34) }
      return CGSize(width: Self.gridLength(near: 116, grid), height: flow == .leftToRight ? 2 * Self.rowPitch(grid) : 34)
    case .card: return .zero
    }
  }

  /// The wide edge of a Merge: it grows with its ports, so they are never squeezed.
  private func fanWidth(_ count: Int) -> CGFloat {
    guard let grid else { return max(56, CGFloat(count) * 30 + 12) }
    return Self.gridLength(atLeast: max(56, CGFloat(count - 1) * fanPitch(count, grid) + 16), grid)
  }

  /// A Merge along the flow: a quarter of its wide edge once that is
  /// past 40, so many ports make a bigger triangle and not a flatter one (its
  /// corners stay at 27 degrees or more). On whole grids, as far as it grows.
  private func fanLength(_ count: Int) -> CGFloat {
    let length = fanWidth(count) / 4
    guard length > 40 else { return 40 }
    return grid.map { (length / $0).rounded(.up) * $0 } ?? length
  }

  /// An even number of grids, so half of it is on a grid line.
  private static func gridLength(near length: CGFloat, _ grid: CGFloat) -> CGFloat {
    2 * grid * (length / (2 * grid) + 0.5).rounded(.down)
  }
  private static func gridLength(atLeast length: CGFloat, _ grid: CGFloat) -> CGFloat {
    2 * grid * (length / (2 * grid)).rounded(.up)
  }

  /// The closest ports can be with a row each (grid/n, or whole grids on a fine grid).
  static func rowPitch(_ grid: CGFloat) -> CGFloat {
    grid < portRow ? (portRow / grid).rounded(.up) * grid : grid / (grid / portRow).rounded(.down)
  }

  /// Ports of a Merge. Two or four around the middle take twice the
  /// pitch to stay on the same lines as a Card's ports across a wire; more
  /// of them would make the mark too wide, and close up onto grid/2 instead.
  private func fanPitch(_ count: Int, _ grid: CGFloat) -> CGFloat {
    Self.rowPitch(grid) * (count == 2 || count == 4 ? 2 : 1)
  }

  /// Ports of a Card's edge across the flow, gathered around its middle in
  /// a bit over half its length: far enough apart for their names while few, then
  /// by the grid, grid/2, grid/3 and so on. Past `minPortPitch` they stop
  /// closing up and spread further along the edge instead.
  private func edgePitch(_ count: Int, _ grid: CGFloat) -> CGFloat {
    let room = Self.gridLength(near: Self.width, grid) * 0.6
    // An even number of grids: two ports around the middle land on grid lines.
    var pitch = Self.gridLength(atLeast: Self.labelPitch, grid)
    var n: CGFloat = 1
    while CGFloat(count - 1) * pitch > room {
      var next = pitch - grid
      if pitch <= grid {
        n += 1
        next = grid / n
      }
      if next < Self.minPortPitch { break }
      pitch = next
    }
    return pitch
  }

  /// The Card's length across the flow; it grows if its ports would not fit.
  var cardWidth: CGFloat {
    guard let grid else { return Self.width }
    var w = Self.gridLength(near: Self.width, grid)
    if flow == .topToBottom {
      for count in [inputs.count, outputs.count] where count > 1 {
        w = max(w, Self.gridLength(atLeast: CGFloat(count - 1) * edgePitch(count, grid) + 2 * Self.portMargin, grid))
      }
    }
    return w
  }

  /// With the grid on, the place of a port along an edge across the flow.
  private func alongGrid(_ index: Int, of count: Int, output: Bool) -> CGFloat? {
    guard let grid else { return nil }
    let middle = CGFloat(index) - CGFloat(count - 1) / 2
    if shape == .card { return cardWidth / 2 + middle * edgePitch(count, grid) }
    // A triangle's apex is one point: everything on that side is on it.
    let w = routingSize.width
    return output ? w / 2 : w / 2 + middle * fanPitch(count, grid)
  }

  /// Labels of a routing Card sit outside its outline, in these bands.
  private var bandIn: CGFloat { shape != .card && flow == .topToBottom && showLabels(input: true) ? Self.portRow : 0 }
  private var bandOut: CGFloat { shape != .card && flow == .topToBottom && showLabels(input: false) ? Self.portRow : 0 }

  var portBand: CGFloat {
    guard flow == .leftToRight, shape == .card else { return 0 }
    let count = max(inputs.count, outputs.count, 1)
    if draft {
      // A Draft's wire anchors sit at the middle of its sides, with no port rows.
      // On the grid they do not close up, so it makes room.
      guard let grid else { return 0 }
      let p = Self.rowPitch(grid)
      return max(0, CGFloat(count) * p + 8 - contentHeight)
    }
    guard let grid else { return CGFloat(count) * Self.portRow }
    let p = Self.rowPitch(grid)
    return firstRow(p) - rowsTop + CGFloat(count - 1) * p + Self.portRow / 2
  }
  /// Where the rows of a left-to-right Card begin, under its face (or header and summary).
  private var rowsTop: CGFloat { face ? Self.face : Self.header + (summary ? Self.summaryRow : 0) }
  /// The first grid line a port's row fits under them.
  private func firstRow(_ pitch: CGFloat) -> CGFloat {
    ((rowsTop + Self.portRow / 2) / pitch).rounded(.up) * pitch
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
    if collapsed { return CGSize(width: cardWidth, height: Self.header) }
    return CGSize(width: cardWidth, height: contentHeight + portBand)
  }
  /// A Card's height without its port rows.
  private var contentHeight: CGFloat {
    // Left to right, a little room under the last port row.
    if face { return topLabels + Self.face + bottomLabels + (flow == .leftToRight ? 4 : 0) }
    return topLabels + headerRow + (summary ? Self.summaryRow : 0) + draftRow + bottomLabels + 4
  }
  var height: CGFloat { size.height }

  /// The outline's frame inside the Card's box.
  var outlineRect: CGRect {
    let s = size
    guard shape != .card, shape != .start else { return CGRect(origin: .zero, size: s) }
    if flow == .topToBottom { return CGRect(x: 0, y: bandIn, width: s.width, height: s.height - bandIn - bandOut) }
    return CGRect(x: 30, y: 0, width: s.width - 60, height: s.height)
  }

  /// The outline itself (Card box, fan or funnel) inside `r`.
  /// `cornerScale` scales the corner radius with the outline.
  func outline(in r: CGRect, cornerScale: CGFloat = 1) -> Path {
    switch shape {
    case .card:
      return Path(roundedRect: r, cornerRadius: 9 * cornerScale)
    case .start:
      return Path(roundedRect: r, cornerRadius: r.height / 2)
    case .collect:
      func point(_ across: CGFloat, _ along: CGFloat) -> CGPoint {
        flow == .topToBottom ? CGPoint(x: r.minX + r.width * across, y: r.minY + r.height * along)
                            : CGPoint(x: r.minX + r.width * along, y: r.minY + r.height * across)
      }
      return roundedPolygon([point(0, 0), point(1, 0), point(0.65, 0.57), point(0.65, 1),
                             point(0.35, 1), point(0.35, 0.57)], radius: 3 * cornerScale)
    case .merge:
      // Triangle: downstream apex. The apex is listed
      // twice so it stays sharp: the ports of that side are on its very tip.
      // Its corners shrink with the zoom, as `r` does.
      let radius = 7 * (flow == .topToBottom ? r.height : r.width) / routingSize.height
      if flow == .topToBottom {
        let apex = CGPoint(x: r.midX, y: r.maxY)
        return roundedPolygon([CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), apex, apex], radius: radius)
      }
      let apex = CGPoint(x: r.maxX, y: r.midY)
      return roundedPolygon([CGPoint(x: r.minX, y: r.minY), apex, apex, CGPoint(x: r.minX, y: r.maxY)], radius: radius)
    }
  }

  /// Where the icon of a routing Card sits in the outline `r`: on the axis of the
  /// flow, and in a triangle toward the wide edge, where it has room.
  func iconCenter(in r: CGRect) -> CGPoint {
    let along: CGFloat = shape == .merge || shape == .collect ? 0.38 : 0.5
    return flow == .topToBottom ? CGPoint(x: r.midX, y: r.minY + r.height * along)
                                : CGPoint(x: r.minX + r.width * along, y: r.midY)
  }

  // MARK: Ports

  /// The segment ports of one side are spread along (top-to-bottom: an x
  /// range at a fixed y).
  private func edgeSpan(input: Bool) -> (start: CGFloat, width: CGFloat, at: CGFloat) {
    switch shape {
    case .card, .start:
      let s = shape == .card ? CGSize(width: cardWidth, height: collapsed ? Self.header : 0) : routingSize
      return (0, s.width, 0)
    case .merge, .collect:
      let w = routingSize.width
      let span = input ? w : w * Self.narrow
      return ((w - span) / 2, span, 0)
    }
  }

  /// Room for one port's label on its edge.
  func spacing(input: Bool) -> CGFloat {
    let list = input ? inputs : outputs
    guard !list.isEmpty else { return 0 }
    let span = edgeSpan(input: input)
    // A single label may be as wide as the Card; several share the edge
    // without overlapping their neighbours.
    if list.count == 1 { return max(span.width, 90) }
    if let grid, shape == .card { return edgePitch(list.count, grid) - 4 }
    return span.width / CGFloat(list.count + 1) - 4
  }

  /// Labels are shown when there is room for them; otherwise the whole name
  /// and type show on hover.
  func showLabels(input: Bool) -> Bool {
    let list = input ? inputs : outputs
    guard !list.isEmpty else { return false }
    // Draft attachment points have no labels.
    if draft { return false }
    if shape == .card && collapsed { return false }
    if shape == .start { return false }
    // Routing marks name their ports on hover.
    if shape == .merge || shape == .collect { return false }
    return spacing(input: input) >= Self.minLabel
  }

  /// Where wires attach directly to a Draft: the middle of either side.
  func newPortPoint(output: Bool) -> CGPoint? {
    guard draft, !collapsed, (output ? outputs : inputs).isEmpty else { return nil }
    let r = outlineRect
    return flow == .topToBottom ? CGPoint(x: cardWidth / 2, y: output ? r.maxY : r.minY)
                                : CGPoint(x: output ? cardWidth : 0, y: grid.map { snapToGrid(r.midY, Self.rowPitch($0)) } ?? r.midY)
  }

  /// Routing bodies must keep room for a move even at the minimum zoom.
  /// A fixed screen-space floor would cover the start's title at 20%.
  func portHitSize(zoom: CGFloat) -> CGFloat {
    shape == .card ? max(16, 18 * zoom) : 12 * zoom
  }

  func port(_ name: String, output: Bool) -> CGPoint {
    if draft, let p = newPortPoint(output: output) { return p }
    let list = output ? outputs : inputs
    let index = list.firstIndex(of: name) ?? 0
    let i = CGFloat(index)
    let n = CGFloat(max(list.count, 1))
    let r = outlineRect
    if shape == .start {
      // Its one output, in the middle of the downstream edge.
      return flow == .topToBottom ? CGPoint(x: r.midX, y: r.maxY) : CGPoint(x: r.maxX, y: r.midY)
    }
    switch flow {
    case .topToBottom:
      let y = output ? r.maxY : r.minY
      if let along = alongGrid(index, of: list.count, output: output) { return CGPoint(x: along, y: y) }
      let span = edgeSpan(input: !output)
      return CGPoint(x: span.start + span.width * (i + 1) / (n + 1), y: y)
    case .leftToRight:
      if shape != .card {
        if let along = alongGrid(index, of: list.count, output: output) { return CGPoint(x: output ? r.maxX : r.minX, y: r.minY + along) }
        let h = output ? r.height * Self.narrow : r.height
        return CGPoint(x: output ? r.maxX : r.minX, y: r.midY - h / 2 + h * (i + 1) / (n + 1))
      }
      let x = output ? cardWidth : 0
      if collapsed { return CGPoint(x: x, y: grid.map { snapToGrid(Self.header / 2, Self.rowPitch($0)) } ?? Self.header / 2) }
      if let grid {
        let p = Self.rowPitch(grid)
        return CGPoint(x: x, y: firstRow(p) + i * p)
      }
      return CGPoint(x: x, y: rowsTop + i * Self.portRow + Self.portRow / 2)
    }
  }
}

/// A closed polygon with rounded corners: each corner is cut `radius` along its
/// sides (at most half of them; a side of no length leaves it sharp). An arc of
/// a fixed radius would not do: it cuts a sharp corner far deeper as the corner
/// narrows, and took the ends off a flat triangle along with the ports on them.
func roundedPolygon(_ pts: [CGPoint], radius: CGFloat) -> Path {
  func toward(_ a: CGPoint, _ b: CGPoint, _ length: CGFloat) -> CGPoint {
    let l = hypot(b.x - a.x, b.y - a.y)
    return l > 0 ? CGPoint(x: a.x + (b.x - a.x) * length / l, y: a.y + (b.y - a.y) * length / l) : a
  }
  return Path { p in
    let n = pts.count
    guard n > 2 else { return }
    p.move(to: CGPoint(x: (pts[n - 1].x + pts[0].x) / 2, y: (pts[n - 1].y + pts[0].y) / 2))
    for i in 0..<n {
      let previous = pts[(i + n - 1) % n], corner = pts[i], next = pts[(i + 1) % n]
      let cut = min(radius, hypot(corner.x - previous.x, corner.y - previous.y) / 2, hypot(next.x - corner.x, next.y - corner.y) / 2)
      p.addLine(to: toward(corner, previous, cut))
      p.addQuadCurve(to: toward(corner, next, cut), control: corner)
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

  func showsCard(_ cards: [CGRect], in viewport: CGSize) -> Bool {
    let bounds = CGRect(origin: .zero, size: viewport)
    return cards.contains { !$0.applying(transform).intersection(bounds).isEmpty }
  }

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

/// The canvas's content, laid out for the camera `base` and shown through the
/// camera as it is now. The content is in a hosting view of its own, and a move
/// of the canvas changes the bounds of that view's parent and nothing of the
/// content: to SwiftUI a transform over the content costs time for every view
/// under it at every move, to AppKit moving a view costs the same whatever is
/// in it.
///
/// Zoomed, the content is scaled as it was drawn (soft for a moment). Once the
/// zoom has stopped, or a pan has gone further than the content reaches beyond
/// the canvas (`margin`, on every side), `onSettled` has it laid out again for
/// the camera of the moment.
struct CanvasPanHost<Content: View>: NSViewRepresentable {
  let session: ProjectSession
  let scope: CardKey
  let base: CanvasCamera
  /// Counts the passes that laid the content out: the hosting view takes the content of a new pass only.
  let pass: Int
  let margin: CGFloat
  /// What of the content takes the mouse (the canvas's own units): anywhere
  /// else a click is the canvas's (its background's gestures and menu).
  let solid: [CGRect]
  let onSettled: () -> Void
  let content: Content

  init(session: ProjectSession, scope: CardKey, base: CanvasCamera, pass: Int, margin: CGFloat, solid: [CGRect],
       onSettled: @escaping () -> Void, @ViewBuilder content: () -> Content) {
    self.session = session
    self.scope = scope
    self.base = base
    self.pass = pass
    self.margin = margin
    self.solid = solid
    self.onSettled = onSettled
    self.content = content()
  }

  final class Mover: NSView {
    override var isFlipped: Bool { true }
  }

  final class Container: NSView {
    let mover = Mover()
    let hosting = NSHostingView(rootView: AnyView(EmptyView()))
    var pass = -1
    var base = CanvasCamera()
    var now = CanvasCamera()
    var margin: CGFloat = 0
    var solid: [CGRect] = []
    var settle: Task<Void, Never>?
    override var isFlipped: Bool { true }

    override init(frame: NSRect) {
      super.init(frame: frame)
      wantsLayer = true
      layer?.masksToBounds = true
      mover.wantsLayer = true
      // The canvas supplies window-relative coordinates. Keep the nested
      // hosting view from adding the window's top inset a second time.
      hosting.safeAreaRegions = []
      addSubview(mover)
      mover.addSubview(hosting)
    }

    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
      super.layout()
      place()
    }

    /// Where the content is, from the camera of the moment: a point q of it
    /// (laid out for `base`) is shown at q × scale + d.
    var shift: (scale: CGFloat, dx: CGFloat, dy: CGFloat) {
      let scale = now.zoom / base.zoom
      return (scale, now.x - base.x * scale, now.y - base.y * scale)
    }

    func place() {
      let (scale, dx, dy) = shift
      mover.frame = bounds
      mover.bounds = CGRect(x: -dx / scale, y: -dy / scale, width: bounds.width / scale, height: bounds.height / scale)
      let whole = CGRect(origin: .zero, size: bounds.size).insetBy(dx: -margin, dy: -margin)
      if hosting.frame != whole { hosting.frame = whole }
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
      let p = superview.map { convert(point, from: $0) } ?? point
      let world = CGPoint(x: (p.x - now.x) / now.zoom, y: (p.y - now.y) / now.zoom)
      guard solid.contains(where: { $0.contains(world) }) else { return nil }
      return super.hitTest(point)
    }
  }

  func makeNSView(context: Context) -> Container { Container() }

  static func dismantleNSView(_ view: Container, coordinator: ()) {
    view.settle?.cancel()
  }

  func updateNSView(_ view: Container, context: Context) {
    if view.pass != pass {
      view.pass = pass
      view.base = base
      view.margin = margin
      view.solid = solid
      let environment = context.environment
      view.hosting.rootView = AnyView(content.transformEnvironment(\.self) { $0 = environment })
    }
    let c = session.cameras[scope] ?? (40, 40, 1)
    view.now = CanvasCamera(x: c.x, y: c.y, zoom: c.zoom)
    view.place()
    view.settle?.cancel()
    guard view.now != view.base else { return }
    let (scale, dx, dy) = view.shift
    // A pan within what the content covers needs nothing more: it is where AppKit has it.
    // Past that, it is laid out again at once; zoomed, when the canvas has stopped.
    let beyond = max(abs(dx), abs(dy)) > margin * 0.8
    guard scale != 1 || beyond else { return }
    let done = onSettled
    view.settle = Task { @MainActor in
      if scale != 1 { try? await Task.sleep(for: .milliseconds(140)) }
      if !Task.isCancelled { done() }
    }
  }
}

/// The canvas's grid of dots, which follows the camera by itself.
struct CanvasDots: View {
  let session: ProjectSession
  let scope: CardKey
  let shown: Bool
  let spacing: Double

  var body: some View {
    let camera = session.cameras[scope] ?? (40, 40, 1)
    CanvasGrid(camera: CanvasCamera(x: camera.x, y: camera.y, zoom: camera.zoom),
               show: shown, size: spacing)
  }
}

/// A part of the canvas that is drawn again only when what it shows changes
/// (`key`), not at every move of the canvas: its place is given outside of it.
/// The content's closures are those of when the key last changed, so the key
/// names everything they depend on that is not read when they are called.
/// Without a key it is drawn again every time.
struct Settled<Key: Equatable, Content: View>: View, @MainActor Equatable {
  let key: Key?
  @ViewBuilder let content: () -> Content

  static func == (a: Self, b: Self) -> Bool { a.key != nil && a.key == b.key }

  var body: some View { content() }
}

extension View {
  /// Places a view drawn at screen scale (`size` × `zoom`) with its top-left
  /// corner at `topLeft` (screen units), using the same coordinates as the wires.
  func placeScaled(at topLeft: CGPoint, size: CGSize, zoom: Double) -> some View {
    let w = size.width * zoom, h = size.height * zoom
    return frame(width: w, height: h).position(x: topLeft.x + w / 2, y: topLeft.y + h / 2)
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
  let content: Binding<String>
  /// Done, the focus having moved elsewhere (false) or by the keyboard (true).
  let onDone: (_ byKey: Bool) -> Void
}

/// A Card as drawn on every canvas, at the screen scale `zoom`.
struct CardNodeView: View {
  let data: CardNodeData
  let flow: FlowDirection
  var zoom: Double = 1
  var selected = false
  var portsEditable = true
  var onPortDrag: ((String, CGPoint) -> Void)?
  var onPortDrop: ((String, CGPoint) -> Void)?
  /// Dragging from an input port (a wire drawn back to an output).
  var onInputDrag: ((String, CGPoint) -> Void)?
  var onInputDrop: ((String, CGPoint) -> Void)?
  var onStageTap: ((Stage) -> Void)?
  var onToggleCollapse: (() -> Void)?
  /// The badge of open comments: opens them (nil: only shows the number).
  var onComments: (() -> Void)?
  /// What the start Card's button does now (nil: not offered).
  enum StartButton: Equatable {
    case run(enabled: Bool)
    case stop(enabled: Bool)

    /// Whether it shows stop, and whether it can be pressed.
    var shown: (stop: Bool, enabled: Bool) {
      switch self {
      case .run(let e): return (false, e)
      case .stop(let e): return (true, e)
      }
    }
  }
  var start: StartButton?
  /// The root start Card: its button is what ⌘R does, and the guide's "run".
  var rootStart = false
  /// The start Card's button: runs the workflow from it, or stops the run.
  var onRun: (() -> Void)?
  var onStop: (() -> Void)?
  var draftEdit: DraftEdit?
  private enum DraftField { case content }
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
    let path = g.outline(in: outline, cornerScale: z)
    ZStack(alignment: .topLeading) {
      path.fill(fillColor)
        .shadow(color: .black.opacity(selected ? 0.2 : 0.07), radius: selected ? 4 : 1.5, y: 1)
      switch g.shape {
      case .card: cardBody(g)
      case .start: startBody(outline: outline)
      default: routingBody(g, outline: outline)
      }
      path.stroke(borderColor, style: StrokeStyle(lineWidth: selected ? 2.4 : 1, dash: data.isDraft ? [5 * z, 3 * z] : []))
        .allowsHitTesting(false)
      portLabels(g)
      portMarks(g)
      if data.openComments > 0 { commentsBadge(outline: outline) }
    }
    .frame(width: size.width, height: size.height, alignment: .topLeading)
    // The outline and the ports: a port sits on the edge, and the half of it
    // outside the outline must still start a wire.
    .contentShape(Self.hitShape(data, flow: flow, zoom: zoom, newPorts: onPortDrag != nil && portsEditable))
    .accessibilityElement(children: .contain)
    .accessibilityLabel(accessibilityText)
  }

  /// On the outline's top right corner, for every shape of Card.
  private func commentsBadge(outline: CGRect) -> some View {
    Button { onComments?() } label: {
      HStack(spacing: s(2)) {
        Image(systemName: "bubble.left").font(font(9, .semibold))
        Text(verbatim: "\(data.openComments)").font(font(9.5, .semibold)).monospacedDigit()
      }
      .foregroundStyle(.white)
      .padding(.horizontal, s(4))
      .frame(minWidth: s(15), minHeight: s(15))
      .background(Color.accentColor, in: Capsule())
    }
    .buttonStyle(.plain)
    .disabled(onComments == nil)
    .help("未解決のコメント \(data.openComments) 件")
    .accessibilityLabel(String(localized: "未解決のコメント \(data.openComments) 件"))
    .alignmentGuide(.leading) { $0.width * 0.7 - outline.maxX }
    .alignmentGuide(.top) { $0.height / 2 - outline.minY }
  }

  /// What of a Card takes the mouse, in its view's own units at `zoom`: the
  /// outline, the ports (with the new-port handles of a Draft) and the
  /// comments badge. The debug panel draws the same shape.
  static func hitShape(_ data: CardNodeData, flow: FlowDirection, zoom: Double, newPorts: Bool) -> Path {
    let g = CardGeometry(data, flow: flow)
    let z = CGFloat(zoom)
    let o = g.outlineRect
    let outline = g.outline(in: CGRect(x: o.minX * z, y: o.minY * z, width: o.width * z, height: o.height * z), cornerScale: z)
    // A rotated outline can wind opposite to a port rectangle. Appending
    // subpaths would then cut holes in their overlap, so take their union.
    var p = outline
    if data.openComments > 0 {
      // The badge sits half outside the outline and must still take the click.
      let badge = max(16, 18 * z)
      p = p.union(Path(CGRect(x: outline.boundingRect.maxX - badge, y: outline.boundingRect.minY - badge / 2, width: badge * 1.4, height: badge)))
    }
    let hit = g.portHitSize(zoom: z)
    for (list, output) in [(data.inputs, false), (data.outputs, true)] {
      for port in list {
        let pt = g.port(port.name, output: output)
        p = p.union(Path(CGRect(x: pt.x * z - hit / 2, y: pt.y * z - hit / 2, width: hit, height: hit)))
      }
      if newPorts, let pt = g.newPortPoint(output: output) {
        p = p.union(Path(CGRect(x: pt.x * z - hit / 2, y: pt.y * z - hit / 2, width: hit, height: hit)))
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
      if g.face {
        face(g).frame(height: s(CardGeometry.face))
      } else if g.untitled {
        Color.clear.frame(height: s(g.headerRow))
      } else {
        header.frame(height: s(CardGeometry.header))
      }
      if !g.collapsed {
        if g.draft { draftBody(g) }
        if g.summary, let summary = data.summary {
          Text(summary).font(font(10)).foregroundStyle(Tsugumi_Data_CardState.failed.color)
            .lineLimit(1).truncationMode(.tail).help(summary)
            .padding(.horizontal, s(10)).frame(height: s(CardGeometry.summaryRow), alignment: .top)
        }
        if flow == .leftToRight { Color.clear.frame(height: s(g.portBand)) }
      }
    }
    .frame(width: s(g.cardWidth), alignment: .leading)
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
    let width = s(g.cardWidth - 20)
    let box = (text as NSString).boundingRect(with: CGSize(width: width, height: .greatestFiniteMagnitude),
                                              options: [.usesLineFragmentOrigin, .usesFontLeading],
                                              attributes: [.font: NSFont.systemFont(ofSize: max(6, 11 * z))])
    return box.height <= s(g.draftRow - 8)
  }

  private var header: some View {
    HStack(spacing: s(6)) {
      Image(systemName: data.symbol ?? "xmark.octagon").font(font(12)).foregroundStyle(.secondary).frame(width: s(15))
      Text(verbatim: data.untitled && edit == nil ? data.summary?.split(separator: "\n").first.map(String.init) ?? data.title : data.title)
        .font(font(12, .semibold)).lineLimit(1).truncationMode(.tail)
      if edit == nil { Spacer(minLength: s(2)) }
      ForEach(data.permissions, id: \.rawValue) { p in
        Image(systemName: p.symbol).font(font(10)).foregroundStyle(p == .sandboxOff ? .orange : .secondary).help(p.label)
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

  /// An ordinary Card: its name (two lines at most) and its status line, with
  /// the stage circles on the right. The marks sit beside the name's first line.
  private func face(_ g: CardGeometry) -> some View {
    HStack(alignment: .center, spacing: s(8)) {
      VStack(alignment: .leading, spacing: s(1)) {
        HStack(alignment: .firstTextBaseline, spacing: s(4)) {
          Text(verbatim: data.title).font(font(12, .semibold)).lineLimit(2).truncationMode(.tail)
            .fixedSize(horizontal: false, vertical: true)
          Spacer(minLength: 0)
          marks
        }
        TimelineView(.periodic(from: .now, by: 30)) { t in
          let line = data.statusLine(now: t.date)
          Text(verbatim: line.text).font(font(10)).foregroundStyle(line.color)
            .lineLimit(1).truncationMode(.tail)
            .help(line.hover ?? "")
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      if g.stages {
        StageDots(progress: data.stage.flatMap { data.invocation > 0 ? $0 : nil } ?? .notRun(code: true),
                  zoom: zoom, onTap: onStageTap)
      } else if data.kind == .subgraph {
        Image(systemName: data.symbol ?? "link").font(font(10)).foregroundStyle(.secondary)
      }
    }
    .padding(.leading, s(10)).padding(.trailing, s(9))
  }

  /// Permissions and the collapse button.
  @ViewBuilder private var marks: some View {
    ForEach(data.permissions, id: \.rawValue) { p in
      Image(systemName: p.symbol).font(font(10)).foregroundStyle(p == .sandboxOff ? .orange : .secondary).help(p.label)
    }
    if !data.warnings.isEmpty {
      Image(systemName: "exclamationmark.triangle.fill").font(font(10)).foregroundStyle(.yellow)
        .help(data.warnings.joined(separator: "\n"))
    }
    if let toggle = onToggleCollapse {
      Button(action: toggle) {
        Image(systemName: data.collapsed ? "chevron.down" : "chevron.up").font(font(9, .semibold))
      }
      .buttonStyle(.borderless).foregroundStyle(.secondary)
      .help(data.collapsed ? "展開" : "折りたたむ（実行には影響しません）")
    }
  }

  /// State icon and invocation number.
  @ViewBuilder private var stateBadge: some View {
    if !data.errors.isEmpty, data.state != .failed {
      CardStateMark(state: .failed, size: s(9))
    } else if let st = data.state, st != .notStarted {
      HStack(spacing: s(2)) {
        CardStateMark(state: st, size: s(9))
        if data.invocation > 0 { Text("#\(data.invocation)").monospacedDigit().foregroundStyle(.secondary) }
      }
      .font(font(10))
      .help(data.runText ?? st.label)
    }
  }

  // MARK: Start Card

  /// Its name and a small run button (stop while a run is on); the run's state beside the name.
  private func startBody(outline: CGRect) -> some View {
    HStack(spacing: s(6)) {
      startButton
      Text(data.title).font(font(12, .semibold)).lineLimit(1).truncationMode(.tail)
      Spacer(minLength: 0)
      startStatus.help(data.state?.label ?? StageProgress.Status.pending.label)
    }
    .padding(.horizontal, s(7))
    .frame(width: outline.width, height: outline.height)
  }

  /// The start Card's last state as one stage circle (not run: an empty
  /// ring); the run gear while it is under way.
  @ViewBuilder private var startStatus: some View {
    let state = data.state ?? .notStarted
    if [.waiting, .ready, .preparing, .running].contains(state) {
      RunGear(size: s(11))
    } else {
      CardStateMark(state: state, size: s(9))
    }
  }

  /// Runs the workflow from this Card; while a run is on, stops it.
  @ViewBuilder private var startButton: some View {
    let (stop, enabled) = start?.shown ?? (false, false)
    let run = String(localized: "ここから Workflow を実行")
    let button = Button { stop ? onStop?() : onRun?() } label: {
      Image(systemName: stop ? "stop.fill" : "play.fill").font(font(9, .bold)).foregroundStyle(.white)
        .frame(width: s(20), height: s(20))
        .background(Circle().fill(!enabled ? Color.secondary : stop ? Color.red : Color.accentColor))
    }
    .buttonStyle(.plain)
    .disabled(!enabled)
    .accessibilityLabel(stop ? "停止" : "実行")
    .help(stop ? tip(String(localized: "Workflow の実行を停止（チャットの回答停止とは別の操作です）"), .stop)
               : rootStart ? tip(run, .run) : run)
    if rootStart { button.guideTarget("run", radius: s(10), zoom: zoom) } else { button }
  }

  // MARK: Routing Card

  /// The Card's icon, coloured by the state; name and details on hover.
  private func routingBody(_ g: CardGeometry, outline: CGRect) -> some View {
    let center = g.iconCenter(in: outline)
    return Image(systemName: data.symbol ?? "xmark.octagon")
      .font(font(10, .semibold))
      .foregroundStyle(data.state.map { $0 == .notStarted ? Color.secondary : $0.color } ?? .secondary)
      .position(center)
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
      let w = g.cardWidth / 2 - 14
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
    let label = Text(p.name).font(font(9.5)).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
    if g.shape == .card {
      // Inside an ordinary Card: below its input, above its output.
      let half = CardGeometry.portRow / 2 + 0.5
      return AnyView(label.frame(width: s(w)).position(x: s(pt.x), y: s(pt.y + (output ? -half : half))).allowsHitTesting(false))
    }
    // Beside the port of a routing Card, so the wire (vertical at the port)
    // never runs through the name: right of it, or left on the left half.
    let left = pt.x < g.outlineRect.midX - 1 && g.spacing(input: !output) < 70
    let lw: CGFloat = max(CardGeometry.minLabel, min(w, 80))
    let dy: CGFloat = output ? CardGeometry.portRow / 2 : -CardGeometry.portRow / 2
    let x = left ? pt.x - 6 - lw / 2 : pt.x + 6 + lw / 2
    return AnyView(label.frame(width: s(lw), alignment: left ? .trailing : .leading)
      .position(x: s(x), y: s(pt.y + dy)).allowsHitTesting(false))
  }

  /// Inputs are small squares, outputs small circles; dragging from either
  /// draws a wire to connect (it takes priority over moving the Card). The
  /// hit area is larger than the mark.
  private func portMarks(_ g: CardGeometry) -> some View {
    let mark = g.shape == .card ? max(6, s(8)) : s(8)
    let hit = g.portHitSize(zoom: z)
    return ZStack(alignment: .topLeading) {
      ForEach(data.inputs, id: \.name) { p in
        let pt = g.port(p.name, output: false)
        RoundedRectangle(cornerRadius: max(1, s(1.5)))
          .fill(Color(nsColor: .controlBackgroundColor))
          .overlay(RoundedRectangle(cornerRadius: max(1, s(1.5))).strokeBorder(.secondary, lineWidth: 1.4))
          .frame(width: mark, height: mark)
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
      if onPortDrag != nil, portsEditable {
        newPortMark(g, output: false)
        newPortMark(g, output: true)
      }
    }
  }

  /// Dragging from here draws a wire attached directly to the Draft on this
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
    if let t = p.typeName { s += " : \(t)" }
    return s
  }

  private var borderColor: Color {
    if selected { return .accentColor }
    // A Card whose definition cannot be read is drawn as a failure.
    if data.kind == .invalid { return Tsugumi_Data_CardState.failed.color.opacity(0.7) }
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

/// Stages of the latest invocation as small circles stacked top to bottom
/// (build, run, validate). Shape and colour say how each went; a click opens
/// that stage in the log.
struct StageDots: View {
  let progress: StageProgress
  var zoom: Double = 1
  var onTap: ((Stage) -> Void)?
  static let dot: CGFloat = 9
  static let gap: CGFloat = 4

  private func s(_ v: CGFloat) -> CGFloat { v * CGFloat(zoom) }

  var body: some View {
    VStack(spacing: s(Self.gap)) {
      ForEach(progress.items) { item in
        let waits = item.waiting
        Button { onTap?(item.stage) } label: { StageDot(status: item.status, size: s(Self.dot), waiting: waits) }
          .buttonStyle(.plain)
          .disabled(onTap == nil)
          .help(Self.help(item, tappable: onTap != nil, waiting: waits))
          .accessibilityLabel(item.stage.label + "：" + SectionStatus(item.status, waiting: waits).label)
      }
    }
    .accessibilityElement(children: .contain)
  }

  static func help(_ item: StageProgress.Item, tappable: Bool, waiting: Bool = false) -> String {
    var t = item.stage.label + "：" + SectionStatus(item.status, waiting: waiting).label
    if let sec = item.seconds { t += " · " + formatDuration(sec) }
    if item.status == .failed || item.status == .cancelled { t += String(localized: "（クリックでこの段階のログ）") }
    else if tappable, item.status != .pending { t += String(localized: "（クリックでログ）") }
    return t
  }

}

/// The stage states' colours, one set for the Card's circles, the log's
/// pipeline line and sections and the hover's stage table: Okabe–Ito, safe
/// for colour-blind readers, darker on a light ground. Done is blue, cached
/// and reused a lighter sky blue, running and waiting orange, failed
/// vermillion, stopped grey; shapes and marks tell them apart as well. The
/// same set as STAGE_COLOR in the Web UI (styles.css).
enum StagePalette {
  static let done = dynamic(0x0072B2, dark: 0x3D9BE0)
  static let reuse = dynamic(0x3D9BE0, dark: 0x9BD3F3)
  static let run = dynamic(0xC07A00, dark: 0xE69F00)
  static let failed = dynamic(0xD55E00, dark: 0xF2762E)
  static let cancelled = dynamic(0x8E8E93, dark: 0x98989D)

  static func color(_ status: SectionStatus) -> Color {
    switch status {
    case .pending: return .secondary
    case .waiting, .running: return run
    case .done: return done
    case .cached, .reused: return reuse
    case .failed: return failed
    case .cancelled: return cancelled
    }
  }

  private static func dynamic(_ light: UInt32, dark: UInt32) -> Color {
    func rgb(_ v: UInt32) -> NSColor {
      NSColor(srgbRed: CGFloat(v >> 16 & 0xFF) / 255, green: CGFloat(v >> 8 & 0xFF) / 255, blue: CGFloat(v & 0xFF) / 255, alpha: 1)
    }
    return Color(nsColor: NSColor(name: nil) { $0.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? rgb(dark) : rgb(light) })
  }
}

/// One stage circle: an empty ring before it is reached, a spinning arc while
/// it runs, a dashed ring while it waits (for the worker). Once it ended a
/// shape says how, not the colour alone: done is filled, cached a ring with a
/// dot, reused filled with a hole, failed filled with a cross, stopped filled
/// with a bar. The same geometry as StageDot in the Web UI.
struct StageDot: View {
  let status: StageProgress.Status
  let size: CGFloat
  var waiting = false

  var body: some View {
    let c = StagePalette.color(SectionStatus(status, waiting: waiting))
    let line = max(0.8, size * 0.15)
    Group {
      switch status {
      case .running where waiting:
        Circle().fill(Color(nsColor: .controlBackgroundColor))
          .overlay(Circle().inset(by: line * 0.6)
            .stroke(c, style: StrokeStyle(lineWidth: line * 1.1, dash: [size * 0.22, size * 0.16])))
      case .pending, .notRun:
        Circle().fill(Color(nsColor: .controlBackgroundColor))
          .overlay(Circle().strokeBorder(c.opacity(0.6), lineWidth: line))
      case .running:
        // Turns for as long as the stage runs.
        TimelineView(.animation(minimumInterval: 1.0 / 30)) { t in
          let angle = t.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1) * 360
          ZStack {
            Circle().fill(Color(nsColor: .controlBackgroundColor))
            Circle().strokeBorder(c.opacity(0.25), lineWidth: line * 1.2)
            Circle().inset(by: line * 0.6).trim(from: 0, to: 0.3)
              .stroke(c, style: StrokeStyle(lineWidth: line * 1.2, lineCap: .round))
              .rotationEffect(.degrees(angle))
          }
        }
      default:
        ended(c, line: line)
      }
    }
    .frame(width: size, height: size)
    .contentShape(Circle().inset(by: -size * 0.3))
  }

  /// An ended stage: its fill and mark, cut out in the background colour.
  private func ended(_ c: Color, line: CGFloat) -> some View {
    Canvas { g, box in
      let d = box.width, r = d / 2, k = d * 0.2, mark = max(1, d * 0.15)
      let hole = GraphicsContext.Shading.color(Color(nsColor: .controlBackgroundColor))
      let whole = CGRect(origin: .zero, size: box)
      let dot = Path(ellipseIn: CGRect(x: r - k, y: r - k, width: 2 * k, height: 2 * k))
      if status == .cached {
        g.fill(Path(ellipseIn: whole), with: hole)
        g.stroke(Path(ellipseIn: whole.insetBy(dx: line / 2, dy: line / 2)), with: .color(c), lineWidth: line)
        g.fill(dot, with: .color(c))
      } else {
        g.fill(Path(ellipseIn: whole), with: .color(c))
      }
      switch status {
      case .reused:
        g.fill(dot, with: hole)
      case .failed:
        var cross = Path()
        cross.move(to: CGPoint(x: r - k, y: r - k)); cross.addLine(to: CGPoint(x: r + k, y: r + k))
        cross.move(to: CGPoint(x: r + k, y: r - k)); cross.addLine(to: CGPoint(x: r - k, y: r + k))
        g.stroke(cross, with: hole, style: StrokeStyle(lineWidth: mark, lineCap: .round))
      case .cancelled:
        var bar = Path()
        bar.move(to: CGPoint(x: r - d * 0.24, y: r)); bar.addLine(to: CGPoint(x: r + d * 0.24, y: r))
        g.stroke(bar, with: hole, style: StrokeStyle(lineWidth: mark * 1.1, lineCap: .round))
      default: break
      }
    }
  }
}

/// A Card's run state as the stage circles draw it, where one stands for it:
/// waiting for inputs or a slot is a dashed ring, preparing and running the
/// turning arc, succeeded filled, failed and stopped their marks, not run an
/// empty ring. Skipped and blocked have no stage of their own and keep their
/// symbol.
extension Tsugumi_Data_CardState {
  var stageLook: (status: StageProgress.Status, waiting: Bool)? {
    switch self {
    case .notStarted: return (.pending, false)
    case .waiting, .ready: return (.running, true)
    case .preparing, .running: return (.running, false)
    case .succeeded: return (.done, false)
    case .failed: return (.failed, false)
    case .cancelled: return (.cancelled, false)
    default: return nil
    }
  }
}

/// A Card's state mark (the sidebar's Card list, the Card's header): the
/// stage circle for the state, sized for the place.
struct CardStateMark: View {
  let state: Tsugumi_Data_CardState
  let size: CGFloat

  var body: some View {
    if let look = state.stageLook {
      StageDot(status: look.status, size: size, waiting: look.waiting).accessibilityLabel(Text(state.label))
    } else {
      Image(systemName: state.symbol).font(.system(size: size)).foregroundStyle(state.color)
    }
  }
}

/// What a run is doing as a gear in the running colour: it steps one tooth at
/// a time (an eased 45° turn, then a hold). Still and dimmed while the run only
/// waits (for a worker slot or its Nix store); still with Reduce Motion. The
/// same drawing and timing as RunGear in the Web UI.
struct RunGear: View {
  var size: CGFloat = 15
  var waiting = false
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  /// One step: 0.22 s turning (ease in and out), then held until 0.45 s.
  static let period = 0.45, turn = 0.22

  /// The gear's turn at `time` (seconds), in degrees within one tooth.
  static func angle(at time: TimeInterval) -> Double {
    let u = min(1, time.truncatingRemainder(dividingBy: period) / turn)
    return 45 * (u < 0.5 ? 2 * u * u : 1 - pow(-2 * u + 2, 2) / 2)
  }

  var body: some View {
    Group {
      if waiting || reduceMotion {
        gear(0)
      } else {
        TimelineView(.animation(minimumInterval: 1.0 / 60)) { t in
          gear(Self.angle(at: t.date.timeIntervalSinceReferenceDate))
        }
      }
    }
    .frame(width: size, height: size)
    .opacity(waiting ? 0.45 : 1)
    .accessibilityHidden(true)
  }

  private func gear(_ angle: Double) -> some View {
    RunGearShape().fill(StagePalette.run, style: FillStyle(eoFill: true)).rotationEffect(.degrees(angle))
  }
}

/// Eight teeth, outer radius 10, teeth 2.6 deep, a hole of 3.2, in a box of 24.
struct RunGearShape: Shape {
  func path(in rect: CGRect) -> Path {
    let k = min(rect.width, rect.height) / 24, c = CGPoint(x: rect.midX, y: rect.midY)
    func at(_ r: CGFloat, _ deg: Double) -> CGPoint {
      let a = (deg - 90) * .pi / 180
      return CGPoint(x: c.x + r * k * cos(a), y: c.y + r * k * sin(a))
    }
    var p = Path()
    for i in 0..<8 {
      let a = Double(i) * 45
      let points = [at(7.4, a - 13), at(10, a - 8), at(10, a + 8), at(7.4, a + 13)]
      if i == 0 { p.move(to: points[0]) } else { p.addLine(to: points[0]) }
      for q in points.dropFirst() { p.addLine(to: q) }
    }
    p.closeSubpath()
    p.addEllipse(in: CGRect(x: c.x - 3.2 * k, y: c.y - 3.2 * k, width: 6.4 * k, height: 6.4 * k))
    return p
  }
}

// MARK: - Wires

/// A wire as drawn (screen units).
struct WireDraw {
  enum Style { case normal, carried, active, selected }
  var path: Path
  var style: Style
  /// A wire to or from a Draft is dotted, whatever its style.
  var dotted = false
  var executionList = false
  var countLabel: String?
  var countAt: CGPoint?
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

// Drawn routes are each UI's own work. hostd's route_wires is for Untangle,
// which edits the layout, not for drawing.
/// Routes wires in world units around the Cards. A wire going along the flow
/// whose curve touches no Card stays that curve. Any other wire takes the
/// cheapest path on a grid of lines beside the Cards: by length, bends and
/// crossings of other wires, leaving and entering its ports along the flow.
/// The same path draws and hit-tests a wire. The same algorithm as `WireRouter`
/// in ui/web/src/model/canvasKit.ts: tests/fixtures/wire-routes.json holds both to it.
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
    // A port sits on its Card's edge: only the inside counts. A routing port is
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
    // On an edge is not in it: a Card that begins where another Card's box ends is
    // beside the wire, and the wire would otherwise go on through all of it.
    if p.x <= o.minX || p.x >= o.maxX || p.y <= o.minY || p.y >= o.maxY { return 0 }
    return dir > 0 ? o.maxY - p.y : p.y - o.minY
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
  /// A Draft's wires: round dots, spaced so that they flow without a jump
  /// at the phase of the active wires' dashes (a whole number per period),
  /// and never smaller than can be seen on a Canvas zoomed out.
  static let dot: [CGFloat] = [0, dash.reduce(0, +) / 3]
  static let dotMin: CGFloat = 2.2
  /// A selected wire is thicker: further apart, its dots still read as dots.
  static let dotSelected: [CGFloat] = [0, dash.reduce(0, +) / 2]

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
          let dots = wire.dotted ? Self.dot : []
          let width = { (w: CGFloat) in
            let thick = w + (wire.executionList ? 2.8 : 0)
            return wire.dotted ? max(thick, Self.dotMin) : thick
          }
          switch wire.style {
          case .normal:
            ctx.stroke(wire.path, with: .color(.secondary.opacity(0.75)), style: StrokeStyle(lineWidth: width(w), lineCap: .round, lineJoin: .round, dash: dots))
          case .carried:
            ctx.stroke(wire.path, with: .color(.orange.opacity(0.8)), style: StrokeStyle(lineWidth: width(w + 0.4), lineCap: .round, lineJoin: .round, dash: dots))
          case .active:
            ctx.stroke(wire.path, with: .color(.orange), style: StrokeStyle(lineWidth: width(w + 0.6), lineCap: .round, lineJoin: .round,
                                                                             dash: wire.dotted ? dots : anyActive ? Self.dash : [], dashPhase: phase))
          case .selected:
            ctx.stroke(wire.path, with: .color(.accentColor), style: StrokeStyle(lineWidth: width(w + 1.4), lineCap: .round, lineJoin: .round, dash: wire.dotted ? Self.dotSelected : []))
          }
          if let count = wire.countLabel, let at = wire.countAt {
            let label = ctx.resolve(Text(verbatim: count).font(.system(size: 11, weight: .medium, design: .monospaced)))
            let size = label.measure(in: CGSize(width: 100, height: 24))
            let point = CGPoint(x: at.x, y: at.y - 11)
            let rect = CGRect(x: point.x - size.width / 2 - 2, y: point.y - size.height / 2,
                              width: size.width + 4, height: size.height)
            ctx.fill(Path(roundedRect: rect, cornerRadius: 3), with: .color(Color(nsColor: .windowBackgroundColor)))
            ctx.draw(label, at: point)
          }
        }
      }
    }
    .allowsHitTesting(false)
  }
}

// MARK: - Card view links

/// The line from a Card to one of its view panels on the canvas (world
/// units). It says which Card a panel belongs to; it is not a connection.
struct CardViewLink: Equatable {
  let panel: UUID
  let card: CardKey
  /// Where it meets the Card and the panel.
  let a: CGPoint
  let b: CGPoint
  let horizontal: Bool

  /// The ends sit on the edges that face each other (where the two are
  /// closest), each in the middle of the stretch the two share, or toward
  /// the other when they share none.
  init(panel: UUID, card: CardKey, cardRect: CGRect, panelRect: CGRect) {
    self.panel = panel
    self.card = card
    let gx = max(panelRect.minX - cardRect.maxX, cardRect.minX - panelRect.maxX)
    let gy = max(panelRect.minY - cardRect.maxY, cardRect.minY - panelRect.maxY)
    func clamp(_ v: CGFloat, _ lo: CGFloat, _ hi: CGFloat) -> CGFloat { lo > hi ? (lo + hi) / 2 : min(max(v, lo), hi) }
    if gx >= gy {
      let right = panelRect.midX >= cardRect.midX
      let lo = max(cardRect.minY, panelRect.minY), hi = min(cardRect.maxY, panelRect.maxY)
      let ay = lo <= hi ? (lo + hi) / 2 : clamp(panelRect.midY, cardRect.minY + 10, cardRect.maxY - 10)
      let by = lo <= hi ? (lo + hi) / 2 : clamp(cardRect.midY, panelRect.minY + 10, panelRect.maxY - 10)
      a = CGPoint(x: right ? cardRect.maxX : cardRect.minX, y: ay)
      b = CGPoint(x: right ? panelRect.minX : panelRect.maxX, y: by)
      horizontal = true
    } else {
      let down = panelRect.midY >= cardRect.midY
      let lo = max(cardRect.minX, panelRect.minX), hi = min(cardRect.maxX, panelRect.maxX)
      let ax = lo <= hi ? (lo + hi) / 2 : clamp(panelRect.midX, cardRect.minX + 10, cardRect.maxX - 10)
      let bx = lo <= hi ? (lo + hi) / 2 : clamp(cardRect.midX, panelRect.minX + 10, panelRect.maxX - 10)
      a = CGPoint(x: ax, y: down ? cardRect.maxY : cardRect.minY)
      b = CGPoint(x: bx, y: down ? panelRect.minY : panelRect.maxY)
      horizontal = false
    }
  }

  /// One link per panel whose Card is on this canvas (`cardRect` gives its
  /// outline, nil when it is not shown here).
  static func links(_ panels: [(id: UUID, card: CardKey, rect: CGRect)],
                    cardRect: (CardKey) -> CGRect?) -> [CardViewLink] {
    panels.compactMap { panel in
      cardRect(panel.card).map { CardViewLink(panel: panel.id, card: panel.card, cardRect: $0, panelRect: panel.rect) }
    }
  }

  /// A short curve leaving and entering straight.
  var path: Path {
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
}

/// The Card view links as drawn: thin, dashed and faint, under the wires, so
/// they do not read as data connections; they never take the mouse.
struct CardViewLinkLayer: View {
  let links: [CardViewLink]
  /// World to layer.
  let transform: CGAffineTransform
  var zoom: Double = 1

  static let color = Color.purple.opacity(0.45)
  static let dash: [CGFloat] = [5, 4]

  var body: some View {
    Canvas { ctx, _ in
      let w = max(0.8, 1.6 * min(1.4, zoom) * 0.6)
      let r: CGFloat = 2.5
      for link in links {
        ctx.stroke(link.path.applying(transform), with: .color(Self.color),
                   style: StrokeStyle(lineWidth: w, lineCap: .round, dash: Self.dash))
        for p in [link.a, link.b].map({ $0.applying(transform) }) {
          ctx.fill(Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r)), with: .color(Self.color))
        }
      }
    }
    .allowsHitTesting(false)
  }
}

// MARK: - Card view panel placement

/// Where a Card's view panels first appear (world units). A panel belongs to
/// the canvas that shows its Card and starts next to it, in room that no
/// Card, wire, note or other panel there takes. A placed panel keeps its
/// place: only panels without one are placed.
enum CardViewPanelPlacer {
  /// The canvas a view's Card is on: the scope that holds the Card.
  static func scope(of card: [String]) -> CardKey { key(Array(card.dropLast())) }

  /// A modest size: what the Card asked for, within bounds, or a default.
  static func size(width: UInt32, height: UInt32) -> CGSize {
    CGSize(width: width == 0 ? 400 : min(480, max(240, CGFloat(width))),
           height: height == 0 ? 280 : min(360, max(160, CGFloat(height))))
  }

  /// Room left between the panel and its Card (for the line) and around everything else.
  static let cardGap: CGFloat = 56
  static let clearance: CGFloat = 16
  private static let step: CGFloat = 20
  private static let reach: CGFloat = 1400

  /// What a panel on this canvas must keep clear of.
  struct Obstacles {
    var rects: [CGRect]
    /// Wire paths, stroked to the clearance they need.
    var wires: [CGPath]

    init(rects: [CGRect], wires: [Path]) {
      self.rects = rects
      self.wires = wires.map {
        $0.cgPath.copy(strokingWithWidth: 2 * CardViewPanelPlacer.clearance, lineCap: .round, lineJoin: .round, miterLimit: 1)
      }
    }

    func blocks(_ r: CGRect) -> Bool {
      if rects.contains(where: { $0.intersects(r) }) { return true }
      let box = CGPath(rect: r, transform: nil)
      return wires.contains { $0.boundingBoxOfPath.intersects(r) && $0.intersects(box) }
    }
  }

  /// The free place for a panel of `size` closest to its Card: beside it to
  /// the right first, then below, to the left and above. Directly to the
  /// right when no free place is within reach.
  static func place(_ size: CGSize, beside card: CGRect, avoiding obstacles: Obstacles) -> CGRect {
    let keepOff = card.insetBy(dx: -cardGap, dy: -cardGap)
    let preferred = CGRect(origin: CGPoint(x: card.maxX + cardGap, y: card.minY), size: size)
    var candidates: [(score: CGFloat, rect: CGRect)] = []
    var y = card.minY - reach - size.height
    while y <= card.maxY + reach {
      var x = card.minX - reach - size.width
      while x <= card.maxX + reach {
        let r = CGRect(origin: CGPoint(x: x, y: y), size: size)
        if !r.intersects(keepOff) { candidates.append((score(r, card), r)) }
        x += step
      }
      y += step
    }
    candidates.sort { $0.score < $1.score }
    return candidates.first { !obstacles.blocks($0.rect) }?.rect ?? preferred
  }

  /// Nearer is better; at a like distance, the right, then below, left and above.
  private static func score(_ r: CGRect, _ card: CGRect) -> CGFloat {
    let gx = max(0, r.minX - card.maxX, card.minX - r.maxX)
    let gy = max(0, r.minY - card.maxY, card.minY - r.maxY)
    let side: CGFloat = r.minX >= card.maxX ? 0 : r.minY >= card.maxY ? 40 : r.maxX <= card.minX ? 80 : 120
    // Beside the Card, level with it rather than off to one end.
    let along = gx >= gy ? abs(r.minY - card.minY) : abs(r.minX - card.minX)
    return (gx * gx + gy * gy).squareRoot() + side + along * 0.25
  }

  /// The placements of the panels still open as panels, folded ones
  /// included. A panel that left (its session ended, or it is shown in a
  /// window) is placed anew when it comes back.
  static func kept(_ placements: [UUID: CanvasCardViewPlacement],
                   for panels: [CardViewOpening]) -> [UUID: CanvasCardViewPlacement] {
    let ids = Set(panels.map(\.id))
    return placements.filter { ids.contains($0.key) }
  }

  /// The panels drawn on the canvas: those not folded away.
  static func shown(_ panels: [CardViewOpening], folded: Set<UUID>) -> [CardViewOpening] {
    panels.filter { !folded.contains($0.id) }
  }

  /// Placements for the shown panels of this canvas that have none yet; the
  /// others (folded panels' too) are returned as they are. `cardRect` gives
  /// a Card's outline on this canvas (nil when it is not here yet).
  static func placeNew(_ openings: [CardViewOpening], in scope: CardKey,
                       placements: [UUID: CanvasCardViewPlacement],
                       cardRect: (CardKey) -> CGRect?,
                       obstacles: Obstacles) -> [UUID: CanvasCardViewPlacement] {
    var placements = placements
    var obstacles = obstacles
    obstacles.rects += placements.values.filter { $0.scope == scope }
      .map { $0.rect.insetBy(dx: -clearance, dy: -clearance) }
    for opening in openings where placements[opening.id] == nil && self.scope(of: opening.card) == scope {
      guard let card = cardRect(key(opening.card)) else { continue }
      let rect = place(size(width: opening.width, height: opening.height), beside: card, avoiding: obstacles)
      placements[opening.id] = CanvasCardViewPlacement(scope: scope, rect: rect)
      obstacles.rects.append(rect.insetBy(dx: -clearance, dy: -clearance))
    }
    return placements
  }
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
          .background(Color(nsColor: .textBackgroundColor))
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
          CardNodeView(data: data(item), flow: flow, zoom: camera.zoom, selected: selection.contains(item.id),
                       onToggleCollapse: { toggle(item.id) })
            .placeScaled(at: camera.screen(origin(item)), size: CardGeometry(data(item), flow: flow).size,
                         zoom: camera.zoom)
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
    items.reduce(CGRect.null) { $0.union(CGRect(origin: placed($1), size: CardGeometry(data($1), flow: flow).size)) }
  }

  private var wireDraws: [WireDraw] {
    var router = WireRouter(flow: flow, obstacles: items.map { CGRect(origin: origin($0), size: CardGeometry(data($0), flow: flow).size) })
    let routes = router.routeAll(wires.map { (a: point($0.from, $0.fromPort, output: true), b: point($0.to, $0.toPort, output: false)) })
    return zip(wires, routes).map { w, world in
      WireDraw(path: world.applying(camera.transform), style: w.active ? .active : .normal)
    }
  }

  private func data(_ item: CanvasItem) -> CardNodeData {
    var d = item.data
    d.portGrid = grid ? gridSize : nil
    return d
  }

  private func placed(_ item: CanvasItem) -> CGPoint {
    grid ? CGPoint(x: snapToGrid(item.origin.x, gridSize), y: snapToGrid(item.origin.y, gridSize)) : item.origin
  }

  private func origin(_ item: CanvasItem) -> CGPoint {
    var o = placed(item)
    if let d = drag, selection.contains(item.id) {
      o.x += d.offset.width / camera.zoom
      o.y += d.offset.height / camera.zoom
    }
    return o
  }

  /// A port in world units.
  private func point(_ id: String, _ port: String, output: Bool) -> CGPoint {
    guard let item = items.first(where: { $0.id == id }) else { return .zero }
    let o = origin(item), p = CardGeometry(data(item), flow: flow).port(port, output: output)
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
      var step = size * camera.zoom
      guard step.isFinite, step > 0 else { return }
      // Every coarser grid is a subset of the finer one, anchored at the
      // same world origin. Dot size and contrast stay constant.
      let minimumSpacing = min(size, 12)
      while step < minimumSpacing { step *= 2 }
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
  }
}

/// Sample Cards for previews of appearance settings.
enum PreviewSamples {
  static func items() -> [CanvasItem] {
    func port(_ n: String) -> PortData { PortData(name: n, typeName: "t.Number") }
    let input = CardNodeData(title: String(localized: "初期値"), kind: .code, summary: nil,
                             inputs: [], outputs: [port("value")],
                             state: .succeeded, invocation: 1)
    var step = CardNodeData(title: String(localized: "n / 2"), kind: .code,
                            summary: String(localized: "偶数を半分にする"), inputs: [port("number")], outputs: [port("next")],
                            state: .running, invocation: 3, runContinues: true)
    step.stage = StageProgress(items: [
      .init(stage: .build, status: .reused), .init(stage: .execute, status: .running), .init(stage: .validate, status: .pending),
    ], workerReused: true)
    let gate = CardNodeData(title: String(localized: "合流"), kind: .merge, summary: nil,
                            inputs: [port("value"), port("next")], outputs: [port("out")])
    let out = CardNodeData(title: String(localized: "結果"), kind: .code, summary: nil,
                           inputs: [port("in")], outputs: [], state: .succeeded, invocation: 1)
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
    CanvasWire(from: "gate", fromPort: "out", to: "out", toPort: "in"),
  ]
}
