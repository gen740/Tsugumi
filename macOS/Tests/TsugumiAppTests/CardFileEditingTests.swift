import Foundation
import Testing
import TsugumiLauncher
import TsugumiRPC
@testable import TsugumiApp

@MainActor struct CardFileEditingTests {
  let binary = Data([0, 255, 128, 195, 40, 10])

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

  @Test func conversionAndCopiesKeepBinaryFilesNixSourcesAndNestedDocuments() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("card-files-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let document = #"{"name":"test","pkgsPath":"https://example.test/pinned#sha256-pin","nixSources":[{"name":"science","type":"flake","ref":"github:org/science/revision?narHash=sha256-pin","overrides":{"tool":{"override":{"flag":true}}}}],"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}}"#
    try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.project = try JSONValue.parse(document)
    s.definitionEdits["a"] = DefinitionDraft(card: try JSONValue.parse(#"{"language":"bash"}"#))
    s.definitionEdits["b"] = DefinitionDraft(card: .object([]), link: try JSONValue.parse(#"{"external":"child"}"#))
    s.sourceEdits["definitions/a/data.bin"] = binary
    for prefix in ["externals/child/", "externals/child/externals/grand/"] {
      s.externalEdits[prefix] = ExternalProject(project: try JSONValue.parse(#"{"name":"inner","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}}"#), layout: .object([]), definitions: [:])
      s.sourceEdits[prefix + "asset.bin"] = binary
    }
    let before = s.undoStack.count
    let nested = try #require(await s.makeSubgraph(["a", "b"], in: []))
    #expect(nested == "sub" && s.undoStack.count == before + 1)
    let child = try #require(s.external("externals/sub/"))
    #expect(child.project["nixSources"] == s.project["nixSources"])
    #expect(child.project["pkgsPath"] == s.project["pkgsPath"])
    for p in ["definitions/a/data.bin", "externals/child/asset.bin", "externals/child/externals/grand/asset.bin"] {
      #expect(s.sourceEdits["externals/sub/" + p] == binary)
    }
    #expect(s.external("externals/sub/externals/child/externals/grand/") != nil)
    let copies = await s.duplicate(["sub/a"], in: ["sub"])
    let copy = try #require(copies.first)
    #expect(s.sourceEdits["externals/sub/definitions/\(path(copy).last!)/data.bin"] == binary)
    // Rename keeps the permanent identifier and all inner file bytes.
    do { try await s.renameCard("sub/a", to: "renamed") } catch { /* No core confirms this presentation fixture. */ }
    #expect(s.displayName("sub/a") == "renamed")
    #expect(s.sourceEdits["externals/sub/definitions/a/data.bin"] == binary)
  }
}
