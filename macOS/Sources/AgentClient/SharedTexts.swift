import Foundation

extension Shared {
  /// App notes from agent/Shared/agent.json, shared with the guide in agentd.
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
