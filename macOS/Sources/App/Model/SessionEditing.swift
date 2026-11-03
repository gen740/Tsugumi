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

  /// Whether a Card is one of this project's own (not inside a Card in Card).
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

  func collapsed(_ k: CardKey) -> Bool {
    layout(of: Array(path(k).dropLast()))["cards"]?[layoutKey(k)]?["collapsed"]?.bool ?? false
  }

  // MARK: - Cards and connections

  /// A free Card name in a scope's project: not a Card, a definition folder
  /// or an external there.
  func uniqueId(_ base: String, in scope: [String] = []) -> String {
    let prefix = projectPrefix(scope) ?? ""
    var existing = Set(cards(in: scope).compactMap { $0["id"]?.string })
    if prefix.isEmpty {
      existing.formUnion(definitions.keys)
      existing.formUnion(links.keys)
      existing.formUnion(confirmedDefinitions.keys)
      existing.formUnion(confirmedLinks.keys)
    } else {
      existing.formUnion(external(prefix).map { Array($0.definitions.keys) } ?? [])
    }
    let ext = prefix + "externals/"
    existing.formUnion(sourcePaths.filter { $0.hasPrefix(ext) }.compactMap { $0.dropFirst(ext.count).split(separator: "/").first.map(String.init) })
    existing.formUnion(effectiveExternals.keys.filter { $0.hasPrefix(ext) }.compactMap { $0.dropFirst(ext.count).split(separator: "/").first.map(String.init) })
    let clean = base.lowercased().map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "-" }
    var stem = String(clean).trimmingCharacters(in: CharacterSet(charactersIn: "-"))
    if stem.isEmpty { stem = "card" }
    var id = stem
    var n = 2
    while existing.contains(id) { id = "\(stem)-\(n)"; n += 1 }
    return id
  }

  /// Adds a Card instance to a scope's canvas.
  func appendCard(_ id: String, in scope: [String]) {
    updateGraph(scope) { g in
      var cards = g["cards"]?.array ?? []
      cards.append(.object([("id", .string(id))]))
      g["cards"] = .array(cards)
    }
  }

  /// A new Card from a template, with its own definition folder.
  @discardableResult
  func addCard(template t: DefinitionTemplate, in scope: [String], at point: CGPoint, name: String? = nil) -> CardKey {
    let id = uniqueId(name ?? t.baseName, in: scope)
    let k = key(scope + [id])
    edit { s in
      s.appendCard(id, in: scope)
      s.writeTemplate(t, card: k)
      s.setPosition(k, point)
    }
    return k
  }

  /// Copies a Card's definition folder (settings and every file) into a
  /// scope as a new Card. A Card in Card brings its own copy of the external.
  func copyDefinition(from k: CardKey, to scope: [String], name: String? = nil) async -> (id: String, apply: (ProjectSession) -> Void)? {
    guard let d = cardDefinitionDraft(k), let dir = definitionDir(k) else { return nil }
    let newId = uniqueId(name ?? path(k).last ?? "card", in: scope)
    let toPrefix = projectPrefix(scope) ?? ""
    var files: [String: Data] = [:]
    for p in sourcePaths where p.hasPrefix(dir) {
      let rel = String(p.dropFirst(dir.count))
      if ["_card.json", "_link.json", "_meta.json"].contains(rel) { continue }
      if let t = try? await sourceText(p) { files[toPrefix + "definitions/\(newId)/" + rel] = Data(t.utf8) }
    }
    // A Card in Card's external is copied along (a copy of its own).
    var link = d.link
    var extFiles: [String: Data] = [:]
    var extDocs: (String, ExternalProject)?
    if let from = cardExternal(k), let name = link?["external"]?.string, let e = external(from) {
      let extName = uniqueId(name, in: scope)
      let to = toPrefix + "externals/\(extName)/"
      for p in sourcePaths where p.hasPrefix(from) && !ExternalProject.isDocument(String(p.dropFirst(from.count))) {
        if let t = try? await sourceText(p) { extFiles[to + p.dropFirst(from.count)] = Data(t.utf8) }
      }
      // Externals nested in it come along as files.
      for (q, inner) in effectiveExternals where q.hasPrefix(from) && q != from {
        for (rel, text) in inner.files { extFiles[to + q.dropFirst(from.count) + rel] = Data(text.utf8) }
      }
      extDocs = (to, e)
      link = .object([("external", .string(extName))])
    }
    let draft = DefinitionDraft(card: d.card, meta: d.meta, link: link)
    let newKey = key(scope + [newId])
    return (newId, { s in
      s.setCardDefinition(newKey, draft)
      for (p, bytes) in files.merging(extFiles, uniquingKeysWith: { a, _ in a }) {
        s.sourceRemovals.remove(p)
        s.sourceEdits[p] = bytes
      }
      if let (to, e) = extDocs { s.createExternal(to, e) }
    })
  }

  /// Copies Cards with their definition folders (settings, sources) under new
  /// names; connections between the copied Cards are copied too.
  func duplicate(_ keys: Set<CardKey>, in scope: [String]) async -> Set<CardKey> {
    var map: [String: String] = [:]
    var applies: [(ProjectSession) -> Void] = []
    var taken: Set<String> = []
    for k in keys.sorted() {
      guard let id = path(k).last else { continue }
      var n = 1
      var want = id
      while taken.contains(uniqueId(want, in: scope)) { n += 1; want = "\(id)-\(n)" }
      guard let c = await copyDefinition(from: k, to: scope, name: uniqueId(want, in: scope)) else { continue }
      taken.insert(c.id)
      map[id] = c.id
      applies.append(c.apply)
    }
    var made: Set<CardKey> = []
    edit { s in
      for a in applies { a(s) }
      s.updateGraph(scope) { g in
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
        let p = s.position(key(scope + [old]))
        s.setPosition(key(scope + [new]), CGPoint(x: p.x + 30, y: p.y + 30))
        made.insert(key(scope + [new]))
      }
    }
    return made
  }

  /// Renames a Card (its id, its definition folder): saved at once, with its
  /// connections and positions following (and, for this project's Cards, its
  /// run history).
  func renameCard(_ k: CardKey, to raw: String) async throws {
    guard let old = path(k).last else { return }
    let scope = Array(path(k).dropLast())
    let new = raw.trimmingCharacters(in: .whitespaces)
    guard new != old else { return }
    guard !new.isEmpty, new.count <= 128, new != ".", new != "..",
          new.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || "-_.".contains($0)) }) else {
      throw RPCFailure(kind: .invalidRequest, diagnostic: String(localized: "名前に使えるのは英数字と - _ . だけです（128 文字まで）。"))
    }
    if uniqueId(new, in: scope) != new { throw RPCFailure(kind: .invalidRequest, diagnostic: String(localized: "「\(new)」はすでに使われています。")) }
    if dirty { await save() }
    if dirty { throw RPCFailure(kind: .validationFailed, diagnostic: saveError ?? String(localized: "変更を保存できませんでした。")) }
    guard let rpc else { throw RPCError.disconnected }
    func renamedGraph(_ g: JSONValue) -> JSONValue {
      var g = g
      g["cards"] = .array((g["cards"]?.array ?? []).map { $0["id"]?.string == old ? .object([("id", .string(new))]) : $0 })
      g["connections"] = .array((g["connections"]?.array ?? []).map { c in
        var c = c
        for side in ["from", "to"] where c[side]?["card"]?.string == old { c[side]?["card"] = .string(new) }
        return c
      })
      return g
    }
    func renamedLayout(_ l: JSONValue) -> JSONValue {
      var lay = l
      func rk(_ s: String) -> String { s == old ? new : s.hasPrefix(old + "/") ? new + s.dropFirst(old.count) : s }
      for field in ["cards", "displays"] {
        if let o = lay[field]?.object { lay[field] = .object(o.map { (rk($0.0), $0.1) }) }
      }
      for field in ["notes", "groups"] {
        if let arr = lay[field]?.array {
          lay[field] = .array(arr.map { item in
            var item = item
            if let cs = item["cards"]?.array { item["cards"] = .array(cs.map { $0.string == old ? .string(new) : $0 }) }
            if let outs = item["outputs"]?.array {
              item["outputs"] = .array(outs.map { o in var o = o; if o["card"]?.string == old { o["card"] = .string(new) }; return o })
            }
            return item
          })
        }
      }
      return lay
    }
    guard let prefix = projectPrefix(scope) else { return }
    if prefix.isEmpty {
      var proj = confirmedProject
      proj["canvas"] = renamedGraph(proj["canvas"] ?? .object([]))
      let lay = renamedLayout(confirmedLayout)
      var changes = [Change.setProject(proj.serialized()), Change.renameDefinition(id: old, to: new)]
      if lay != confirmedLayout { changes.append(Change.setLayout(lay.serialized())) }
      _ = try await rpc.save(baseRev: rev, opId: UUID(), origin: "ui:\(viewId.uuidString.lowercased())", changes: changes)
      return
    }
    // Inside a Card in Card: the external's documents and files are rewritten.
    let from = prefix + "definitions/\(old)/", to = prefix + "definitions/\(new)/"
    var files: [String: Data] = [:]
    for p in sourcePaths where p.hasPrefix(from) && !ExternalProject.isDocument(String(p.dropFirst(prefix.count))) {
      files[to + p.dropFirst(from.count)] = Data(try await sourceText(p).utf8)
    }
    edit { s in
      s.updateExternal(prefix) { e in
        e.project["canvas"] = renamedGraph(e.project["canvas"] ?? .object([]))
        e.project["boundary"] = e.project["boundary"].map { b in
          var b = b
          for side in ["inputs", "outputs"] {
            if let o = b[side]?.object {
              b[side] = .object(o.map { n, v in var v = v; if v["card"]?.string == old { v["card"] = .string(new) }; return (n, v) })
            }
          }
          return b
        }
        e.layout = renamedLayout(e.layout)
        e.definitions[new] = e.definitions[old]
        e.definitions[old] = nil
      }
      for p in s.sourcePaths where p.hasPrefix(from) && !ExternalProject.isDocument(String(p.dropFirst(prefix.count))) { s.removeSource(p) }
      for (p, d) in files {
        s.sourceRemovals.remove(p)
        s.sourceEdits[p] = d
      }
    }
    await save()
  }

  func deleteCards(_ keys: Set<CardKey>, in scope: [String]) {
    let ids = Set(keys.compactMap { path($0).last })
    guard !ids.isEmpty else { return }
    let prefix = projectPrefix(scope) ?? ""
    edit { s in
      // Each Card's definition folder goes with it, and a Card in Card's own external copy.
      for k in keys {
        if let ext = s.cardExternal(k) { s.removeExternal(ext) }
        s.removeCardDefinition(k)
      }
      s.updateGraph(scope) { g in
        g["cards"] = .array((g["cards"]?.array ?? []).filter { !ids.contains($0["id"]?.string ?? "") })
        g["connections"] = .array((g["connections"]?.array ?? []).filter {
          !ids.contains($0["from"]?["card"]?.string ?? "") && !ids.contains($0["to"]?["card"]?.string ?? "")
        })
      }
      if !prefix.isEmpty {
        // Boundary ports of removed Cards go too.
        s.updateExternal(prefix) { e in
          for side in ["inputs", "outputs"] {
            if let o = e.project["boundary"]?[side]?.object {
              e.project["boundary"]?[side] = .object(o.filter { !ids.contains($0.1["card"]?.string ?? "") })
            }
          }
        }
      }
      s.updateLayout(of: scope) { l in
        for k in keys {
          // Layout entries of the Card and of anything inside it.
          let lk = s.layoutKey(k)
          if var cards = l["cards"]?.object {
            cards.removeAll { $0.0 == lk || $0.0.hasPrefix(lk + "/") }
            l["cards"] = .object(cards)
          }
          l["displays"]?[lk] = nil
        }
      }
      s.updateGroups(in: scope) { groups in
        for i in groups.indices {
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

  /// Removes connections (as they are in the scope's canvas).
  func removeConnections(_ list: [JSONValue], in scope: [String]) {
    guard !list.isEmpty else { return }
    edit { s in
      s.updateGraph(scope) { g in
        g["connections"] = .array((g["connections"]?.array ?? []).filter { !list.contains($0) })
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
        let lk = s.layoutKey(k)
        s.updateLayout(of: Array(path(k).dropLast())) { l in
          var cards = l["cards"] ?? .object([])
          var entry = cards[lk] ?? .object([("x", .real(0)), ("y", .real(0))])
          entry["collapsed"] = value ? .bool(true) : nil
          cards[lk] = entry
          l["cards"] = cards
        }
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
    guard !ids.isEmpty, let scopePrefix = projectPrefix(scope) else { return nil }
    let newId = uniqueId(name, in: scope)
    let newKey = key(scope + [newId])
    let ext = scopePrefix + "externals/\(newId)/"
    // Files of the Cards' definition folders, and of their own externals.
    var files: [String: Data] = [:]
    var moved: [String: ExternalProject] = [:]
    for k in keys {
      guard let dir = definitionDir(k), let id = path(k).last else { continue }
      for p in sourcePaths where p.hasPrefix(dir) && !ExternalProject.isDocument(String(p.dropFirst(scopePrefix.count))) {
        if let t = try? await sourceText(p) { files[ext + "definitions/\(id)/" + p.dropFirst(dir.count)] = Data(t.utf8) }
      }
      if let inner = cardExternal(k), let e = external(inner), let name = cardDefinitionDraft(k)?.link?["external"]?.string {
        let to = ext + "externals/\(name)/"
        for p in sourcePaths where p.hasPrefix(inner) && !ExternalProject.isDocument(String(p.dropFirst(inner.count))) {
          if let t = try? await sourceText(p) { files[to + p.dropFirst(inner.count)] = Data(t.utf8) }
        }
        moved[to] = e
        moved[inner] = nil
      }
    }
    edit { s in
      var center = CGPoint.zero
      for k in keys { let p = s.position(k); center.x += p.x; center.y += p.y }
      center.x /= CGFloat(keys.count)
      center.y /= CGFloat(keys.count)
      var inner: [JSONValue] = [], innerConns: [JSONValue] = []
      var inputs: [(String, JSONValue)] = [], outputs: [(String, JSONValue)] = []
      s.updateGraph(scope) { g in
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
      // The external: a project of its own (it also opens on its own).
      let root = scopePrefix.isEmpty ? s.project : (s.external(scopePrefix)?.project ?? .object([]))
      var e = ExternalProject(project: .object([
        ("name", .string(newId)), ("pkgsPath", root["pkgsPath"] ?? .string("tsugumi:default")),
        ("databaseConnections", .array([])),
        ("canvas", .object([("cards", .array(inner)), ("connections", .array(innerConns))])),
        ("boundary", .object([("inputs", .object(inputs)), ("outputs", .object(outputs))])),
      ]), layout: .object([]), definitions: [:])
      var extCards: [(String, JSONValue)] = []
      for k in keys.sorted() {
        guard let id = path(k).last else { continue }
        let p = s.position(k)
        extCards.append((id, .object([("x", .real(p.x - center.x)), ("y", .real(p.y - center.y))])))
        e.definitions[id] = s.cardDefinitionDraft(k)
      }
      e.layout = .object([("cards", .object(extCards))])
      for k in keys {
        if let inner = s.cardExternal(k) { s.removeExternal(inner) }
        s.removeCardDefinition(k)
      }
      for (p, d) in files {
        s.sourceRemovals.remove(p)
        s.sourceEdits[p] = d
      }
      for (to, inner) in moved { s.createExternal(to, inner) }
      s.createExternal(ext, e)
      s.setCardDefinition(newKey, DefinitionDraft(card: .object([]), meta: nil, link: .object([("external", .string(newId))])))
      s.updateLayout(of: scope) { l in
        if var cards = l["cards"]?.object {
          cards.removeAll { k, _ in ids.contains(where: { k == $0 || k.hasPrefix($0 + "/") }) }
          l["cards"] = .object(cards)
        }
      }
      s.setPosition(newKey, center)
      s.updateGroups(in: scope) { groups in
        for i in groups.indices {
          groups[i]["cards"] = .array((groups[i]["cards"]?.array ?? []).filter { !ids.contains($0.string ?? "") })
        }
      }
    }
    return newKey
  }

  // MARK: - Layout: notes, groups, output frames

  func notes(in scope: [String]) -> [JSONValue] {
    (layout(of: scope)["notes"]?.array ?? []).filter { ($0["scope"]?.string ?? "") == "" }
  }

  func addNote(in scope: [String], at p: CGPoint) -> String {
    let id = "note-" + UUID().uuidString.prefix(8).lowercased()
    edit { s in
      s.updateLayout(of: scope) { l in
        var notes = l["notes"]?.array ?? []
        notes.append(.object([
          ("id", .string(id)), ("scope", .string("")), ("text", .string("メモ")), ("color", .string("yellow")),
          ("x", .real(p.x)), ("y", .real(p.y)), ("width", .real(240)), ("height", .real(120)),
        ]))
        l["notes"] = .array(notes)
      }
    }
    return id
  }

  /// The scope whose layout holds a note or group (ids are unique).
  private func scopeHolding(_ field: String, _ id: String) -> [String]? {
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

  func updateNote(_ id: String, _ fn: (inout JSONValue) -> Void) {
    guard let sc = scopeHolding("notes", id) else { return }
    edit { s in
      s.updateLayout(of: sc) { l in
        guard var notes = l["notes"]?.array, let i = notes.firstIndex(where: { $0["id"]?.string == id }) else { return }
        fn(&notes[i])
        l["notes"] = .array(notes)
      }
    }
  }

  func removeNote(_ id: String) {
    guard let sc = scopeHolding("notes", id) else { return }
    edit { s in s.updateLayout(of: sc) { $0["notes"] = .array(($0["notes"]?.array ?? []).filter { $0["id"]?.string != id }) } }
  }

  func groups(in scope: [String]) -> [JSONValue] {
    (layout(of: scope)["groups"]?.array ?? []).filter { ($0["scope"]?.string ?? "") == "" }
  }

  func updateGroups(in scope: [String], _ fn: (inout [JSONValue]) -> Void) {
    updateLayout(of: scope) { l in
      var groups = l["groups"]?.array ?? []
      fn(&groups)
      l["groups"] = groups.isEmpty ? nil : .array(groups)
    }
  }

  func addGroup(_ keys: Set<CardKey>, in scope: [String], name: String = "グループ") -> String {
    let id = "group-" + UUID().uuidString.prefix(8).lowercased()
    edit { s in
      s.updateGroups(in: scope) { g in
        g.append(.object([
          ("id", .string(id)), ("scope", .string("")), ("name", .string(name)), ("color", .string("blue")),
          ("cards", .array(keys.compactMap { path($0).last }.sorted().map { .string($0) })),
        ]))
      }
    }
    return id
  }

  func updateGroup(_ id: String, _ fn: (inout JSONValue) -> Void) {
    guard let sc = scopeHolding("groups", id) else { return }
    edit { s in
      s.updateGroups(in: sc) { g in
        guard let i = g.firstIndex(where: { $0["id"]?.string == id }) else { return }
        fn(&g[i])
      }
    }
  }

  /// Removes the group frame only; its Cards stay where they are.
  func ungroup(_ id: String) {
    guard let sc = scopeHolding("groups", id) else { return }
    edit { s in s.updateGroups(in: sc) { $0.removeAll { $0["id"]?.string == id } } }
  }

  func displayFrame(_ k: CardKey) -> JSONValue? { layout(of: Array(path(k).dropLast()))["displays"]?[layoutKey(k)] }

  func updateDisplayFrame(_ k: CardKey, _ fn: (inout JSONValue) -> Void) {
    let lk = layoutKey(k)
    edit { s in
      s.updateLayout(of: Array(path(k).dropLast())) { l in
        var displays = l["displays"] ?? .object([])
        var d = displays[lk] ?? .object([])
        fn(&d)
        displays[lk] = d
        l["displays"] = displays
      }
    }
  }

  /// Where a scope's boundary ports are drawn (inside a Card in Card).
  func boundaryPosition(_ scope: [String], _ side: String) -> CGPoint? {
    guard let v = layout(of: scope)["boundary"]?[side], let x = v["x"]?.double, let y = v["y"]?.double else { return nil }
    return CGPoint(x: x, y: y)
  }

  func setBoundaryPosition(_ scope: [String], _ side: String, _ p: CGPoint) {
    edit { s in
      s.updateLayout(of: scope) { l in
        var b = l["boundary"] ?? .object([])
        b[side] = .object([("x", .real((p.x * 10).rounded() / 10)), ("y", .real((p.y * 10).rounded() / 10))])
        l["boundary"] = b
      }
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

  func removeDefinition(_ id: String) { removeCardDefinition(key([id])) }

  /// Removes a Card's definition folder (its documents and every file in it).
  func removeCardDefinition(_ k: CardKey) {
    guard let dir = definitionDir(k) else { return }
    setCardDefinition(k, nil)
    let prefix = projectPrefix(Array(path(k).dropLast())) ?? ""
    for p in sourcePaths where p.hasPrefix(dir) && !(prefix.isEmpty ? false : ExternalProject.isDocument(String(p.dropFirst(prefix.count)))) {
      removeSource(p)
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
  func writeTemplate(_ t: DefinitionTemplate, card k: CardKey) {
    guard let dir = definitionDir(k), let id = path(k).last else { return }
    let ns = id.replacingOccurrences(of: "-", with: "_").replacingOccurrences(of: ".", with: "_")
    func set(_ card: JSONValue, meta: JSONValue? = nil) { setCardDefinition(k, DefinitionDraft(card: card, meta: meta)) }
    switch t {
    case .draft:
      set(.object([("template", .string("draft"))]), meta: .object([("description", .string("")), ("content", .string(""))]))
    case .trigger, .fork, .branch, .logger:
      set(.object([("template", .string(t.rawValue))]))
    case .merge:
      set(.object([
        ("template", .string("merge")), ("inputs", .array([.object([("name", .string("a"))]), .object([("name", .string("b"))])])),
      ]))
    case .input:
      set(.object([
        ("template", .string("input")),
        ("params", .array([.object([("name", .string("value")), ("type", .string("double")), ("data", .number("0"))])])),
      ]))
      putSource(dir + "_outputs.fbs", "namespace \(ns);\ntable Value { value:double; }\ntable Outputs { value:Value (required); }\nroot_type Outputs;\n")
    case .python:
      set(.object([("language", .string("python")), ("inputs", .array([.object([("name", .string("in"))])]))]))
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
      set(.object([("language", .string("cpp")), ("inputs", .array([.object([("name", .string("in"))])]))]))
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
