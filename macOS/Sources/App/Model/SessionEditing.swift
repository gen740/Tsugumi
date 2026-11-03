import AppKit
import Foundation
import TsugumiRPC

/// What a Card is, for choosing what the canvas and the editor offer.
enum CardKind: String {
  case trigger, input, fork, branch, merge, logger, draft, subgraph, code

  var label: String {
    switch self {
    case .trigger: return String(localized: "開始")
    case .input: return String(localized: "入力")
    case .fork: return String(localized: "分配")
    case .branch: return String(localized: "分岐")
    case .merge: return String(localized: "合流")
    case .logger: return String(localized: "記録")
    case .draft: return "Draft"
    case .subgraph: return "Card in Card"
    case .code: return String(localized: "処理")
    }
  }

  var symbol: String {
    switch self {
    case .trigger: return "play.circle"
    case .input: return "slider.horizontal.3"
    case .fork: return "arrow.triangle.branch"
    case .branch: return "arrow.triangle.swap"
    case .merge: return "arrow.triangle.merge"
    case .logger: return "list.bullet.rectangle"
    case .draft: return "pencil.and.outline"
    case .subgraph: return "link"
    case .code: return "chevron.left.forwardslash.chevron.right"
    }
  }
}

/// What a new Card starts from: a built-in Card or a scaffold. Every Card
/// gets a definition folder of its own (definitions/<id>/).
enum DefinitionTemplate: String, CaseIterable, Identifiable {
  case python, cpp, input, draft, trigger, fork, branch, merge, logger
  var id: String { rawValue }
  var label: String {
    switch self {
    case .draft: return String(localized: "Draft（未実装の計画）")
    case .python: return String(localized: "Python の処理")
    case .cpp: return String(localized: "C++ の処理")
    case .input: return String(localized: "入力値")
    case .trigger: return String(localized: "開始")
    case .fork: return String(localized: "分配")
    case .branch: return String(localized: "分岐")
    case .merge: return String(localized: "合流")
    case .logger: return String(localized: "記録")
    }
  }
  var symbol: String { (CardKind(rawValue: rawValue) ?? (self == .python || self == .cpp ? .code : .draft)).symbol }
  /// A name for the new Card (its id; made unique).
  var baseName: String {
    switch self {
    case .python, .cpp: return "card"
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

  func kind(_ k: CardKey) -> CardKind {
    if cardExternal(k) != nil { return .subgraph }
    guard let def = cardDefinition(k) else { return .draft }
    if let t = def["template"]?.string, let k = CardKind(rawValue: t) { return k }
    return def["language"] == nil ? .draft : .code
  }

  /// A Card's name is its id (and its definition folder).
  func displayName(_ k: CardKey) -> String { path(k).last ?? k }

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

  /// Whether a Card can be changed here (not inside a Card in Card).
  func isOwnCard(_ k: CardKey) -> Bool { path(k).count == 1 }

  /// Definition folders of this project that no Card uses (copied from
  /// another project, or left over).
  var unplacedDefinitions: [String] {
    let used = Set(cards(in: []).compactMap { $0["id"]?.string })
    return Set(definitions.keys).union(links.keys).subtracting(used).sorted()
  }

  /// Places a definition folder no Card uses as its Card (the same name).
  @discardableResult
  func placeDefinition(_ id: String, at point: CGPoint) -> CardKey {
    let k = key([id])
    guard unplacedDefinitions.contains(id) else { return k }
    edit { s in
      s.updateGraph([]) { g in
        var cards = g["cards"]?.array ?? []
        cards.append(.object([("id", .string(id))]))
        g["cards"] = .array(cards)
      }
      s.setPosition(k, point)
    }
    return k
  }

  var isEditable: Bool { connected && !runActive }

  /// Source bytes as the draft sees them.
  func sourceText(_ p: String) async throws -> String {
    if let d = sourceEdits[p] { return String(decoding: d, as: UTF8.self) }
    guard let rpc else { throw RPCError.disconnected }
    return String(decoding: try await rpc.readFile(path: p, atRev: rev), as: UTF8.self)
  }

  func collapsed(_ k: CardKey) -> Bool { layout["cards"]?[k]?["collapsed"]?.bool ?? false }

  /// The Card's settings as last confirmed by the core (not the draft).
  func confirmedCardDefinition(_ k: CardKey) -> JSONValue? {
    guard isOwnCard(k), let id = path(k).first else { return nil }
    return confirmedDefinitions[id]
  }

  // MARK: - Cards and connections

  /// A free Card name: not a Card, nor a definition folder, of this project.
  func uniqueId(_ base: String, in scope: [String] = []) -> String {
    var existing = Set(cards(in: scope).compactMap { $0["id"]?.string })
    existing.formUnion(definitions.keys)
    existing.formUnion(links.keys)
    existing.formUnion(confirmedDefinitions.keys)
    existing.formUnion(confirmedLinks.keys)
    existing.formUnion(sourcePaths.filter { $0.hasPrefix("externals/") }.compactMap { $0.split(separator: "/").dropFirst().first.map(String.init) })
    let clean = base.lowercased().map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "-" }
    var stem = String(clean).trimmingCharacters(in: CharacterSet(charactersIn: "-"))
    if stem.isEmpty { stem = "card" }
    var id = stem
    var n = 2
    while existing.contains(id) { id = "\(stem)-\(n)"; n += 1 }
    return id
  }

  /// A new Card from a template, with its own definition folder.
  @discardableResult
  func addCard(template t: DefinitionTemplate, in scope: [String], at point: CGPoint, name: String? = nil) -> CardKey {
    let id = uniqueId(name ?? t.baseName)
    let k = key([id])
    edit { s in
      s.writeTemplate(t, id: id)
      s.updateGraph([]) { g in
        var cards = g["cards"]?.array ?? []
        cards.append(.object([("id", .string(id))]))
        g["cards"] = .array(cards)
      }
      s.setPosition(k, point)
    }
    return k
  }

  /// Copies Cards with their definition folders (settings, sources) under new
  /// names; connections between the copied Cards are copied too.
  func duplicate(_ keys: Set<CardKey>, in scope: [String]) async -> Set<CardKey> {
    guard scope.isEmpty else { return [] }
    var map: [String: String] = [:]
    var files: [String: String] = [:]  // new path -> text
    var taken: Set<String> = []
    for k in keys.sorted() {
      guard let id = path(k).first else { continue }
      var newId = uniqueId(id)
      var n = 2
      while taken.contains(newId) { newId = uniqueId("\(id)-\(n)"); n += 1 }
      taken.insert(newId)
      map[id] = newId
      for p in sourcePaths where p.hasPrefix("definitions/\(id)/") {
        if let text = try? await sourceText(p) { files["definitions/\(newId)/" + p.dropFirst("definitions/\(id)/".count)] = text }
      }
    }
    var made: Set<CardKey> = []
    edit { s in
      for (old, new) in map {
        if let link = s.links[old] {
          s.definitionEdits[new] = DefinitionDraft(card: .object([]), meta: s.definitionMeta[old], link: link)
        } else if let card = s.definitions[old] {
          s.setDefinition(new, card: card, meta: s.definitionMeta[old])
        }
      }
      for (p, text) in files { s.putSource(p, text) }
      s.updateGraph([]) { g in
        var cards = g["cards"]?.array ?? []
        var conns = g["connections"]?.array ?? []
        for (_, new) in map.sorted(by: { $0.key < $1.key }) { cards.append(.object([("id", .string(new))])) }
        for c in conns {
          if let f = c["from"]?["card"]?.string, let t = c["to"]?["card"]?.string, let nf = map[f], let nt = map[t] {
            var copy = c
            copy["from"]?["card"] = .string(nf)
            copy["to"]?["card"] = .string(nt)
            conns.append(copy)
          }
        }
        g["cards"] = .array(cards)
        g["connections"] = .array(conns)
      }
      for (old, new) in map {
        let p = s.position(key([old]))
        s.setPosition(key([new]), CGPoint(x: p.x + 30, y: p.y + 30))
        made.insert(key([new]))
      }
    }
    return made
  }

  /// Renames a Card (its id, its definition folder): saved at once, with its
  /// connections, positions and run history following.
  func renameCard(_ k: CardKey, to raw: String) async throws {
    guard isOwnCard(k), let old = path(k).first else { return }
    let new = raw.trimmingCharacters(in: .whitespaces)
    guard new != old else { return }
    guard !new.isEmpty, new.count <= 128, new != ".", new != "..",
          new.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || "-_.".contains($0)) }) else {
      throw RPCFailure(kind: .invalidRequest, diagnostic: String(localized: "名前に使えるのは英数字と - _ . だけです（128 文字まで）。"))
    }
    if uniqueId(new) != new { throw RPCFailure(kind: .invalidRequest, diagnostic: String(localized: "「\(new)」はすでに使われています。")) }
    if dirty { await save() }
    if dirty { throw RPCFailure(kind: .validationFailed, diagnostic: saveError ?? String(localized: "変更を保存できませんでした。")) }
    guard let rpc else { throw RPCError.disconnected }
    var proj = confirmedProject
    var lay = confirmedLayout
    var canvas = proj["canvas"] ?? .object([])
    canvas["cards"] = .array((canvas["cards"]?.array ?? []).map { $0["id"]?.string == old ? .object([("id", .string(new))]) : $0 })
    canvas["connections"] = .array((canvas["connections"]?.array ?? []).map { c in
      var c = c
      for side in ["from", "to"] where c[side]?["card"]?.string == old { c[side]?["card"] = .string(new) }
      return c
    })
    proj["canvas"] = canvas
    func rk(_ s: String) -> String { s == old ? new : s.hasPrefix(old + "/") ? new + s.dropFirst(old.count) : s }
    for field in ["cards", "displays", "boundaries"] {
      if let o = lay[field]?.object { lay[field] = .object(o.map { (rk($0.0), $0.1) }) }
    }
    for field in ["notes", "groups"] {
      if let arr = lay[field]?.array {
        lay[field] = .array(arr.map { item in
          var item = item
          if let sc = item["scope"]?.string { item["scope"] = .string(rk(sc)) }
          if (item["scope"]?.string ?? "") == "", let cs = item["cards"]?.array { item["cards"] = .array(cs.map { $0.string == old ? .string(new) : $0 }) }
          if let outs = item["outputs"]?.array {
            item["outputs"] = .array(outs.map { o in var o = o; if o["card"]?.string == old { o["card"] = .string(new) }; return o })
          }
          return item
        })
      }
    }
    var changes = [Change.setProject(proj.serialized()), Change.renameDefinition(id: old, to: new)]
    if lay != confirmedLayout { changes.append(Change.setLayout(lay.serialized())) }
    _ = try await rpc.save(baseRev: rev, opId: UUID(), origin: "ui:\(viewId.uuidString.lowercased())", changes: changes)
  }

  func deleteCards(_ keys: Set<CardKey>, in scope: [String]) {
    let ids = Set(keys.compactMap { path($0).last })
    guard !ids.isEmpty, scope.isEmpty else { return }
    edit { s in
      // Each Card's definition folder goes with it.
      for id in ids { s.removeDefinition(id) }
      s.updateGraph(scope) { g in
        g["cards"] = .array((g["cards"]?.array ?? []).filter { !ids.contains($0["id"]?.string ?? "") })
        g["connections"] = .array((g["connections"]?.array ?? []).filter {
          !ids.contains($0["from"]?["card"]?.string ?? "") && !ids.contains($0["to"]?["card"]?.string ?? "")
        })
      }
      for k in keys {
        // Layout entries of the Card and of anything inside it.
        if var cards = s.layout["cards"]?.object {
          cards.removeAll { $0.0 == k || $0.0.hasPrefix(k + "/") }
          s.layout["cards"] = .object(cards)
        }
        s.layout["displays"]?[k] = nil
      }
      s.updateGroups { groups in
        for i in groups.indices where (groups[i]["scope"]?.string ?? "") == key(scope) {
          groups[i]["cards"] = .array((groups[i]["cards"]?.array ?? []).filter { !ids.contains($0.string ?? "") })
        }
      }
    }
  }

  func connect(from: (CardKey, String), to: (CardKey, String), in scope: [String]) {
    guard let fromId = path(from.0).last, let toId = path(to.0).last else { return }
    edit { s in
      s.updateGraph(scope) { g in
        var conns = g["connections"]?.array ?? []
        // One connection per input port: a new wire replaces the old one.
        conns.removeAll { $0["to"]?["card"]?.string == toId && $0["to"]?["port"]?.string == to.1 }
        conns.append(.object([
          ("from", .object([("card", .string(fromId)), ("port", .string(from.1))])),
          ("to", .object([("card", .string(toId)), ("port", .string(to.1))])),
        ]))
        g["connections"] = .array(conns)
      }
    }
  }

  func removeConnection(at index: Int, in scope: [String]) {
    edit { s in
      s.updateGraph(scope) { g in
        guard var conns = g["connections"]?.array, index < conns.count else { return }
        conns.remove(at: index)
        g["connections"] = .array(conns)
      }
    }
  }

  func disconnect(_ k: CardKey, inputs: Bool, in scope: [String]) {
    guard let id = path(k).last else { return }
    edit { s in
      s.updateGraph(scope) { g in
        g["connections"] = .array((g["connections"]?.array ?? []).filter {
          ($0[inputs ? "to" : "from"]?["card"]?.string ?? "") != id
        })
      }
    }
  }

  func setCollapsed(_ keys: Set<CardKey>, _ value: Bool) {
    edit { s in
      for k in keys {
        var cards = s.layout["cards"] ?? .object([])
        var entry = cards[k] ?? .object([("x", .real(0)), ("y", .real(0))])
        entry["collapsed"] = value ? .bool(true) : nil
        cards[k] = entry
        s.layout["cards"] = cards
      }
    }
  }

  func setParam(_ k: CardKey, _ name: String, _ value: JSONValue?) {
    edit { s in
      s.updateCard(k) { c in
        var params = c["params"]?.array ?? []
        if let i = params.firstIndex(where: { $0["name"]?.string == name }) { params[i]["data"] = value }
        c["params"] = .array(params)
      }
    }
  }

  /// Moves the selected Cards into a new Card in Card: a new external project
  /// (externals/<name>/) with their definitions and the wires between them.
  /// Wires crossing the selection become its boundary ports, so the graph
  /// computes the same.
  @discardableResult
  func makeSubgraph(_ keys: Set<CardKey>, in scope: [String], name: String = "sub") async -> CardKey? {
    let ids = Set(keys.compactMap { path($0).last })
    guard !ids.isEmpty, scope.isEmpty else { return nil }
    let newId = uniqueId(name)
    let newKey = key([newId])
    let ext = "externals/\(newId)/"
    var files: [String: String] = [:]
    for id in ids {
      for p in sourcePaths where p.hasPrefix("definitions/\(id)/") {
        if let text = try? await sourceText(p) { files[ext + p] = text }
      }
    }
    edit { s in
      var center = CGPoint.zero
      for k in keys { let p = s.position(k); center.x += p.x; center.y += p.y }
      center.x /= CGFloat(keys.count)
      center.y /= CGFloat(keys.count)
      var inner: [JSONValue] = [], innerConns: [JSONValue] = []
      var inputs: [(String, JSONValue)] = [], outputs: [(String, JSONValue)] = []
      s.updateGraph([]) { g in
        let all = g["cards"]?.array ?? []
        let conns = g["connections"]?.array ?? []
        var outer: [JSONValue] = [], outerConns: [JSONValue] = []
        for c in all { if ids.contains(c["id"]?.string ?? "") { inner.append(c) } else { outer.append(c) } }
        func portName(_ list: [(String, JSONValue)], _ card: String, _ port: String) -> String {
          var n = "\(card)_\(port)"
          var i = 2
          while list.contains(where: { $0.0 == n }) { n = "\(card)_\(port)\(i)"; i += 1 }
          return n
        }
        for c in conns {
          let f = c["from"]?["card"]?.string ?? "", t = c["to"]?["card"]?.string ?? ""
          let fp = c["from"]?["port"]?.string ?? "", tp = c["to"]?["port"]?.string ?? ""
          switch (ids.contains(f), ids.contains(t)) {
          case (true, true): innerConns.append(c)
          case (false, false): outerConns.append(c)
          case (false, true):
            // One boundary input per inner input port (several wires may share it).
            let existing = inputs.first { $0.1["card"]?.string == t && $0.1["port"]?.string == tp }?.0
            let n = existing ?? portName(inputs, t, tp)
            if existing == nil { inputs.append((n, .object([("card", .string(t)), ("port", .string(tp))]))) }
            var nc = c
            nc["to"] = .object([("card", .string(newId)), ("port", .string(n))])
            outerConns.append(nc)
          case (true, false):
            let existing = outputs.first { $0.1["card"]?.string == f && $0.1["port"]?.string == fp }?.0
            let n = existing ?? portName(outputs, f, fp)
            if existing == nil { outputs.append((n, .object([("card", .string(f)), ("port", .string(fp))]))) }
            var nc = c
            nc["from"] = .object([("card", .string(newId)), ("port", .string(n))])
            outerConns.append(nc)
          }
        }
        outer.append(.object([("id", .string(newId))]))
        g["cards"] = .array(outer)
        g["connections"] = .array(outerConns)
      }
      // The external: a project of its own, opened on its own to edit.
      let extProject: JSONValue = .object([
        ("name", .string(newId)), ("pkgsPath", s.project["pkgsPath"] ?? .string("tsugumi:default")),
        ("databaseConnections", .array([])),
        ("canvas", .object([("cards", .array(inner)), ("connections", .array(innerConns))])),
        ("boundary", .object([("inputs", .object(inputs)), ("outputs", .object(outputs))])),
      ])
      var extCards: [(String, JSONValue)] = []
      for id in ids.sorted() {
        let p = s.position(key([id]))
        extCards.append((id, .object([("x", .real(p.x - center.x)), ("y", .real(p.y - center.y))])))
        if let card = s.definitions[id] { s.putSource(ext + "definitions/\(id)/_card.json", card.serialized() + "\n") }
        if let link = s.links[id] { s.putSource(ext + "definitions/\(id)/_link.json", link.serialized() + "\n") }
        if let meta = s.definitionMeta[id] { s.putSource(ext + "definitions/\(id)/_meta.json", meta.serialized() + "\n") }
      }
      for (p, text) in files { s.putSource(p, text) }
      s.putSource(ext + "tsugumi.json", extProject.serialized() + "\n")
      s.putSource(ext + "layout.json", JSONValue.object([("cards", .object(extCards))]).serialized() + "\n")
      for id in ids { s.removeDefinition(id) }
      s.definitionEdits[newId] = DefinitionDraft(card: .object([]), meta: nil, link: .object([("external", .string(newId))]))
      if var cards = s.layout["cards"]?.object {
        cards.removeAll { k, _ in ids.contains(where: { k == $0 || k.hasPrefix($0 + "/") }) }
        s.layout["cards"] = .object(cards)
      }
      s.setPosition(newKey, center)
      s.updateGroups { groups in
        for i in groups.indices where (groups[i]["scope"]?.string ?? "") == "" {
          groups[i]["cards"] = .array((groups[i]["cards"]?.array ?? []).filter { !ids.contains($0.string ?? "") })
        }
      }
    }
    return newKey
  }

  // MARK: - Layout: notes, groups, output frames

  func addNote(in scope: [String], at p: CGPoint) -> String {
    let id = "note-" + UUID().uuidString.prefix(8).lowercased()
    edit { s in
      var notes = s.layout["notes"]?.array ?? []
      notes.append(.object([
        ("id", .string(id)), ("scope", .string(key(scope))), ("text", .string("メモ")), ("color", .string("yellow")),
        ("x", .real(p.x)), ("y", .real(p.y)), ("width", .real(240)), ("height", .real(120)),
      ]))
      s.layout["notes"] = .array(notes)
    }
    return id
  }

  func updateNote(_ id: String, _ fn: (inout JSONValue) -> Void) {
    edit { s in
      guard var notes = s.layout["notes"]?.array, let i = notes.firstIndex(where: { $0["id"]?.string == id }) else { return }
      fn(&notes[i])
      s.layout["notes"] = .array(notes)
    }
  }

  func removeNote(_ id: String) {
    edit { s in s.layout["notes"] = .array((s.layout["notes"]?.array ?? []).filter { $0["id"]?.string != id }) }
  }

  func groups(in scope: [String]) -> [JSONValue] {
    (layout["groups"]?.array ?? []).filter { ($0["scope"]?.string ?? "") == key(scope) }
  }

  func updateGroups(_ fn: (inout [JSONValue]) -> Void) {
    var groups = layout["groups"]?.array ?? []
    fn(&groups)
    layout["groups"] = groups.isEmpty ? nil : .array(groups)
  }

  func addGroup(_ keys: Set<CardKey>, in scope: [String], name: String = "グループ") -> String {
    let id = "group-" + UUID().uuidString.prefix(8).lowercased()
    edit { s in
      s.updateGroups { g in
        g.append(.object([
          ("id", .string(id)), ("scope", .string(key(scope))), ("name", .string(name)), ("color", .string("blue")),
          ("cards", .array(keys.compactMap { path($0).last }.sorted().map { .string($0) })),
        ]))
      }
    }
    return id
  }

  func updateGroup(_ id: String, _ fn: (inout JSONValue) -> Void) {
    edit { s in
      s.updateGroups { g in
        guard let i = g.firstIndex(where: { $0["id"]?.string == id }) else { return }
        fn(&g[i])
      }
    }
  }

  /// Removes the group frame only; its Cards stay where they are.
  func ungroup(_ id: String) {
    edit { s in s.updateGroups { $0.removeAll { $0["id"]?.string == id } } }
  }

  func displayFrame(_ k: CardKey) -> JSONValue? { layout["displays"]?[k] }

  func updateDisplayFrame(_ k: CardKey, _ fn: (inout JSONValue) -> Void) {
    edit { s in
      var displays = s.layout["displays"] ?? .object([])
      var d = displays[k] ?? .object([])
      fn(&d)
      displays[k] = d
      s.layout["displays"] = displays
    }
  }

  /// Arranges the Cards of a scope in layers by their distance from the
  /// sources, following the flow direction (layout only).
  func arrange(_ scope: [String]) {
    let ids = cards(in: scope).compactMap { $0["id"]?.string }
    var incoming: [String: Set<String>] = [:]
    for c in connections(in: scope) {
      if let f = c["from"]?["card"]?.string, let t = c["to"]?["card"]?.string, f != t { incoming[t, default: []].insert(f) }
    }
    var depth: [String: Int] = [:]
    func level(_ id: String, _ seen: Set<String>) -> Int {
      if let d = depth[id] { return d }
      let preds = (incoming[id] ?? []).filter { !seen.contains($0) }
      let d = preds.isEmpty ? 0 : (preds.map { level($0, seen.union([id])) }.max() ?? 0) + 1
      depth[id] = d
      return d
    }
    for id in ids { _ = level(id, []) }
    var layers: [Int: [String]] = [:]
    for id in ids { layers[depth[id] ?? 0, default: []].append(id) }
    let vertical = AppPreferences.shared.flowDirection == .topToBottom
    edit { s in
      for (level, members) in layers {
        for (i, id) in members.enumerated() {
          let across = Double(i) * 260 - Double(members.count - 1) * 130
          let along = Double(level) * (vertical ? 200 : 270)
          s.setPosition(key(scope + [id]), vertical ? CGPoint(x: across, y: along) : CGPoint(x: along, y: across * 0.6))
        }
      }
    }
  }

  // MARK: - Definitions

  func setDefinition(_ id: String, card: JSONValue, meta: JSONValue?) {
    definitionEdits[id] = DefinitionDraft(card: card, meta: meta)
  }

  func removeDefinition(_ id: String) {
    definitionEdits[id] = DefinitionDraft(card: .object([]), meta: nil, removed: true)
    for p in sourcePaths where p.hasPrefix("definitions/\(id)/") {
      sourceEdits[p] = nil
      if sources[p] != nil { sourceRemovals.insert(p) }
    }
  }

  func putSource(_ p: String, _ text: String) {
    sourceRemovals.remove(p)
    sourceEdits[p] = Data(text.utf8)
  }

  func removeSource(_ p: String) {
    sourceEdits[p] = nil
    if sources[p] != nil { sourceRemovals.insert(p) }
  }

  /// The definition folder of a new Card made from a template (in the draft).
  func writeTemplate(_ t: DefinitionTemplate, id: String) {
    let ns = id.replacingOccurrences(of: "-", with: "_").replacingOccurrences(of: ".", with: "_")
    let dir = "definitions/\(id)/"
    switch t {
    case .draft:
      setDefinition(id, card: .object([("template", .string("draft"))]),
                    meta: .object([("description", .string("")), ("plan", .string(""))]))
    case .trigger, .fork, .branch, .logger:
      setDefinition(id, card: .object([("template", .string(t.rawValue))]), meta: nil)
    case .merge:
      setDefinition(id, card: .object([
        ("template", .string("merge")), ("inputs", .array([.object([("name", .string("a"))]), .object([("name", .string("b"))])])),
      ]), meta: nil)
    case .input:
      setDefinition(id, card: .object([
        ("template", .string("input")),
        ("params", .array([.object([("name", .string("value")), ("type", .string("double")), ("data", .number("0"))])])),
      ]), meta: nil)
      putSource(dir + "_outputs.fbs", "namespace \(ns);\ntable Value { value:double; }\ntable Outputs { value:Value (required); }\nroot_type Outputs;\n")
    case .python:
      setDefinition(id, card: .object([
        ("language", .string("python")), ("inputs", .array([.object([("name", .string("in"))])])),
      ]), meta: nil)
      putSource(dir + "default.nix", "{ pkgs, inputs, params }:\nlet\n  sdk = inputs.tsugumi.sdk { languages = [ \"python\" ]; };\nin\n{\n  run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; };\n}\n")
      putSource(dir + "_outputs.fbs", "namespace \(ns);\ntable Text { value:string; }\ntable Outputs { out:Text (required); }\nroot_type Outputs;\n")
      putSource(dir + "src/main.py", """
      import outputs.out.\(ns).Text as out


      def main(ctx):
          b = ctx.builder()
          text = b.CreateString("hello")
          out.Start(b)
          out.AddValue(b, text)
          ctx.output("out", b, out.End(b))

      """)
    case .cpp:
      setDefinition(id, card: .object([
        ("language", .string("cpp")), ("inputs", .array([.object([("name", .string("in"))])])),
      ]), meta: nil)
      putSource(dir + "default.nix", "{ pkgs, inputs, params }:\nlet\n  sdk = inputs.tsugumi.sdk { languages = [ \"cpp\" ]; };\nin\n{\n  run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; };\n}\n")
      putSource(dir + "_outputs.fbs", "namespace \(ns);\ntable Text { value:string; }\ntable Outputs { out:Text (required); }\nroot_type Outputs;\n")
      putSource(dir + "src/card.cc", """
      #include <card.hh>

      auto run(Card::Input& input) -> Card::Output {
        Card::Output out;
        out.out.emplace().value = "hello";
        return out;
      }

      """)
    }
  }

  /// Copies definitions from another project folder through an import grant.
  func importDefinitions(from folder: URL, ids: [String]) async throws -> [String: String] {
    guard let rpc else { throw RPCError.disconnected }
    if dirty { throw RPCFailure(kind: .validationFailed, diagnostic: "コピーする前に変更を保存してください。") }
    let grant = try await environments.grantImport(identity, source: folder)
    let r = try await rpc.importDefinitions(baseRev: rev, opId: UUID(), origin: "ui:\(viewId.uuidString.lowercased())",
                                            grant: grant, definitions: ids)
    var map: [String: String] = [:]
    for m in r.idMap.compactMap({ $0 }) { map[m.from] = m.to }
    return map
  }

  func clearBuildCache() async throws -> Tsugumi_RPC_CacheClearResultT {
    guard let rpc else { throw RPCError.disconnected }
    return try await rpc.clearCache(opId: UUID())
  }

  func deleteRunHistory(endedBefore: Date?) async throws -> Tsugumi_RPC_HistoryDeleteResultT {
    guard let rpc else { throw RPCError.disconnected }
    return try await rpc.deleteHistory(opId: UUID(), endedBefore: endedBefore)
  }
}

/// Definition ids readable from another project folder (for Import).
func definitionIds(inProject folder: URL) -> [(id: String, name: String)] {
  let defs = folder.appendingPathComponent("definitions")
  let ids = (try? FileManager.default.contentsOfDirectory(atPath: defs.path)) ?? []
  return ids.sorted().compactMap { id in
    let card = defs.appendingPathComponent("\(id)/_card.json")
    guard let data = try? Data(contentsOf: card) else { return nil }
    let name = (try? JSONValue.parse(String(decoding: data, as: UTF8.self)))?["name"]?.string ?? id
    return (id, name)
  }
}
