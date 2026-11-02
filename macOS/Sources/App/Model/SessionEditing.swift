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
    case .subgraph: return String(localized: "子 Workflow")
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
    case .subgraph: return "square.stack.3d.up"
    case .code: return "chevron.left.forwardslash.chevron.right"
    }
  }
}

/// New definition scaffolds offered by the Card database.
enum DefinitionTemplate: String, CaseIterable, Identifiable {
  case draft, python, cpp, input
  var id: String { rawValue }
  var label: String {
    switch self {
    case .draft: return String(localized: "Draft（未実装の計画）")
    case .python: return String(localized: "Python の処理")
    case .cpp: return String(localized: "C++ の処理")
    case .input: return String(localized: "入力値")
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
    guard let inst = instance(k) else { return .code }
    if inst["canvas"] != nil || inst["project"] != nil { return .subgraph }
    guard let defId = inst["definition"]?.string, let def = definitions[defId] else { return .draft }
    if let t = def["template"]?.string, let k = CardKind(rawValue: t) { return k }
    return def["language"] == nil ? .draft : .code
  }

  func displayName(_ k: CardKey) -> String {
    if let n = instance(k)?["name"]?.string, !n.isEmpty { return n }
    if let d = instance(k)?["definition"]?.string, let n = definitions[d]?["name"]?.string, !n.isEmpty { return n }
    return path(k).last ?? k
  }

  func scopeName(_ scope: [String]) -> String {
    scope.isEmpty ? (project["name"]?.string ?? identity.url.lastPathComponent) : displayName(key(scope))
  }

  /// Every scope of the document (root first), for the hierarchy.
  func childScopes(of scope: [String]) -> [[String]] {
    cards(in: scope).compactMap { c in
      guard c["canvas"] != nil, let id = c["id"]?.string else { return nil }
      return scope + [id]
    }
  }

  func usageCount(ofDefinition id: String) -> Int {
    func count(_ scope: [String]) -> Int {
      cards(in: scope).reduce(0) { n, c in
        var m = n + (c["definition"]?.string == id ? 1 : 0)
        if c["canvas"] != nil, let cid = c["id"]?.string { m += count(scope + [cid]) }
        return m
      }
    }
    return count([])
  }

  var isEditable: Bool { connected && !runActive }

  /// Source bytes as the draft sees them.
  func sourceText(_ p: String) async throws -> String {
    if let d = sourceEdits[p] { return String(decoding: d, as: UTF8.self) }
    guard let rpc else { throw RPCError.disconnected }
    return String(decoding: try await rpc.readFile(path: p, atRev: rev), as: UTF8.self)
  }

  func collapsed(_ k: CardKey) -> Bool { layout["cards"]?[k]?["collapsed"]?.bool ?? false }

  // MARK: - Cards and connections

  func uniqueId(_ base: String, in scope: [String]) -> String {
    let existing = Set(cards(in: scope).compactMap { $0["id"]?.string })
    let clean = base.lowercased().map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? $0 : "-" }
    var stem = String(clean).trimmingCharacters(in: CharacterSet(charactersIn: "-"))
    if stem.isEmpty { stem = "card" }
    var id = stem
    var n = 2
    while existing.contains(id) { id = "\(stem)-\(n)"; n += 1 }
    return id
  }

  @discardableResult
  func addCard(definition def: String, in scope: [String], at point: CGPoint) -> CardKey {
    let id = uniqueId(def, in: scope)
    let k = key(scope + [id])
    edit { s in
      s.updateGraph(scope) { g in
        var cards = g["cards"]?.array ?? []
        cards.append(.object([("id", .string(id)), ("definition", .string(def))]))
        g["cards"] = .array(cards)
      }
      s.setPosition(k, point)
    }
    return k
  }

  /// Copies Cards (settings and inner graphs) with new ids; connections
  /// between the copied Cards are copied too.
  func duplicate(_ keys: Set<CardKey>, in scope: [String]) -> Set<CardKey> {
    var made: Set<CardKey> = []
    edit { s in
      var map: [String: String] = [:]
      s.updateGraph(scope) { g in
        var cards = g["cards"]?.array ?? []
        var conns = g["connections"]?.array ?? []
        for k in keys.sorted() {
          guard let id = path(k).last, var c = cards.first(where: { $0["id"]?.string == id }) else { continue }
          var newId = id
          let taken = Set(cards.compactMap { $0["id"]?.string })
          var n = 2
          repeat { newId = "\(id)-\(n)"; n += 1 } while taken.contains(newId)
          map[id] = newId
          c["id"] = .string(newId)
          cards.append(c)
        }
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
        let ok = key(scope + [old]), nk = key(scope + [new])
        let p = s.position(ok)
        s.setPosition(nk, CGPoint(x: p.x + 30, y: p.y + 30))
        made.insert(nk)
      }
    }
    return made
  }

  func deleteCards(_ keys: Set<CardKey>, in scope: [String]) {
    let ids = Set(keys.compactMap { path($0).last })
    guard !ids.isEmpty else { return }
    edit { s in
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
      s.updateInstance(k) { c in
        var params = c["params"] ?? .object([])
        params[name] = value
        c["params"] = (params.object?.isEmpty ?? true) ? nil : params
      }
    }
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

  func uniqueDefinitionId(_ base: String) -> String {
    var stem = String(base.lowercased().map { $0.isLetter || $0.isNumber || $0 == "-" ? $0 : "-" })
    stem = stem.trimmingCharacters(in: CharacterSet(charactersIn: "-"))
    if stem.isEmpty { stem = "card" }
    var id = stem
    var n = 2
    while definitions[id] != nil || confirmedDefinitions[id] != nil { id = "\(stem)-\(n)"; n += 1 }
    return id
  }

  /// Adds a new definition from a scaffold to the draft.
  @discardableResult
  func createDefinition(name: String, template: DefinitionTemplate) -> String {
    let id = uniqueDefinitionId(name.isEmpty ? template.rawValue : name)
    let ns = id.replacingOccurrences(of: "-", with: "_")
    edit { s in
      let dir = "definitions/\(id)/"
      switch template {
      case .draft:
        s.setDefinition(id, card: .object([("name", .string(name)), ("template", .string("draft"))]),
                        meta: .object([("description", .string("")), ("plan", .string(""))]))
      case .input:
        s.setDefinition(id, card: .object([
          ("name", .string(name)), ("template", .string("input")),
          ("params", .array([.object([("name", .string("value")), ("type", .string("double")), ("data", .number("0"))])])),
        ]), meta: nil)
        s.putSource(dir + "_outputs.fbs", "namespace \(ns);\ntable Value { value:double; }\ntable Outputs { value:Value (required); }\nroot_type Outputs;\n")
      case .python:
        s.setDefinition(id, card: .object([
          ("name", .string(name)), ("language", .string("python")), ("inputs", .array([.object([("name", .string("in"))])])),
        ]), meta: nil)
        s.putSource(dir + "default.nix", "{ pkgs, inputs, params }:\n{\n  package = inputs.tsugumi.pythonCard { src = ./src; };\n}\n")
        s.putSource(dir + "_outputs.fbs", "namespace \(ns);\ntable Text { value:string; }\ntable Outputs { out:Text (required); }\nroot_type Outputs;\n")
        s.putSource(dir + "src/main.py", """
        import outputs.out.\(ns).Text as out


        def main(ctx):
            b = ctx.builder()
            text = b.CreateString("hello")
            out.Start(b)
            out.AddValue(b, text)
            ctx.output("out", b, out.End(b))

        """)
      case .cpp:
        s.setDefinition(id, card: .object([
          ("name", .string(name)), ("language", .string("cpp")), ("inputs", .array([.object([("name", .string("in"))])])),
        ]), meta: nil)
        s.putSource(dir + "default.nix", "{ pkgs, inputs, params }:\n{\n  package = inputs.tsugumi.cppCard { src = ./src; };\n}\n")
        s.putSource(dir + "_outputs.fbs", "namespace \(ns);\ntable Text { value:string; }\ntable Outputs { out:Text (required); }\nroot_type Outputs;\n")
        s.putSource(dir + "src/card.cpp", """
        #include <tsugumi/sdk/card.hpp>

        #include "_outputs_generated.h"

        void run(tsugumi::sdk::Context& ctx) {
          flatbuffers::FlatBufferBuilder b;
          b.Finish(outputs::out::\(ns)::CreateTextDirect(b, "hello"));
          ctx.output("out", b);
        }
        TSUGUMI_CARD(run)

        """)
      }
    }
    return id
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
