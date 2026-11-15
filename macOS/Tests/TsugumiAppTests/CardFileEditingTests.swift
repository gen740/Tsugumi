import Foundation
import Testing
import TsugumiAgentClient
import TsugumiLauncher
import TsugumiRPC
@testable import TsugumiApp

@MainActor @Suite(.hostService) struct CardFileEditingTests {
  let binary = Data([0, 255, 128, 195, 40, 10])

  @Test func copyReadsCheckContentAcrossUnrelatedSaves() async throws {
    let dir = try SyncTests.project()
    let daemon = try SyncTests.Daemon(project: dir)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
    let client = RPCClient(endpoint: daemon.endpoint.url)
    _ = try await client.connect(token: daemon.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    let source = "definitions/a/data.bin"
    // More than one read chunk, including bytes that are not UTF-8.
    let bytes = Data(repeating: 255, count: (1 << 20) + 1) + binary
    var snap = try await client.inspect()
    _ = try await client.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "test", changes: [
      Change.putSource(path: source, bytes: bytes, baseHash: nil),
    ])
    snap = try await client.inspect()
    let hash = try #require(snap.sources.compactMap { $0 }.first { $0.path == source }?.hash)
    _ = try await client.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "test", changes: [
      Change.setLayout(#"{"cards":{"a":{"x":100,"y":200}}}"#),
    ])
    do {
      _ = try await client.readFile(path: source, atRev: snap.rev)
      Issue.record("the old document revision must be stale")
    } catch let error as RPCFailure { #expect(error.kind == .revisionConflict) }
    #expect(try await client.readFile(path: source, atRev: nil, ifHash: hash) == bytes)
    snap = try await client.inspect()
    _ = try await client.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "test", changes: [
      Change.putSource(path: source, bytes: binary, baseHash: hash),
    ])
    do {
      _ = try await client.readFile(path: source, atRev: nil, ifHash: hash)
      Issue.record("copying a changed file must fail")
    } catch let error as RPCFailure { #expect(error.kind == .hashConflict) }
    await client.close()
  }

  // Use the same real-project/daemon setup as the neighbouring SyncTests.
  @Test func failedReadAbortsConversionAndDuplicationWithoutAnyEdits() async throws {
    let dir = try SyncTests.project()
    let daemon = try SyncTests.Daemon(project: dir)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await SyncTests.connected(s)
    await s.close() // Keep the manifest; every confirmed-file read now fails.
    for p in s.sourcePaths where p.hasPrefix("definitions/a/") { s.sourceEdits[p] = binary }
    let before = s.docState(), edits = s.sourceEdits, removed = s.sourceRemovals, undos = s.undoStack.count
    #expect(await s.makeSubgraph(["a", "b"], in: []) == nil)
    #expect(s.notice?.contains("definitions/b/") == true)
    #expect(s.docState() == before && s.sourceEdits == edits && s.sourceRemovals == removed && s.undoStack.count == undos)
    #expect(await s.duplicate(["a", "b"], in: []).isEmpty)
    #expect(s.notice?.contains("definitions/b/") == true)
    #expect(s.docState() == before && s.sourceEdits == edits && s.sourceRemovals == removed && s.undoStack.count == undos)
  }

  @Test func copiedBytesSurviveSaveUndoAndImmediateRedo() async throws {
    let dir = try SyncTests.project()
    let daemon = try SyncTests.Daemon(project: dir)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await SyncTests.connected(s)
    s.sourceEdits["definitions/a/data.bin"] = binary
    let made = try #require(await s.duplicate(["a"], in: []).first)
    let target = try #require(s.definitionDir(made)) + "data.bin"
    await s.save()
    for _ in 0..<100 where s.dirty { try await Task.sleep(for: .milliseconds(20)) }
    #expect(s.sourceEdits[target] == nil)
    await s.undo()
    #expect(s.instance(made) == nil)
    await s.redo()
    #expect(s.instance(made) != nil)
    #expect(try await s.rpcBytes(target) == binary)
    await s.close()
  }

  @Test func conversionAndCopiesKeepBinaryFilesAndNestedDocuments() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("card-files-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let document = #"{"name":"test","pkgsPath":"https://example.test/pinned#sha256-pin","nixSources":[{"name":"science","type":"flake","ref":"github:org/science/revision?narHash=sha256-pin","overrides":{"tool":{"override":{"flag":true}}}}],"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}}"#
    try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
    for id in ["a", "b"] {
      let definition = dir.appendingPathComponent("definitions/\(id)")
      try FileManager.default.createDirectory(at: definition, withIntermediateDirectories: true)
      try Data(#"{"language":"bash"}"#.utf8).write(to: definition.appendingPathComponent("_card.json"))
    }
    let daemon = try SyncTests.Daemon(project: dir)
    defer { daemon.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    // Exercise the native Host requests; only Card ids stay deterministic.
    s.prepareSubgraph = { try await CoreHostClient.shared.makeSubgraph(project: $0, cards: $1, id: $2) }
    s.prepareCopy = { try await CoreHostClient.shared.copyDocuments(state: $0, paths: $1, from: $2, to: $3, name: $4) }
    s.endpointOverride = daemon.endpoint
    s.start()
    do {
      try await SyncTests.connected(s)
      s.definitionEdits["a"] = DefinitionDraft(card: try JSONValue.parse(#"{"language":"bash"}"#))
      s.definitionEdits["b"] = DefinitionDraft(card: .object([]), link: try JSONValue.parse(#"{"external":"child"}"#))
      s.sourceEdits["definitions/a/data.bin"] = binary
      for prefix in ["externals/child/", "externals/child/externals/grand/"] {
        s.externalEdits[prefix] = ExternalProject(project: try JSONValue.parse(#"{"name":"inner","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}}"#), layout: .object([]), definitions: [:])
        s.sourceEdits[prefix + "asset.bin"] = binary
      }
      let before = s.undoStack.count
      let nested = try #require(await s.makeSubgraph(["a", "b"], in: []), "\(s.notice ?? "")")
      #expect(nested == "sub" && s.undoStack.count == before + 1)
      #expect(s.external("externals/sub/") != nil)
      for p in ["definitions/a/data.bin", "externals/child/asset.bin", "externals/child/externals/grand/asset.bin"] {
        #expect(s.sourceEdits["externals/sub/" + p] == binary)
      }
      #expect(s.external("externals/sub/externals/child/externals/grand/") != nil)
      let copies = await s.duplicate(["sub/a"], in: ["sub"])
      let copy = try #require(copies.first)
      #expect(s.sourceEdits["externals/sub/definitions/\(path(copy).last!)/data.bin"] == binary)
      // Rename keeps the permanent identifier and all inner file bytes.
      try await s.renameCard("sub/a", to: "renamed")
      #expect(s.displayName("sub/a") == "renamed")
      #expect(try await s.rpcBytes("externals/sub/definitions/a/data.bin") == binary)
    } catch {
      await s.close()
      throw error
    }
    await s.close()
  }
}
