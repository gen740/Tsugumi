import Foundation
import Testing
@testable import TsugumiApp
import TsugumiAgentClient
import TsugumiLauncher
import TsugumiRPC

@MainActor struct ReviewFixTests {
  @Test func restoredToolResultsFollowIDsAcrossArchive() {
    var conversation = Conversation()
    conversation.archive = [ChatMessage(role: "assistant", content: [
      .toolUse(id: "a", name: "project_save", input: "A"),
      .toolUse(id: "b", name: "project_save", input: "B"),
    ])]
    conversation.messages = [ChatMessage(role: "user", content: [
      .toolResult(toolUseId: "missing", content: "ignore", isError: false),
      .toolResult(toolUseId: "a", content: "A rejected", isError: true),
      .toolResult(toolUseId: "b", content: "B saved", isError: false),
    ])]
    let items = ChatController.restoredItems(conversation)
    #expect(items.map(\.text) == ["A\n→ A rejected", "B\n→ B saved"])
    #expect(items[0].tool?.ok == false && items[1].tool?.ok == true)
  }

  @Test func approvalsWaitInOrderAndReleaseEveryCaller() {
    var queue = ChatController.PermissionQueue()
    var answered: [String: Set<String>] = [:]
    queue.append(ChatController.PermissionPrompt(raw: ["first"]) { answered["first"] = $0 })
    queue.append(ChatController.PermissionPrompt(raw: ["second"]) { answered["second"] = $0 })
    #expect(queue.first?.raw == ["first"] && answered.isEmpty)
    let first = queue.first!.id
    queue.answer(first, allow: true)
    #expect(answered["first"] == ["first"] && answered["second"] == nil)
    #expect(queue.first?.raw == ["second"])
    queue.answer(first, allow: true)
    #expect(queue.first?.raw == ["second"] && answered["second"] == nil)
    queue.answer(queue.first!.id, allow: false)
    #expect(answered["second"] == [] && queue.first == nil)
    queue.append(ChatController.PermissionPrompt(raw: ["third"]) { answered["third"] = $0 })
    queue.append(ChatController.PermissionPrompt(raw: ["fourth"]) { answered["fourth"] = $0 })
    queue.release()
    #expect(answered["third"] == [] && answered["fourth"] == [] && queue.first == nil)
  }

  @Test func scratchSelectionExcludesUnknownBytes() {
    let a = Tsugumi_RPC_RunScratchUsageT(); a.path = "runs/a"; a.bytes = 1000
    let b = Tsugumi_RPC_RunScratchUsageT(); b.path = "runs/b"; b.bytes = 5
    let unknown = Tsugumi_RPC_RunScratchUsageT(); unknown.path = "unreadable"; unknown.bytes = 99; unknown.sizeUnknown = true
    let candidates = [a, b, unknown]
    var selection = ScratchSelection()
    #expect(selection.rows(candidates).isEmpty)
    selection.paths.insert(b.path)
    #expect(selection.knownBytes(candidates) == 5 && !selection.hasUnknown(candidates))
    selection.paths.insert(unknown.path)
    selection.paths.insert(a.path)
    selection.paths.remove(a.path)
    #expect(selection.rows(candidates).map(\.path) == [b.path, unknown.path])
    #expect(selection.knownBytes(candidates) == 5 && selection.hasUnknown(candidates))
  }

  @Test func displayKeepsItsRunMarkAfterAnotherBranchRuns() throws {
    let directory = try SyncTests.project()
    defer { try? FileManager.default.removeItem(at: directory) }
    let session = ProjectSession(identity: try ProjectIdentity(url: directory), environments: EnvironmentManager(image: nil))
    func accept(_ id: String, partial: Bool) {
      let status = Tsugumi_RPC_RunStatusT(); status.runId = id; status.partialRun = partial
      let accepted = Tsugumi_RPC_RunAcceptedEventT(); accepted.status = status
      let event = Tsugumi_RPC_RunEventT(); event.runId = id
      event.value = Tsugumi_RPC_RunEventValueUnion(accepted, type: .runacceptedevent)
      session.applyRunEvent(event)
    }
    func display(_ run: String) {
      let display = Tsugumi_Data_DisplayT(); display.runId = run; display.card = cardPath(["b"])
      display.invocation = 1; display.revision = 1
      let text = Tsugumi_Data_DisplayTextT(); text.text = "B result"
      display.value = Tsugumi_Data_DisplayValueUnion(text, type: .displaytext)
      session.applyDisplay(display)
    }
    accept("partial-b", partial: true); display("partial-b")
    #expect(session.nodeData("c").runMark == nil)
    accept("other-input", partial: false)
    #expect(session.nodeData("b").runMark == RunMarkLabel.partial)
    #expect(session.nodeData("b").runText == RunMarkLabel.partial)
    accept("whole", partial: false); display("whole")
    #expect(session.nodeData("b").runMark == nil)
  }
}

// Use the same daemon, positional RPC API and save helpers as the passing
// synchronization tests. This test requires a Unix socket outside the sandbox.
extension SyncTests {
  @Test func unchangedDraftDoesNotOverwriteAgentAndEditedDraftAsks() async throws {
    let directory = try Self.project()
    let daemon = try Daemon(project: directory)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: directory) }
    let session = ProjectSession(identity: try ProjectIdentity(url: directory), environments: EnvironmentManager(image: nil))
    session.endpointOverride = daemon.endpoint
    session.start()
    try await Self.connected(session)
    let agent = RPCClient(endpoint: daemon.endpoint.url)
    _ = try await agent.connect(token: daemon.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    func save(_ name: String, _ content: String) async throws {
      let snapshot = try await agent.inspect()
      let meta = JSONValue.object([("name", .string(name)), ("content", .string(content))])
      _ = try await agent.save(baseRev: snapshot.rev ?? 0, opId: UUID(), origin: "agent", changes: [
        Change.putDefinition(id: "a", cardJSON: #"{"template":"draft"}"#, metaJSON: meta.serialized()),
      ])
      for _ in 0..<100 where session.rev <= (snapshot.rev ?? 0) { try await Task.sleep(for: .milliseconds(20)) }
      #expect(session.rev > (snapshot.rev ?? 0))
    }
    try await save("base", "base")
    await session.startDraftEdit("a")
    try await save("agent", "agent")
    let untouchedRev = session.rev
    _ = await session.finishDraftEdit()
    #expect(session.inlineDraft == nil && !session.dirty && session.rev == untouchedRev)
    #expect(session.cardMeta("a")?["content"]?.string == "agent" && session.displayName("a") == "agent")
    for field in ["name", "content"] {
      await session.startDraftEdit("a")
      if field == "name" { session.inlineDraft?.name = "mine" } else { session.inlineDraft?.content = "mine" }
      try await save("newer", "newer")
      _ = await session.finishDraftEdit()
      #expect(session.inlineDraft?.revision.conflictRev == session.rev && !session.dirty)
      try await save("newest", "newest")
      _ = await session.finishDraftEdit(overwrite: true)
      #expect(session.inlineDraft?.revision.conflictRev == session.rev && !session.dirty)
      _ = await session.finishDraftEdit(overwrite: true)
      #expect(session.inlineDraft == nil && !session.dirty)
      #expect(session.cardMeta("a")?[field]?.string == "mine")
      #expect(session.cardMeta("a")?[field == "name" ? "content" : "name"]?.string == "newest")
    }
    await agent.close()
    await session.close()
  }
}
