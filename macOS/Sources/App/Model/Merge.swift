import Foundation
import TsugumiRPC

/// Three-way merge. Named records merge by identity; only graph connections
/// and Card membership use set semantics. Ordered values stay ordered, with
/// the local value winning a same-field conflict.
func merge3(base: JSONValue?, ours: JSONValue?, theirs: JSONValue?, field: String = "") -> JSONValue? {
  if ours == base { return theirs }
  if theirs == base || theirs == ours { return ours }
  guard let base, let ours, let theirs else { return ours }
  switch (base, ours, theirs) {
  case (.object(let b), .object(let o), .object(let t)):
    var out: [(String, JSONValue)] = []
    var seen = Set<String>()
    func value(_ list: [(String, JSONValue)], _ k: String) -> JSONValue? { list.first { $0.0 == k }?.1 }
    for (k, tv) in t {
      seen.insert(k)
      if let v = merge3(base: value(b, k), ours: value(o, k), theirs: tv, field: k) { out.append((k, v)) }
    }
    for (k, ov) in o where !seen.contains(k) {
      if let v = merge3(base: value(b, k), ours: ov, theirs: nil, field: k) { out.append((k, v)) }
    }
    return .object(out)
  case (.array(let b), .array(let o), .array(let t)):
    let identity = ["params", "inputs", "hostFolders"].contains(field) ? "name" : "id"
    func ids(_ a: [JSONValue]) -> [String]? {
      let list = a.compactMap { $0[identity]?.string }
      return list.count == a.count && Set(list).count == list.count ? list : nil
    }
    if let bi = ids(b), let oi = ids(o), let ti = ids(t) {
      let bm = Dictionary(uniqueKeysWithValues: zip(bi, b)), om = Dictionary(uniqueKeysWithValues: zip(oi, o))
      let tm = Dictionary(uniqueKeysWithValues: zip(ti, t))
      // Preserve a local reorder; otherwise follow the remote order. Include
      // additions from both sides, then let merge3 decide deletions and edits.
      let first = oi == bi ? ti : oi, second = oi == bi ? oi : ti
      let order = first + second.filter { !first.contains($0) }
      return .array(order.compactMap { merge3(base: bm[$0], ours: om[$0], theirs: tm[$0]) })
    }
    if field == "connections" || (field == "cards" && (b + o + t).allSatisfy { $0.string != nil }) {
      let removed = b.filter { !o.contains($0) }
      var out = t.filter { !removed.contains($0) }
      for v in o where !b.contains(v) && !out.contains(v) { out.append(v) }
      return .array(out)
    }
    return ours
  default:
    return ours
  }
}

extension DefinitionDraft {
  /// Three-way merge of one Card's definition files (nil = no definition).
  static func merge(base: DefinitionDraft?, ours: DefinitionDraft?, theirs: DefinitionDraft?) -> DefinitionDraft? {
    if ours == base { return theirs }
    if theirs == base || theirs == ours { return ours }
    guard let ours else { return nil }  // removed here: the removal wins
    guard let theirs else { return ours }
    let card = merge3(base: base?.card, ours: ours.card, theirs: theirs.card) ?? ours.card
    let meta = merge3(base: base?.meta, ours: ours.meta, theirs: theirs.meta)
    let link = merge3(base: base?.link, ours: ours.link, theirs: theirs.link)
    return DefinitionDraft(card: link == nil ? card : .object([]), meta: meta, link: link)
  }
}

extension ExternalProject {
  static func merge(base: ExternalProject?, ours: ExternalProject, theirs: ExternalProject?) -> ExternalProject {
    if ours == base { return theirs ?? ours }
    guard let theirs, theirs != base else { return ours }
    var out = theirs
    out.project = merge3(base: base?.project, ours: ours.project, theirs: theirs.project) ?? ours.project
    out.layout = merge3(base: base?.layout, ours: ours.layout, theirs: theirs.layout) ?? ours.layout
    let ids = Set(ours.definitions.keys).union(theirs.definitions.keys).union(base.map { Set($0.definitions.keys) } ?? [])
    out.definitions = [:]
    for id in ids {
      if let d = DefinitionDraft.merge(base: base?.definitions[id], ours: ours.definitions[id], theirs: theirs.definitions[id]) {
        out.definitions[id] = d
      }
    }
    return out
  }
}
