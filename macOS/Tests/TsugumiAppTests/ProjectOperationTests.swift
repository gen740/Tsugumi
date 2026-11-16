import FlatBuffers
import Foundation
import Testing
import TsugumiLauncher
import TsugumiRPC
import TsugumiTestSupport
@testable import TsugumiApp

/// Session tests advance the reply and notification independently, without timers.
@MainActor struct ProjectOperationTests {
  struct Call {
    let request: Tsugumi_Host_ProjectOperateRequestT
    let reply: CheckedContinuation<Tsugumi_RPC_SaveResultT, Error>
  }

  private func session() throws -> (ProjectSession, URL) {
    let dir = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    try Data("{}".utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
    let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    session.prepareLayout = { _, layout, _ in layout }
    let snapshot = Tsugumi_RPC_ProjectSnapshotT()
    snapshot.rev = 0; snapshot.projectJson = #"{"name":"old","canvas":{"cards":[],"connections":[]}}"#
    snapshot.layoutJson = "{}"
    snapshot.run = Tsugumi_RPC_RunStatusT()
    session.receiveProjectSnapshot(snapshot, projectId: "p")
    return (session, dir)
  }

  private func delta(_ base: UInt64, _ id: String, _ name: String) -> Tsugumi_RPC_DocumentDeltaT {
    let delta = Tsugumi_RPC_DocumentDeltaT()
    delta.baseRev = base; delta.rev = base + 1; delta.opId = id; delta.origin = "ui"
    delta.projectJson = #"{"name":"\#(name)","canvas":{"cards":[],"connections":[]}}"#
    return delta
  }

  private func result(_ delta: Tsugumi_RPC_DocumentDeltaT) -> Tsugumi_RPC_SaveResultT {
    let inverse = Tsugumi_Host_ProjectEditsT()
    let edit = Tsugumi_Host_ProjectFieldEditT()
    edit.filePath = "tsugumi.json"
    let selector = Tsugumi_Host_ProjectSelectorT(); selector.field = "name"
    edit.selector = [selector]
    edit.oldValue = Tsugumi_Host_ProjectValueT(); edit.oldValue.present = true; edit.oldValue.json = #""new""#
    edit.newValue = Tsugumi_Host_ProjectValueT(); edit.newValue.present = true; edit.newValue.json = #""old""#
    let entry = Tsugumi_Host_ProjectEditT(); entry.value = Tsugumi_Host_ProjectEditValueUnion(edit, type: .projectfieldedit)
    inverse.edits = [entry]
    var builder = FlatBufferBuilder(initialSize: 256)
    var object: Tsugumi_Host_ProjectEditsT? = inverse
    let offset = Tsugumi_Host_ProjectEdits.pack(&builder, obj: &object)
    builder.finish(offset: offset)
    let result = Tsugumi_RPC_SaveResultT()
    result.rev = delta.rev; result.delta = delta; result.operationData = Array(builder.data)
    return result
  }

  @Test func replyAndEchoOrdersAndAnotherWriter() async throws {
    for echoFirst in [false, true] {
      let (session, dir) = try session()
      defer { try? FileManager.default.removeItem(at: dir) }
      let calls = AsyncStream<Call>.makeStream()
      var iterator = calls.stream.makeAsyncIterator()
      session.operateProject = { request in
        try await withCheckedThrowingContinuation { calls.continuation.yield(Call(request: request, reply: $0)) }
      }
      let action = Task { try await session.applyMutation { $0.setProjectField("name", .string("new")) } }
      let first = try #require(await iterator.next())
      let saved = delta(0, first.request.opId, "new")
      if echoFirst { session.applyDelta(saved) }
      first.reply.resume(returning: result(saved))
      _ = try await action.value
      session.applyDelta(saved)
      #expect(session.rev == 1 && session.project["name"]?.string == "new")
      #expect(session.undoStack.count == 1)
      session.applyDelta(delta(1, "other", "other"))
      let second = Task { try await session.applyMutation { $0.setProjectField("name", .string("last")) } }
      let next = try #require(await iterator.next())
      #expect(next.request.opId != first.request.opId)
      next.reply.resume(returning: result(delta(2, next.request.opId, "last")))
      _ = try await second.value
      #expect(session.rev == 3 && session.project["name"]?.string == "last")
      calls.continuation.finish()
    }
  }

  @Test func undoWaitsForPendingReplyAndRevisionGap() async throws {
    let (session, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    let calls = AsyncStream<Call>.makeStream()
    var iterator = calls.stream.makeAsyncIterator()
    session.operateProject = { request in
      try await withCheckedThrowingContinuation { calls.continuation.yield(Call(request: request, reply: $0)) }
    }
    let action = Task { try await session.applyMutation { $0.setProjectField("name", .string("new")) } }
    let first = try #require(await iterator.next())
    let undo = Task { await session.undo() }
    // Our reply is ahead of another writer's notification. The action cannot
    // finish until the missing revision is explicitly delivered.
    first.reply.resume(returning: result(delta(1, first.request.opId, "new")))
    #expect(session.rev == 0 && session.undoStack.isEmpty)
    session.applyDelta(delta(0, "other", "other"))
    _ = try await action.value
    let inverse = try #require(await iterator.next())
    #expect(inverse.request.body?.type == .projectedits)
    inverse.reply.resume(returning: result(delta(2, inverse.request.opId, "old")))
    await undo.value
    #expect(session.rev == 3 && session.undoStack.isEmpty && session.redoStack.count == 1)
    calls.continuation.finish()
  }
  @Test func sourceRenameEmitsAGuardedMoveWithoutReading() async throws {
    let (session, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    let snapshot = Tsugumi_RPC_ProjectSnapshotT()
    snapshot.rev = 0; snapshot.projectJson = #"{"canvas":{"cards":[{"id":"a"}],"connections":[]}}"#; snapshot.layoutJson = "{}"
    let definition = Tsugumi_RPC_DefinitionT(); definition.id = "a"; definition.cardJson = #"{"template":"draft"}"#
    snapshot.definitions = [definition]
    let source = Tsugumi_RPC_SourceInfoT(); source.path = "definitions/a/data.bin"; source.hash = "selected-hash"; source.sizeBytes = 3
    snapshot.sources = [source]
    session.receiveProjectSnapshot(snapshot, projectId: "p")
    let calls = AsyncStream<Call>.makeStream()
    var iterator = calls.stream.makeAsyncIterator()
    session.operateProject = { request in
      try await withCheckedThrowingContinuation { calls.continuation.yield(Call(request: request, reply: $0)) }
    }
    let editor = CardEditorModel(session: session, cardKey: "a")
    // This embedded session has no source RPC connection. Rename must not read.
    #expect(await editor.rename("definitions/a/data.bin", to: "renamed.bin"))
    let action = Task { await editor.apply() }
    let call = try #require(await iterator.next())
    let body = try #require(call.request.body?.value as? Tsugumi_Host_ProjectEditsT)
    let move = try #require(body.edits.compactMap { $0?.value?.value as? Tsugumi_Host_ProjectFileMoveT }.first)
    #expect(move.filePath == "definitions/a/data.bin" && move.to == "definitions/a/renamed.bin")
    #expect(move.expectedHash == "selected-hash" && move.destinationMissing)
    #expect(!body.edits.contains { $0?.value?.type == .projectfileput || $0?.value?.type == .projectfileremove })
    let saved = delta(0, call.request.opId, "saved"); saved.projectJson = snapshot.projectJson
    call.reply.resume(returning: result(saved))
    #expect(await action.value)
    calls.continuation.finish()
  }

}
