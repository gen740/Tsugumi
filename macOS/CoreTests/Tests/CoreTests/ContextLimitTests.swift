import Foundation
import Testing
import TsugumiAgent
import TsugumiRPC

/// The context limit: whichever way the model's context runs out, the
/// conversation goes on, or the Agent says what to do. The fixture refuses a
/// request over --context-limit as the APIs do, and answers nothing of it.
@Suite(.serialized) struct ContextLimitTests {
  private func setUp(_ h: AgentHarness, bigFile: Int = 20_000, provider: ProviderConfig? = nil) async throws -> (Agent, RPCClient) {
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    try FileManager.default.createDirectory(at: h.dir, withIntermediateDirectories: true)
    let big = h.dir.appendingPathComponent("big.txt")
    try Data(String(repeating: "x", count: bigFile).utf8).write(to: big)
    return (try makeAgent(h, client, provider: provider, references: ["big.txt": big]), client)
  }

  private func say(_ agent: Agent, _ text: String) async -> EventLog {
    let log = EventLog()
    await agent.send(text, context: []) { log.add($0) }
    await waitFinished(log)
    return log
  }

  private func errors(_ log: EventLog) -> [String] {
    log.all.compactMap { if case .error(let s) = $0 { return s }; return nil }
  }

  private func finished(_ log: EventLog) -> Bool {
    log.all.contains { if case .finished = $0 { return true }; return false }
  }

  private func requests(_ h: AgentHarness) async throws -> [[String: Any]] {
    let url = URL(string: "http://127.0.0.1:\(h.fixturePort)/fixture/requests")!
    let data = try await URLSession(configuration: .ephemeral).data(from: url).0
    return try JSONSerialization.jsonObject(with: data) as! [[String: Any]]
  }

  /// Ten reads in one reply are more than the model can take in the next
  /// request (the chat shows ten rows of Check). Refused, the results are cut
  /// until they fit, the model answers, and the next request goes through. The
  /// conversation is too short to summarize: nothing but the results can shrink.
  @Test func resultsTooLargeForTheContextAreCutAndTheTurnEnds() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--context-limit", "6000"])
    defer { h.stop() }
    let (agent, client) = try await setUp(h)
    let log = await say(agent, "read big.txt 10 times")
    #expect(finished(log) && errors(log).isEmpty, "\(log.all)")
    #expect(log.text.contains("Read 10 results, 10 cut."), "\(log.all)")
    // The whole results stay in the records.
    #expect(await agent.conversation.tools.allSatisfy { $0.result?.count == 20_000 })
    let next = await say(agent, "and now?")
    #expect(finished(next) && errors(next).isEmpty, "\(next.all)")
    await client.close()
  }

  /// With earlier turns there is more to summarize, and that is not enough:
  /// the same reads stay in the last messages, which compaction keeps.
  @Test func compactionAloneDoesNotKeepOversizedResults() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--context-limit", "6000"])
    defer { h.stop() }
    let (agent, client) = try await setUp(h)
    for text in ["one", "two", "three"] { _ = await say(agent, text) }
    let log = await say(agent, "read big.txt 10 times")
    #expect(finished(log) && errors(log).isEmpty, "\(log.all)")
    #expect(log.all.contains { if case .compacted = $0 { return true }; return false }, "\(log.all)")
    let conv = await agent.conversation
    #expect(conv.summary != nil && !conv.archive.isEmpty)
    let next = await say(agent, "and now?")
    #expect(finished(next) && errors(next).isEmpty, "\(next.all)")
    await client.close()
  }

  /// Compaction over the threshold makes room: what it keeps is under the
  /// threshold, so it does not start again at each step (each time a summary
  /// and the whole prompt uncached) while the context stays as full. Before,
  /// it kept the last reads whole, and the next request was as large.
  @Test func compactionBringsTheContextUnderTheThreshold() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let (agent, client) = try await setUp(h, bigFile: 15_000)
    let log = await say(agent, "read big.txt 2 times in 3 rounds")
    #expect(finished(log), "\(log.all)")
    #expect(log.all.contains { if case .compacted = $0 { return true }; return false }, "\(log.all)")
    let next = await say(agent, "and now?")
    #expect(finished(next), "\(next.all)")
    let conv = await agent.conversation
    let threshold = ProviderConfig.localFixture(port: h.fixturePort).contextLimit * 3 / 4
    #expect(conv.usage.inputTokens <= threshold, "\(conv.usage)")
    await client.close()
  }

  /// A message that cannot fit however the rest shrinks: no wait, no retry
  /// that cannot work. The Agent says so, and a new conversation works.
  @Test func aMessageThatCannotFitSaysWhatToDo() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--context-limit", "6000"])
    defer { h.stop() }
    let (agent, client) = try await setUp(h)
    let log = await say(agent, String(repeating: "y", count: 100_000))
    #expect(errors(log) == [Agent.contextFullText], "\(log.all)")
    #expect(try await requests(h).count <= 4)
    try await agent.reset()
    let next = await say(agent, "hello")
    #expect(finished(next) && errors(next).isEmpty, "\(next.all)")
    await client.close()
  }

  /// The request for the summary has its own limit to fit in: a long text in
  /// the earlier turns does not end the conversation there.
  @Test func theSummaryRequestFitsTheContext() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--context-limit", "6000"])
    defer { h.stop() }
    var old = Conversation()
    old.messages = [ChatMessage(role: "user", content: [.text(String(repeating: "z", count: 60_000))]),
                    ChatMessage(role: "assistant", content: [.text("Noted.")])]
    for i in 0..<3 {
      old.messages += [ChatMessage(role: "user", content: [.text("question \(i)")]), ChatMessage(role: "assistant", content: [.text("answer \(i)")])]
    }
    old.usage.inputTokens = 5000
    try FileManager.default.createDirectory(at: h.dir, withIntermediateDirectories: true)
    try old.save(h.dir.appendingPathComponent("conversation.json"))
    let (agent, client) = try await setUp(h)
    let log = await say(agent, "hello")
    #expect(finished(log) && errors(log).isEmpty, "\(log.all)")
    #expect(await agent.conversation.summary != nil)
    await client.close()
  }

  /// A summary that fails over the threshold costs nothing yet: the model
  /// still takes the request, so the turn goes on and the user is told.
  @Test func aFailedSummaryDoesNotEndTheTurn() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--fail-summaries", "99"])
    defer { h.stop() }
    var old = Conversation()
    for i in 0..<4 {
      old.messages += [ChatMessage(role: "user", content: [.text("question \(i)")]), ChatMessage(role: "assistant", content: [.text("answer \(i)")])]
    }
    old.usage.inputTokens = 5000
    try FileManager.default.createDirectory(at: h.dir, withIntermediateDirectories: true)
    try old.save(h.dir.appendingPathComponent("conversation.json"))
    let (agent, client) = try await setUp(h)
    let log = await say(agent, "hello")
    #expect(finished(log) && errors(log).isEmpty, "\(log.all)")
    #expect(log.all.contains { if case .retrying(let s) = $0 { return s.contains("summar") }; return false }, "\(log.all)")
    await client.close()
  }

  /// A reply ended at the context window (HTTP 200, its own stop reason) is
  /// not an answer: its tool call does not run and the work goes on.
  @Test func aReplyEndedAtTheContextWindowGoesOn() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--window-cut-once"])
    defer { h.stop() }
    let (agent, client) = try await setUp(h)
    let log = await say(agent, "set src.text to \"after the window\" and run, then report out")
    #expect(finished(log), "\(log.all)")
    #expect(log.text.contains("succeeded"), "\(log.all)")
    #expect(await agent.conversation.tools.filter { $0.name == "project_save" }.count == 1, "the cut call never ran")
    await client.close()
  }

  /// Which refusals are the context limit (the same cases as the web Agent's).
  @Test func theRefusalsOfTheContextLimitAreKnown() throws {
    for c in try agentBehaviorCases("contextOverflow") {
      #expect(Agent.isContextOverflow(text: c["text"] as! String) == (c["overflow"] as! Bool), "\(c["text"]!)")
    }
  }
}
