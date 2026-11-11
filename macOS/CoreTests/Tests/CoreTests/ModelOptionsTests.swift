import Foundation
import Testing
@testable import TsugumiAgent
import TsugumiRPC

/// What the model menu offers and what its choices put into a request: the
/// cases of tests/fixtures/agent-contract.json, as in web/server/contract.ts.
@Suite struct ModelOptionsContractTests {
  static let fixtures = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("tests/fixtures")

  func same(_ a: Any?, _ b: Any?) throws -> Bool {
    func normal(_ v: Any?) throws -> Any {
      try JSONSerialization.jsonObject(with: JSONSerialization.data(withJSONObject: [v ?? NSNull()]))
    }
    return (try normal(a) as! NSArray).isEqual(try normal(b) as! NSArray)
  }

  @Test func offersAndRequestsMatchTheContract() throws {
    let data = try Data(contentsOf: Self.fixtures.appendingPathComponent("agent-contract.json"))
    let fixture = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
    for c in try #require(fixture["modelOptions"] as? [[String: Any]]) {
      let connection = c["connection"] as! String, model = c["model"] as! String
      let effort = c["effort"] as! String, fastModel = c["fastModel"] as! String
      let expect = c["expect"] as! [String: Any]
      let label = Comment(rawValue: "\(connection) \(model) \(effort) \(fastModel)")

      let o = ModelOptions(connection: connection, model: model)
      #expect(o.efforts == expect["efforts"] as! [String], label)
      #expect(o.defaultEffort == expect["default"] as? String, label)
      #expect(o.fastPrice == expect["fastPrice"] as? Double, label)

      var config: ProviderConfig
      switch connection {
      case "anthropic": config = .anthropic
      case "openai": config = .openAI
      case "chatgpt": config = .chatGPT
      default: config = .localFixture(port: 1)
      }
      #expect(config.connection == connection, label)
      config.model = model
      let provider = MessagesProvider(config: config.choosing(effort: effort, fastModel: fastModel))
      let body = config.api == .openAIResponses
        ? provider.responsesBody(system: "rules", messages: [], tools: [])
        : provider.messagesBody(MessagesProvider.cachedBody(system: "rules", messages: [], tools: []))
      for (k, v) in expect["present"] as! [String: Any] {
        #expect(try same(body[k], v), "\(label) \(k)")
      }
      for k in expect["absent"] as! [String] {
        #expect(body[k] == nil, "\(label): \(k) must not be sent")
      }
    }
  }
}

extension AgentFixtureTests {
  /// The chat's choices go with every step of its Agent; a compaction's
  /// summary carries neither. The same case as web/server/contract.ts.
  @Suite struct ModelChoicesRequestTests {
    struct Seen: Decodable {
      let effort: String?
      let speed: String?
      let output_limit: Int
    }

    func last(_ h: AgentHarness) async throws -> Seen? {
      let (data, _) = try await URLSession.shared.data(from: URL(string: "http://127.0.0.1:\(h.fixturePort)/fixture/requests")!)
      return try JSONDecoder().decode([Seen].self, from: data).last
    }

    @Test(arguments: [ProviderConfig.API.anthropicMessages, .openAIResponses])
    func theyGoWithTheAgentAndNotWithASummary(api: ProviderConfig.API) async throws {
      var h = AgentHarness()
      try h.start()
      defer { h.stop() }
      let account = "Tsugumi test options \(UUID().uuidString)"
      try Keychain.save("fixture-key", account: account)
      defer { Keychain.delete(account: account) }
      let client = RPCClient(endpoint: h.endpoint!)
      _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
      defer { Task { await client.close() } }

      var config = ProviderConfig(name: account, baseURL: URL(string: "http://127.0.0.1:\(h.fixturePort)")!, model: "fixture", auth: .apiKey, api: api)
      config.effort = "max"
      config.fast = true
      let agent = try makeAgent(h, client, provider: config)
      for said in ["one", "two", "three"] {
        let log = EventLog()
        await agent.send(said, context: []) { log.add($0) }
        await waitFinished(log)
      }
      let asked = try #require(try await last(h))
      #expect(asked.effort == "max" && asked.speed == "fast" && asked.output_limit == 64000, "\(asked)")

      let log = EventLog()
      await agent.compact { log.add($0) }
      await waitFinished(log)
      let summary = try #require(try await last(h))
      #expect(summary.effort == nil && summary.speed == nil && summary.output_limit == 32768, "\(summary)")
    }
  }
}
