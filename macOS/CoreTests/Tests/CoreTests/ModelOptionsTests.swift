import Foundation
import Testing
import TsugumiAgent

/// The settings menu's choices. Provider HTTP requests are tested through the
/// core's public RPC and an independent HTTP peer in tests/core.
struct ModelOptionsTests {
  @Test func offersMatchTheModelMenu() throws {
    let fixtures = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
      .appendingPathComponent("../../../../tests/fixtures").standardizedFileURL
    let data = try Data(contentsOf: fixtures.appendingPathComponent("agent-contract.json"))
    let fixture = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
    for c in try #require(fixture["modelOptions"] as? [[String: Any]]) {
      let connection = c["connection"] as! String, model = c["model"] as! String
      let expected = c["expect"] as! [String: Any]
      let options = ModelOptions(connection: connection, model: model)
      #expect(options.efforts == expected["efforts"] as! [String])
      #expect(options.defaultEffort == expected["default"] as? String)
      #expect(options.fastPrice == expected["fastPrice"] as? Double)
    }
  }
}
