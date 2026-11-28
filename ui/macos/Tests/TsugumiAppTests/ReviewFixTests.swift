import AppKit
import Foundation
import SwiftUI
import Testing
@testable import TsugumiApp
@testable import TsugumiAgentClient
import TsugumiLauncher
import TsugumiRPC

@MainActor struct ReviewFixTests {
  @Test func restoredRowsDrawTheHostProjectionWithoutReorderingOrParsing() throws {
    let host = Tsugumi_Agent_ConversationT()
    host.id = "conversation"
    let step = Tsugumi_Agent_ChatRowT(); step.id = "step"; step.kind = "step"
    step.summaryJa = "2 件の操作"; step.summaryEn = "2 operations"; step.failed = 1
    let failure = Tsugumi_Agent_ChatRowT(); failure.id = "a"; failure.kind = "tool"
    failure.name = "project_save"; failure.arguments = "A"; failure.result = "A rejected"; failure.state = "failed"
    let success = Tsugumi_Agent_ChatRowT(); success.id = "b"; success.kind = "tool"
    success.name = "project_save"; success.arguments = "B"; success.result = "B saved"; success.state = "completed"
    step.children = [failure, success]
    let answer = Tsugumi_Agent_ChatRowT(); answer.id = "answer"; answer.kind = "text"; answer.role = "assistant"; answer.text = "Finished"
    host.rows = [step, answer]
    let conversation = try ServiceWire.conversation(host)
    let items = ChatController.restoredItems(conversation)
    #expect(items.map(\.id) == ["step", "answer"])
    #expect(items[0].presentation?.children.map(\.id) == ["a", "b"])
    #expect(items[0].presentation?.failed == 1)
    #expect(items[0].presentation?.children[0].result == "A rejected")
    #expect(items.last?.text == "Finished")
  }

  @Test func approvalsWaitInOrderAndReleaseEveryCaller() {
    var queue = ChatController.PermissionQueue()
    var answered: [String: Set<String>] = [:]
    queue.append(ChatController.PermissionPrompt(raw: ["first"]) { answered["first"] = $0 })
    queue.append(ChatController.PermissionPrompt(raw: ["second"]) { answered["second"] = $0 })
    #expect(queue.first?.raw == ["first"] && answered.isEmpty)
    let first = queue.first!.id
    // A kind the client does not know is never selected by default: the person picks it.
    queue.answer(first, allow: true, selected: ["first"])
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

  @Test func stopTracksOnlyTheCurrentActiveRun() async throws {
    let directory = try SyncTests.project()
    defer { try? FileManager.default.removeItem(at: directory) }
    let session = ProjectSession(identity: try ProjectIdentity(url: directory), environments: EnvironmentManager(image: nil))
    func status(_ id: String, _ state: Tsugumi_Data_RunState) -> Tsugumi_RPC_RunStatusT {
      let status = Tsugumi_RPC_RunStatusT()
      status.runId = id
      status.state = state
      return status
    }
    func accept(_ id: String) {
      let accepted = Tsugumi_RPC_RunAcceptedEventT()
      accepted.status = status(id, .preparing)
      let event = Tsugumi_RPC_RunEventT()
      event.runId = id
      event.value = Tsugumi_RPC_RunEventValueUnion(accepted, type: .runacceptedevent)
      session.applyRunEvent(event)
    }
    func finish(_ id: String) {
      let summary = Tsugumi_RPC_RunSummaryT()
      summary.status = status(id, .succeeded)
      let finished = Tsugumi_RPC_RunFinishedT()
      finished.summary = summary
      let event = Tsugumi_RPC_RunEventT()
      event.runId = id
      event.value = Tsugumi_RPC_RunEventValueUnion(finished, type: .runfinished)
      session.applyRunEvent(event)
    }

    accept("failed-stop")
    func toolbarHeight() -> CGFloat {
      NSHostingView(rootView: CanvasRunControl(session: session)
        .frame(width: 320)).fittingSize.height
    }
    let before = toolbarHeight()
    struct Rejected: LocalizedError {
      let message = "The execution service refused this Stop request."
      var errorDescription: String? { message }
    }
    await session.cancelRun { id in
      #expect(id == "failed-stop")
      #expect(session.stopRequested && !session.canStopRun)
      throw Rejected()
    }
    #expect(session.stopError?.contains(Rejected().message) == true && session.canStopRun)
    // The canvas displays the error below the controls without resizing them.
    #expect(toolbarHeight() == before)
    await session.cancelRun { id in status(id, .cancelling) }
    #expect(session.runStatus?.state == .cancelling && !session.stopRequested && !session.canStopRun)
    #expect(session.stopError == nil)

    accept("finished")
    await session.cancelRun { id in
      finish(id)
      return status(id, .cancelling)
    }
    #expect(session.runStatus?.state == .succeeded && !session.stopRequested)

    accept("replaced")
    await session.cancelRun { id in
      accept("new")
      return status(id, .cancelling)
    }
    #expect(session.currentRunId == "new" && session.canStopRun)
  }

}

// Use the same daemon, positional RPC API and save helpers as the passing
// synchronization tests. This test requires a Unix socket outside the sandbox.
extension SyncTests {
  @Test func unchangedDraftDoesNotOverwriteAgentAndEditedDraftAsks() async throws {
    let directory = try Self.project()
    defer { try? FileManager.default.removeItem(at: directory) }
    let daemon = try Daemon(project: directory)
    defer { daemon.stop() }
    let session = ProjectSession(identity: try ProjectIdentity(url: directory), environments: EnvironmentManager(image: nil))
    session.endpointOverride = daemon.endpoint
    session.start()
    try await Self.connected(session)
    let agent = RPCClient(endpoint: daemon.endpoint.url)
    _ = try await agent.connect(token: daemon.endpoint.token, expectedProjectId: nil)
    func save(_ description: String, _ content: String) async throws {
      let snapshot = try await agent.inspect()
      let readme = Documents.printReadme(title: "a", description: description, body: content)
      _ = try await agent.save(baseRev: snapshot.rev ?? 0, opId: UUID(), origin: "agent", changes: [
        Change.putDefinition(id: "a", cardJSON: fixtureCardJSON(.draft), readme: readme),
      ])
      try await waitForModel { !(session.rev <= (snapshot.rev ?? 0)) }
      #expect(session.rev > (snapshot.rev ?? 0))
    }
    try await save("base", "base")
    await session.startDraftEdit("a")
    try await save("agent", "agent")
    let untouchedRev = session.rev
    _ = await session.finishDraftEdit()
    #expect(session.inlineDraft == nil && !session.dirty && session.rev == untouchedRev)
    #expect(session.cardMeta("a")?["content"]?.string == "agent" && session.displayName("a") == "a")

    await session.startDraftEdit("a")
    session.inlineDraft?.content = "mine"
    try await save("newer", "newer")
    _ = await session.finishDraftEdit()
    #expect(session.inlineDraft?.revision.conflictRev == session.rev && !session.dirty)
    // A third writer's change is refused again before the next overwrite.
    try await save("newest", "newest")
    _ = await session.finishDraftEdit(overwrite: true)
    #expect(session.inlineDraft?.revision.conflictRev == session.rev && !session.dirty)
    _ = await session.finishDraftEdit(overwrite: true)
    #expect(session.inlineDraft == nil && !session.dirty)
    #expect(session.cardMeta("a")?["content"]?.string == "mine")
    #expect(session.cardMeta("a")?["description"]?.string == "newest")
    await agent.close()
    await session.close()
  }
}
