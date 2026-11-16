import Foundation
import Testing
import TsugumiAgentClient
import TsugumiRPC
@testable import TsugumiApp
@testable import TsugumiLauncher

extension SyncTests {
  @Test func placementBetweenTheUIReadAndEverySaveEntryKeepsBothEdits() async throws {
    for entry in ["description", "project", "project noticed", "layout", "import", "description conflict", "layout conflict"] {
      let dir = try Self.project()
      defer { try? FileManager.default.removeItem(at: dir) }
      try Data(#"{"cards":{"a":{"x":0,"y":0}}}"#.utf8).write(to: dir.appendingPathComponent("layout.json"))
      let daemon = try Daemon(project: dir)
      defer { daemon.stop() }
      let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
      s.endpointOverride = daemon.endpoint
      // Delay registration (and its observer) until the exact save boundary.
      // The UI really reads a project with missing places, without a timer.
      let register = s.registerProject
      s.registerProject = { _, _ in }
      s.start()
      try await Self.connected(s)
      let other = RPCClient(endpoint: daemon.endpoint.url)
      _ = try await other.connect(token: daemon.endpoint.token, clientId: UUID(), expectedProjectId: nil)
      let openingRev = s.rev
      let placementJSON = #"{"cards":{"a":{"x":0,"y":0},"b":{"x":400,"y":0},"c":{"x":800,"y":0},"start":{"x":1200,"y":0}}}"#
      if entry == "project noticed" {
        let saved = try await other.save(baseRev: openingRev, opId: UUID(), origin: "host-layout", changes: [Change.setLayout(placementJSON)])
        try await Self.received(saved.rev ?? 0, by: s)
      }
      var intercepted = false
      s.saveDocuments = { project, base, request in
        if intercepted { return try await CoreHostClient.shared.saveProject(project: project, base: base, save: request) }
        intercepted = true
        let before = try await other.inspect()
        #expect(request.baseRev == (entry == "project noticed" ? openingRev : before.rev))
        var placed = try JSONValue.parse(placementJSON)
        if entry == "layout conflict" { placed["cards"]?["a"]?["x"] = .number("600") }
        var changes = [Change.setLayout(placed.serialized())]
        if entry == "description conflict" {
          changes.append(Change.putDefinition(id: "a", cardJSON: Self.sinkCard, metaJSON: #"{"description":"theirs"}"#))
        }
        if entry != "project noticed" {
          _ = try await other.save(baseRev: before.rev ?? 0, opId: UUID(), origin: "host-layout", changes: changes)
        }
        // This is the check the old UI hit: a strict core save from its
        // captured revision is refused even though only layout changed.
        do {
          _ = try await other.save(baseRev: request.baseRev ?? 0, opId: UUID(), origin: "ui", changes: request.changes.compactMap { $0 })
          Issue.record("The stale direct core save must be refused")
        } catch let failure as RPCFailure { #expect(failure.kind == .revisionConflict) }
        try await register(daemon.endpoint, dir)
        // Exercise the real host merge and core CAS, whether its notice has
        // reached the UI or is still waiting in the subscription stream.
        let saved = try await CoreHostClient.shared.saveProject(project: project, base: base, save: request)
        // Retrying rereads and merges the original edit. An applied edit
        // is a no-op at the same revision, without a host resend record.
        let replay = try await CoreHostClient.shared.saveProject(project: project, base: base, save: request)
        let first = try #require(saved.reply.value?.value as? Tsugumi_RPC_SaveResultT)
        let repeated = try #require(replay.reply.value?.value as? Tsugumi_RPC_SaveResultT)
        #expect(first.rev == repeated.rev && repeated.delta == nil)
        return replay
      }
      let editor = CardEditorModel(session: s, cardKey: "a")
      if entry.hasPrefix("description") {
        editor.meta = .object([("description", .string("mine"))])
        #expect(await editor.apply() == (entry == "description"))
        if entry == "description conflict" {
          #expect(editor.meta?["description"]?.string == "mine")
          #expect(!s.dirty, "A rejected editor copy must not enter autosave")
          #expect(editor.revisionEdit.error?.contains("_meta.json") == true)
        }
      } else if entry.hasPrefix("project") {
        let edit = RevisionEdit(); edit.reload(entry == "project noticed" ? openingRev : s.rev)
        #expect(await edit.apply(s) { $0.project["name"] = .string("mine") })
      } else if entry.hasPrefix("layout") {
        s.edit { $0.layout["cards"]?["a"]?["x"] = .number("200") }
        await s.save()
        #expect((s.saveError != nil) == (entry == "layout conflict"))
        if entry == "layout conflict" { #expect(s.saveError?.contains("layout") == true) }
        #expect(s.position("a").x == 200)
      } else {
        let imported = try Self.project()
        defer { try? FileManager.default.removeItem(at: imported) }
        _ = try await s.importProject(from: imported, in: [], at: CGPoint(x: 200, y: 400))
        await s.save()
        #expect(s.saveError == nil, "\(entry): \(s.saveError ?? "")")
      }
      #expect(intercepted)
      let saved = try await other.inspect()
      let layout = try JSONValue.parse(saved.layoutJson)
      #expect(layout["cards"]?["b"]?["x"]?.double == 400)
      #expect(layout["cards"]?["a"]?["x"]?.double == (entry == "layout" ? 200 : entry == "layout conflict" ? 600 : 0))
      if entry.hasPrefix("description") {
        let a = try #require(saved.definitions.compactMap { $0 }.first { $0.id == "a" })
        let meta = try JSONValue.parse(a.metaJson ?? "{}")
        #expect(meta["description"]?.string == (entry == "description" ? "mine" : "theirs"))
      }
      if entry == "description" { #expect(s.cardDescription("a") == "mine") }
      if entry.hasPrefix("project") { #expect(s.project["name"]?.string == "mine") }
      await other.close()
      await s.close()
    }
  }
}

extension SyncTests {
  @Test func aRemovedGroupIsAConflictForItsOpenNameEditor() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    var layout = try JSONValue.parse(String(contentsOf: dir.appendingPathComponent("layout.json"), encoding: .utf8))
    layout["groups"] = .array([.object([("id", .string("row")), ("scope", .string("")), ("name", .string("base")), ("color", .string("blue")), ("cards", .array([.string("a")]))])])
    try Data(layout.serialized().utf8).write(to: dir.appendingPathComponent("layout.json"))
    let daemon = try Daemon(project: dir)
    defer { daemon.stop() }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = daemon.endpoint
    let register = s.registerProject
    s.registerProject = { _, _ in }
    s.start()
    try await Self.connected(s)
    let edit = InlineParamEdit(); edit.begin("base", rev: s.rev)
    let other = RPCClient(endpoint: daemon.endpoint.url)
    _ = try await other.connect(token: daemon.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    layout["groups"] = .array([])
    let removed = try await other.save(baseRev: s.rev, opId: UUID(), origin: "agent", changes: [Change.setLayout(layout.serialized())])
    try await Self.received(removed.rev ?? 0, by: s)
    try await register(daemon.endpoint, dir)
    #expect(!(await edit.commit(s, text: "mine") { x in
      x.updateGroups(in: []) { $0[0]["name"] = .string("mine") }
    }))
    #expect(edit.revision.error?.contains("groups") == true)
    #expect(edit.revision.conflictRev == s.rev)
    #expect(s.groups(in: []).isEmpty && !s.dirty)
    await other.close()
    await s.close()
  }
}

extension SyncTests {
  @Test func undoMergedRenameDoesNotResurrectAnotherWritersDeletedGroup() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let daemon = try Daemon(project: dir)
    defer { daemon.stop() }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await Self.connected(s)
    let other = RPCClient(endpoint: daemon.endpoint.url)
    _ = try await other.connect(token: daemon.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    // r0: the person opens the name editor before the other writer adds a group.
    let edit = RevisionEdit(); edit.reload(s.rev)
    let originalName = s.project["name"]
    var layout = s.layout
    layout["groups"] = .array([.object([("id", .string("row")), ("scope", .string("")), ("name", .string("other")), ("color", .string("blue")), ("cards", .array([.string("a")]))])])
    let added = try await other.save(baseRev: s.rev, opId: UUID(), origin: "other", changes: [Change.setLayout(layout.serialized())])
    try await Self.received(added.rev ?? 0, by: s)
    #expect(await edit.apply(s) { $0.project["name"] = .string("mine") })
    #expect(s.groups(in: []).count == 1)
    // After the merged rename, the same other writer deletes their group.
    layout = s.layout; layout["groups"] = .array([])
    let removed = try await other.save(baseRev: s.rev, opId: UUID(), origin: "other", changes: [Change.setLayout(layout.serialized())])
    try await Self.received(removed.rev ?? 0, by: s)
    await s.undo()
    #expect(s.project["name"] == originalName)
    #expect(s.groups(in: []).isEmpty)
    await s.save()
    #expect(s.saveError == nil)
    let saved = try await other.inspect()
    #expect(try JSONValue.parse(saved.layoutJson)["groups"]?.array?.isEmpty == true)
    await s.redo()
    #expect(s.project["name"]?.string == "mine")
    #expect(s.groups(in: []).isEmpty)
    await s.save()
    await other.close()
    await s.close()
  }
}
