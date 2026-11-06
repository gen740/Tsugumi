import Foundation
import Testing
@testable import TsugumiAgent
import TsugumiRPC

/// The arguments stream as the model writes them (eager_input_streaming), so
/// the API does not check them: a reply can end as usual with broken ones. They
/// never run, and the model is asked to write them again. The same cases as
/// web/server/contract.ts.
@Suite(.serialized) struct BrokenArgumentsTests {
  private func says(_ log: EventLog) -> String { log.text }

  @Test(arguments: [("write a broken project_save 2 times", "[{}, {}]", 2), ("write a broken delegate 1 times as a list", "[{}]", 1)])
  func theyRunNothingAndAreAskedAgain(_ asked: String, _ sentBack: String, _ tries: Int) async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send(asked, context: []) { log.add($0) }
    await waitFinished(log)
    #expect(log.all.contains { if case .finished = $0 { return true }; return false }, "\(log.all)")
    #expect(says(log).contains("Errors: [\(Array(repeating: "True", count: tries).joined(separator: ", "))]"), "\(says(log))")
    #expect(says(log).contains("Inputs sent back: \(sentBack)"), "\(says(log))")
    #expect(says(log).contains("Told to write it again: True. Shown my text: True"), "\(says(log))")
    let conv = await agent.conversation
    #expect(conv.tools.allSatisfy { $0.isError && $0.state == .completed && $0.opId == nil }, "never sent")
    await client.close()
  }

  @Test func theyDoNotRunWhatNeedsFewEither() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let shown = try await agent.runTool(id: "t", name: "guide_show", input: #"{"text":"hi"#) { _ in }
    guard case .toolResult(_, let content, let isError) = shown else { Issue.record("a result expected"); return }
    #expect(isError && content.contains("INVALID_JSON"), "\(content)")
    await client.close()
  }

  @Test func aModelThatKeepsBreakingThemIsNotAskedForEver() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send("write a broken project_save 9 times", context: []) { log.add($0) }
    await waitFinished(log)
    #expect(log.all.contains { if case .error(let s) = $0 { return s.contains("broken JSON three times") }; return false }, "\(log.all)")
    let conv = await agent.conversation
    #expect(conv.tools.count == 4)
    #expect(conv.messages.last?.role == "user", "each call has its result")
    await client.close()
  }

  @Test func theyGoBackAsAnEmptyCallAndOnlyTheMessagesAPIStreamsThemEagerly() throws {
    #expect(ContentBlock.arguments(#"{"a":1}"#) != nil)
    for broken in [#"{"a":1"#, "[1, 2]", "1", ""] {
      #expect(ContentBlock.arguments(broken) == nil)
      #expect((ContentBlock.toolUse(id: "t", name: "n", input: broken).wire["input"] as? [String: Any])?.isEmpty == true)
      let item = MessagesProvider.responsesInput([ChatMessage(role: "assistant", content: [.toolUse(id: "t", name: "n", input: broken)])])
      #expect(item[0]["arguments"] as? String == "{}")
    }
    let text = Agent.invalidArgumentsText(#"{"bytes":"a"b/c"#)
    #expect(text.contains("Make the call again") && text.contains(#"{"INVALID_JSON":"{\"bytes\":\"a\"b/c"}"#), "\(text)")

    let own = ToolSpec(name: "own", requestName: "", payloadType: 0, object: -1, description: "", inputSchema: [:], mutating: false)
    let tools = [Agent.webSearchTool, own]
    let wire = MessagesProvider.cachedBody(system: "", messages: [], tools: tools).tools as! [[String: Any]]
    #expect(wire[0]["eager_input_streaming"] == nil, "a server tool does not take it")
    #expect(wire[1]["eager_input_streaming"] as? Bool == true)
    for config in [ProviderConfig.openAI, ProviderConfig.chatGPT] {
      let body = MessagesProvider(config: config).responsesBody(system: "", messages: [], tools: tools)
      let json = String(decoding: try JSONSerialization.data(withJSONObject: body), as: UTF8.self)
      #expect(!json.contains("eager_input_streaming"))
    }
  }
}
