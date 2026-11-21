import TsugumiAgentClient
import AppKit
import FlatBuffers
import Foundation
import TsugumiRPC

/// What a Card is, for choosing what the canvas and the editor offer.
enum CardKind: String {
  case trigger, input, branch, fork, merge, collect, draft, subgraph, code

  var editsValueOnCanvas: Bool { self == .input }

  var label: String {
    switch self {
    case .trigger: return String(localized: "開始")
    case .input: return String(localized: "入力")
    case .branch: return String(localized: "分岐")
    case .fork: return String(localized: "分配")
    case .merge: return String(localized: "合流")
    case .collect: return String(localized: "収集")
    case .draft: return "Draft"
    case .subgraph: return "Card in Card"
    case .code: return String(localized: "処理")
    }
  }

  var symbol: String {
    switch self {
    case .trigger: return "play.circle"
    case .input: return "slider.horizontal.3"
    case .branch: return "arrow.triangle.swap"
    case .fork: return "arrow.triangle.branch"
    case .merge: return "arrow.triangle.merge"
    case .collect: return "line.3.horizontal.decrease"
    case .draft: return "pencil.and.outline"
    case .subgraph: return "link"
    case .code: return "chevron.left.forwardslash.chevron.right"
    }
  }
}

/// What a _card.json says the Card is: its "type". A missing or unknown type
/// is a definition error the core reports; such a Card, like one without a
/// definition, shows as a Draft: no ports, no run.
func definitionKind(_ def: JSONValue?) -> CardKind {
  def?["type"]?.string.flatMap(CardKind.init(rawValue:)) ?? .draft
}

/// What a new Card starts from: a built-in Card or a scaffold. Every Card
/// gets a definition folder of its own (definitions/<id>/).
enum DefinitionTemplate: String, CaseIterable, Identifiable {
  case booleanInput = "input_bool"
  case python, cpp, go, rust, ruby, csharp, input, draft, trigger, branch, fork, merge, collect
  case report_rmarkdown, report_quarto
  /// Visualization starting points: ordinary Python Cards, one per plotting library.
  case matplotlib = "visualization_matplotlib"
  case plotly = "visualization_plotly"
  case altair = "visualization_altair"

  case marimo

  var id: String { rawValue }
  var isVisualization: Bool { self == .matplotlib || self == .plotly || self == .altair }
  var label: String {
    switch self {
    case .matplotlib: return String(localized: "Matplotlib（PNG / SVG / PDF）")
    case .plotly: return String(localized: "Plotly（対話型 HTML）")
    case .altair: return String(localized: "Altair（対話型 HTML）")
    case .draft: return String(localized: "Draft（未実装の計画）")
    case .marimo: return String(localized: "marimo ノートブック")
    case .report_rmarkdown: return String(localized: "R Markdown レポート（HTML）")
    case .report_quarto: return String(localized: "Quarto レポート（HTML）")
    case .python: return String(localized: "Python の処理")
    case .ruby: return String(localized: "Ruby の処理")
    case .cpp: return String(localized: "C++ の処理")
    case .go: return String(localized: "Go の処理")
    case .csharp: return String(localized: "C# の処理")
    case .rust: return String(localized: "Rust の処理")
    case .input: return String(localized: "入力値")
    case .booleanInput: return String(localized: "真偽値の入力")
    case .trigger: return String(localized: "開始")
    case .branch: return String(localized: "分岐")
    case .fork: return String(localized: "分配")
    case .merge: return String(localized: "合流")
    case .collect: return String(localized: "収集")
    }
  }
  var symbol: String {
    if self == .booleanInput { return CardKind.input.symbol }
    return isVisualization ? "chart.xyaxis.line" : (CardKind(rawValue: rawValue) ?? (rawValue.hasPrefix("report_") || [.python, .cpp, .go, .rust, .ruby, .csharp, .marimo].contains(self) ? .code : .draft)).symbol
  }
  /// The human name used when creating a Card; the core generates its identifier.
  var baseName: String {
    switch self {
    case .matplotlib, .plotly, .altair: return "visualization"
    case .python, .cpp, .go, .rust, .ruby, .csharp: return "card"
    case .trigger: return "start"
    case .booleanInput: return "condition"
    default: return rawValue
    }
  }
}

struct NoteColor {
  static let all: [(id: String, label: String)] = [
    ("yellow", String(localized: "黄")), ("blue", String(localized: "青")), ("green", String(localized: "緑")),
    ("pink", String(localized: "桃")), ("gray", String(localized: "灰")),
  ]
}

extension ProjectSession {
  // MARK: - Reading

  func kind(_ k: CardKey) -> CardKind { definitionKind(cardDefinitionDraft(k)?.card) }

  /// The human name; exact addresses keep the permanent identifier.
  func displayName(_ k: CardKey) -> String { cardMeta(k)?["name"]?.string.flatMap { $0.isEmpty ? nil : $0 } ?? path(k).last ?? k }

  /// A human-readable path. Tools and evidence links use the original segments.
  func displayPath(_ segments: [String]) -> String {
    segments.indices.map { displayName(key(Array(segments.prefix($0 + 1)))) }.joined(separator: "/")
  }

  func scopeName(_ scope: [String]) -> String {
    scope.isEmpty ? (project["name"]?.string ?? identity.url.lastPathComponent) : displayName(key(scope))
  }

  /// The Cards in Card of a scope, for the hierarchy.
  func childScopes(of scope: [String]) -> [[String]] {
    cards(in: scope).compactMap { c in
      guard let id = c["id"]?.string, cardExternal(key(scope + [id])) != nil else { return nil }
      return scope + [id]
    }
  }

  /// Definition folders of this project that no Card uses (copied from
  /// another project, or left over).
  var unplacedDefinitions: [String] {
    let used = Set(cards(in: []).compactMap { $0["id"]?.string })
    return Set(definitions.keys).subtracting(used).sorted()
  }

  /// Places a definition folder no Card uses as its Card (the same name).
  @discardableResult
  func placeDefinition(_ id: String, at point: CGPoint) -> CardKey {
    selectionOperation(.place, cards: [key([id])], in: [], at: point)
    return key([id])
  }

  private func selectionOperation(_ action: Tsugumi_Host_ProjectSelectionAction, cards: Set<CardKey>, notes: Set<String> = [], frames: Set<CardKey> = [], in scope: [String], connections: [JSONValue]? = nil, at point: CGPoint = .zero) {
    guard let prefix = projectPrefix(scope) else { return }
    let body = Tsugumi_Host_ProjectSelectionT()
    body.projectPath = prefix.trimmingCharacters(in: CharacterSet(charactersIn: "/")); body.action = action
    body.cards = cards.map { layoutKey($0) }.sorted(); body.notes = notes.sorted(); body.frames = frames.map { layoutKey($0) }.sorted()
    body.connectionsJson = connections.map { JSONValue.array($0).serialized() }; body.x = point.x; body.y = point.y; body.scene = layoutScene(scope)
    _ = enqueueOperation(.projectselection, body: body)
  }

  var isEditable: Bool { connected && !runActive }

  /// Source bytes as the draft sees them.
  func sourceText(_ p: String) async throws -> String {
    guard let rpc else { throw RPCError.disconnected }
    return String(decoding: try await rpc.readFile(path: p, atRev: rev), as: UTF8.self)
  }

  func collapsed(_ k: CardKey) -> Bool {
    layout(of: Array(path(k).dropLast()))["cards"]?[layoutKey(k)]?["collapsed"]?.bool ?? false
  }

  // MARK: - Cards and connections

  /// Runs an asynchronous canvas edit and presents any failure in the workbench.
  func performCardEdit(_ operation: @escaping @MainActor () async throws -> Void) {
    Task { do { try await operation() } catch { notice = "\(error)" } }
  }

  /// All new identifiers come from the core, including unsaved nested projects.
  func newCardId(_ name: String, in scope: [String] = []) async throws -> String {
    guard let prefix = projectPrefix(scope) else { throw RPCFailure(kind: .invalidPath, diagnostic: "unknown Card scope") }
    let id: String
    if let cardIdProvider { id = try await cardIdProvider(name, prefix) }
    else {
      guard let rpc else { throw RPCError.disconnected }
      id = try await rpc.cardId(name: name, projectPath: prefix).id
    }
    guard projectPrefix(scope) == prefix else { throw RPCFailure(kind: .invalidPath, diagnostic: "Card scope changed") }
    return id
  }

  /// The host assembles the template, sources and placement in one operation.
  @discardableResult
  func addCard(template t: DefinitionTemplate, in scope: [String], at point: CGPoint, name: String? = nil,
               automaticPlacement: Bool = false) async throws -> CardKey {
    let id = try await newCardId(name ?? t.baseName, in: scope)
    let body = Tsugumi_Host_ProjectCreateCardT()
    body.projectPath = (projectPrefix(scope) ?? "").trimmingCharacters(in: CharacterSet(charactersIn: "/"))
    body.id = id; body.template = t.rawValue; body.name = name ?? t.baseName
    body.x = point.x; body.y = point.y; body.automaticPlacement = automaticPlacement
    try await operate(.projectcreatecard, body: body)
    forgetResult(key(scope + [id]))
    return key(scope + [id])
  }

  @discardableResult
  func placeExternalCopy(from: String, in scope: [String], at point: CGPoint) async -> CardKey? {
    let base = from.split(separator: "/").last.map(String.init) ?? "sub"
    do {
      let id = try await newCardId(base, in: scope)
      let item = Tsugumi_Host_ProjectCopyItemT()
      item.from = from; item.id = id; item.name = base; item.x = point.x; item.y = point.y
      try await copyCards([item], in: scope)
      return key(scope + [id])
    } catch { notice = error.localizedDescription; return nil }
  }

  @discardableResult
  func placeDefinitionCopy(prefix: String, id: String, in scope: [String], at point: CGPoint) async -> CardKey? {
    do {
      let newId = try await newCardId(id, in: scope)
      let item = Tsugumi_Host_ProjectCopyItemT()
      item.from = prefix + "definitions/\(id)/"; item.id = newId; item.x = point.x; item.y = point.y
      try await copyCards([item], in: scope)
      return key(scope + [newId])
    } catch { notice = error.localizedDescription; return nil }
  }

  @discardableResult
  func importProject(from folder: URL, in scope: [String], at point: CGPoint) async throws -> CardKey {
    let id = try await newCardId(folder.lastPathComponent, in: scope)
    let body = Tsugumi_Host_ProjectImportT()
    body.projectPath = (projectPrefix(scope) ?? "").trimmingCharacters(in: CharacterSet(charactersIn: "/"))
    body.path = folder.path; body.id = id; body.x = point.x; body.y = point.y
    try await operate(.projectimport, body: body)
    forgetResult(key(scope + [id]))
    return key(scope + [id])
  }

  private func copyCards(_ items: [Tsugumi_Host_ProjectCopyItemT], in scope: [String], connections: Bool = false) async throws {
    let body = Tsugumi_Host_ProjectCopyT()
    body.projectPath = (projectPrefix(scope) ?? "").trimmingCharacters(in: CharacterSet(charactersIn: "/"))
    body.items = items.map(Optional.some); body.copyConnections = connections
    try await operate(.projectcopy, body: body)
    for item in items { forgetResult(key(scope + [item.id])) }
  }

  func duplicate(_ keys: Set<CardKey>, in scope: [String]) async -> Set<CardKey> {
    guard !keys.isEmpty, keys.allSatisfy({ Array(path($0).dropLast()) == scope }), let prefix = projectPrefix(scope) else { return [] }
    do {
      var items: [Tsugumi_Host_ProjectCopyItemT] = []
      for k in keys.sorted() {
        guard let old = path(k).last else { continue }
        let item = Tsugumi_Host_ProjectCopyItemT()
        item.from = prefix + "definitions/\(old)/"; item.id = try await newCardId(displayName(k), in: scope)
        items.append(item)
      }
      try await copyCards(items, in: scope, connections: true)
      return Set(items.map { key(scope + [$0.id]) })
    } catch { notice = error.localizedDescription; return [] }
  }

  /// Names are independent of identifiers and may be shared by several Cards.
  func cardNameProblem(_ k: CardKey, _ raw: String) -> String? {
    let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
    return name.isEmpty || name.utf8.count > 4096 ? String(localized: "名前を入力してください（4096 バイトまで）。") : nil
  }

  // MARK: - A Draft written on the canvas

  /// Whether a Draft has no name of its own: its id is one made for it.
  func draftUntitled(_ k: CardKey) -> Bool {
    kind(k) == .draft && displayName(k) == "draft"
  }

  /// Starts writing a Draft's name and content on its Card.
  func startDraftEdit(_ k: CardKey, revision existing: RevisionEdit? = nil) async {
    if existing == nil, let current = inlineDraft {
      guard current.card != k else { return }
      _ = await finishDraftEdit()
      guard inlineDraft == nil else { return }
    }
    let name = draftUntitled(k) ? "" : displayName(k), content = cardMeta(k)?["content"]?.string ?? ""
    let revision = existing ?? RevisionEdit()
    revision.reload(rev)
    inlineDraft = InlineDraft(card: k, name: name, content: content, initialName: name, initialContent: content, initialMeta: cardMeta(k), revision: revision)
  }

  /// Why the name typed on a Draft's Card cannot be taken (no name is fine).
  func draftNameProblem(_ k: CardKey, _ name: String) -> String? {
    name.trimmingCharacters(in: .whitespaces).isEmpty ? nil : cardNameProblem(k, name)
  }

  /// Keeps what was written. A name the Card cannot take stays as it was, with
  /// a notice. The Card's permanent key when its name changed.
  func finishDraftEdit(overwrite: Bool = false) async -> CardKey? {
    if let task = draftFinishTask { return await task.value }
    guard inlineDraft != nil else { return nil }
    let task = Task { @MainActor in
      var renamed: CardKey?
      repeat {
        let result = await self.finishDraftEditOnce(overwrite: overwrite)
        renamed = result ?? renamed
        guard let current = self.inlineDraft else { break }
        if current.revision.conflictRev != nil || current.revision.error != nil { break }
      } while true
      self.draftFinishTask = nil
      return renamed
    }
    draftFinishTask = task
    return await task.value
  }

  private func finishDraftEditOnce(overwrite: Bool) async -> CardKey? {
    // Local canvas edits may still be saving when typing starts. Their deltas
    // advance this editor's base; another client's deltas never do.
    await save()
    guard let e = inlineDraft else { return nil }
    let contentChanged = e.content != e.initialContent, nameChanged = e.name != e.initialName
    guard instance(e.card) != nil, contentChanged || nameChanged else { inlineDraft = nil; return nil }
    let trimmed = e.name.trimmingCharacters(in: .whitespacesAndNewlines)
    let name = trimmed.isEmpty ? "draft" : trimmed
    if nameChanged, let problem = cardNameProblem(e.card, name) { notice = problem; e.revision.error = problem; return nil }
    guard let dir = definitionDir(e.card) else { return nil }
    let before = e.revision.jsonBase(dir + "_meta.json", initial: e.initialMeta, overwrite: overwrite)
    var after = before ?? .object([])
    if contentChanged { after["content"] = e.content.isEmpty ? nil : .string(e.content) }
    if nameChanged { after["name"] = .string(name) }
    let ok = await e.revision.apply(self) { s in
      s.appendEdits([s.jsonEdit(dir + "_meta.json", old: before, new: after)])
    }
    if ok, inlineDraft?.revision === e.revision {
      if inlineDraft?.name == e.name && inlineDraft?.content == e.content { inlineDraft = nil }
      else { inlineDraft?.initialName = e.name; inlineDraft?.initialContent = e.content; inlineDraft?.initialMeta = after }
    }
    return ok && nameChanged ? e.card : nil
  }

  func deleteCards(_ keys: Set<CardKey>, in scope: [String]) {
    selectionOperation(.delete, cards: keys, in: scope)
  }

  func connect(from: (CardKey, String), to: (CardKey, String), in scope: [String]) async throws {
    guard let fromId = path(from.0).last, let toId = path(to.0).last, let prefix = projectPrefix(scope), isEditable else { return }
    let body = Tsugumi_Host_ProjectConnectT()
    body.projectPath = prefix.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
    body.fromCard = fromId; body.fromPort = from.1; body.toCard = toId; body.toPort = to.1
    try await operate(.projectconnect, body: body)
  }

  func removeConnection(at index: Int, in scope: [String]) {
    let wires = connections(in: scope)
    guard wires.indices.contains(index) else { return }
    removeConnections([wires[index]], in: scope)
  }

  func removeConnections(_ list: [JSONValue], in scope: [String]) {
    if !list.isEmpty { selectionOperation(.removeconnections, cards: [], in: scope, connections: list) }
  }

  func disconnect(_ k: CardKey, inputs: Bool, in scope: [String]) {
    selectionOperation(inputs ? .disconnectinputs : .disconnectoutputs, cards: [k], in: scope)
  }

  func setCollapsed(_ keys: Set<CardKey>, _ value: Bool) {
    guard !keys.isEmpty else { return }
    queueFields { [self] in
      keys.compactMap { k in
        let scope = Array(path(k).dropLast())
        guard let prefix = projectPrefix(scope) else { return nil }
        let entry = layout(of: scope)["cards"]?[layoutKey(k)]
        if entry == nil {
          let at = position(k)
          var placed: JSONValue = .object([("x", .real(at.x)), ("y", .real(at.y))])
          if value { placed["collapsed"] = .bool(true) }
          return fieldEdit(prefix + "layout.json", [.string("cards"), .string(layoutKey(k))], old: nil, new: placed)
        }
        return fieldEdit(prefix + "layout.json", [.string("cards"), .string(layoutKey(k)), .string("collapsed")],
                         old: entry?["collapsed"], new: value ? .bool(true) : nil)
      }
    }
  }

  func setParam(_ k: CardKey, _ name: String, _ value: JSONValue?) {
    guard let dir = definitionDir(k) else { return }
    queueFields { [self] in
      let definition = cardDefinition(k), match = JSONValue.object([("name", .string(name))])
      if let param = (definition?["params"]?.array ?? []).first(where: { $0["name"]?.string == name }) {
        return [fieldEdit(dir + "_card.json", [.string("params"), match, .string("data")], old: param["data"], new: value)]
      }
      throw RPCFailure(kind: .invalidRequest, diagnostic: "Unknown param: \(name)")
    }
  }

  /// Mutates the value inside the caller's transaction, without an autosave.
  func updateParam(_ k: CardKey, _ name: String, _ value: JSONValue?, initial: InlineParamEdit.ParamBase? = nil) {
    if let initial {
      if let dir = definitionDir(k) {
        let selector: [JSONValue] = [.string("params"), .object([("name", .string(name))])]
        appendEdits([fieldEdit(dir + "_card.json", selector + [.string("data")], old: initial.parameter?["data"], new: value)])
      }
      return
    }
    updateCard(k) { c in
      var params = c["params"]?.array ?? []
      if let i = params.firstIndex(where: { $0["name"]?.string == name }) {
        params[i]["data"] = value
      }
      c["params"] = .array(params)
    }
  }

  /// Moves the selected Cards into a new Card in Card: a new external project
  /// (externals/<name>/) with their definitions and the wires between them.
  /// Wires crossing the selection become its boundary ports, so the graph
  /// The host moves the complete selected definition/external closure.
  @discardableResult
  func makeSubgraph(_ keys: Set<CardKey>, in scope: [String], name: String = "sub") async -> CardKey? {
    guard !keys.isEmpty, keys.allSatisfy({ Array(path($0).dropLast()) == scope }), let prefix = projectPrefix(scope) else { return nil }
    do {
      let id = try await newCardId(name, in: scope)
      let body = Tsugumi_Host_ProjectMakeSubgraphT()
      body.projectPath = prefix.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
      body.cards = keys.compactMap { path($0).last }.sorted(); body.id = id; body.name = name
      try await operate(.projectmakesubgraph, body: body)
      return key(scope + [id])
    } catch { notice = error.localizedDescription; return nil }
  }

  // MARK: - Layout: notes, groups, output frames

  func reserveReference(_ kind: String, scope: String = "") async throws -> String {
    let project = projectId
    var occupied: [String] = []
    if kind == "comment" { occupied = comments(scope).map(\.id) }
    else {
      var queue: [[String]] = [[]]
      while !queue.isEmpty {
        let at = queue.removeFirst()
        occupied += (layout(of: at)[kind == "note" ? "notes" : "groups"]?.array ?? []).compactMap { $0["id"]?.string }
        queue += childScopes(of: at)
      }
    }
    if let referenceIdProvider { return try await referenceIdProvider(kind, scope, occupied) }
    guard let project else { throw RPCError.disconnected }
    let reply = try await CoreHostClient.shared.call("IdentifierReserveRequest", ["project_id": project, "kind": kind, "scope": scope, "occupied": occupied])
    guard projectId == project, connected, let id = reply["id"] as? String else { throw RPCError.disconnected }
    return id
  }

  func notes(in scope: [String]) -> [JSONValue] {
    (layout(of: scope)["notes"]?.array ?? []).filter { ($0["scope"]?.string ?? "") == "" }
  }

  func addNote(in scope: [String], at p: CGPoint) async throws -> String {
    let id = try await reserveReference("note")
    try await operateLayoutEntity(scope, "notes", id, old: nil, new: .object([
      ("id", .string(id)), ("scope", .string("")), ("text", .string(String(localized: "メモ"))), ("color", .string("yellow")),
      ("x", .real(p.x)), ("y", .real(p.y)), ("width", .real(240)), ("height", .real(120)),
    ]))
    return id
  }

  /// The scope whose layout holds a note or group (ids are unique).
  func scopeHolding(_ field: String, _ id: String) -> [String]? {
    func has(_ l: JSONValue) -> Bool { (l[field]?.array ?? []).contains { $0["id"]?.string == id } }
    if has(layout) { return [] }
    var queue: [[String]] = childScopes(of: [])
    while let sc = queue.first {
      queue.removeFirst()
      if has(layout(of: sc)) { return sc }
      queue += childScopes(of: sc)
    }
    return nil
  }

  func note(_ id: String) -> JSONValue? {
    guard let sc = scopeHolding("notes", id) else { return nil }
    return (layout(of: sc)["notes"]?.array ?? []).first { $0["id"]?.string == id }
  }

  private func operateLayoutEntity(_ scope: [String], _ field: String, _ id: String, old: JSONValue?, new: JSONValue?) async throws {
    guard let prefix = projectPrefix(scope) else { return }
    try await operateFields([fieldEdit(prefix + "layout.json", [.string(field), .object([("id", .string(id))])], old: old, new: new)])
  }

  func updateNote(_ id: String, _ fn: @escaping (inout JSONValue) -> Void) {
    queueFields { [self] in
      guard let scope = scopeHolding("notes", id), let prefix = projectPrefix(scope), let before = note(id) else {
        throw RPCFailure(kind: .notFound, diagnostic: "Unknown note: \(id)")
      }
      var after = before; fn(&after)
      return [fieldEdit(prefix + "layout.json", [.string("notes"), .object([("id", .string(id))])], old: before, new: after)]
    }
  }

  func removeNote(_ id: String) {
    queueFields { [self] in
      guard let scope = scopeHolding("notes", id), let prefix = projectPrefix(scope), let before = note(id) else {
        throw RPCFailure(kind: .notFound, diagnostic: "Unknown note: \(id)")
      }
      return [fieldEdit(prefix + "layout.json", [.string("notes"), .object([("id", .string(id))])], old: before, new: nil)]
    }
  }

  func noteRect(_ n: JSONValue) -> CGRect {
    CGRect(x: n["x"]?.double ?? 0, y: n["y"]?.double ?? 0, width: n["width"]?.double ?? 240, height: n["height"]?.double ?? 120)
  }

  func groups(in scope: [String]) -> [JSONValue] {
    (layout(of: scope)["groups"]?.array ?? []).filter { ($0["scope"]?.string ?? "") == "" }
  }

  func addGroup(_ keys: Set<CardKey>, in scope: [String], name: String = String(localized: "グループ")) async throws -> String {
    let id = try await reserveReference("group")
    try await operateLayoutEntity(scope, "groups", id, old: nil, new: .object([
      ("id", .string(id)), ("scope", .string("")), ("name", .string(name)), ("color", .string("blue")),
      ("cards", .array(keys.compactMap { path($0).last }.sorted().map { .string($0) })),
    ]))
    return id
  }

  /// Removes the group frame only; its Cards stay where they are.
  func ungroup(_ id: String) {
    queueFields { [self] in
      guard let scope = scopeHolding("groups", id), let prefix = projectPrefix(scope),
            let before = groups(in: scope).first(where: { $0["id"]?.string == id }) else {
        throw RPCFailure(kind: .notFound, diagnostic: "Unknown group: \(id)")
      }
      return [fieldEdit(prefix + "layout.json", [.string("groups"), .object([("id", .string(id))])], old: before, new: nil)]
    }
  }

  func displayFrame(_ k: CardKey) -> JSONValue? { layout(of: Array(path(k).dropLast()))["displays"]?[layoutKey(k)] }

  func updateDisplayFrame(_ k: CardKey, _ fn: @escaping (inout JSONValue) -> Void) {
    queueFields { [self] in
      let scope = Array(path(k).dropLast())
      guard let prefix = projectPrefix(scope) else { throw RPCError.disconnected }
      let before = displayFrame(k), r = displayRect(k, origin: placedPosition(k))
      var after = before ?? .object([("x", .real(r.minX)), ("y", .real(r.minY)), ("width", .real(r.width)), ("height", .real(r.height))]); fn(&after)
      return [fieldEdit(prefix + "layout.json", [.string("displays"), .string(layoutKey(k))], old: before, new: after)]
    }
  }

  /// Closes a frame; its Card stays. The layout keeps a frame only with its
  /// place and size, so one still drawn beside its Card gets them now.
  func closeDisplayFrame(_ k: CardKey) {
    let r = displayRect(k, origin: placedPosition(k))
    updateDisplayFrame(k) { f in
      f["x"] = .real(r.minX.rounded()); f["y"] = .real(r.minY.rounded())
      f["width"] = .real(r.width); f["height"] = .real(r.height); f["hidden"] = .bool(true)
    }
  }

  /// The Cards of a scope whose output frame shows: once their code has shown
  /// something (display SDK), until the frame is closed.
  func framedCards(in scope: [String]) -> [CardKey] {
    cards(in: scope).compactMap { $0["id"]?.string }.map { key(scope + [$0]) }.filter {
      (displays[$0] != nil || displayFrame($0) != nil) && displayFrame($0)?["hidden"]?.bool != true
    }
  }

  /// A frame without a place of its own is drawn beside its Card, so it goes
  /// where the Card goes and is not moved itself.
  func frameFollowsCard(_ k: CardKey, moving cards: Set<CardKey>) -> Bool {
    cards.contains(k) && displayFrame(k)?["x"]?.double == nil
  }

  /// What a marquee (world units) touches in a scope.
  func marqueeItems(_ r: CGRect, in scope: [String]) -> (cards: Set<CardKey>, notes: Set<String>, frames: Set<CardKey>) {
    let keys = cards(in: scope).compactMap { $0["id"]?.string }.map { key(scope + [$0]) }
    return (Set(keys.filter { r.intersects(CGRect(origin: placedPosition($0), size: geometry($0).size)) }),
            Set(notes(in: scope).filter { r.intersects(noteRect($0)) }.compactMap { $0["id"]?.string }),
            Set(framedCards(in: scope).filter { r.intersects(displayRect($0, origin: placedPosition($0))) }))
  }

  /// Moves a selection (world units) as one edit: one save, one undo. Layout
  /// only: no computation is re-resolved for a move. Ports around the moved Cards
  /// are then reordered in the same undo step when that removes wire crossings. With a grid (`snap`) the
  /// Cards go by whole grids from where they are drawn, and what moves with them
  /// goes by the same step, so the arrangement stays. Notes and frames alone
  /// land on whole numbers, as when dragged alone.
  @discardableResult
  func moveSelection(cards: Set<CardKey>, notes: Set<String>, frames: Set<CardKey>, by moved: CGSize, snap: Double?) -> Task<Void, Never> {
    let scope = cards.first.map { Array(path($0).dropLast()) } ?? frames.first.map { Array(path($0).dropLast()) } ?? scopeHolding("notes", notes.first ?? "") ?? []
    let body = Tsugumi_Host_ProjectMoveT()
    body.projectPath = (projectPrefix(scope) ?? "").trimmingCharacters(in: CharacterSet(charactersIn: "/"))
    body.cards = cards.map { layoutKey($0) }.sorted(); body.notes = notes.sorted(); body.frames = frames.map { layoutKey($0) }.sorted()
    body.dx = moved.width; body.dy = moved.height; body.snap = snap ?? 0; body.scene = layoutScene(scope)
    return enqueueOperation(.projectmove, body: body)
  }

  /// Deletes a selection as one edit. Notes go. A frame alone is closed and its
  /// Card stays (the frame's own Remove Frame); a deleted Card takes its frame with it.
  func deleteSelection(cards: Set<CardKey>, notes: Set<String>, frames: Set<CardKey>, in scope: [String]) {
    selectionOperation(.delete, cards: cards, notes: notes, frames: frames, in: scope)
  }

  /// Where a scope's boundary ports are drawn (inside a Card in Card).
  func boundaryPosition(_ scope: [String], _ side: String) -> CGPoint? {
    guard let v = layout(of: scope)["boundary"]?[side], let x = v["x"]?.double, let y = v["y"]?.double else { return nil }
    return CGPoint(x: x, y: y)
  }

  func setBoundaryPosition(_ scope: [String], _ side: String, _ p: CGPoint) {
    guard let prefix = projectPrefix(scope) else { return }
    queueFields { [self] in [fieldEdit(prefix + "layout.json", [.string("boundary"), .string(side)], old: layout(of: scope)["boundary"]?[side],
                         new: .object([("x", .real((p.x * 10).rounded() / 10)), ("y", .real((p.y * 10).rounded() / 10)) ]))] }
  }

  /// The host applies the measured layout in one operation with undo.
  func arrange(_ scope: [String]) async {
    guard let prefix = projectPrefix(scope) else { return }
    let body = Tsugumi_Host_ProjectLayoutT()
    body.projectPath = prefix.isEmpty ? "" : String(prefix.dropLast())
    body.action = .arrange
    body.scene = layoutScene(scope)
    do { try await operate(.projectlayout, body: body) }
    catch { notice = String(describing: error) }
  }

  /// Measurements only. Geometry and saved port order belong to presentation;
  /// all decisions that rewrite the layout are made by the host service.
  func layoutScene(_ scope: [String]) -> Tsugumi_Host_LayoutSceneT {
    let scene = Tsugumi_Host_LayoutSceneT()
    scene.flow = flow.rawValue
    scene.grid = AppPreferences.shared.gridSize
    scene.snap = AppPreferences.shared.showGrid
    let ids = cards(in: scope).compactMap { $0["id"]?.string }
    scene.cards = ids.map { id in
      let k = key(scope + [id]), g = geometry(k), at = placedPosition(k)
      func ports(_ names: [String], output: Bool) -> [Tsugumi_Host_LayoutPortT?] {
        names.map { name in
          let p = g.port(name, output: output), v = Tsugumi_Host_LayoutPortT()
          v.name = name; v.x = p.x; v.y = p.y
          return v
        }
      }
      let c = Tsugumi_Host_LayoutCardT()
      c.id = id; c.x = at.x; c.y = at.y; c.width = g.size.width; c.height = g.size.height
      // Unnamed visual anchors locate Draft wires without declaring Card ports.
      c.inputs = ports(kind(k) == .draft ? [""] : g.inputs, output: false)
      c.outputs = ports(kind(k) == .draft ? [""] : g.outputs, output: true)
      c.reorderable = [.code, .input, .subgraph].contains(kind(k))
      return c
    }
    scene.wires = connections(in: scope).compactMap { c in
      guard let f = c["from"]?["card"]?.string, let fp = c["from"]?["port"]?.string,
            let t = c["to"]?["card"]?.string, let tp = c["to"]?["port"]?.string else { return nil }
      let w = Tsugumi_Host_LayoutWireT()
      w.from = f; w.fromPort = fp; w.to = t; w.toPort = tp
      return w
    }
    scene.frames = framedCards(in: scope).map { k in
      let r = displayRect(k, origin: placedPosition(k)), b = Tsugumi_Host_LayoutBoxT()
      b.id = path(k).last!; b.x = r.minX; b.y = r.minY; b.width = r.width; b.height = r.height
      return b
    }
    scene.notes = notes(in: scope).compactMap { n in
      guard let id = n["id"]?.string else { return nil }
      let b = Tsugumi_Host_LayoutBoxT()
      b.id = id; b.x = n["x"]?.double ?? 0; b.y = n["y"]?.double ?? 0
      b.width = n["width"]?.double ?? 240; b.height = n["height"]?.double ?? 120
      return b
    }
    func bar(_ side: String) -> Tsugumi_Host_LayoutBarT? {
      guard !scope.isEmpty else { return nil }
      let b = Tsugumi_Host_LayoutBarT()
      b.ends = (boundary(key(scope))?[side]?.object ?? []).map { name, endpoint in
        let e = Tsugumi_Host_LayoutBarEndT()
        e.name = name; e.card = endpoint["card"]?.string ?? ""; e.port = endpoint["port"]?.string ?? ""
        return e
      }
      let at = boundaryPosition(scope, side)
      b.x = at.map { Double($0.x) }; b.y = at.map { Double($0.y) }
      return b
    }
    scene.inputs = bar("inputs"); scene.outputs = bar("outputs")
    return scene
  }

  /// Reduces crossings while keeping all positions, as one undo step.
  func untangle(_ scope: [String]) async {
    guard let prefix = projectPrefix(scope) else { return }
    let body = Tsugumi_Host_ProjectLayoutT()
    body.projectPath = prefix.isEmpty ? "" : String(prefix.dropLast())
    body.action = .untangle
    body.scene = layoutScene(scope)
    do { try await operate(.projectlayout, body: body) }
    catch { notice = String(describing: error) }
  }

  /// Whether the canvas shows this Card's ports in an order of its own.
  func hasPortOrder(_ k: CardKey) -> Bool {
    let entry = layout(of: Array(path(k).dropLast()))["cards"]?[layoutKey(k)]
    return !(entry?["inputOrder"]?.array ?? []).isEmpty || !(entry?["outputOrder"]?.array ?? []).isEmpty
  }

  /// Back to the declared order of the ports, in one undo step.
  @discardableResult
  func resetPortOrder(_ keys: Set<CardKey>) -> Task<Void, Never> {
    let targets = keys.filter { hasPortOrder($0) }
    if targets.isEmpty { return Task {} }
    var edits: [Tsugumi_Host_ProjectEditT] = []
    for k in targets.sorted() {
      let scope = Array(path(k).dropLast())
      guard let prefix = projectPrefix(scope) else { continue }
      let entry = layout(of: scope)["cards"]?[layoutKey(k)]
      for field in ["inputOrder", "outputOrder"] {
        if let value = entry?[field] {
          edits.append(fieldEdit(prefix + "layout.json", [.string("cards"), .string(layoutKey(k)), .string(field)], old: value, new: nil))
        }
      }
    }
    return Task { @MainActor in
      do { try await operateFields(edits) }
      catch { notice = String(describing: error) }
    }
  }

  // MARK: - Definitions

  func removeDefinition(_ id: String) {
    selectionOperation(.deletedefinitions, cards: [key([id])], in: [])
  }

  func clearBuildCache() async throws -> Tsugumi_RPC_ProjectCacheClearResultT {
    guard let rpc else { throw RPCError.disconnected }
    return try await rpc.clearCache(opId: UUID())
  }

  func deleteRunScratch(paths: [String]) async throws -> Tsugumi_RPC_RunScratchDeleteResultT {
    guard let rpc else { throw RPCError.disconnected }
    let result = try await rpc.deleteRunScratch(opId: UUID(), paths: paths)
    await refreshCacheUsage()
    return result
  }

  func deleteRunHistory(endedBefore: Date?) async throws -> Tsugumi_RPC_RunHistoryDeleteResultT {
    guard let rpc else { throw RPCError.disconnected }
    return try await rpc.deleteHistory(opId: UUID(), endedBefore: endedBefore)
  }
}
