import Foundation
import Testing
import TsugumiAgent
import TsugumiRPC

/// The guide bird (Agent.Role.guide): it reads and explains, never changes
/// the project, and offers what to do as buttons (guide_show).
@Suite(.serialized) struct GuideTests {
  @Test func theGuideIsOfferedOnlyReadsAndItsOwnTool() async throws {
    let catalog = try ToolCatalog(schema: try BFBS(data: try #require(RPCSchema.bfbs())))
    let guide = try Agent(catalog: catalog, transport: NoTransport(), provider: .localFixture(port: 1),
                      storage: FileManager.default.temporaryDirectory.appendingPathComponent("guide-\(UUID()).json"), role: .guide)
    let names = Set(guide.catalog.tools.map(\.name))
    let offered = Set(await guide.tools.map(\.name))
    #expect(offered.contains("guide_show") && !offered.contains("delegate"))
    for t in offered where t != "guide_show" {
      #expect(names.contains(t), "\(t) is not an RPC tool")
      #expect(catalog.tool(named: t)?.mutating == false, "\(t) changes the project")
    }
    #expect(!offered.contains("project_save") && !offered.contains("run") && !offered.contains("run_cancel"))
    #expect(Agent.guidePrompt.hasPrefix("You are Tsugumi, the guide bird"))
    // The chat's Agent is the same Tsugumi: it can fly and offer choices too; its helpers cannot.
    let chat = try Agent(catalog: catalog, transport: NoTransport(), provider: .localFixture(port: 1),
                         storage: FileManager.default.temporaryDirectory.appendingPathComponent("chat-\(UUID()).json"))
    let chatTools = Set(await chat.tools.map(\.name))
    #expect(chatTools.contains("guide_show") && chatTools.contains("project_save"))
    // Its guiding is the built-in skill $demo (a tour or the user starts it), ending with a farewell.
    #expect(Agent.demoSkill.hasPrefix("---\nname: demo\n") && Agent.demoSkill.contains("\"endTour\": true"))
    let helper = try Agent(catalog: catalog, transport: NoTransport(), provider: .localFixture(port: 1),
                           storage: FileManager.default.temporaryDirectory.appendingPathComponent("helper-\(UUID()).json"),
                           helper: .init(name: "a", cards: ["a"], instruction: "x"))
    let helperTools = Set(await helper.tools.map(\.name))
    #expect(!helperTools.contains("guide_show"))
  }

  /// With the local model and a real tsugumid: the guide reads the project,
  /// its attempt to save is refused before anything is sent, it points at
  /// the run button with a button to press, and the project is unchanged.
  @Test func aQuestionReadsAndChangesNothing() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let before = try await client.inspect().rev
    let guide = try makeAgent(h, client, role: .guide)
    let log = EventLog()
    await guide.send(prompt: "What does this do?\n\n[Screen]\nscreen: the project", title: "What does this do?") { log.add($0) }
    await waitFinished(log)
    #expect(log.text.contains("I read 3 Cards; the save was refused"), "\(log.all)")
    #expect(!log.text.contains("project_save") && log.text.contains("guide_show") && log.text.contains("project_inspect"))
    let shown = log.all.compactMap { e -> String? in
      if case .toolStarted(_, "guide_show", let args) = e { return args }
      return nil
    }
    #expect(shown.count == 1 && shown[0].contains("\"run\""))
    let refused = log.all.contains { if case .toolFinished(_, "project_save", false, let s) = $0 { return s.contains("only reads") }; return false }
    #expect(refused)
    #expect(try await client.inspect().rev == before)
    // The trace has no save sent to tsugumid.
    #expect(!(await guide.conversation).tools.contains { $0.name == "project_save" && $0.opId != nil })
    await client.close()
  }

  /// An [Event] note of the app is answered from the state it names.
  @Test func anEventIsAnsweredFromItsState() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let guide = try makeAgent(h, client, role: .guide)
    let log = EventLog()
    let note = Agent.guideText("event", ["event": Agent.guideText("runEnded", ["run": "abc", "state": "failed"])])
    await guide.send(prompt: note, title: "run ended", display: "実行が終わりました") { log.add($0) }
    await waitFinished(log)
    #expect(log.text == "The run abc ended: failed.", "\(log.all)")
    await client.close()
  }
}
