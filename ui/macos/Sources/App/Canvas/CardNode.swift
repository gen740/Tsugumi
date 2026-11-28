import AppKit
import SwiftUI
import TsugumiRPC

/// Builds the canvas data of Cards from a project session (and, for previews
/// in Card settings, from unapplied working copies).
extension ProjectSession {
  /// Ports for drawing: resolved ports when available, else declared inputs.
  func ports(_ k: CardKey) -> (inputs: [String], outputs: [String]) {
    let d = portData(k)
    return (d.inputs.map(\.name), d.outputs.map(\.name))
  }

  func portData(_ k: CardKey, definition: JSONValue? = nil) -> (inputs: [PortData], outputs: [PortData]) {
    let def = definition ?? cardDefinition(k)
    let draft = definitionKind(def) == .draft
    if draft || definitionKind(def) == .invalid { return ([], []) }
    if kind(k) == .collect {
      let r = resolution[k]
      return ([PortData(name: "in", typeName: r?.inputs.first { $0.name == "in" }?.typeName)],
              [PortData(name: "out", typeName: r?.outputs.first { $0.name == "out" }?.typeName)])
    }
    if definition == nil, !draft, let r = resolution[k] {
      return (r.inputs.map { PortData(name: $0.name, typeName: $0.typeName) },
              r.outputs.map { PortData(name: $0.name, typeName: $0.typeName) })
    }
    // Not resolved yet (unsaved) or a working copy: declared ports.
    let ins = (def?["inputs"]?.array ?? []).compactMap { i -> PortData? in
      guard let n = i["name"]?.string else { return nil }
      return PortData(name: n, typeName: resolution[k]?.inputs.first { $0.name == n }?.typeName)
    }
    let outs = resolution[k]?.outputs.map { PortData(name: $0.name, typeName: $0.typeName) } ?? []
    return (ins, outs)
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

  /// Whether a wire is drawn dotted: either end is a Draft, with no code
  /// behind it yet (`to` is nil at the bar of a Card in Card's outputs or inputs).
  func isDraftWire(from: CardKey, to: CardKey?) -> Bool {
    kind(from) == .draft || to.map { kind($0) == .draft } ?? false
  }

  /// Whether a Card does not run because it needs a Draft's output: a
  /// connected input fed by a Draft or a Card waiting on one (the core
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
        let sources = conns.filter { $0["to"]?["card"]?.string == id }
          .compactMap { $0["from"]?["card"]?.string }
        let blocked = kind(k) == .merge
          ? !sources.isEmpty && sources.allSatisfy(waits.contains)
          : sources.contains(where: waits.contains)
        if blocked {
          waits.insert(id)
          changed = true
        }
      }
    }
    return waits
  }

  /// Stages of the latest invocation (nil before any run of the Card).
  func stageProgress(_ k: CardKey) -> StageProgress? {
    guard let s = cardStates[k], s.invocation > 0 else { return nil }
    return .of(s, code: kind(k) == .code)
  }

  // Presentation: each UI combines the inner Cards' states itself, not hostd.
  /// State of a Card in Card from the Cards inside it: failed or running
  /// when one of them is, succeeded when every one that ran succeeded.
  func compositeState(_ k: CardKey) -> Tsugumi_Data_CardState? {
    let inner = Set(cardStates.keys).union(resolution.keys).filter { $0.hasPrefix(k + "/") }.compactMap(canvasCardState)
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

  func boundaryInputSource(_ scope: [String], port: String) -> (CardKey, String)? {
    guard let container = scope.last else { return nil }
    let parent = Array(scope.dropLast())
    let links = connections(in: parent).filter { $0["to"]?["card"]?.string == container && $0["to"]?["port"]?.string == port }
    guard links.count == 1, let from = links[0]["from"]?["card"]?.string, let output = links[0]["from"]?["port"]?.string else { return nil }
    return (key(parent + [from]), output)
  }

  func nodeData(_ k: CardKey, definition: JSONValue? = nil, notes: JSONValue? = nil,
                collapsed: Bool? = nil, outputs: [PortData]? = nil) -> CardNodeData {
    let kind = definition.map(definitionKind) ?? self.kind(k)
    let state = cardStates[k]
    let batch = executionBatch(k)
    let executionList = batch?.executionList == true
    let empty = executionList && batch?.complete == true && batch?.failed == false && batch?.invocationCount == 0
    let diags = resolution[k]?.diagnostics ?? []
    let ports = portData(k, definition: definition)
    let def = definition ?? cardDefinition(k)
    let m = notes ?? cardMeta(k)
    let editedTitle = m?["title"]?.string
    let title = editedTitle.flatMap { $0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : $0 } ?? displayName(k)
    var summary = m?["description"]?.string
    // What a Draft shows on its Card is what it says it will do.
    if kind == .draft, let content = m?["content"]?.string, !content.isEmpty { summary = content }
    let errors = diags.filter(\.error).map(\.message)
    let invalid = kind == .invalid && errors.isEmpty ? [String(localized: "Card の定義を読めません")] : []
    if summary?.isEmpty ?? true { summary = nil }
    let editing = kind == .draft && inlineDraft?.card == k
    return CardNodeData(
      title: title, kind: kind,
      symbol: kind.displaySymbol(def?["symbol"]?.string),
      summary: summary, inputs: visualPorts(k, ports.inputs, output: false), outputs: visualPorts(k, outputs ?? ports.outputs, output: true),
      result: state == nil && kind != .draft && waitsOnDraft(k) ? String(localized: "Draft のため実行しない") : nil,
      activity: state?.transfer ?? state.flatMap(ExecutionActivityText.init)?.short ?? state?.executionState?.pendingLabel,
      state: kind == .subgraph ? compositeState(k) : canvasCardState(k), executionList: executionList, stage: empty ? nil : stageProgress(k),
      invocation: empty ? 0 : state?.invocation ?? 0, runContinues: runActive, runMark: (state != nil || batch != nil ? runMark(runStatus) : nil),
      failure: state.flatMap { $0.state == .failed || $0.state == .blocked ? ($0.failure ?? $0.state.label) : nil },
      notStarted: state.flatMap { $0.state == .failed ? $0.notStarted : nil },
      // The core's clock, on this Mac's.
      endedAt: state?.endedAt.map { Date(timeIntervalSince1970: Double($0 + (coreClockOffset ?? 0)) / 1000) },
      seconds: state.flatMap { s in s.endedAt.flatMap { e in s.startedAt.map { Double(max(0, e - $0)) / 1000 } } },
      errors: errors + invalid, warnings: diags.filter { !$0.error }.map(\.message),
      collapsed: !editing && (collapsed ?? self.collapsed(k)), permissions: CardPermission.of(def),
      draftEditing: editing,
      portGrid: AppPreferences.shared.showGrid ? AppPreferences.shared.gridSize : nil,
      openComments: CardComment.list(in: m).filter { !$0.resolved }.count,
      liveness: state.flatMap(livenessText),
      activityDetail: state.flatMap(ExecutionActivityText.init)?.text,
      worker: state?.executionService.flatMap { $0.isEmpty ? nil : $0 == "local" ? String(localized: "プロジェクトの実行環境") : $0 })
  }

  /// Where a Card is drawn: with the grid on, one saved off the grid (placed
  /// before the grid was on, or under another spacing) is shown on the nearest
  /// line, so its ports are on the grid too. Nothing is saved until it is moved.
  func placedPosition(_ k: CardKey) -> CGPoint {
    let p = position(k), prefs = AppPreferences.shared
    return prefs.showGrid ? CGPoint(x: snapToGrid(p.x, prefs.gridSize), y: snapToGrid(p.y, prefs.gridSize)) : p
  }

  // Shown ports and their order (layout.bin inputOrder / outputOrder) are
  // presentation: each UI works them out itself rather than asking hostd.
  private func visualPorts(_ k: CardKey, _ ports: [PortData], output: Bool) -> [PortData] {
    guard [.code, .draft, .subgraph].contains(kind(k)) else { return ports }
    let saved = layout(of: Array(path(k).dropLast()))["cards"]?[layoutKey(k)]?[output ? "outputOrder" : "inputOrder"]?.array?.compactMap(\.string) ?? []
    return orderedPorts(ports, saved: saved)
  }

  func geometry(_ k: CardKey) -> CardGeometry { CardGeometry(nodeData(k)) }
}

func orderedPorts(_ ports: [PortData], saved: [String]) -> [PortData] {
  var rank: [String: Int] = [:]
  for (i, name) in saved.enumerated() where rank[name] == nil { rank[name] = i }
  return ports.enumerated().sorted {
    (rank[$0.element.name] ?? saved.count + $0.offset) < (rank[$1.element.name] ?? saved.count + $1.offset)
  }.map(\.element)
}
