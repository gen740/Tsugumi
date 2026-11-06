import Foundation
import TsugumiRPC

/// From what the Agent does to the scenes of the canvas.
enum PresenceEvents {
  enum Look: Equatable {
    case visit([String])  // a Card path in the project
    case run([String])
  }

  /// The Card a tool call looks at: a file under `definitions/<id>/` or a
  /// `card` argument, a CardPath as the model writes it
  /// (`{"segments": ["bands"]}`, or a bare list).
  static func look(tool: String, arguments: String) -> Look? {
    guard let obj = (try? JSONSerialization.jsonObject(with: Data(arguments.utf8))) as? [String: Any] else { return nil }
    let segments = (obj["card"] as? [String]) ?? ((obj["card"] as? [String: Any])?["segments"] as? [String])
    let card = segments.flatMap { $0.isEmpty ? nil : $0 }
    switch tool {
    case "run": return card.map(Look.run)
    case "run_record", "log_range", "run_plan": return card.map(Look.visit)
    case "project_file_read":
      guard let p = obj["path"] as? String else { return nil }
      return cardOfFile(p).map { .visit([$0]) }
    default: return nil
    }
  }

  /// What the bubble says while the Agent does something that is not a
  /// scene of a Card (looking things up, checking, waiting); nil for the
  /// calls the scenes show.
  static func activity(tool: String) -> String? {
    switch tool {
    case "project_inspect": String(localized: "プロジェクトを見ています")
    case "reference_read": String(localized: "書き方を調べています")
    case "project_validate": String(localized: "変更を確かめています")
    case "project_save": String(localized: "保存しています")
    case "run_status": String(localized: "実行の終わりを待っています")
    case "run_record", "log_range", "run_invocations", "run_history", "run_plan": String(localized: "実行の記録を見ています")
    case "artifact_list", "artifact_read": String(localized: "出力を見ています")
    case "project_file_list", "project_file_read": String(localized: "ファイルを読んでいます")
    case "attached_folder_read": String(localized: "添えられたフォルダを読んでいます")
    case "run_cancel": String(localized: "実行を止めています")
    case "delegate": String(localized: "手分けして進めています")
    case "web_search": String(localized: "Web を検索しています")
    default: nil
    }
  }

  /// The tools whose arguments carry whole files, shown as they are written.
  static func writesSources(tool: String) -> Bool { tool == "project_save" || tool == "project_validate" }

  /// `definitions/<id>/<file>` → (id, file).
  static func fileOfCard(_ path: String) -> (card: String, file: String)? {
    let parts = path.split(separator: "/", omittingEmptySubsequences: true)
    guard parts.count >= 3, parts[0] == "definitions" else { return nil }
    return (String(parts[1]), parts[2...].joined(separator: "/"))
  }

  /// `definitions/<id>/...` → id.
  static func cardOfFile(_ path: String) -> String? {
    let parts = path.split(separator: "/", omittingEmptySubsequences: true)
    guard parts.count >= 2, parts[0] == "definitions" else { return nil }
    return String(parts[1])
  }

  struct Connection: Hashable {
    let from: String, fromPort: String, to: String, toPort: String

    init(from: String, fromPort: String, to: String, toPort: String) {
      self.from = from; self.fromPort = fromPort; self.to = to; self.toPort = toPort
    }

    init?(_ c: JSONValue) {
      guard let f = c["from"]?["card"]?.string, let fp = c["from"]?["port"]?.string,
            let t = c["to"]?["card"]?.string, let tp = c["to"]?["port"]?.string else { return nil }
      self.init(from: f, fromPort: fp, to: t, toPort: tp)
    }
  }

  /// What a save by the Agent wrote at the top level of the document.
  /// - Parameters:
  ///   - before/after: `template` of each definition (nil: a code Card, or none)
  ///   - existedBefore: definitions there before the save
  ///   - upserted: definitions the save wrote; `touchedSources`: Cards whose sources it wrote
  ///   - connectionsBefore/After: the top-level wiring
  ///   - order: where a Card is along the flow (written upstream first)
  static func written(upserted: [String], touchedSources: [String], existedBefore: Set<String>,
                      templateBefore: [String: String], templateAfter: [String: String], cards: Set<String>,
                      connectionsBefore: [Connection], connectionsAfter: [Connection],
                      order: (String) -> Double) -> (cards: [(String, reveal: Bool)], wires: [Connection]) {
    var out: [String: Bool] = [:]
    for id in upserted where cards.contains(id) {
      let wasDraftOrNew = !existedBefore.contains(id) || templateBefore[id] == "draft"
      let isDraft = templateAfter[id] == "draft"
      out[id] = wasDraftOrNew && !isDraft
    }
    for id in touchedSources where cards.contains(id) && out[id] == nil { out[id] = false }
    let ordered = out.keys.sorted { (order($0), $0) < (order($1), $1) }
    let revealed = Set(out.filter { $0.value }.keys)
    let before = Set(connectionsBefore)
    // New wires, and the wires out of a Card just written out (they are
    // threaded again from it).
    var wires: [Connection] = []
    for c in connectionsAfter where !before.contains(c) || revealed.contains(c.from) {
      if !wires.contains(c) { wires.append(c) }
    }
    wires.sort { (order($0.from), $0.from, order($0.to)) < (order($1.from), $1.from, order($1.to)) }
    return (ordered.map { ($0, out[$0]!) }, wires)
  }
}
