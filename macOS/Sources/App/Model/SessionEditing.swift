import AppKit
import Foundation
import TsugumiRPC

/// What a Card is, for choosing what the canvas and the editor offer.
enum CardKind: String {
  case trigger, input, fork, branch, merge, logger, draft, subgraph, code
  case switchCard = "switch"

  var editsValueOnCanvas: Bool { self == .input || self == .switchCard }

  var label: String {
    switch self {
    case .trigger: return String(localized: "開始")
    case .input: return String(localized: "入力")
    case .switchCard: return String(localized: "スイッチ")
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
    case .switchCard: return "switch.2"
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
  case report_rmarkdown, report_quarto
  case switchCard = "switch"
  var id: String { rawValue }
  var label: String {
    switch self {
    case .draft: return String(localized: "Draft（未実装の計画）")
    case .report_rmarkdown: return String(localized: "R Markdown レポート（HTML）")
    case .report_quarto: return String(localized: "Quarto レポート（HTML）")
    case .python: return String(localized: "Python の処理")
    case .cpp: return String(localized: "C++ の処理")
    case .input: return String(localized: "入力値")
    case .switchCard: return String(localized: "スイッチ")
    case .trigger: return String(localized: "開始")
    case .fork: return String(localized: "分配")
    case .branch: return String(localized: "分岐")
    case .merge: return String(localized: "合流")
    case .logger: return String(localized: "記録")
    }
  }
  var symbol: String { (CardKind(rawValue: rawValue) ?? (rawValue.hasPrefix("report_") || self == .python || self == .cpp ? .code : .draft)).symbol }
  /// A name for the new Card (its id; made unique).
  var baseName: String {
    switch self {
    case .python, .cpp: return "card"
    case .trigger: return "start"
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
    startClean(key(scope + [id]))
    updateGraph(scope) { g in
      var cards = g["cards"]?.array ?? []
      cards.append(.object([("id", .string(id))]))
      g["cards"] = .array(cards)
    }
  }

  /// A new Card's name can be one a Card had: nothing of that Card stays with it.
  /// The core keeps this project's layout up with renames and deletes, but a Card
  /// in Card's layout or a file written by hand can still hold a frame.
  private func startClean(_ k: CardKey) {
    forgetResult(k)
    updateLayout(of: Array(path(k).dropLast())) { dropLayoutEntries(of: k, from: &$0) }
  }

  /// The Card's place and frame (and the places of anything inside it).
  private func dropLayoutEntries(of k: CardKey, from l: inout JSONValue) {
    let lk = layoutKey(k)
    if var cards = l["cards"]?.object {
      cards.removeAll { $0.0 == lk || $0.0.hasPrefix(lk + "/") }
      l["cards"] = .object(cards)
    }
    l["displays"]?[lk] = nil
  }

  /// A new Card from a template, with its own definition folder.
  @discardableResult
  /// `clearOfOthers`: at the nearest place where it covers nothing (a Card put
  /// on the canvas without the user aiming it at this very spot).
  func addCard(template t: DefinitionTemplate, in scope: [String], at point: CGPoint, name: String? = nil,
               clearOfOthers: Bool = false) -> CardKey {
    let id = uniqueId(name ?? t.baseName, in: scope)
    let k = key(scope + [id])
    edit { s in
      s.appendCard(id, in: scope)
      s.writeTemplate(t, card: k)
      s.setPosition(k, clearOfOthers ? s.freeSpot(near: point, for: k, in: scope) : point)
    }
    return k
  }

  /// The nearest place to `point` where a Card covers no other Card, output
  /// frame or sticky note: `point` itself when it is free, else on rings
  /// around it. On the grid when the grid is on. The web's Scene.freeSpot is
  /// the same search.
  func freeSpot(near point: CGPoint, for k: CardKey, in scope: [String]) -> CGPoint {
    let prefs = AppPreferences.shared
    let step = prefs.showGrid ? prefs.gridSize : 20
    let start = prefs.showGrid ? CGPoint(x: snapToGrid(point.x, step), y: snapToGrid(point.y, step)) : point
    let size = geometry(k).size
    let margin = 16.0
    let others = cards(in: scope).compactMap { $0["id"]?.string }.map { key(scope + [$0]) }.filter { $0 != k }
    let taken = others.map { CGRect(origin: placedPosition($0), size: geometry($0).size) }
      + framedCards(in: scope).filter { $0 != k }.map { displayRect($0, origin: placedPosition($0)) }
      + notes(in: scope).map(noteRect)
    func free(_ p: CGPoint) -> Bool {
      let r = CGRect(origin: p, size: size).insetBy(dx: -margin, dy: -margin)
      return !taken.contains { r.intersects($0) }
    }
    if free(start) { return start }
    for ring in 1...200 {
      var spots: [(dx: Int, dy: Int)] = []
      for i in -ring...ring { spots += [(i, -ring), (i, ring)] }
      for i in (1 - ring)..<ring { spots += [(-ring, i), (ring, i)] }
      // Nearest first; among equals, the same order on the Mac and on the web.
      spots.sort { a, b in
        let da = a.dx * a.dx + a.dy * a.dy, db = b.dx * b.dx + b.dy * b.dy
        return da != db ? da < db : a.dy != b.dy ? a.dy > b.dy : a.dx > b.dx
      }
      for s in spots {
        let p = CGPoint(x: start.x + Double(s.dx) * step, y: start.y + Double(s.dy) * step)
        if free(p) { return p }
      }
    }
    return start
  }

  /// Copies a Card's definition folder (settings and every file) into a
  /// scope as a new Card. A Card in Card brings its own copy of the external.
  func copyDefinition(from k: CardKey, to scope: [String], name: String? = nil) async -> (id: String, apply: (ProjectSession) -> Void)? {
    guard let id = path(k).last, let prefix = projectPrefix(Array(path(k).dropLast())) else { return nil }
    return await copyDefinition(prefix: prefix, id: id, to: scope, name: name)
  }

  /// Copies the definition `id` of the project at `prefix` ("" for this one).
  func copyDefinition(prefix: String, id: String, to scope: [String], name: String? = nil) async
    -> (id: String, apply: (ProjectSession) -> Void)? {
    guard let d = prefix.isEmpty ? definitionDrafts[id] : external(prefix)?.definitions[id] else { return nil }
    let dir = prefix + "definitions/\(id)/"
    let newId = uniqueId(name ?? id, in: scope)
    let toPrefix = projectPrefix(scope) ?? ""
    var files: [String: Data] = [:]
    for p in sourcePaths where p.hasPrefix(dir) {
      let rel = String(p.dropFirst(dir.count))
      if ["_card.json", "_link.json", "_meta.json"].contains(rel) { continue }
      if let d = try? await rpcBytes(p) { files[toPrefix + "definitions/\(newId)/" + rel] = d }
    }
    // A Card in Card's external is copied along (a copy of its own).
    var link = d.link
    var ext: ExternalCopy?
    if let name = link?["external"]?.string {
      let extName = uniqueId(name, in: scope)
      ext = await copyExternal(from: prefix + "externals/\(name)/", to: toPrefix + "externals/\(extName)/")
      link = .object([("external", .string(extName))])
    }
    // The copy's comments are comments of its own.
    var meta = d.meta
    if let comments = meta?["comments"]?.array {
      meta?["comments"] = .array(comments.map { c in
        var n = c
        n["id"] = .string(UUID().uuidString.lowercased())
        return n
      })
    }
    let draft = DefinitionDraft(card: d.card, meta: meta, link: link)
    let newKey = key(scope + [newId])
    return (newId, { s in
      s.setCardDefinition(newKey, draft)
      for (p, bytes) in files {
        s.sourceRemovals.remove(p)
        s.sourceEdits[p] = bytes
      }
      ext?.apply(s)
    })
  }

  /// An external's documents and files, ready to be written under another folder.
  struct ExternalCopy {
    var docs: [String: ExternalProject] = [:]  // folder -> documents (the external and the ones in it)
    var files: [String: Data] = [:]

    @MainActor func apply(_ s: ProjectSession) {
      for (p, d) in files {
        s.sourceRemovals.remove(p)
        s.sourceEdits[p] = d
      }
      for (p, e) in docs.sorted(by: { $0.key < $1.key }) { s.createExternal(p, e) }
    }
  }

  /// Copies the external at `from` (with the externals in it) to the folder `to`.
  func copyExternal(from: String, to: String) async -> ExternalCopy {
    var c = ExternalCopy()
    let inside = effectiveExternals.filter { $0.key.hasPrefix(from) }
    for (p, e) in inside { c.docs[to + p.dropFirst(from.count)] = e }
    for p in sourcePaths where p.hasPrefix(from) {
      let rel = String(p.dropFirst(from.count))
      // Documents come from the parsed externals; every other file is copied.
      if inside.keys.contains(where: { p.hasPrefix($0) && ExternalProject.isDocument(String(p.dropFirst($0.count))) }) { continue }
      if let t = try? await rpcBytes(p) { c.files[to + rel] = t }
    }
    return c
  }

  /// A file's bytes as the draft sees them.
  func rpcBytes(_ p: String) async throws -> Data {
    if let d = sourceEdits[p] { return d }
    guard let rpc else { throw RPCError.disconnected }
    return try await rpc.readFile(path: p, atRev: rev)
  }

  /// Places a copy of an external (`from`) in a scope as a new Card in Card.
  @discardableResult
  func placeExternalCopy(from: String, in scope: [String], at point: CGPoint) async -> CardKey? {
    guard external(from) != nil else { return nil }
    let base = from.split(separator: "/").last.map(String.init) ?? "sub"
    let name = uniqueId(base, in: scope)
    let toPrefix = projectPrefix(scope) ?? ""
    let copy = await copyExternal(from: from, to: toPrefix + "externals/\(name)/")
    let k = key(scope + [name])
    edit { s in
      copy.apply(s)
      s.setCardDefinition(k, DefinitionDraft(card: .object([]), meta: nil, link: .object([("external", .string(name))])))
      s.appendCard(name, in: scope)
      s.setPosition(k, point)
    }
    return k
  }

  /// Places a copy of a definition of an external (or of this project) as a new Card.
  @discardableResult
  func placeDefinitionCopy(prefix: String, id: String, in scope: [String], at point: CGPoint) async -> CardKey? {
    guard let c = await copyDefinition(prefix: prefix, id: id, to: scope) else { return nil }
    let k = key(scope + [c.id])
    edit { s in
      c.apply(s)
      s.appendCard(c.id, in: scope)
      s.setPosition(k, point)
    }
    return k
  }

  /// Takes in another Tsugumi project as a Card in Card: its folder is
  /// copied to externals/<name>/ (without .tsugumi, .git and other hidden
  /// folders) and placed in the scope.
  @discardableResult
  func importProject(from folder: URL, in scope: [String], at point: CGPoint) throws -> CardKey {
    let fm = FileManager.default
    guard fm.fileExists(atPath: folder.appendingPathComponent("tsugumi.json").path) else {
      throw RPCFailure(kind: .invalidRequest, diagnostic: String(localized: "tsugumi.json のあるフォルダを選んでください。"))
    }
    var files: [String: Data] = [:]
    var total = 0
    let root = folder.standardizedFileURL.resolvingSymlinksInPath()
    let e = fm.enumerator(at: root, includingPropertiesForKeys: [.isDirectoryKey, .isSymbolicLinkKey, .isRegularFileKey],
                          options: [.skipsHiddenFiles])
    while let u = e?.nextObject() as? URL {
      let v = try? u.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey, .isRegularFileKey])
      if v?.isSymbolicLink == true { if v?.isDirectory == true { e?.skipDescendants() }; continue }  // e.g. Nix result links
      guard v?.isRegularFile == true else { continue }
      let rel = String(u.standardizedFileURL.resolvingSymlinksInPath().path.dropFirst(root.path.count + 1))
      guard let d = try? Data(contentsOf: u) else { continue }
      total += d.count
      if total > 64 << 20 {
        throw RPCFailure(kind: .invalidRequest, diagnostic: String(localized: "プロジェクトが大きすぎます（64 MB まで）。"))
      }
      files[rel] = d
    }
    func json(_ rel: String) -> JSONValue? { files[rel].flatMap { try? JSONValue.parse(String(decoding: $0, as: UTF8.self)) } }
    // The project and the externals in it, each with its documents.
    let projects = files.keys.filter { rel in
      guard rel.hasSuffix("tsugumi.json") else { return false }
      let parts = rel.split(separator: "/").dropLast()
      return parts.count % 2 == 0 && stride(from: 0, to: parts.count, by: 2).allSatisfy { parts[parts.startIndex + $0] == "externals" }
    }.map { String($0.dropLast("tsugumi.json".count)) }
    let name = uniqueId(folder.lastPathComponent, in: scope)
    let to = (projectPrefix(scope) ?? "") + "externals/\(name)/"
    var docs: [String: ExternalProject] = [:]
    for p in projects {
      guard let proj = json(p + "tsugumi.json") else { continue }
      var ext = ExternalProject(project: proj, layout: json(p + "layout.json") ?? .object([]), definitions: [:])
      for rel in files.keys where rel.hasPrefix(p + "definitions/") {
        let rest = rel.dropFirst((p + "definitions/").count).split(separator: "/")
        guard rest.count == 2 else { continue }
        let id = String(rest[0]), dir = p + "definitions/\(id)/"
        if rest[1] == "_link.json", let l = json(rel) { ext.definitions[id] = DefinitionDraft(card: .object([]), meta: json(dir + "_meta.json"), link: l) }
        if rest[1] == "_card.json", let c = json(rel) { ext.definitions[id] = DefinitionDraft(card: c, meta: json(dir + "_meta.json")) }
      }
      docs[to + p] = ext
    }
    let k = key(scope + [name])
    edit { s in
      for (rel, d) in files where !projects.contains(where: { rel.hasPrefix($0) && ExternalProject.isDocument(String(rel.dropFirst($0.count))) }) {
        s.sourceRemovals.remove(to + rel)
        s.sourceEdits[to + rel] = d
      }
      for (p, e) in docs.sorted(by: { $0.key < $1.key }) { s.createExternal(p, e) }
      s.setCardDefinition(k, DefinitionDraft(card: .object([]), meta: nil, link: .object([("external", .string(name))])))
      s.appendCard(name, in: scope)
      s.setPosition(k, point)
    }
    return k
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
      for new in map.values { s.startClean(key(scope + [new])) }
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

  /// Why a Card cannot take a name (nil: it can).
  func cardNameProblem(_ k: CardKey, _ raw: String) -> String? {
    let new = raw.trimmingCharacters(in: .whitespaces)
    guard new != path(k).last else { return nil }
    guard !new.isEmpty, new.count <= 128, new != ".", new != "..",
          new.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || "-_.".contains($0)) }) else {
      return String(localized: "名前に使えるのは英数字と - _ . だけです（128 文字まで）。")
    }
    if uniqueId(new, in: Array(path(k).dropLast())) != new { return String(localized: "「\(new)」はすでに使われています。") }
    return nil
  }

  // MARK: - A Draft written on the canvas

  /// Whether a Draft has no name of its own: its id is one made for it.
  func draftUntitled(_ k: CardKey) -> Bool {
    kind(k) == .draft && displayName(k).range(of: #"^draft(-[0-9]+)?$"#, options: .regularExpression) != nil
  }

  /// Starts writing a Draft's name and content on its Card.
  func startDraftEdit(_ k: CardKey) {
    inlineDraft = InlineDraft(card: k, name: draftUntitled(k) ? "" : displayName(k), content: cardMeta(k)?["content"]?.string ?? "")
  }

  /// Why the name typed on a Draft's Card cannot be taken (no name is fine).
  func draftNameProblem(_ k: CardKey, _ name: String) -> String? {
    name.trimmingCharacters(in: .whitespaces).isEmpty ? nil : cardNameProblem(k, name)
  }

  /// Keeps what was written. A name the Card cannot take stays as it was, with
  /// a notice. The Card's key when it was renamed.
  func finishDraftEdit() async -> CardKey? {
    guard let e = inlineDraft else { return nil }
    inlineDraft = nil
    guard instance(e.card) != nil else { return nil }
    if e.content != cardMeta(e.card)?["content"]?.string ?? "" {
      edit { s in s.updateMeta(e.card) { $0["content"] = e.content.isEmpty ? nil : .string(e.content) } }
    }
    // No name: an id is made for it (a Draft that has none keeps the one it has).
    var name = e.name.trimmingCharacters(in: .whitespaces)
    if name.isEmpty {
      if draftUntitled(e.card) { return nil }
      name = uniqueId("draft", in: Array(path(e.card).dropLast()))
    }
    guard name != displayName(e.card) else { return nil }
    do {
      try await renameCard(e.card, to: name)
      return key(Array(path(e.card).dropLast()) + [name])
    } catch {
      notice = (error as? RPCFailure)?.diagnostic ?? "\(error)"
      return nil
    }
  }

  /// Renames a Card (its id, its definition folder): saved at once, with its
  /// connections and positions following (and, for this project's Cards, its
  /// run history).
  func renameCard(_ k: CardKey, to raw: String) async throws {
    guard let old = path(k).last else { return }
    let scope = Array(path(k).dropLast())
    let new = raw.trimmingCharacters(in: .whitespaces)
    guard new != old else { return }
    if let problem = cardNameProblem(k, new) { throw RPCFailure(kind: .invalidRequest, diagnostic: problem) }
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
      files[to + p.dropFirst(from.count)] = try await rpcBytes(p)
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
    // The run history of the Card (and of anything inside it) follows the name.
    pendingCardRenames.append((from: scope + [old], to: scope + [new]))
    await save()
  }

  func deleteCards(_ keys: Set<CardKey>, in scope: [String]) {
    edit { $0.removeCards(keys, in: scope) }
  }

  private func removeCards(_ keys: Set<CardKey>, in scope: [String]) {
    let ids = Set(keys.compactMap { path($0).last })
    guard !ids.isEmpty else { return }
    let prefix = projectPrefix(scope) ?? ""
    let s = self
    // Each Card's definition folder goes with it, and a Card in Card's own external copy.
    for k in keys {
      if let ext = s.cardExternal(k) { s.removeExternal(ext) }
      s.removeCardDefinition(k)
    }
    let cut = s.connections(in: scope).filter {
      ids.contains($0["from"]?["card"]?.string ?? "") || ids.contains($0["to"]?["card"]?.string ?? "")
    }
    s.updateGraph(scope) { g in
      g["cards"] = .array((g["cards"]?.array ?? []).filter { !ids.contains($0["id"]?.string ?? "") })
      g["connections"] = .array((g["connections"]?.array ?? []).filter { !cut.contains($0) })
    }
    s.dropDraftPorts(cut, in: scope)
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
      for k in keys { s.dropLayoutEntries(of: k, from: &l) }
    }
    s.updateGroups(in: scope) { groups in
      for i in groups.indices {
        groups[i]["cards"] = .array((groups[i]["cards"]?.array ?? []).filter { !ids.contains($0.string ?? "") })
      }
    }
  }

  func connect(from: (CardKey, String), to: (CardKey, String), in scope: [String]) {
    guard let fromId = path(from.0).last, let toId = path(to.0).last else { return }
    edit { s in
      // An end at CardGeometry.newPort makes the Draft's port now.
      let fromPort = s.draftPort(from, output: true, other: to.1, in: scope)
      let toPort = s.draftPort(to, output: false, other: fromPort, in: scope)
      // One wire per output: a value goes to several Cards through a fork Card.
      // A second wire from an output inserts one (or, from a fork, adds a branch).
      let taken = s.connections(in: scope).filter {
        $0["from"]?["card"]?.string == fromId && $0["from"]?["port"]?.string == fromPort
          && !($0["to"]?["card"]?.string == toId && $0["to"]?["port"]?.string == toPort)
      }
      var source = (card: fromId, port: fromPort)
      if !taken.isEmpty {
        let next = taken.count == 1 ? taken[0]["to"]?["card"]?.string.map { key(scope + [$0]) } : nil
        if s.kind(from.0) == .fork {
          source = (fromId, s.addForkBranch(from.0, for: toId))
        } else if let next, s.kind(next) == .fork, let forkId = path(next).last {
          // The output already feeds a fork: one more branch of it.
          source = (forkId, s.addForkBranch(next, for: toId))
        } else {
          let fork = s.insertFork(after: (fromId, fromPort), wires: taken, in: scope)
          source = (fork, s.addForkBranch(key(scope + [fork]), for: toId))
        }
      }
      // One connection per input port: a new wire replaces the old one.
      let replaced = s.connections(in: scope).filter { $0["to"]?["card"]?.string == toId && $0["to"]?["port"]?.string == toPort }
      s.updateGraph(scope) { g in
        var conns = (g["connections"]?.array ?? []).filter { !replaced.contains($0) }
        conns.append(.object([
          ("from", .object([("card", .string(source.card)), ("port", .string(source.port))])),
          ("to", .object([("card", .string(toId)), ("port", .string(toPort))])),
        ]))
        g["connections"] = .array(conns)
      }
      s.dropDraftPorts(replaced, in: scope)
    }
  }

  /// Whether a wire of the scope ends at this port.
  func portWired(_ k: CardKey, _ port: String, output: Bool, in scope: [String]) -> Bool {
    let id = path(k).last
    let side = output ? "from" : "to"
    return connections(in: scope).contains { $0[side]?["card"]?.string == id && $0[side]?["port"]?.string == port }
  }

  /// The port a wire's end names. At CardGeometry.newPort: a Draft's port
  /// without a wire or a new one, named after the port at the wire's other end
  /// when that says more than "in" or "out". A Draft's ports are declared by
  /// its wires.
  private func draftPort(_ end: (CardKey, String), output: Bool, other: String, in scope: [String]) -> String {
    guard end.1 == CardGeometry.newPort else { return end.1 }
    let ports = portData(end.0)
    let names = (output ? ports.outputs : ports.inputs).map(\.name)
    if let free = names.first(where: { !portWired(end.0, $0, output: output, in: scope) }) { return free }
    let says = other.range(of: #"^[A-Za-z_][A-Za-z0-9_]*$"#, options: .regularExpression) != nil
      && other.range(of: #"^(in|out)[0-9]*$"#, options: .regularExpression) == nil
    let base = says ? other : output ? "out" : "in"
    var name = base
    var n = 2
    while names.contains(name) { name = "\(base)\(n)"; n += 1 }
    let note: JSONValue = .object([("name", .string(name))])
    if output {
      updateMeta(end.0) { $0["outputs"] = .array(($0["outputs"]?.array ?? []) + [note]) }
    } else {
      updateCard(end.0) { $0["inputs"] = .array(($0["inputs"]?.array ?? []) + [note]) }
    }
    return name
  }

  /// After wires are gone: a Draft's port goes with its last wire.
  private func dropDraftPorts(_ removed: [JSONValue], in scope: [String]) {
    func held(_ k: CardKey, _ port: String, output: Bool) -> Bool {
      if portWired(k, port, output: output, in: scope) { return true }
      // Inside a Card in Card, the outer Card's boundary may name it.
      guard !scope.isEmpty, let side = boundary(key(scope))?[output ? "outputs" : "inputs"]?.object else { return false }
      return side.contains { $0.1["card"]?.string == path(k).last && $0.1["port"]?.string == port }
    }
    func without(_ list: JSONValue?, _ port: String) -> JSONValue? {
      let rest = (list?.array ?? []).filter { $0["name"]?.string != port }
      return rest.isEmpty ? nil : .array(rest)
    }
    for c in removed {
      for (side, output) in [("from", true), ("to", false)] {
        guard let id = c[side]?["card"]?.string, let port = c[side]?["port"]?.string else { continue }
        let k = key(scope + [id])
        guard instance(k) != nil, kind(k) == .draft, !held(k, port, output: output) else { continue }
        if !output { updateCard(k) { $0["inputs"] = without($0["inputs"], port) } }
        let list = output ? "outputs" : "inputs"
        updateMeta(k) { $0[list] = without($0[list], port) }
      }
    }
  }

  /// A fork Card after an output: the output's wires (`wires`) now leave the
  /// fork, a branch each, and the output feeds the fork. Its id.
  func insertFork(after from: (card: String, port: String), wires: [JSONValue], in scope: [String]) -> String {
    let id = uniqueId("fork", in: scope)
    let k = key(scope + [id])
    appendCard(id, in: scope)
    writeTemplate(.fork, card: k)
    updateCard(k) { c in c["params"] = .array([.object([("name", .string("branches")), ("type", .string("string[]")), ("data", .array([]))])]) }
    // Between the output's Card and the first Card it fed.
    let a = position(key(scope + [from.card]))
    let b = wires.first?["to"]?["card"]?.string.map { position(key(scope + [$0])) } ?? CGPoint(x: a.x + 300, y: a.y)
    setPosition(k, CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2))
    var branches: [String] = []
    for w in wires { branches.append(addForkBranch(k, for: w["to"]?["card"]?.string ?? "out")) }
    updateGraph(scope) { g in
      var conns = (g["connections"]?.array ?? []).filter { !wires.contains($0) }
      for (w, branch) in zip(wires, branches) {
        conns.append(.object([("from", .object([("card", .string(id)), ("port", .string(branch))])), ("to", w["to"] ?? .null)]))
      }
      conns.append(.object([
        ("from", .object([("card", .string(from.card)), ("port", .string(from.port))])),
        ("to", .object([("card", .string(id)), ("port", .string("in"))])),
      ]))
      g["connections"] = .array(conns)
    }
    return id
  }

  /// A new output of a fork Card, named after the Card it will feed. Its name.
  @discardableResult
  func addForkBranch(_ k: CardKey, for target: String) -> String {
    var branches = cardDefinition(k)?["params"]?.array?.first { $0["name"]?.string == "branches" }?["data"]?.array?.compactMap(\.string)
      ?? ["out1", "out2"]
    let base = target.isEmpty ? "out" : target
    var name = base
    var n = 2
    while branches.contains(name) { name = "\(base)_\(n)"; n += 1 }
    branches.append(name)
    updateCard(k) { c in
      var params = (c["params"]?.array ?? []).filter { $0["name"]?.string != "branches" }
      params.append(.object([("name", .string("branches")), ("type", .string("string[]")), ("data", .array(branches.map { .string($0) }))]))
      c["params"] = .array(params)
    }
    return name
  }

  func removeConnection(at index: Int, in scope: [String]) {
    edit { s in
      var conns = s.connections(in: scope)
      guard index < conns.count else { return }
      let removed = conns.remove(at: index)
      s.updateGraph(scope) { g in g["connections"] = .array(conns) }
      s.dropDraftPorts([removed], in: scope)
    }
  }

  /// Removes connections (as they are in the scope's canvas).
  func removeConnections(_ list: [JSONValue], in scope: [String]) {
    guard !list.isEmpty else { return }
    edit { s in
      s.updateGraph(scope) { g in
        g["connections"] = .array((g["connections"]?.array ?? []).filter { !list.contains($0) })
      }
      s.dropDraftPorts(list, in: scope)
    }
  }

  func disconnect(_ k: CardKey, inputs: Bool, in scope: [String]) {
    guard let id = path(k).last else { return }
    edit { s in
      let removed = s.connections(in: scope).filter { ($0[inputs ? "to" : "from"]?["card"]?.string ?? "") == id }
      s.updateGraph(scope) { g in
        g["connections"] = .array((g["connections"]?.array ?? []).filter { !removed.contains($0) })
      }
      s.dropDraftPorts(removed, in: scope)
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
        if let i = params.firstIndex(where: { $0["name"]?.string == name }) {
          params[i]["data"] = value
        } else if c["template"]?.string == CardKind.switchCard.rawValue, name == "value", let value {
          params.append(.object([("name", .string(name)), ("type", .string("bool")), ("data", value)]))
        }
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
        if let d = try? await rpcBytes(p) { files[ext + "definitions/\(id)/" + p.dropFirst(dir.count)] = d }
      }
      if let inner = cardExternal(k), let e = external(inner), let name = cardDefinitionDraft(k)?.link?["external"]?.string {
        let to = ext + "externals/\(name)/"
        for p in sourcePaths where p.hasPrefix(inner) && !ExternalProject.isDocument(String(p.dropFirst(inner.count))) {
          if let d = try? await rpcBytes(p) { files[to + p.dropFirst(inner.count)] = d }
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
          ("id", .string(id)), ("scope", .string("")), ("text", .string(String(localized: "メモ"))), ("color", .string("yellow")),
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
    edit { $0.changeNote(id, fn) }
  }

  private func changeNote(_ id: String, _ fn: (inout JSONValue) -> Void) {
    guard let sc = scopeHolding("notes", id) else { return }
    updateLayout(of: sc) { l in
      guard var notes = l["notes"]?.array, let i = notes.firstIndex(where: { $0["id"]?.string == id }) else { return }
      fn(&notes[i])
      l["notes"] = .array(notes)
    }
  }

  func removeNote(_ id: String) {
    edit { $0.dropNote(id) }
  }

  private func dropNote(_ id: String) {
    guard let sc = scopeHolding("notes", id) else { return }
    updateLayout(of: sc) { $0["notes"] = .array(($0["notes"]?.array ?? []).filter { $0["id"]?.string != id }) }
  }

  func noteRect(_ n: JSONValue) -> CGRect {
    CGRect(x: n["x"]?.double ?? 0, y: n["y"]?.double ?? 0, width: n["width"]?.double ?? 240, height: n["height"]?.double ?? 120)
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

  func addGroup(_ keys: Set<CardKey>, in scope: [String], name: String = String(localized: "グループ")) -> String {
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
    edit { $0.changeDisplayFrame(k, fn) }
  }

  private func changeDisplayFrame(_ k: CardKey, _ fn: (inout JSONValue) -> Void) {
    let lk = layoutKey(k)
    updateLayout(of: Array(path(k).dropLast())) { l in
      var displays = l["displays"] ?? .object([])
      var d = displays[lk] ?? .object([])
      fn(&d)
      displays[lk] = d
      l["displays"] = displays
    }
  }

  /// Closes a frame; its Card stays. The layout keeps a frame only with its
  /// place and size, so one still drawn beside its Card gets them now.
  func closeDisplayFrame(_ k: CardKey) {
    edit { $0.hideDisplayFrame(k) }
  }

  private func hideDisplayFrame(_ k: CardKey) {
    let r = displayRect(k, origin: placedPosition(k))
    changeDisplayFrame(k) { f in
      f["x"] = .real(r.minX.rounded())
      f["y"] = .real(r.minY.rounded())
      f["width"] = .real(r.width)
      f["height"] = .real(r.height)
      f["hidden"] = .bool(true)
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
  /// only: no computation is re-resolved for a move. With a grid (`snap`) the
  /// Cards go by whole grids from where they are drawn, and what moves with them
  /// goes by the same step, so the arrangement stays. Notes and frames alone
  /// land on whole numbers, as when dragged alone.
  func moveSelection(cards: Set<CardKey>, notes: Set<String>, frames: Set<CardKey>, by moved: CGSize, snap: Double?) {
    let d = snap.flatMap { g in cards.isEmpty ? nil : CGSize(width: snapToGrid(moved.width, g), height: snapToGrid(moved.height, g)) } ?? moved
    edit { s in
      // Frames first: where one stands is worked out from its Card.
      for k in frames where s.instance(k) != nil && !s.frameFollowsCard(k, moving: cards) {
        let r = s.displayRect(k, origin: s.placedPosition(k))
        s.changeDisplayFrame(k) { f in
          f["x"] = .real((r.minX + d.width).rounded())
          f["y"] = .real((r.minY + d.height).rounded())
          f["width"] = .real(r.width)
          f["height"] = .real(r.height)
        }
      }
      for k in cards {
        let p = s.position(k)
        let from = snap.map { CGPoint(x: snapToGrid(p.x, $0), y: snapToGrid(p.y, $0)) } ?? p
        s.setPosition(k, CGPoint(x: from.x + d.width, y: from.y + d.height))
      }
      for id in notes {
        s.changeNote(id) { n in
          n["x"] = .real(((n["x"]?.double ?? 0) + d.width).rounded())
          n["y"] = .real(((n["y"]?.double ?? 0) + d.height).rounded())
        }
      }
    }
  }

  /// Deletes a selection as one edit. Notes go. A frame alone is closed and its
  /// Card stays (the frame's own Remove Frame); a deleted Card takes its frame with it.
  func deleteSelection(cards: Set<CardKey>, notes: Set<String>, frames: Set<CardKey>, in scope: [String]) {
    edit { s in
      s.removeCards(cards, in: scope)
      for k in frames where !cards.contains(k) && s.instance(k) != nil { s.hideDisplayFrame(k) }
      for id in notes { s.dropNote(id) }
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

  /// Lays out a scope's Cards, output frames and notes (layout only, one undo):
  /// see `arrangeCards`. Sizes and port positions are those of `CardGeometry`.
  func arrange(_ scope: [String]) {
    let ids = cards(in: scope).compactMap { $0["id"]?.string }
    let nodes: [ArrangeCard] = ids.map { id in
      let g = geometry(key(scope + [id]))
      func ports(_ names: [String], output: Bool) -> [ArrangePort] {
        names.map { let p = g.port($0, output: output); return ArrangePort(name: $0, x: Double(p.x), y: Double(p.y)) }
      }
      return ArrangeCard(id: id, width: Double(g.size.width), height: Double(g.size.height), inputs: ports(g.inputs, output: false), outputs: ports(g.outputs, output: true))
    }
    let wires: [ArrangeWire] = connections(in: scope).compactMap { c in
      guard let f = c["from"]?["card"]?.string, let fp = c["from"]?["port"]?.string,
            let t = c["to"]?["card"]?.string, let tp = c["to"]?["port"]?.string else { return nil }
      return ArrangeWire(from: f, fromPort: fp, to: t, toPort: tp)
    }
    let frames: [ArrangeBox] = ids.compactMap { id in
      let k = key(scope + [id]), f = displayFrame(k)
      guard displays[k] != nil || f != nil, f?["hidden"]?.bool != true else { return nil }
      return ArrangeBox(id: id, width: f?["width"]?.double ?? 320, height: f?["height"]?.double ?? 170)
    }
    let boxes: [ArrangeBox] = notes(in: scope).compactMap { n in
      n["id"]?.string.map { ArrangeBox(id: $0, width: n["width"]?.double ?? 240, height: n["height"]?.double ?? 120) }
    }
    func bar(_ side: String) -> [ArrangeBarEnd] {
      guard !scope.isEmpty else { return [] }
      return (boundary(key(scope))?[side]?.object ?? []).map { ArrangeBarEnd(card: $0.1["card"]?.string ?? "", port: $0.1["port"]?.string ?? "") }
    }
    let placed = arrangeCards(ArrangeInput(flow: flow, grid: AppPreferences.shared.gridSize, snap: AppPreferences.shared.showGrid, cards: nodes, wires: wires, frames: frames, notes: boxes,
                                           boundaryInputs: bar("inputs"), boundaryOutputs: bar("outputs")))
    edit { s in
      for c in placed.cards { s.setPosition(key(scope + [c.id]), CGPoint(x: c.x, y: c.y)) }
      s.updateLayout(of: scope) { l in
        if !placed.frames.isEmpty {
          var displays = l["displays"] ?? .object([])
          for f in placed.frames {
            let lk = layoutKey(key(scope + [f.id]))
            var d = displays[lk] ?? .object([])
            d["x"] = .real(f.x)
            d["y"] = .real(f.y)
            // The layout keeps a frame only with its place and size: one still drawn beside its Card gets them now.
            if let size = frames.first(where: { $0.id == f.id }) {
              d["width"] = .real(size.width)
              d["height"] = .real(size.height)
            }
            displays[lk] = d
          }
          l["displays"] = displays
        }
        if !placed.notes.isEmpty, let list = l["notes"]?.array {
          l["notes"] = .array(list.map { n in
            guard let at = placed.notes.first(where: { $0.id == n["id"]?.string }), (n["scope"]?.string ?? "") == "" else { return n }
            var copy = n
            copy["x"] = .real(at.x)
            copy["y"] = .real(at.y)
            return copy
          })
        }
        // The bars of a Card in Card go back to following the Cards.
        l["boundary"] = nil
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

  func hasSource(_ p: String) -> Bool {
    !sourceRemovals.contains(p) && (sources[p] != nil || sourceEdits[p] != nil)
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
    case .report_rmarkdown, .report_quarto:
      let files = ReportTemplates.files[t.rawValue]!
      set(try! JSONValue.parse(files["_card.json"]!), meta: try! JSONValue.parse(files["_meta.json"]!))
      for (file, text) in files where file != "_card.json" && file != "_meta.json" { putSource(dir + file, text) }
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
    case .switchCard:
      set(.object([
        ("template", .string(t.rawValue)),
        ("params", .array([.object([("name", .string("value")), ("type", .string("bool")), ("data", .bool(false))])])),
      ]))
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

  func clearBuildCache() async throws -> Tsugumi_RPC_ProjectCacheClearResultT {
    guard let rpc else { throw RPCError.disconnected }
    return try await rpc.clearCache(opId: UUID())
  }

  func deleteRunHistory(endedBefore: Date?) async throws -> Tsugumi_RPC_RunHistoryDeleteResultT {
    guard let rpc else { throw RPCError.disconnected }
    return try await rpc.deleteHistory(opId: UUID(), endedBefore: endedBefore)
  }
}
