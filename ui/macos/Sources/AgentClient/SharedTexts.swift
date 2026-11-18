import Foundation

extension Shared {
  /// App notes from hostd/agent/Shared/agent.json, shared with the guide in hostd.
  static let guideTexts: [String: String] = {
    let data = try! JSONSerialization.jsonObject(with: Data(agentJSON.utf8)) as! [String: Any]
    let guide = data["guide"] as! [String: Any]
    return guide.compactMapValues { $0 as? String }
  }()

  /// `{name}` in a text replaced by its value, in one pass (values are not read again).
  static func fill(_ text: String, _ values: [String: String]) -> String {
    let re = try! Regex(#"\{(\w+)\}"#)
    return text.replacing(re) { m in values[String(m.output[1].substring ?? "")] ?? String(m.output[0].substring ?? "") }
  }
}

/// Button requests use the same model text as the Web UI.
public enum AgentRequests {
  public static func make(_ kind: String, scope: [String], cards: [String]) -> String {
    let data = try! JSONSerialization.jsonObject(with: Data(Shared.agentJSON.utf8)) as! [String: Any]
    let request = data[kind] as! [String: String]
    let location = data["draftRequest"] as! [String: String]
    let whereText = scope.isEmpty ? location["root"]!
      : Shared.fill(location["child"]!, ["scope": scope.joined(separator: "/")])
    return Shared.fill(request["text"]!, ["where": whereText, "list": cards.map { "- \($0)" }.joined(separator: "\n")])
  }
}
