import AppKit
import SwiftUI
import TsugumiRPC

/// Builds the canvas data of Cards from a project session (and, for previews
/// in Card settings, from unapplied working copies).
extension ProjectSession {
  /// The direction this project's Cards are laid out in (layout.json
  /// "flow"; projects without it follow the app's default).
  /// Without one, it is read from where the wired Cards sit (mostly to the
  /// right of their sources: left to right), else the app's default.
  var flow: FlowDirection {
    if let f = FlowDirection(rawValue: layout["flow"]?.string ?? "") { return f }
    var across = 0.0, down = 0.0
    for c in connections(in: []) {
      guard let f = c["from"]?["card"]?.string, let t = c["to"]?["card"]?.string else { continue }
      let a = position(f), b = position(t)
      across += abs(b.x - a.x)
      down += abs(b.y - a.y)
    }
    if across + down > 0 { return across > down ? .leftToRight : .topToBottom }
    return AppPreferences.shared.flowDirection
  }

  func setFlow(_ f: FlowDirection) { edit { $0.layout["flow"] = .string(f.rawValue) } }

  /// Ports for drawing: resolved ports when available, else declared inputs.
  func ports(_ k: CardKey) -> (inputs: [String], outputs: [String]) {
    let d = portData(k)
    return (d.inputs.map(\.name), d.outputs.map(\.name))
  }

  func portData(_ k: CardKey, definition: JSONValue? = nil, meta: JSONValue? = nil) -> (inputs: [PortData], outputs: [PortData]) {
    let def = definition ?? cardDefinition(k)
    let m = meta ?? cardMeta(k)
    func note(_ list: String, _ name: String) -> String? {
      m?[list]?.array?.first { $0["name"]?.string == name }?["description"]?.string
    }
    // A Draft's ports change with its wires, before they are resolved again.
    let draft = kind(k) == .draft
    if definition == nil, !draft, let r = resolution[k] {
      return (r.inputs.map { PortData(name: $0.name, typeName: $0.rootType, optional: $0.optional, note: note("inputs", $0.name)) },
              r.outputs.map { PortData(name: $0.name, typeName: $0.rootType, optional: $0.optional, note: note("outputs", $0.name)) })
    }
    // Not resolved yet (unsaved) or a working copy: declared ports.
    let ins = (def?["inputs"]?.array ?? []).compactMap { i -> PortData? in
      guard let n = i["name"]?.string else { return nil }
      return PortData(name: n, typeName: resolution[k]?.inputs.first { $0.name == n }?.rootType,
                      optional: i["optional"]?.bool ?? false, note: note("inputs", n))
    }
    // A Draft's resolved outputs beyond the declared ones are those of its _outputs.fbs.
    let typed = !draft || definitionDir(k).map { hasSource($0 + "_outputs.fbs") } ?? false
    var outs = typed ? resolution[k]?.outputs.map { PortData(name: $0.name, typeName: $0.rootType, optional: $0.optional, note: note("outputs", $0.name)) } ?? [] : []
    if draft || outs.isEmpty {
      for o in m?["outputs"]?.array ?? [] {
        if let n = o["name"]?.string, !outs.contains(where: { $0.name == n }) {
          outs.append(PortData(name: n, typeName: resolution[k]?.outputs.first { $0.name == n }?.rootType, note: o["description"]?.string))
        }
      }
    }
    return (ins, outs)
  }

  /// Params edited directly on input Cards.
  func inlineParams(_ k: CardKey, definition: JSONValue? = nil) -> [ParamData] {
    guard kind(k) == .input, let def = definition ?? cardDefinition(k) else { return [] }
    return (def["params"]?.array ?? []).compactMap { p -> ParamData? in
      guard let name = p["name"]?.string, let type = p["type"]?.string,
            ["int64", "uint64", "double", "bool", "string"].contains(type) else { return nil }
      let v = p["data"]
      let text: String
      switch v {
      case .string(let s)?: text = s
      case .number(let n)?: text = n
      case .bool(let b)?: text = b ? "true" : "false"
      default: text = ""
      }
      return ParamData(name: name, type: type, value: text)
    }
  }

  func cardDescription(_ k: CardKey) -> String? {
    if let d = cardMeta(k)?["description"]?.string, !d.isEmpty { return d }
    return nil
  }

  /// What a Card says about itself on hover: a Draft's summary and content.
  func cardDetails(_ k: CardKey) -> [String] {
    var lines = cardDescription(k).map { [$0] } ?? []
    if kind(k) == .draft, let c = cardMeta(k)?["content"]?.string, !c.isEmpty { lines.append(c) }
    lines += openComments(k).map {
      $0.author == "agent" ? String(localized: "コメント（Agent）: \($0.text)") : String(localized: "コメント: \($0.text)")
    }
    return lines
  }

  /// Whether a Card does not run because it needs a Draft's output: a
  /// required input fed only by Drafts or Cards waiting on one (the core
  /// leaves the same Cards out of a run).
  func waitsOnDraft(_ k: CardKey) -> Bool {
    let scope = Array(path(k).dropLast())
    return draftWaiting(in: scope).contains(path(k).last ?? k)
  }

  /// Card ids of a scope that do not run because of a Draft.
  func draftWaiting(in scope: [String]) -> Set<String> {
    let ids = cards(in: scope).compactMap { $0["id"]?.string }
    var waits = Set(ids.filter { kind(key(scope + [$0])) == .draft })
    guard !waits.isEmpty else { return [] }
    let conns = connections(in: scope)
    var changed = true
    while changed {
      changed = false
      for id in ids where !waits.contains(id) {
        let k = key(scope + [id])
        for p in resolution[k]?.inputs ?? [] where !p.optional {
          let from = conns.filter { $0["to"]?["card"]?.string == id && $0["to"]?["port"]?.string == p.name }
            .compactMap { $0["from"]?["card"]?.string }
          if !from.isEmpty && from.allSatisfy(waits.contains) {
            waits.insert(id)
            changed = true
            break
          }
        }
      }
    }
    return waits
  }

  /// One line describing the latest result of a Card.
  func resultSummary(_ k: CardKey) -> String? {
    if let s = cardStates[k], s.state == .failed { return s.failure ?? String(localized: "失敗") }
    if let s = cardStates[k], s.state == .blocked { return s.failure ?? String(localized: "入力が届きません") }
    return displays[k]?.summary
  }

  /// Stage strip of the latest invocation (nil before any run of the Card),
  /// from the stages the core recorded. Stages the invocation did not reach
  /// are "pending" while it runs and "not run" once it ended.
  func stageProgress(_ k: CardKey) -> StageProgress? {
    guard let s = cardStates[k], s.invocation > 0 else { return nil }
    let ended = [.succeeded, .failed, .cancelled, .skipped, .blocked].contains(s.state)
    // One shown stage covers several of the core's (build: SDK, Nix
    // evaluation, build; validate: check and save).
    let items = Stage.applicable(code: kind(k) == .code).map { shown -> StageProgress.Item in
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
    return StageProgress(items: items, workerReused: s.workerReused)
  }

  /// State of a Card in Card from the Cards inside it: failed or running
  /// when one of them is, succeeded when every one that ran succeeded.
  func compositeState(_ k: CardKey) -> Tsugumi_Data_CardState? {
    let inner = cardStates.filter { $0.key.hasPrefix(k + "/") }.map(\.value.state)
    guard !inner.isEmpty else { return nil }
    if inner.contains(.failed) { return .failed }
    if inner.contains(.cancelled) { return .cancelled }
    if inner.contains(where: { [.running, .preparing].contains($0) }) { return .running }
    if runActive && inner.contains(where: { [.waiting, .ready].contains($0) }) { return .running }
    if inner.contains(.blocked) { return .blocked }
    return .succeeded
  }

  /// The inner Card and port an outer output of a Card in Card comes from.
  func boundarySource(_ k: CardKey, output port: String) -> (CardKey, String)? {
    guard let ep = boundary(k)?["outputs"]?[port], let c = ep["card"]?.string, let p = ep["port"]?.string else { return nil }
    return (key(path(k) + [c]), p)
  }

  func nodeData(_ k: CardKey, definition: JSONValue? = nil, meta: JSONValue? = nil,
                collapsed: Bool? = nil, outputs: [PortData]? = nil) -> CardNodeData {
    let kind = self.kind(k)
    let state = cardStates[k]
    let diags = resolution[k]?.diagnostics ?? []
    let ports = portData(k, definition: definition, meta: meta)
    let def = definition ?? cardDefinition(k)
    // The name is the Card's id.
    let title = path(k).last ?? k
    let m = meta ?? cardMeta(k)
    var summary = m?["description"]?.string
    // What a Draft shows on its Card is what it says it will do.
    if kind == .draft, let content = m?["content"]?.string, !content.isEmpty { summary = content }
    if summary?.isEmpty ?? true { summary = nil }
    let editing = kind == .draft && inlineDraft?.card == k
    let display = (displays[k] != nil || displayFrame(k) != nil) && displayFrame(k)?["hidden"]?.bool != true
    return CardNodeData(
      title: title, kind: kind,
      symbol: (def?["symbol"]?.string).flatMap { $0.isEmpty ? nil : $0 },
      summary: summary, inputs: ports.inputs, outputs: outputs ?? ports.outputs,
      params: inlineParams(k, definition: definition),
      result: displays[k]?.summary ?? (state == nil && kind != .draft && waitsOnDraft(k) ? String(localized: "Draft のため実行しない") : nil),
      resultFailed: false,
      state: kind == .subgraph ? compositeState(k) : state?.state, stage: stageProgress(k),
      invocation: state?.invocation ?? 0, runContinues: runActive,
      failure: state.flatMap { $0.state == .failed || $0.state == .blocked ? ($0.failure ?? $0.state.label) : nil },
      errors: diags.filter(\.error).map(\.message), warnings: diags.filter { !$0.error }.map(\.message),
      collapsed: !editing && (collapsed ?? self.collapsed(k)), hasDisplay: display, permissions: CardPermission.of(def),
      draftEditing: editing, untitled: draftUntitled(k),
      portGrid: AppPreferences.shared.showGrid ? AppPreferences.shared.gridSize : nil,
      openComments: CardComment.list(in: m).filter { !$0.resolved }.count)
  }

  /// Where a Card is drawn: with the grid on, one saved off the grid (placed
  /// before the grid was on, or under another spacing) is shown on the nearest
  /// line, so its ports are on the grid too. Nothing is saved until it is moved.
  func placedPosition(_ k: CardKey) -> CGPoint {
    let p = position(k), prefs = AppPreferences.shared
    return prefs.showGrid ? CGPoint(x: snapToGrid(p.x, prefs.gridSize), y: snapToGrid(p.y, prefs.gridSize)) : p
  }

  func geometry(_ k: CardKey) -> CardGeometry { CardGeometry(nodeData(k), flow: flow) }
}

extension DisplaySnapshot {
  var summary: String {
    switch value {
    case .text(let t): return t.split(separator: "\n").first.map(String.init) ?? ""
    case .markdown(let t):
      // The first line with text, without Markdown marks.
      let line = t.split(separator: "\n").first { !$0.trimmingCharacters(in: .whitespaces).isEmpty } ?? ""
      return line.trimmingCharacters(in: CharacterSet(charactersIn: "#>*-` ")).replacingOccurrences(of: "**", with: "")
    case .scalar(let v, let l): return "\(l) = \(v.formatted(.number.precision(.significantDigits(6))))"
    case .lineChart(let s): return String(localized: "折れ線 \(s.count) 系列 · \(s.map(\.x.count).max() ?? 0) 点")
    case .table(let cols, let rows): return String(localized: "表 \(cols.count) 列 × \(rows.count) 行")
    case .image(_, _, let alt): return alt.isEmpty ? String(localized: "画像") : alt
    case .points(let x, _, _, _): return String(localized: "点群 \(x.count) 点")
    case .module(let name, let json): return Self.plotTitle(name, json) ?? String(localized: "モジュール \(name)")
    }
  }

  /// A Plotly figure's or Vega-Lite chart's title (as an image shows its alt
  /// text), or what it is when it has none.
  static func plotTitle(_ name: String, _ json: String) -> String? {
    guard ModuleViewer.modules.contains(name) else { return nil }
    let v = try? JSONValue.parse(json)
    let title = name == "plotly" ? v?["layout"]?["title"] : v?["title"]
    if let t = title?.string ?? title?["text"]?.string, !t.isEmpty { return t }
    return name == "plotly" ? String(localized: "Plotly のグラフ") : String(localized: "Vega-Lite のグラフ")
  }
}

/// Integers keep their exact decimal spelling (64 bit safe); the core
/// validates types and ranges and reports what it rejects.
func parseParam(_ s: String, _ type: String) -> JSONValue {
  let t = s.trimmingCharacters(in: .whitespaces)
  switch type {
  case "int64", "uint64": return t.range(of: #"^-?[0-9]+$"#, options: .regularExpression) != nil ? .number(t) : .string(t)
  case "double": return Double(t) != nil && !t.isEmpty ? .number(t) : .string(t)
  case "bool": return .bool(t == "true")
  case "string": return .string(s)
  default: return (try? JSONValue.parse(t)) ?? .string(s)
  }
}
