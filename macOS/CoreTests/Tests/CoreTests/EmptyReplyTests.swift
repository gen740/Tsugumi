import Foundation
import Testing
import TsugumiAgent
import TsugumiRPC

/// A reply with nothing in it (declined before any output, or only thinking,
/// which is not kept): the turn says so, and the conversation can go on. The
/// fixture refuses empty content as the Messages API does. The same cases as
/// web/server/contract.ts.
@Suite(.serialized) struct EmptyReplyTests {
  private func errorText(_ log: EventLog) -> String? {
    log.all.compactMap { if case .error(let s) = $0 { return s }; return nil }.first
  }

  private func turn(_ agent: Agent, _ text: String) async -> EventLog {
    let log = EventLog()
    await agent.send(text, context: []) { log.add($0) }
    await waitFinished(log)
    return log
  }

  @Test(arguments: [("refuse this", "refusal"), ("think only", "end_turn")])
  func theTurnSaysSoAndTheConversationGoesOn(_ asked: String, _ stop: String) async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)

    let empty = await turn(agent, asked)
    let said = try #require(errorText(empty), "a silent stop: \(empty.all)")
    #expect(said.contains("empty reply") && said.contains(stop), "\(said)")
    #expect(await agent.conversation.messages.allSatisfy { !$0.content.isEmpty })
    // Nothing is left to resume: opening the conversation asks nothing again.
    #expect(await agent.hasUnfinishedWork == false)

    // The next request is not refused for the empty reply.
    let next = await turn(agent, "thanks")
    #expect(errorText(next) == nil, "\(next.all)")
    #expect(next.all.contains { if case .finished = $0 { return true }; return false }, "\(next.all)")
    #expect(next.text.contains("set <card>"), "\(next.all)")
    await client.close()
  }

  /// Retry asks the same again; it may be answered the same, and says so again.
  @Test func retryAsksAgain() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    _ = await turn(agent, "refuse this")
    let again = EventLog()
    await agent.retry { again.add($0) }
    await waitFinished(again)
    #expect(errorText(again)?.contains("empty reply") == true, "\(again.all)")
    await client.close()
  }

  /// A reply over the Responses API whose only output is a refusal has no text either.
  @Test func overTheResponsesAPI() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let account = "Tsugumi test OpenAI \(UUID().uuidString)"
    try Keychain.save("fixture-key", account: account)
    defer { Keychain.delete(account: account) }
    let provider = ProviderConfig(name: account, baseURL: URL(string: "http://127.0.0.1:\(h.fixturePort)")!, model: "fixture",
                                  auth: .apiKey, api: .openAIResponses)
    let agent = try makeAgent(h, client, provider: provider)
    let empty = await turn(agent, "refuse this")
    #expect(errorText(empty)?.contains("empty reply") == true, "\(empty.all)")
    let next = await turn(agent, "thanks")
    #expect(errorText(next) == nil, "\(next.all)")
    await client.close()
  }
}
