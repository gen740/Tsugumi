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
    let moves = body.edits.compactMap { $0?.value?.value as? Tsugumi_Host_ProjectFileMoveT }
    let move = try #require(moves.first)
    #expect(move.filePath == "definitions/a/data.bin" && move.to == "definitions/a/renamed.bin")
    #expect(move.expectedHash == "selected-hash" && move.destinationMissing)
    #expect(!body.edits.contains { $0?.value?.type == .projectfileput || $0?.value?.type == .projectfileremove })
    let saved = delta(0, call.request.opId, "saved"); saved.projectJson = snapshot.projectJson
    call.reply.resume(returning: result(saved))
    #expect(await action.value)
    calls.continuation.finish()
  }

  @Test func parameterAndDraftKeepTheirSelectedValues() async throws {
    let (session, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    let snapshot = Tsugumi_RPC_ProjectSnapshotT()
    snapshot.rev = 0; snapshot.projectJson = #"{"canvas":{"cards":[{"id":"a"}],"connections":[]}}"#; snapshot.layoutJson = "{}"
    let definition = Tsugumi_RPC_DefinitionT(); definition.id = "a"
    definition.cardJson = #"{"template":"input","params":[{"name":"value","type":"int","data":1}]}"#
    definition.metaJson = #"{"content":"before"}"#
    snapshot.definitions = [definition]
    session.receiveProjectSnapshot(snapshot, projectId: "p")
    let edit = InlineParamEdit(); edit.begin("1", rev: 0)
    edit.paramBase = .init(parameter: session.cardDefinition("a")?["params"]?.array?.first)
    await session.startDraftEdit("a")
    session.inlineDraft?.content = "mine"
    let other = delta(0, "agent", "old"); other.projectJson = nil; other.origin = "agent"
    let changed = Tsugumi_RPC_DefinitionT(); changed.id = "a"
    changed.cardJson = #"{"template":"input","params":[{"name":"value","type":"int","data":2}]}"#
    changed.metaJson = #"{"content":"agent","description":"unrelated"}"#
    other.definitionsUpsert = [changed]; session.applyDelta(other)
    // The replies keep the documents: the Card under edit stays on the canvas.
    func unchanged(_ id: String) -> Tsugumi_RPC_DocumentDeltaT {
      let d = self.delta(session.rev, id, "saved"); d.projectJson = nil
      return d
    }
    var oldValues: [JSONValue?] = []
    session.operateProject = { request in
      let body = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
      if let field = body.edits[0]?.value?.value as? Tsugumi_Host_ProjectFieldEditT {
        oldValues.append(try field.oldValue.json.map(JSONValue.parse))
      } else {
        let edit = try #require(body.edits[0]?.value?.value as? Tsugumi_Host_ProjectJSONEditT)
        let before = try JSONValue.parse(try #require(edit.oldValue.json))
        oldValues.append(before["params"]?.array?.first?["data"])
      }
      if oldValues.last! != .number("2") { throw RPCFailure(kind: .valueConflict, diagnostic: "") }
      return self.result(unchanged(request.opId))
    }
    #expect(await edit.commit(session, text: "3", { $0.updateParam("a", "value", .number("3"), initial: edit.paramBase) }) == false)
    #expect(edit.revision.conflictRev == 1 && edit.revision.error == nil)
    #expect(await edit.commit(session, text: "3", overwrite: true, { $0.updateParam("a", "value", .number("3")) }))
    #expect(oldValues == [.number("1"), .number("2")])
    var draftBases: [String] = []
    session.operateProject = { request in
      let body = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
      let edit = try #require(body.edits[0]?.value?.value as? Tsugumi_Host_ProjectJSONEditT)
      let before = try JSONValue.parse(try #require(edit.oldValue.json))
      draftBases.append(try #require(before["content"]?.string))
      if before["content"]?.string != "agent" { throw RPCFailure(kind: .valueConflict, diagnostic: "") }
      let after = try JSONValue.parse(try #require(edit.newValue.json))
      #expect(after["description"]?.string == "unrelated")
      return self.result(unchanged(request.opId))
    }
    _ = await session.finishDraftEdit()
    #expect(session.inlineDraft?.revision.conflictRev != nil && session.inlineDraft?.content == "mine")
    _ = await session.finishDraftEdit(overwrite: true)
    #expect(session.inlineDraft == nil && draftBases == ["before", "agent"])
  }

  @Test func rapidHistoryAndOrdinaryEditsKeepQueueOrder() async throws {
    let (session, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    func inverse(_ name: String) throws -> Tsugumi_Host_ProjectEditsT {
      let saved = result(delta(0, name, name))
      var bytes = ByteBuffer(bytes: saved.operationData)
      let root: Tsugumi_Host_ProjectEdits = try getCheckedRoot(byteBuffer: &bytes)
      let edits = root.unpack()
      (edits.edits[0]?.value?.value as? Tsugumi_Host_ProjectFieldEditT)?.newValue.json = JSONValue.string(name).serialized()
      return edits
    }
    var priming = 0
    session.operateProject = { request in
      let saved = self.result(self.delta(session.rev, request.opId, "saved"))
      var builder = FlatBufferBuilder(initialSize: 256)
      var object: Tsugumi_Host_ProjectEditsT? = try inverse(priming == 0 ? "first" : "second")
      let offset = Tsugumi_Host_ProjectEdits.pack(&builder, obj: &object); builder.finish(offset: offset)
      saved.operationData = Array(builder.data); priming += 1
      return saved
    }
    for name in ["first", "second"] { _ = try await session.applyMutation { $0.setProjectField("name", .string(name)) } }
    let calls = AsyncStream<Call>.makeStream(); var iterator = calls.stream.makeAsyncIterator()
    session.operateProject = { request in
      try await withCheckedThrowingContinuation { calls.continuation.yield(Call(request: request, reply: $0)) }
    }
    let undo1 = Task { await session.undo() }
    let first = try #require(await iterator.next())
    let undo2 = Task { await session.undo() }
    let firstBody = try #require(first.request.body?.value as? Tsugumi_Host_ProjectEditsT)
    func restored(_ body: Tsugumi_Host_ProjectEditsT) -> JSONValue? {
      (body.edits[0]?.value?.value as? Tsugumi_Host_ProjectFieldEditT)?.newValue.json.flatMap { try? JSONValue.parse($0) }
    }
    #expect(restored(firstBody) == .string("second"))
    first.reply.resume(returning: result(delta(session.rev, first.request.opId, "first")))
    let second = try #require(await iterator.next())
    let secondBody = try #require(second.request.body?.value as? Tsugumi_Host_ProjectEditsT)
    #expect(restored(secondBody) == .string("first"))
    second.reply.resume(returning: result(delta(session.rev, second.request.opId, "old")))
    await undo1.value; await undo2.value
    #expect(session.undoStack.isEmpty && session.redoStack.count == 2)
    // A normal edit submitted while Redo is pending must retain its own entry.
    let redo = Task { await session.redo() }
    let third = try #require(await iterator.next())
    session.setFlow(.leftToRight)
    third.reply.resume(returning: result(delta(session.rev, third.request.opId, "redo")))
    let normal = try #require(await iterator.next())
    normal.reply.resume(returning: result(delta(session.rev, normal.request.opId, "normal")))
    await redo.value; await session.save()
    #expect(session.undoStack.count == 2 && session.redoStack.isEmpty)
    calls.continuation.finish()
  }

  @Test func internalDocumentsFinishBeforeTheNextGesture() async throws {
    for echoFirst in [false, true] {
      let (session, dir) = try session()
      defer { try? FileManager.default.removeItem(at: dir) }
      let prefix = "externals/sub/"
      let project = #"{"canvas":{"cards":[{"id":"a"}],"connections":[]}}"#
      func card(_ value: UInt64) -> String { #"{"template":"input","params":[{"name":"value","type":"int","data":\#(value)}]}"# }
      let reads = AsyncStream<CheckedContinuation<Void, Never>>.makeStream()
      var iterator = reads.stream.makeAsyncIterator()
      session.readExternalFile = { path, revision in
        if path.hasSuffix("/_card.json") && revision == 1 {
          await withCheckedContinuation { reads.continuation.yield($0) }
        }
        return Data((path.hasSuffix("/tsugumi.json") ? project : card(revision + 1)).utf8)
      }
      let snapshot = Tsugumi_RPC_ProjectSnapshotT(); snapshot.rev = 0
      snapshot.projectJson = #"{"canvas":{"cards":[{"id":"sub"}],"connections":[]}}"#; snapshot.layoutJson = "{}"
      let link = Tsugumi_RPC_DefinitionT(); link.id = "sub"; link.cardJson = "{}"; link.linkJson = #"{"external":"sub"}"#
      snapshot.definitions = [link]
      func sources(_ value: UInt64) -> [Tsugumi_RPC_SourceInfoT?] {
        [prefix + "tsugumi.json", prefix + "definitions/a/_card.json"].map { path in
          let source = Tsugumi_RPC_SourceInfoT(); source.path = path; source.hash = String(value); return source
        }
      }
      snapshot.sources = sources(0)
      session.receiveProjectSnapshot(snapshot, projectId: "p")
      await session.refreshExternals()?.value
      var calls = 0
      session.operateProject = { request in
        calls += 1
        let body = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
        let field = try #require(body.edits[0]?.value?.value as? Tsugumi_Host_ProjectFieldEditT)
        #expect(try field.oldValue.json.map(JSONValue.parse) == .number(String(calls)))
        let d = self.delta(session.rev, request.opId, "old"); d.projectJson = nil; d.sourcesUpsert = sources(UInt64(calls))
        if echoFirst { session.applyDelta(d) }
        return self.result(d)
      }
      session.setParam("sub/a", "value", .number("2"))
      session.setParam("sub/a", "value", .number("3"))
      let release = try #require(await iterator.next())
      #expect(calls == 1)
      release.resume()
      await session.save()
      #expect(calls == 2 && session.saveError == nil)
      #expect(session.cardDefinition("sub/a")?["params"]?.array?.first?["data"] == .number("3"))
      reads.continuation.finish()
    }
  }

  @Test func currentConflictKindsOfferTheExplicitChoice() async throws {
    for kind: FailureKind in [.valueConflict, .sourceChanged] {
      let (session, dir) = try session()
      defer { try? FileManager.default.removeItem(at: dir) }
      session.operateProject = { _ in throw RPCFailure(kind: kind, diagnostic: "") }
      let edit = RevisionEdit(); edit.reload(session.rev)
      #expect(await edit.apply(session, { $0.setProjectField("name", .string("mine")) }) == false)
      #expect(edit.conflictRev == session.rev && edit.error == nil && !edit.applying)
    }
  }

}
