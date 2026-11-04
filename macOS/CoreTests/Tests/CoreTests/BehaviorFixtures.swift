import Foundation
import Testing

func agentBehaviorCases(_ key: String) throws -> [[String: Any]] {
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../..")
  let data = try Data(contentsOf: root.appendingPathComponent("tests/fixtures/agent-behavior.json"))
  let fixture = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
  return try #require(fixture[key] as? [[String: Any]])
}
