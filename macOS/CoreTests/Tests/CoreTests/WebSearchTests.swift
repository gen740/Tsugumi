import FlatBuffers
import Foundation
import Testing
@testable import TsugumiAgent
import TsugumiRPC

/// Nested in AgentFixtureTests so that it runs one test at a time with that
/// suite (.serialized applies to nested suites): both use the Keychain, and a
/// read without asking fails while another thread is in a Keychain operation.
extension AgentFixtureTests {
  /// Web search is the provider's own tool: off, the model never hears of it;
  /// on, a search shows like any tool, its sources can be followed, and its
  /// blocks go back to the API as they came (the fixture refuses them
  /// otherwise). The same cases as web/server/contract.ts.
  @Suite struct WebSearchTests {
    struct Seen: Decodable {
      let path: String
      let tools: [String]
      let include: [String]?
      let names_web_search: Bool
    }

    func requests(_ h: AgentHarness) async throws -> [Seen] {
      let (data, _) = try await URLSession.shared.data(from: URL(string: "http://127.0.0.1:\(h.fixturePort)/fixture/requests")!)
      return try JSONDecoder().decode([Seen].self, from: data)
    }

    /// A connection to the fixture as a real provider: only the provider's own
    /// connections may search, and those have a key.
    func connection(_ h: AgentHarness, _ account: String, api: ProviderConfig.API = .anthropicMessages, webSearch: Bool) -> ProviderConfig {
      var c = ProviderConfig(name: account, baseURL: URL(string: "http://127.0.0.1:\(h.fixturePort)")!, model: "fixture", auth: .apiKey, api: api)
      c.webSearch = webSearch
      return c
    }

    func turn(_ agent: Agent, _ text: String) async -> EventLog {
      let log = EventLog()
      await agent.send(text, context: []) { log.add($0) }
      await waitFinished(log)
      return log
    }

    func finished(_ log: EventLog, sourceLocation: SourceLocation = #_sourceLocation) {
      #expect(log.all.contains { if case .finished = $0 { return true }; return false }, "\(log.all)", sourceLocation: sourceLocation)
    }

    func names(_ agent: Agent) async -> [String] { await agent.tools.map(\.name) }

    func withConnection(_ body: (AgentHarness, RPCClient, String) async throws -> Void) async throws {
      var h = AgentHarness()
      try h.start()
      defer { h.stop() }
      let account = "Tsugumi test search \(UUID().uuidString)"
      try Keychain.save("fixture-key", account: account)
      defer { Keychain.delete(account: account) }
      let client = RPCClient(endpoint: h.endpoint!)
      _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
      try await body(h, client, account)
      await client.close()
    }

    @Test func offTheModelNeverHearsOfIt() async throws {
      try await withConnection { h, client, account in
        let agent = try makeAgent(h, client, provider: connection(h, account, webSearch: false))
        #expect(await !names(agent).contains("web_search"))
        #expect(await !agent.system.contains("web_search"))
        let log = await turn(agent, "search the web for tsugumi")
        finished(log)
        #expect(log.text.contains("no way to search"), "\(log.all)")
        let seen = try await requests(h)
        #expect(seen.last?.names_web_search == false)
        // On, but the local fixture has no search, and the guide and a helper do not search.
        var fixture = ProviderConfig.localFixture(port: h.fixturePort)
        fixture.webSearch = true
        #expect(await !names(try makeAgent(h, client, provider: fixture)).contains("web_search"))
        #expect(await !names(try makeAgent(h, client, provider: connection(h, account, webSearch: true), role: .guide)).contains("web_search"))
        let helper = try Agent(catalog: agent.catalog, transport: FailingTransport(), provider: connection(h, account, webSearch: true),
                               storage: h.dir.appendingPathComponent("helper.json"), helper: .init(name: "h", cards: ["h"], instruction: ""))
        #expect(await !names(helper).contains("web_search"))
      }
    }

    @Test func aSearchShowsItsSourcesAndGoesBackAsItCame() async throws {
      try await withConnection { h, client, account in
        let agent = try makeAgent(h, client, provider: connection(h, account, webSearch: true))
        let tools = await agent.tools
        #expect(tools.first?.name == "web_search")
        let without = try makeAgent(h, client, provider: connection(h, account, webSearch: false))
        #expect(await tools.count == without.tools.count + 1)
        #expect(await agent.system.contains("web_search"), "the rules say what a query may not hold")
        let wire = MessagesProvider.cachedBody(system: "", messages: [], tools: tools).tools as! [[String: Any]]
        #expect(wire[0]["type"] as? String == "web_search_20250305" && wire[0]["max_uses"] as? Int == 5 && wire[0]["input_schema"] == nil)
        #expect(wire.last?["cache_control"] != nil, "the last tool ends the cached prefix")

        let log = await turn(agent, "search the web for tsugumi")
        finished(log)
        #expect(try await requests(h).last?.tools.first == "web_search")
        var started = false, ended = false
        for e in log.all {
          if case .toolStarted(_, "web_search", let args) = e { started = args.contains("tsugumi") }
          if case .toolFinished(_, "web_search", let ok, let summary) = e { ended = ok && summary.contains("https://example.com/1") }
        }
        #expect(started && ended, "\(log.all)")
        #expect(log.text.contains("[Result 1 on tsugumi](https://example.com/1"), "the cited source is a link: \(log.text)")
        let conversation = await agent.conversation
        let record = conversation.tools.first { $0.name == "web_search" }
        #expect(record?.state == .completed && record?.result?.contains("https://example.com/2") == true && record?.arguments.contains("tsugumi") == true)
        let reply = conversation.messages[1]
        #expect(reply.providerContent?.contains("enc-1") == true, "the reply is kept as the provider sent it")
        #expect(reply.content.contains { if case .webSearch(let s) = $0 { return s.sources.count == 2 }; return false }, "\(reply.content)")
        // The next request sends it back (the fixture refuses it changed or left out).
        finished(await turn(agent, "thanks"))
        let again = MessagesProvider.cachedBody(system: "", messages: await agent.conversation.messages, tools: tools).messages as! [[String: Any]]
        #expect(String(describing: again[1]).contains("encrypted_index"), "\(again[1])")
        // A saved conversation reads back with it.
        #expect(try Conversation.read(h.dir.appendingPathComponent("conversation.json")).messages[1] == reply)

        // Turned off later: the tool and its blocks leave the request, the fact of the search stays.
        await agent.setProvider(connection(h, account, webSearch: false))
        #expect(await !names(agent).contains("web_search"))
        finished(await turn(agent, "thanks again"))
        #expect(try await requests(h).last?.names_web_search == false)
        let later = MessagesProvider.cachedBody(system: "", messages: await agent.conversation.messages, tools: []).messages as! [[String: Any]]
        #expect(String(describing: later[1]).contains("[web search: tsugumi"), "\(later[1])")
      }
    }

    @Test func aPausedTurnGoesOnWithNothingAdded() async throws {
      try await withConnection { h, client, account in
        let agent = try makeAgent(h, client, provider: connection(h, account, webSearch: true))
        let log = await turn(agent, "search the web for tsugumi with a pause")
        finished(log)
        #expect(await agent.conversation.messages.filter { $0.role == "user" }.count == 1)
        #expect(log.all.contains { if case .toolFinished(_, "web_search", true, _) = $0 { return true }; return false }, "\(log.all)")
        finished(await turn(agent, "thanks"))
      }
    }

    @Test func aSearchBesideOurToolRunsAfterIt() async throws {
      try await withConnection { h, client, account in
        let agent = try makeAgent(h, client, provider: connection(h, account, webSearch: true))
        finished(await turn(agent, "search the web for tsugumi alongside inspect"))
        let conversation = await agent.conversation
        #expect(conversation.messages.flatMap(\.content).contains { if case .webSearch(let s) = $0 { return s.sources.count == 2 }; return false })
        #expect(conversation.tools.contains { $0.name == "web_search" && $0.state == .completed })
        finished(await turn(agent, "thanks"))
      }
    }

    /// A search left open by a stopped turn cannot go back as it is (the API
    /// refuses it): it goes in words.
    @Test func aSearchLeftOpenGoesInWords() async throws {
      try await withConnection { h, client, account in
        let open = #"[{"type":"server_tool_use","id":"srvtoolu_open","name":"web_search","input":{"query":"open"}}]"#
        var c = Conversation()
        c.messages = [ChatMessage(role: "user", content: [.text("go")]),
                      ChatMessage(role: "assistant", content: [.webSearch(WebSearch(id: "srvtoolu_open", query: "open"))], providerContent: open)]
        try c.save(h.dir.appendingPathComponent("conversation.json"))
        let agent = try makeAgent(h, client, provider: connection(h, account, webSearch: true))
        finished(await turn(agent, "thanks"))
        // While it is the end of the conversation (pause_turn) it does go back.
        #expect(MessagesProvider.replayable(c.messages) == [1])
        #expect(MessagesProvider.replayable(c.messages + [ChatMessage(role: "user", content: [.text("next")])]).isEmpty)
        #expect(MessagesProvider.replayable(c.messages + [ChatMessage(role: "user", content: [.toolResult(toolUseId: "t", content: "", isError: false)])]) == [1])
      }
    }

    @Test func theResponsesAPIHasItsOwnTool() async throws {
      try await withConnection { h, client, account in
        let agent = try makeAgent(h, client, provider: connection(h, account, api: .openAIResponses, webSearch: true))
        #expect(await names(agent).contains("web_search"))
        let log = await turn(agent, "search the web for tsugumi")
        finished(log)
        let seen = try await requests(h).last
        #expect(seen?.tools.contains("web_search") == true && seen?.include?.contains("web_search_call.action.sources") == true, "\(String(describing: seen))")
        #expect(log.all.contains { if case .toolFinished(_, "web_search", _, let s) = $0 { return s.contains("https://example.com/2") }; return false }, "\(log.all)")
        #expect(log.text.contains("[Result 1 on tsugumi](https://example.com/1"), "\(log.text)")
        // Its history carries the search in words (no call item: nothing of the results could come back).
        finished(await turn(agent, "thanks"))
        let input = MessagesProvider.responsesInput(await agent.conversation.messages)
        #expect(String(describing: input).contains("[web search: tsugumi") && !String(describing: input).contains("web_search_call"), "\(input)")
        let off = try makeAgent(h, client, provider: connection(h, account, api: .openAIResponses, webSearch: false))
        finished(await turn(off, "search the web for tsugumi again"))
        #expect(try await requests(h).last?.names_web_search == false)
      }
    }

    /// The ChatGPT plan: the hosted tool beside the namespace, and no include
    /// (the plan's rules do not say it takes it).
    @Test func thePlanHasTheToolBesideItsNamespace() throws {
      var plan = ProviderConfig.chatGPT
      plan.webSearch = true
      let tool = ToolSpec(name: "project_inspect", requestName: "", payloadType: 0, object: 0, description: "", inputSchema: [:], mutating: false)
      let body = MessagesProvider(config: plan).responsesBody(system: "", messages: [], tools: [Agent.webSearchTool, tool])
      #expect((body["tools"] as? [[String: Any]])?.map { $0["type"] as? String } == ["namespace", "web_search"])
      #expect(body["include"] == nil)
      #expect(MessagesProvider(config: .openAI).responsesBody(system: "", messages: [], tools: [Agent.webSearchTool, tool])["include"] != nil)
      #expect(plan.searchesWeb && ProviderConfig.openAI.searchesWeb == false)
    }

    /// Sources come from the web: only http(s) links, and a title cannot break out of its link.
    @Test func sourcesAreSafeLinks() {
      let text = WebSearch.sourcesText([WebSource(url: "javascript:alert(1)", title: "x"), WebSource(url: "https://a.example/b)c", title: "a]b\nc"),
                                        WebSource(url: "https://a.example/b)c")])
      #expect(!text.contains("javascript:"), "\(text)")
      #expect(text.components(separatedBy: "https:").count == 2, "\(text)")
      #expect(text.contains("[a b c](https://a.example/b%29c)"), "\(text)")
      #expect(WebSearch.sourcesText([WebSource(url: "file:///etc/passwd")]).isEmpty)
    }
  }
}

/// For an Agent that never reaches the core.
private struct FailingTransport: ToolTransport {
  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: FlatBuffers.Table) {
    throw CancellationError()
  }
}
