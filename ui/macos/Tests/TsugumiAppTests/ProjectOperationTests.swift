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

  private func directory() throws -> URL {
    let dir = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir
  }

  private func session() throws -> (ProjectSession, URL) {
    let dir = try directory()
    try Data("{}".utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
    let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    session.prepareLayout = { _, layout, _ in layout }
    let snapshot = Tsugumi_RPC_ProjectSnapshotT()
    snapshot.rev = 0; snapshot.projectJson = #"{"name":"old","canvas":{"cards":[],"connections":[]}}"#
    snapshot.layout = []
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
    return result(delta, inverse: inverse)
  }

  private func result(_ delta: Tsugumi_RPC_DocumentDeltaT, inverse: Tsugumi_Host_ProjectEditsT) -> Tsugumi_RPC_SaveResultT {
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
    snapshot.rev = 0; snapshot.projectJson = #"{"canvas":{"cards":[{"id":"a"}],"connections":[]}}"#; snapshot.layout = []
    let definition = Tsugumi_RPC_DefinitionT(); definition.id = "a"; definition.cardJson = #"{"type":"draft"}"#
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

  @Test func draftInputPresetPreservesExistingSchema() async throws {
    let (session, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    let snapshot = Tsugumi_RPC_ProjectSnapshotT()
    snapshot.projectJson = #"{"canvas":{"cards":[{"id":"a"}],"connections":[]}}"#
    snapshot.layout = []
    let definition = Tsugumi_RPC_DefinitionT()
    definition.id = "a"; definition.cardJson = #"{"type":"draft"}"#
    snapshot.definitions = [definition]
    let schema = Tsugumi_RPC_SourceInfoT()
    schema.path = "definitions/a/_outputs.fbs"; schema.hash = "custom"; schema.sizeBytes = 32
    snapshot.sources = [schema]
    session.receiveProjectSnapshot(snapshot, projectId: "p")
    session.operateProject = { request in
      let body = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
      #expect(!body.edits.contains { $0?.value?.type == .projectfileput || $0?.value?.type == .projectfileremove })
      let saved = self.delta(0, request.opId, "saved")
      saved.projectJson = snapshot.projectJson
      return self.result(saved)
    }
    let editor = CardEditorModel(session: session, cardKey: "a")
    editor.implementAs("input")
    editor.implementAs("input_bool")
    #expect(editor.kind == .input)
    #expect(await editor.apply())
    #expect(session.sources[schema.path]?.hash == "custom")
  }

  @Test func parameterAndDraftKeepTheirSelectedValues() async throws {
    let (session, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    let snapshot = Tsugumi_RPC_ProjectSnapshotT()
    snapshot.rev = 0; snapshot.projectJson = #"{"canvas":{"cards":[{"id":"a"}],"connections":[]}}"#; snapshot.layout = []
    let definition = Tsugumi_RPC_DefinitionT(); definition.id = "a"
    definition.cardJson = #"{"type":"input","params":[{"name":"value","type":"int","data":1}]}"#
    definition.readme = "before"
    snapshot.definitions = [definition]
    session.receiveProjectSnapshot(snapshot, projectId: "p")
    let edit = InlineParamEdit(); edit.begin("1", rev: 0)
    edit.paramBase = .init(parameter: session.cardDefinition("a")?["params"]?.array?.first)
    await session.startDraftEdit("a")
    session.inlineDraft?.content = "mine"
    let other = delta(0, "agent", "old"); other.projectJson = nil; other.origin = "agent"
    let changed = Tsugumi_RPC_DefinitionT(); changed.id = "a"
    changed.cardJson = #"{"type":"input","params":[{"name":"value","type":"int","data":2}]}"#
    changed.readme = "---\ndescription: unrelated\n---\n\nagent"
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
    #expect(await edit.commit(session, text: "3", { $0.updateParam("a", "value", .number("3"), initial: edit.parameterBase(session, "a", "value", overwrite: true)) }))
    #expect(oldValues == [.number("1"), .number("2")])
    var draftBases: [String] = []
    session.operateProject = { request in
      let body = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
      let edit = try #require(body.edits[0]?.value?.value as? Tsugumi_Host_ProjectJSONEditT)
      let before = try JSONValue.parse(try #require(edit.oldValue.json))
      let readme = Documents.parseReadme(try #require(before.string))
      draftBases.append(readme.body)
      if readme.body != "agent" { throw RPCFailure(kind: .valueConflict, diagnostic: "") }
      let after = try JSONValue.parse(try #require(edit.newValue.json))
      #expect(Documents.parseReadme(try #require(after.string)).description == "unrelated")
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

  @Test func internalDocumentsApplyInRevisionOrder() async throws {
    for echoFirst in [false, true] {
      let (session, dir) = try session()
      defer { try? FileManager.default.removeItem(at: dir) }
      let prefix = "externals/sub/"
      let project = #"{"canvas":{"cards":[{"id":"a"}],"connections":[]}}"#
      func card(_ value: UInt64) -> String { #"{"type":"input","params":[{"name":"value","type":"int","data":\#(value)}]}"# }
      let snapshot = Tsugumi_RPC_ProjectSnapshotT(); snapshot.rev = 0
      snapshot.projectJson = #"{"canvas":{"cards":[{"id":"sub"}],"connections":[]}}"#; snapshot.layout = []
      let link = Tsugumi_RPC_DefinitionT(); link.id = "sub"; link.cardJson = #"{"type":"subgraph","external":"sub"}"#
      snapshot.definitions = [link]
      func source(_ file: String, _ text: String) -> Tsugumi_RPC_SourceInfoT {
        let value = Tsugumi_RPC_SourceInfoT(); value.path = prefix + file; value.document = Array(text.utf8)
        return value
      }
      snapshot.sources = [source("tsugumi.json", project), source("definitions/a/_card.json", card(1))]
      session.receiveProjectSnapshot(snapshot, projectId: "p")
      var calls = 0
      session.operateProject = { request in
        calls += 1
        let body = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
        let field = try #require(body.edits[0]?.value?.value as? Tsugumi_Host_ProjectFieldEditT)
        #expect(try field.oldValue.json.map(JSONValue.parse) == .number(String(calls)))
        let d = self.delta(session.rev, request.opId, "old"); d.projectJson = nil; d.sourcesUpsert = [source("definitions/a/_card.json", card(UInt64(calls + 1)))]
        if echoFirst { session.applyDelta(d) }
        return self.result(d)
      }
      session.setParam("sub/a", "value", .number("2"))
      session.setParam("sub/a", "value", .number("3"))
      await session.save()
      #expect(calls == 2 && session.saveError == nil)
      #expect(session.cardDefinition("sub/a")?["params"]?.array?.first?["data"] == .number("3"))
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

extension ProjectOperationTests {
  @Test(arguments: [false, true])
  func entityEditorsOverwriteTheShownConflict(third: Bool) async throws {
    for kind in ["note", "group", "comment"] {
      let (s, dir) = try session()
      defer { try? FileManager.default.removeItem(at: dir) }
      let edit = InlineParamEdit()
      let isLayout = kind == "note" || kind == "group"
      let file = isLayout ? "layout.bin" : "definitions/a/_comments.json"
      let field = kind == "note" ? "notes" : kind == "group" ? "groups" : "comments"
      let selector: [JSONValue] = kind == "name" ? [.string("name")] : [.string(field), .object([("id", .string("item"))])]
      func value(_ text: String) -> JSONValue {
        kind == "name" ? .string(text) : .object([("id", .string("item")), (kind == "group" ? "name" : "text", .string(text))])
      }
      func document(_ text: String) -> JSONValue {
        kind == "name" ? .object([("name", .string(text))]) : .object([(field, .array([value(text)]))])
      }
      func changed(_ text: String, _ id: String) -> Tsugumi_RPC_DocumentDeltaT {
        let d = delta(s.rev, id, "old"); d.projectJson = nil
        if isLayout {
          d.layoutChanged = true; d.layout = fixtureLayout(document(text).serialized())
          if kind == "note" {
            let note = Tsugumi_RPC_SourceInfoT()
            note.path = "notes/item.md"; note.document = Array(text.utf8)
            d.sourcesUpsert = [note]
          }
        } else {
          let def = Tsugumi_RPC_DefinitionT(); def.id = "a"; def.cardJson = #"{"type":"draft"}"#; def.commentsJson = document(text).serialized()
          d.definitionsUpsert = [def]
        }
        return d
      }
      s.applyDelta(changed("before", "initial"))
      edit.beginField("before", session: s, file: file, selector: selector, value: value("before"))
      s.applyDelta(changed("agent", "agent"))
      var actual = "agent", wanted = "mine", guards: [JSONValue] = []
      s.operateProject = { request in
        let body = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
        let field = try #require(body.edits[0]?.value?.value as? Tsugumi_Host_ProjectFieldEditT)
        let before = try JSONValue.parse(try #require(field.oldValue.json))
        guards.append(before)
        if before != value(actual) { throw RPCFailure(kind: .valueConflict, diagnostic: "") }
        let written = try JSONValue.parse(try #require(field.newValue.json))
        #expect(written == value(wanted))
        return self.result(changed(wanted, request.opId))
      }
      func save(_ overwrite: Bool) async -> Bool {
        await edit.commitField(s, text: wanted, overwrite: overwrite) { $0 = value(wanted) }
      }
      #expect(await save(false) == false)
      if third {
        actual = "third"; s.applyDelta(changed(actual, "third"))
        #expect(await save(true) == false)
        #expect(guards.last == value("agent"))
      }
      #expect(await save(true))
      #expect(guards.last == value(actual))
      #expect((isLayout ? s.layout : s.cardMeta("a")) == document("mine"))
      if kind == "name" {
        actual = "mine"; wanted = "again"
        #expect(await save(false))
        #expect(guards.last == value("mine"))
      }
    }
  }

  @Test(arguments: [false, true])
  func cardSettingsOverwriteTheShownConflict(third: Bool) async throws {
    for source in [false, true] {
      let (s, dir) = try session()
      defer { try? FileManager.default.removeItem(at: dir) }
      let snapshot = Tsugumi_RPC_ProjectSnapshotT()
      snapshot.rev = 0; snapshot.projectJson = #"{"canvas":{"cards":[{"id":"a"}],"connections":[]}}"#; snapshot.layout = []
      let definition = Tsugumi_RPC_DefinitionT(); definition.id = "a"; definition.cardJson = #"{"type":"draft","symbol":"before"}"#
      let file = "definitions/a/data.bin"
      let info = Tsugumi_RPC_SourceInfoT(); info.path = file; info.hash = "before"; info.sizeBytes = 1
      snapshot.definitions = [definition]; snapshot.sources = [info]
      s.receiveProjectSnapshot(snapshot, projectId: "p")
      let editor = CardEditorModel(session: s, cardKey: "a")
      if source { #expect(await editor.rename(file, to: "renamed.bin")) }
      else { editor.definition?["symbol"] = .string("mine") }
      func changed(_ text: String, _ id: String) -> Tsugumi_RPC_DocumentDeltaT {
        let d = delta(s.rev, id, "old"); d.projectJson = nil
        let def = Tsugumi_RPC_DefinitionT(); def.id = "a"; def.cardJson = #"{"type":"draft","symbol":"\#(text)"}"#
        let info = Tsugumi_RPC_SourceInfoT(); info.path = file; info.hash = text; info.sizeBytes = 1
        d.definitionsUpsert = [def]; d.sourcesUpsert = [info]
        return d
      }
      s.applyDelta(changed("agent", "agent"))
      var actual = "agent", guards: [String] = []
      s.operateProject = { request in
        let body = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
        let json = try #require(body.edits.compactMap { $0?.value?.value as? Tsugumi_Host_ProjectJSONEditT }.first { $0.filePath.hasSuffix("_card.json") })
        let move = body.edits.compactMap { $0?.value?.value as? Tsugumi_Host_ProjectFileMoveT }.first
        let before = try JSONValue.parse(try #require(json.oldValue.json))
        let guardValue: String
        if source { guardValue = try #require(move?.expectedHash) } else { guardValue = try #require(before["symbol"]?.string) }
        guards.append(guardValue)
        if guardValue != actual { throw RPCFailure(kind: source ? .sourceChanged : .valueConflict, diagnostic: "") }
        if source { #expect(move?.to == "definitions/a/renamed.bin") }
        else {
          let written = try JSONValue.parse(try #require(json.newValue.json))
          #expect(written["symbol"]?.string == "mine")
        }
        return self.result(changed("mine", request.opId))
      }
      #expect(await editor.apply() == false)
      if third {
        actual = "third"; s.applyDelta(changed(actual, "third"))
        #expect(await editor.apply(overwrite: true) == false)
        #expect(guards.last == "agent")
      }
      #expect(await editor.apply(overwrite: true))
      #expect(guards.last == actual)
      #expect(s.cardDefinition("a")?["symbol"]?.string == "mine")
    }
  }

  @Test(arguments: [false, true])
  func rawDefinitionOverwriteUsesTheShownRevision(third: Bool) async throws {
    let (s, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    let edit = RevisionEdit(); edit.reload(s.rev)
    s.applyDelta(delta(0, "agent", "agent"))
    var revisions: [UInt64] = []
    s.operateProject = { request in
      let body = try #require(request.body?.value as? Tsugumi_Host_ProjectReplaceDocumentsT)
      revisions.append(body.expectedRev)
      if body.expectedRev != s.rev { throw RPCFailure(kind: .revisionConflict, diagnostic: "") }
      #expect(body.documents.first??.path == "definitions/a/_card.json")
      let d = self.delta(s.rev, request.opId, "saved")
      let def = Tsugumi_RPC_DefinitionT(); def.id = "a"; def.cardJson = #"{"type":"draft"}"#
      d.definitionsUpsert = [def]
      return self.result(d)
    }
    func save(_ overwrite: Bool) async -> Bool {
      await edit.replaceDocuments(s, overwrite: overwrite, [("definitions/a/_card.json", .object([("type", .string("draft"))]))])
    }
    #expect(await save(false) == false)
    #expect(edit.conflictRev == 1 && edit.error == nil)
    if third {
      s.applyDelta(delta(s.rev, "third", "third"))
      #expect(await save(true) == false)
      #expect(revisions.last == 1)
    }
    #expect(await save(true))
    #expect(s.cardDefinition("a")?["type"]?.string == "draft")
  }
}

extension ProjectOperationTests {
  @Test func inlineOverwriteRefusesAThirdWriter() async throws {
    for draft in [false, true] {
      let (s, dir) = try session()
      defer { try? FileManager.default.removeItem(at: dir) }
      func changed(_ n: Int, _ id: String) -> Tsugumi_RPC_DocumentDeltaT {
        let d = delta(s.rev, id, "old")
        d.projectJson = #"{"canvas":{"cards":[{"id":"a"}],"connections":[]}}"#
        let def = Tsugumi_RPC_DefinitionT(); def.id = "a"
        def.cardJson = #"{"type":"input","params":[{"name":"value","type":"int","data":\#(n)}]}"#
        def.readme = "---\ndescription: unrelated\n---\n\n\(n)"
        d.definitionsUpsert = [def]
        return d
      }
      s.applyDelta(changed(1, "initial"))
      let edit = InlineParamEdit(); edit.begin("1", rev: s.rev)
      edit.paramBase = .init(parameter: s.cardDefinition("a")?["params"]?.array?.first)
      if draft { await s.startDraftEdit("a"); s.inlineDraft?.content = "mine" }
      s.applyDelta(changed(2, "agent"))
      var actual = 2, guards: [String] = []
      s.operateProject = { request in
        let body = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
        let guardValue: String
        if draft {
          let json = try #require(body.edits[0]?.value?.value as? Tsugumi_Host_ProjectJSONEditT)
          let before = try JSONValue.parse(try #require(json.oldValue.json))
          guardValue = Documents.parseReadme(try #require(before.string)).body
          let written = try JSONValue.parse(try #require(json.newValue.json))
          #expect(Documents.parseReadme(try #require(written.string)).description == "unrelated")
        } else {
          let field = try #require(body.edits[0]?.value?.value as? Tsugumi_Host_ProjectFieldEditT)
          guard case .number(let n) = try JSONValue.parse(try #require(field.oldValue.json)) else { throw RPCFailure(kind: .invalidRequest, diagnostic: "") }
          guardValue = n
        }
        guards.append(guardValue)
        if guardValue != String(actual) { throw RPCFailure(kind: .valueConflict, diagnostic: "") }
        let d = changed(3, request.opId)
        if draft { d.definitionsUpsert[0]?.readme = "---\ndescription: unrelated\n---\n\nmine" }
        return self.result(d)
      }
      func save(_ overwrite: Bool) async -> Bool {
        if draft { _ = await s.finishDraftEdit(overwrite: overwrite); return s.inlineDraft == nil }
        return await edit.commit(s, text: "3") {
          $0.updateParam("a", "value", .number("3"), initial: edit.parameterBase(s, "a", "value", overwrite: overwrite))
        }
      }
      #expect(await save(false) == false)
      actual = 4; s.applyDelta(changed(actual, "third"))
      #expect(await save(true) == false)
      #expect(guards.last == "2")
      #expect(await save(true))
      #expect(guards.last == "4")
      #expect(draft ? s.cardMeta("a")?["content"]?.string == "mine" : s.cardDefinition("a")?["params"]?.array?.first?["data"] == .number("3"))
    }
  }
}

extension ProjectOperationTests {
  @Test func aMarqueeMovesCardsNotesAndFramesInOneUndoStep() async throws {
    let dir = try directory()
    defer { try? FileManager.default.removeItem(at: dir) }
    let initial = """
      {"cards":{"a":{"x":0,"y":0},"b":{"x":3000,"y":0},"c":{"x":500,"y":2000}},
       "notes":[{"id":"note","x":100,"y":300},{"id":"other","x":3000,"y":300}],
       "displays":{"b":{"x":400,"y":400,"width":300,"height":200},"c":{"x":400,"y":400,"width":300,"height":200,"hidden":true}}}
      """
    let s = try offlineSession(dir, cards: ["a", "b", "c"].map { ($0, #"{"type":"draft"}"#) }, layout: initial)
    let marquee = CGRect(x: -100, y: -100, width: 1000, height: 1000)
    let items = s.marqueeItems(marquee, in: [])
    #expect(items.cards == ["a"] && items.notes == ["note"])
    let inverse = Tsugumi_Host_ProjectEditsT()
    let old = s.layout
    var moved = old
    moved["cards"]?["a"]?["x"] = .number("30")
    var notes = try #require(moved["notes"]?.array)
    notes[0]["x"] = .number("130")
    moved["notes"] = .array(notes)
    inverse.edits = [s.jsonEdit("layout.bin", old: moved, new: old)]
    s.operateProject = { request in
      let reply = self.delta(s.rev, request.opId, "unused")
      reply.projectJson = nil
      if let move = request.body?.value as? Tsugumi_Host_ProjectMoveT {
        #expect(move.cards == ["a"] && move.notes == ["note"])
        #expect(move.dx == 30 && move.dy == 0)
        reply.layoutChanged = true; reply.layout = fixtureLayout(moved.serialized())
        return self.result(reply, inverse: inverse)
      }
      let undo = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
      let change = try #require(undo.edits.first??.value?.value as? Tsugumi_Host_ProjectJSONEditT)
      #expect(change.filePath == "layout.bin" && change.newValue.json == old.serialized())
      reply.layoutChanged = true; reply.layout = fixtureLayout(initial)
      return self.result(reply, inverse: Tsugumi_Host_ProjectEditsT())
    }
    await s.moveSelection(cards: items.cards, notes: items.notes, by: CGSize(width: 30, height: 0), snap: nil).value
    #expect(s.position("a").x == 30)
    #expect(s.note("note")?["x"]?.double == 130 && s.position("b").x == 3000)
    #expect(s.undoStack.count == 1)
    await s.undo()
    #expect(s.layout == old && s.undoStack.isEmpty)

    s.operateProject = { request in
      let delete = try #require(request.body?.value as? Tsugumi_Host_ProjectSelectionT)
      #expect(delete.action == .delete)
      #expect(delete.cards == ["a"] && delete.notes == ["note"])
      let d = self.delta(s.rev, request.opId, "unused")
      d.projectJson = #"{"canvas":{"cards":[{"id":"b"},{"id":"c"}],"connections":[]}}"#
      d.layoutChanged = true; d.layout = fixtureLayout(#"{"cards":{"b":{"x":3000,"y":0}}}"#)
      d.definitionsRemoved = ["a"]
      return self.result(d, inverse: Tsugumi_Host_ProjectEditsT())
    }
    s.deleteSelection(cards: items.cards, notes: items.notes, in: [])
    await s.save()
    #expect(s.instance("a") == nil && s.note("note") == nil)
    #expect(s.instance("b") != nil)

    let tab = WorkflowTab(scope: [])
    tab.selection = items.cards
    tab.selectedNotes = items.notes
    tab.selection = ["b"]
    #expect(tab.selectedNotes.isEmpty)
    tab.selectedNotes = ["note"]
    tab.enter(["another"])
    #expect(!tab.hasSelection)
  }

  @Test func anAgentDescriptionEditKeepsTheCardPositionAndGroup() throws {
    let dir = try directory()
    defer { try? FileManager.default.removeItem(at: dir) }
    let layout = #"{"cards":{"a":{"x":700,"y":500}},"groups":[{"id":"group","cards":["a","b"]}]}"#
    let s = try offlineSession(dir, cards: [("a", #"{"type":"code"}"#), ("b", #"{"type":"draft"}"#)], layout: layout)
    let renamed = delta(0, "rename", "unused")
    renamed.projectJson = nil; renamed.origin = "agent"
    let definition = Tsugumi_RPC_DefinitionT()
    definition.id = "a"; definition.cardJson = #"{"type":"code"}"#; definition.readme = "---\ndescription: same description\n---\n"
    let other = Tsugumi_RPC_DefinitionT()
    other.id = "b"; other.cardJson = #"{"type":"draft"}"#; other.readme = definition.readme
    renamed.definitionsUpsert = [definition, other]
    let resolved = Tsugumi_RPC_ResolvedCardT()
    resolved.card = cardPath(["a"])
    renamed.resolution = [resolved]
    s.applyDelta(renamed)
    #expect(s.nodeData("a").title == "a" && s.nodeData("b").title == "b")
    #expect(s.position("a") == CGPoint(x: 700, y: 500))
    #expect(Set(s.groups(in: []).first?["cards"]?.array?.compactMap(\.string) ?? []) == ["a", "b"])
  }

  @Test func helpersSavesAreDrawnByTheirOwnBirds() throws {
    let dir = try directory()
    defer { try? FileManager.default.removeItem(at: dir) }
    let s = try offlineSession(dir, cards: ["a", "b", "c"].map { ($0, #"{"type":"code"}"#) })
    s.startHelpers([(name: "first", cards: ["a"]), (name: "second", cards: ["b"])])
    let saved = delta(0, "helper save", "unused")
    saved.origin = "agent"; saved.projectJson = nil
    saved.definitionsUpsert = ["b", "c"].map { id in
      let definition = Tsugumi_RPC_DefinitionT()
      definition.id = id; definition.cardJson = #"{"type":"code","maxRuns":3}"#
      return definition
    }
    s.applyDelta(saved)
    let helper = try #require(s.helperPresences.first { $0.label == "second" })
    let helperSave = AgentPresence.Beat.save([.init(card: "b", reveal: false)], [])
    let mainSave = AgentPresence.Beat.save([.init(card: "c", reveal: false)], [])
    #expect(helper.beats.contains { $0.beat == helperSave })
    #expect(s.presence.beats.contains { $0.beat == mainSave })
    #expect(!s.presence.beats.contains { $0.beat == helperSave })
  }

  @Test func commentEditsStayOnTheirCardAndReopeningClearsTheResolution() async throws {
    let dir = try directory()
    defer { try? FileManager.default.removeItem(at: dir) }
    let s = try offlineSession(dir, cards: [("plan", #"{"type":"draft"}"#)])
    s.referenceIdProvider = { _, _, _ in "comment" }
    s.operateProject = { request in
      let edits = try #require(request.body?.value as? Tsugumi_Host_ProjectEditsT)
      let edit = try #require(edits.edits.first??.value?.value as? Tsugumi_Host_ProjectJSONEditT)
      #expect(edit.filePath == "definitions/plan/_comments.json")
      let definition = Tsugumi_RPC_DefinitionT()
      definition.id = "plan"; definition.cardJson = #"{"type":"draft"}"#; definition.commentsJson = edit.newValue.json
      let d = self.delta(s.rev, request.opId, "unused")
      d.projectJson = nil; d.definitionsUpsert = [definition]
      return self.result(d, inverse: Tsugumi_Host_ProjectEditsT())
    }
    try await s.addComment("plan", "  \n ")
    #expect(s.rev == 0)
    try await s.addComment("plan", "  Check units  ")
    #expect(s.cardsWithOpenComments(in: []) == ["plan"])
    let comment = try #require(s.comments("plan").first)
    #expect(comment.text == "Check units" && comment.author == "user")

    let resolved = delta(s.rev, "agent", "unused")
    resolved.projectJson = nil; resolved.origin = "agent"
    let definition = Tsugumi_RPC_DefinitionT()
    definition.id = "plan"; definition.cardJson = #"{"type":"draft"}"#
    definition.commentsJson = #"{"comments":[{"id":"comment","author":"user","text":"Check units","resolved":true,"state_by":"agent","resolution":"Checked","cards":["a"]}]}"#
    resolved.definitionsUpsert = [definition]
    s.applyDelta(resolved)
    #expect(s.openComments("plan").isEmpty)
    s.edit { $0.setCommentText("plan", id: comment.id, text: "Check the axes") }
    s.setCommentResolved("plan", id: comment.id, false)
    await s.save()
    let reopened = try #require(s.openComments("plan").first)
    #expect(reopened.text == "Check the axes" && reopened.editedBy == "user" && reopened.stateBy == "user")
    #expect(reopened.resolution == nil && reopened.cards.isEmpty)
    s.removeComment("plan", id: comment.id)
    await s.save()
    #expect(s.comments("plan").isEmpty)
  }

  @Test func writingANewDraftWaitsForPlacementAndRevealsIt() async throws {
    let (s, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    let placements = AsyncStream<CheckedContinuation<String, Never>>.makeStream()
    defer { placements.continuation.finish() }
    s.prepareLayout = { project, _, _ in
      let cards = try JSONValue.parse(project)["canvas"]?["cards"]?.array
      guard cards?.first?["id"]?.string == "draft" else { return "{}" }
      return await withCheckedContinuation { placements.continuation.yield($0) }
    }
    s.cardIdProvider = { name, _ in name }
    s.operateProject = { request in
      let d = self.delta(s.rev, request.opId, "draft")
      d.projectJson = #"{"canvas":{"cards":[{"id":"draft"}],"connections":[]}}"#
      return self.result(d, inverse: Tsugumi_Host_ProjectEditsT())
    }
    let tab = WorkflowTab(scope: [])
    let canvas = GraphCanvas(workspace: Workspace(), session: s, tab: tab)
    let viewport = CGSize(width: 800, height: 600)
    let draft = try await s.addCard(template: .draft, in: [], at: .zero, name: "draft", automaticPlacement: true)
    var waiting = placements.stream.makeAsyncIterator()
    let placement = try #require(await waiting.next())
    let writing = canvas.writeDraft(draft, in: viewport)
    #expect(s.inlineDraft == nil)
    placement.resume(returning: #"{"cards":{"draft":{"x":4000,"y":4000}}}"#)
    await writing.value

    let origin = s.placedPosition(draft), geometry = s.geometry(draft).size
    let a = canvas.screen(origin)
    let b = canvas.screen(CGPoint(x: origin.x + geometry.width, y: origin.y + geometry.height))
    #expect(a.x >= 0 && a.y >= 0 && b.x <= viewport.width && b.y <= viewport.height)
    #expect(s.inlineDraft?.card == draft)
  }

  @Test func automaticAndExplicitCardPlacementReachTheHost() async throws {
    let (s, dir) = try session()
    defer { try? FileManager.default.removeItem(at: dir) }
    s.cardIdProvider = { name, _ in name }
    s.prepareLayout = { _, _, _ in #"{"cards":{"auto":{"x":50,"y":4000}}}"# }
    let here = CGPoint(x: 4000, y: 4000)
    var placements: [Bool] = []
    s.operateProject = { request in
      let add = try #require(request.body?.value as? Tsugumi_Host_ProjectCreateCardT)
      #expect(add.x == here.x && add.y == here.y && add.kind == .draft)
      placements.append(add.automaticPlacement)
      let d = self.delta(s.rev, request.opId, "unused")
      let cards = add.automaticPlacement ? #"{"id":"auto"}"# : #"{"id":"auto"},{"id":"drop"}"#
      d.projectJson = #"{"canvas":{"cards":[\#(cards)],"connections":[]}}"#
      if !add.automaticPlacement {
        d.layoutChanged = true; d.layout = fixtureLayout(#"{"cards":{"drop":{"x":4000,"y":4000}}}"#)
      }
      return self.result(d, inverse: Tsugumi_Host_ProjectEditsT())
    }
    let auto = try await s.addCard(template: .draft, in: [], at: here, name: "auto", automaticPlacement: true)
    await s.refreshDerivedLayout().value
    #expect(s.layout["cards"]?["auto"] == nil)
    #expect(s.position(auto) == CGPoint(x: 50, y: 4000))
    let dropped = try await s.addCard(template: .draft, in: [], at: here, name: "drop")
    #expect(s.position(dropped) == here)
    #expect(placements == [true, false])
  }
}
