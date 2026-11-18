import FlatBuffers
import Darwin
import Foundation
import Observation
import Testing
import TsugumiAgentClient
import TsugumiTestSupport
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

/// The UI saves every edit at once and takes in what the Agent saves at the
/// same time: both changes stay, and no save conflict is shown.
@MainActor
@Suite(.serialized, .hostService) struct SyncTests {
  nonisolated static let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()

  final class Daemon {
    let process = Process()
    let dir: URL
    var ready: [String: Any] = [:]

    init(project: URL, mockedBuilds: Bool = false) throws {
      dir = URL(fileURLWithPath: ProcessInfo.processInfo.environment["TSUGUMI_SOCKET_DIR"] ?? "/tmp").appendingPathComponent("tsugumi-\(UUID().uuidString.prefix(8))")
      try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
      let env = ProcessInfo.processInfo.environment
      let build = env["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) }
        ?? SyncTests.root.appendingPathComponent("build")
      let binary = mockedBuilds ? ("TSUGUMI_MOCK_DAEMON", "tests/core/tsugumid-mock") : ("TSUGUMID", "tsugumid/tsugumid")
      process.executableURL = env[binary.0].map { URL(fileURLWithPath: $0) } ?? build.appendingPathComponent(binary.1)
      if mockedBuilds { process.environment = env.merging(["TSUGUMI_MOCK_CONTROL": dir.path]) { _, value in value } }
      process.arguments = ["--scratch-dir", dir.appendingPathComponent("scratch").path,
                           "--run-scratch-dir", canonicalDirectory(dir).appendingPathComponent("run-scratch").path,
                           "--project", project.path, "--unix", dir.appendingPathComponent("s").path,
                           "--control-dir", dir.appendingPathComponent("control").path,
                           "--worker", env["TSUGUMI_WORKER"] ?? build.appendingPathComponent("workerd/tsugumi-worker").path,
                           "--sdk-dir", env["TSUGUMI_SDK_DIR"] ?? SyncTests.root.appendingPathComponent("sdk").path,
                           "--allow-missing-origin", "--listen", "127.0.0.1:0"]
      process.standardOutput = FileHandle.nullDevice
      process.standardError = FileHandle.nullDevice
      do {
        try process.run()
        let file = dir.appendingPathComponent("control/server.ready")
        guard let value = try fixtureReadiness(file, pid: process.processIdentifier, read: {
          guard let data = try? Data(contentsOf: file) else { return nil as [String: Any]? }
          return try JSONSerialization.jsonObject(with: data) as? [String: Any]
        }) else { throw CocoaError(.fileReadNoSuchFile) }
        ready = value
      } catch { stop(); throw error }
    }

    var endpoint: Endpoint {
      Endpoint(url: URL(string: ready["endpoint"] as! String)!, token: ready["token"] as! String,
               projectId: ready["project_id"] as? String ?? "")
    }

    func stop() {
      if process.isRunning {
        try? finishFixtureProcess(process.processIdentifier, signal: SIGTERM)
        process.waitUntilExit()
      }
      try? FileManager.default.removeItem(at: dir)
    }
  }

  static let sinkCard = #"{"language":"bash","inputs":[{"name":"in"}]}"#
  static let sinkSources = [
    "_outputs.fbs": "table Outputs {}\nroot_type Outputs;\n",
    "default.nix": #"""
      { pkgs, inputs, params }:
      {
        run = ''
          test -r "$JSON_IN" || exit 1
          printf '%s' '{"text":"Received input"}' > "$TSUGUMI_REPORT"
        '';
      }
      """#,
  ]

  static func implementSink(_ id: String, showMarkdown: Bool = false) -> [Tsugumi_RPC_DocumentChangeT] {
    [Change.putDefinition(id: id, cardJSON: sinkCard, metaJSON: nil)] + sinkSources.sorted(by: { $0.key < $1.key }).map {
      let (name, contents) = $0
      let source = showMarkdown && name == "default.nix"
        ? contents.replacingOccurrences(of: #""text":"Received input""#, with: #""markdown":"**Received input**""#)
        : contents
      return FixtureChange.putSource(path: "definitions/\(id)/\(name)", bytes: Data(source.utf8), baseHash: nil)
    }
  }

  /// A project with a trigger and a few Bash sinks (no connections yet).
  static func project() throws -> URL {
    let p = testTemporaryDirectory.appendingPathComponent("sync-\(UUID().uuidString.prefix(8))")
    do {
      let fm = FileManager.default
      let cards = ["a", "b", "c", "start"]
      for id in cards {
        let d = p.appendingPathComponent("definitions/\(id)")
        try fm.createDirectory(at: d, withIntermediateDirectories: true)
        let definition = id == "start" ? #"{"template":"trigger"}"# : sinkCard
        try Data(definition.utf8).write(to: d.appendingPathComponent("_card.json"))
        if id != "start" {
          for (name, contents) in sinkSources {
            try Data(contents.utf8).write(to: d.appendingPathComponent(name))
          }
        }
      }
      let list = cards.map { "{\"id\": \"\($0)\"}" }.joined(separator: ", ")
      try Data("{\"name\": \"sync\", \"pkgsPath\": \"tsugumi:default\", \"canvas\": {\"cards\": [\(list)], \"connections\": []}}".utf8)
        .write(to: p.appendingPathComponent("tsugumi.json"))
      try Data("{}".utf8).write(to: p.appendingPathComponent("layout.json"))
      return p
    } catch {
      try? FileManager.default.removeItem(at: p)
      throw error
    }
  }

  /// The Agent's read-modify-save. A conflict is handled by the scenario,
  /// after joining the competing UI save, rather than racing a retry count.
  @discardableResult
  static func agentConnect(_ c: RPCClient, to card: String,
                           snapshot: Tsugumi_RPC_ProjectSnapshotT? = nil) async throws -> UInt64 {
    let snap: Tsugumi_RPC_ProjectSnapshotT
    if let snapshot { snap = snapshot } else { snap = try await c.inspect() }
    var proj = try JSONValue.parse(snap.projectJson)
    var conns = proj["canvas"]?["connections"]?.array ?? []
    conns.append(.object([("from", .object([("card", .string("start")), ("port", .string("out"))])),
                          ("to", .object([("card", .string(card)), ("port", .string("in"))]))]))
    proj["canvas"]?["connections"] = .array(conns)
    let saved = try await c.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent",
                                 changes: [FixtureChange.setProject(proj.serialized())])
    return saved.rev ?? 0
  }

  /// Observe the revision applied by the session's notification handler.
  static func received(_ revision: UInt64, by session: ProjectSession) async throws {
    try await waitForModel("document revision \(revision)") { session.rev >= revision }
  }

  static func connected(_ s: ProjectSession) async throws {
    try await waitForModel { s.connected }
    #expect(s.connected, "\(s.connectionMessage ?? "")")
    if s.connected, let endpoint = s.endpointOverride {
      // Direct test daemons bypass EnvironmentManager's host registration.
      _ = try await CoreHostClient.shared.call("RegisterProjectRequest", [
        "project_id": endpoint.projectId, "endpoint": endpoint.url.absoluteString,
        "token": endpoint.token, "path": s.identity.url.path])
    }
  }

  static func waitForSuccessfulRun(_ s: ProjectSession) async throws {
    try await waitForModel { !(s.runActive) }
    try #require(s.runStatus?.state == .succeeded, "\(s.runStatus?.failure?.diagnostic ?? "")")
  }

  @Test func defaultDisplayRectTransformsTheHostGeometry() throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    let d = Tsugumi_Host_DisplayGeometryT()
    d.anchorX = 0.5; d.anchorY = 0.25
    d.x = 71; d.y = 93; d.width = 411; d.height = 219
    let geometries = Tsugumi_Host_DisplayGeometriesT()
    geometries.topToBottom = d; geometries.leftToRight = d
    s.displayGeometries = geometries
    for flow in ["topToBottom", "leftToRight"] {
      s.layout = try JSONValue.parse("{\"flow\":\"\(flow)\"}")
      let k = key(["a"]), size = s.geometry(k).size
      let rect = s.displayRect(k, origin: CGPoint(x: 10, y: 20))
      #expect(rect == CGRect(x: 10 + size.width * 0.5 + 71,
                             y: 20 + size.height * 0.25 + 93,
                             width: 411, height: 219))
    }
  }

  /// A transport failure is surfaced as a connection failure.
  @Test func anUnreachableTsugumidIsToldAsAConnectionFailure() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = Endpoint(url: URL(string: "ws://127.0.0.1:1/rpc")!, token: "t", projectId: "p")
    s.start()
    try await waitForModel { !(s.connectionMessage == nil) }
    #expect(!s.connected)
    #expect(s.connectionFailed, "\(s.connectionMessage ?? "")")
  }

  /// A rename leaves layout unchanged; the saving client cleans deleted references.
  /// A deleted Card's frame does not come back on a new Card of that name.
  @Test func anAgentRenameKeepsThePlaceFrameAndGroup() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let a = key(["a"]), b = key(["b"]), renamed = b
    s.setPosition(b, CGPoint(x: 700, y: 500))
    s.updateDisplayFrame(b) {
      $0["x"] = .real(700)
      $0["y"] = .real(700)
      $0["width"] = .real(300)
      $0["height"] = .real(200)
    }
    let group = try await s.addGroup([a, b], in: [])
    await s.save()
    #expect(!s.dirty, "\(s.saveError ?? "")")

    let snap = try await s.rpc!.inspect()
    let definition = try #require(snap.definitions.compactMap { $0 }.first(where: { $0.id == "b" }))
    var meta = try JSONValue.parse(definition.metaJson ?? "{}")
    meta["name"] = .string("renamed")
    _ = try await s.rpc!.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent",
                              changes: [Change.putDefinition(id: "b", cardJSON: definition.cardJson, metaJSON: meta.serialized())])
    try await waitForModel { !(s.displayName(b) != "renamed") }
    #expect(s.position(renamed) == CGPoint(x: 700, y: 500))
    #expect(s.layout["displays"]?["b"]?["width"]?.double == 300)
    #expect(s.displayName(b) == "renamed")
    #expect(s.groups(in: []).first { $0["id"]?.string == group }?["cards"]?.array?.map(\.string) == ["a", "b"])
    #expect(!s.dirty)

    let again = try await s.rpc!.inspect()
    var left = try JSONValue.parse(again.projectJson)
    let kept = (left["canvas"]?["cards"]?.array ?? []).filter { $0["id"]?.string != "b" }
    left["canvas"]?["cards"] = .array(kept)
    let request = Tsugumi_Host_LayoutValidateRequestT()
    request.json = again.layoutJson; request.removedCards = ["b"]
    let validated: Tsugumi_Host_LayoutValidatedT = try await CoreHostClient.shared.hostCall(.layoutvalidaterequest, request)
    let cleaned = validated.json
    _ = try await s.rpc!.save(baseRev: again.rev ?? 0, opId: UUID(), origin: "agent",
                              changes: [FixtureChange.setProject(left.serialized()), FixtureChange.removeDefinition(id: "b"), Change.setLayout(cleaned)])
    try await waitForModel { !(s.resolution[renamed] != nil) }
    #expect(s.layout["cards"]?["b"] == nil && s.layout["displays"]?["b"] == nil)
    #expect(s.groups(in: []).first { $0["id"]?.string == group }?["cards"]?.array?.map(\.string) == ["a"])
  }

  /// A project no window shows any more is closed: its connection ends and it
  /// does not come back on its own (its environment is given back).
  @Test func aClosedSessionLetsGo() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    await s.close()
    #expect(!s.connected)
    guard case .stopped = s.environment else { Issue.record("still \(s.environment)"); return }
    #expect(!s.connected)
  }

  /// A frame still drawn beside its Card has no place of its own yet. Arrange gives it
  /// the whole frame, or the host rejects the layout and nothing can be saved.
  @Test func arrangingAFrameWithoutAPlaceCanBeSaved() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let b = key(["b"])
    s.deleteCards([key(["a"]), key(["c"])], in: [])
    try await s.connect(from: (key(["start"]), "out"), to: (b, "in"), in: [])
    await s.save()
    try await s.run(card: ["start"], propagate: true)
    try await Self.waitForSuccessfulRun(s)
    try await waitForModel { !(s.displays[b] == nil) }
    #expect(s.displays[b] != nil && s.displayFrame(b) == nil, "shown beside its Card, with no frame in the layout")

    await s.arrange([])
    await s.save()
    #expect(s.saveError == nil, "\(s.saveError ?? "")")
    #expect(!s.dirty)
    let f = s.confirmedLayout["displays"]?["b"]
    #expect(f?["x"]?.double != nil && f?["y"]?.double != nil)
    #expect(f?["width"]?.double == 320 && f?["height"]?.double == 170)
  }

  /// A draft the core refuses, while the Agent goes on saving: Discard Changes goes back
  /// to what is saved, the Agent's saves included, and saving works again.
  @Test func aMarqueeCatchesNotesAndFramesWhichMoveAndGoWithTheCards() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)

    func check(in scope: [String]) async throws {
      let near = try await s.addCard(template: .draft, in: scope, at: .zero, name: "near")
      let framed = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 3000, y: 0), name: "framed")
      let outside = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 6000, y: 0), name: "outside")
      let beside = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 9000, y: 0), name: "beside")
      let note = try await s.addNote(in: scope, at: CGPoint(x: 100, y: 300))
      let outsideNote = try await s.addNote(in: scope, at: CGPoint(x: 6000, y: 3000))
      s.updateDisplayFrame(framed) {
        $0["x"] = .real(400)
        $0["y"] = .real(400)
        $0["width"] = .real(300)
        $0["height"] = .real(200)
      }
      // Opened where they are drawn beside their Cards: a saved frame has its place.
      s.updateDisplayFrame(near) { $0["hidden"] = .bool(false) }
      s.updateDisplayFrame(beside) { $0["hidden"] = .bool(false) }  // to be closed
      s.closeDisplayFrame(outside)
      await s.save()

      let items = s.marqueeItems(CGRect(x: -500, y: -500, width: 1500, height: 1500), in: scope)
      #expect(items.cards.contains(near) && !items.cards.contains(framed) && !items.cards.contains(outside), "\(scope)")
      #expect(items.notes == [note])
      #expect(items.frames == [near, framed], "the frame caught, though its Card is outside")
      #expect(!items.notes.contains(outsideNote))
      #expect(s.marqueeItems(CGRect(x: 5900, y: -500, width: 1000, height: 1000), in: scope).frames.isEmpty, "a closed frame is not caught")

      let before = (card: s.position(near), note: s.noteRect(s.note(note)!), beside: s.displayRect(near), undos: s.undoStack.count)
      await s.moveSelection(cards: items.cards, notes: items.notes, frames: items.frames, by: CGSize(width: 30, height: 40), snap: nil).value
      #expect(s.position(near) == CGPoint(x: before.card.x + 30, y: before.card.y + 40))
      #expect(s.noteRect(s.note(note)!).origin == CGPoint(x: before.note.minX + 30, y: before.note.minY + 40))
      #expect(s.displayRect(framed) == CGRect(x: 430, y: 440, width: 300, height: 200), "its Card stays, its size too")
      #expect(s.position(framed) == CGPoint(x: 3000, y: 0))
      #expect(s.displayRect(near).origin == CGPoint(x: before.beside.minX + 30, y: before.beside.minY + 40), "not moved twice")
      #expect(s.undoStack.count == before.undos + 1, "one edit")
      await s.undo()
      #expect(s.position(near) == before.card && s.noteRect(s.note(note)!).origin == before.note.origin)
      #expect(s.displayRect(framed).origin == CGPoint(x: 400, y: 400), "one undo brings all back")

      // With the grid, all go by the same whole grids, so the arrangement stays:
      // the Card goes from where it is drawn, what moves with it by the same step.
      let grid = 20.0
      let offGrid = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 3010, y: 950), name: "offgrid")
      let noteBefore = s.noteRect(s.note(note)!).origin
      await s.moveSelection(cards: [offGrid], notes: [note], frames: [framed], by: CGSize(width: 33, height: 47), snap: grid).value
      #expect(s.position(offGrid) == CGPoint(x: snapToGrid(3010, grid) + 40, y: snapToGrid(950, grid) + 40))
      #expect(s.noteRect(s.note(note)!).origin == CGPoint(x: noteBefore.x + 40, y: noteBefore.y + 40))
      #expect(s.displayRect(framed).origin == CGPoint(x: 440, y: 440))
      await s.undo()
      // Without a Card there is nothing to keep to the grid: they land where dropped.
      await s.moveSelection(cards: [], notes: [note], frames: [framed], by: CGSize(width: 33, height: 47), snap: grid).value
      #expect(s.noteRect(s.note(note)!).origin == CGPoint(x: noteBefore.x + 33, y: noteBefore.y + 47))
      #expect(s.displayRect(framed).origin == CGPoint(x: 433, y: 447))
      await s.undo()

      // A frame alone is closed and its Card stays; notes go; one undo.
      let undos = s.undoStack.count
      s.deleteSelection(cards: [], notes: [note], frames: [framed], in: scope)
      await s.save()
      #expect(s.displayFrame(framed)?["hidden"]?.bool == true && s.instance(framed) != nil)
      #expect(s.note(note) == nil && s.undoStack.count == undos + 1)
      await s.undo()
      #expect(s.note(note) != nil && s.displayFrame(framed)?["hidden"]?.bool != true)

      // A Card takes its frame with it; a frame alone is closed and its Card stays.
      s.deleteSelection(cards: [near], notes: [], frames: [near, beside], in: scope)
      await s.save()
      #expect(s.instance(near) == nil && s.displayFrame(near) == nil)
      #expect(s.instance(beside) != nil && s.displayFrame(beside)?["hidden"]?.bool == true)
      await s.save()
      #expect(!s.dirty, "\(s.saveError ?? "")")
    }
    try await check(in: [])
    let inner = await s.makeSubgraph([key(["a"])], in: [])
    let child = try #require(inner)
    await s.save()
    try await check(in: path(child))

    // Choosing Cards anew lets go of the notes and frames; so does another scope.
    let tab = WorkflowTab(scope: [])
    tab.selection = [key(["b"])]
    tab.selectedNotes = ["n"]
    tab.selectedFrames = [key(["b"])]
    #expect(tab.hasSelection)
    tab.selection = [key(["c"])]
    #expect(tab.selectedNotes.isEmpty && tab.selectedFrames.isEmpty)
    tab.selectedNotes = ["n"]
    tab.enter(["x"])
    #expect(tab.selectedNotes.isEmpty && !tab.hasSelection)
  }

  @Test func publicRPCNotificationsKeepTheirOrderDuringRunBursts() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let daemon = try Daemon(project: dir)
    defer { daemon.stop() }
    let rpc = RPCClient(endpoint: daemon.endpoint.url)
    _ = try await rpc.connect(token: daemon.endpoint.token, expectedProjectId: nil)
    let opened = try await rpc.open(viewId: UUID())
    let marker = UUID()
    // A final saved change is the barrier: every run notification before it
    // must be delivered in order, including when replies arrive concurrently.
    let ended = AsyncStream<Tsugumi_RPC_RunStatusT>.makeStream()
    let received = Task {
      var sequences: [UInt64] = []
      for await event in rpc.notifications {
        sequences.append(event.seq)
        if let run = event.payload.value as? Tsugumi_RPC_RunEventT,
           let finished = run.value?.value as? Tsugumi_RPC_RunFinishedT {
          ended.continuation.yield(finished.summary.status)
        }
        if let patch = event.payload.value as? Tsugumi_RPC_DocPatchedT,
           patch.delta.opId == marker.uuidString.lowercased() { return (sequences, true) }
      }
      ended.continuation.finish()
      return (sequences, false)
    }
    defer { ended.continuation.finish(); received.cancel() }
    var project = try JSONValue.parse(opened.snapshot.projectJson)
    let cards = (project["canvas"]?["cards"]?.array ?? []).filter {
      ["start", "b"].contains($0["id"]?.string ?? "")
    }
    project["canvas"]?["cards"] = .array(cards)
    project["canvas"]?["connections"] = .array([
      .object([("from", .object([("card", .string("start")), ("port", .string("out"))])),
               ("to", .object([("card", .string("b")), ("port", .string("in"))]))])
    ])
    let saved = try await rpc.save(baseRev: opened.snapshot.rev ?? 0, opId: UUID(), origin: "ui",
                                   changes: [FixtureChange.setProject(project.serialized())])
    var finishes = ended.stream.makeAsyncIterator()
    for _ in 0..<2 {
      let accepted = try await rpc.run(baseRev: saved.rev ?? 0, opId: UUID(), card: ["start"], propagate: true)
      let status = await finishes.next()
      try #require(status?.runId == accepted.run.runId && status?.state == .succeeded)
    }
    _ = try await rpc.save(baseRev: saved.rev ?? 0, opId: marker, origin: "ui",
                            changes: [Change.setLayout("{\"cards\":{\"b\":{\"x\":1,\"y\":2}}}")])
    let (sequences, reachedMarker) = await received.value
    await rpc.close()
    #expect(reachedMarker, "The saved-change barrier must arrive after the run notifications")
    #expect(sequences.first == (opened.seq ?? 1) + 1)
    let outOfOrder = zip(sequences, sequences.dropFirst()).first { $1 != $0 + 1 }
    #expect(outOfOrder == nil)
  }

  /// A name a Card has had is free again, but its frame and its result stay
  /// behind: a new Card under that name (the next Draft) starts with neither,
  /// also when it is implemented later, and the Card that had them keeps them
  /// through a rename and gets them back when a delete is undone.
  @Test func nameChangesKeepIdentityAndDoNotGiveOtherCardsResults() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let b = key(["b"])
    s.deleteCards([key(["a"]), key(["c"])], in: [])
    try await s.connect(from: (key(["start"]), "out"), to: (b, "in"), in: [])
    s.updateDisplayFrame(b) {
      $0["x"] = .real(400); $0["y"] = .real(300)
      $0["width"] = .real(300); $0["height"] = .real(200)
    }
    await s.save()
    try #require(!s.dirty, "\(s.saveError ?? "")")
    try await s.run(card: ["start"], propagate: true)
    try await Self.waitForSuccessfulRun(s)
    try await waitForModel { !(s.displays[b] == nil) }
    let run = s.displays[b]?.runId
    #expect(run != nil)
    _ = try await s.applyMutation { $0.updateMeta(b) { $0["name"] = .string("同じ名前") } }
    #expect(s.displayName(b) == "同じ名前")
    #expect(s.nodeData(b).title == "同じ名前")
    #expect(s.nodeData(b, meta: .object([("name", .string("Preview"))])).title == "Preview")
    #expect(s.nodeData(b, meta: .object([("name", .string(""))])).title == b)
    #expect(s.displays[b]?.runId == run && s.displayFrame(b)?["width"]?.double == 300)
    #expect(s.displayFrame(b)?["x"]?.double == 400 && s.displayFrame(b)?["y"]?.double == 300)
    let first = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 0, y: 400), name: "同じ名前")
    let second = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 0, y: 800), name: "同じ名前")
    #expect(first != second && first != b && second != b)
    #expect(s.displayName(first) == s.displayName(second) && s.displayName(first) == s.displayName(b))
    for k in [first, second] {
      #expect(s.displays[k] == nil && s.displayFrame(k) == nil && s.cardStates[k] == nil)
    }
    await s.save()
    #expect(!s.dirty, "\(s.saveError ?? "")")
    s.deleteCards([b], in: [])
    await s.save()
    await s.undo()
    await s.save()
    try await waitForModel { !(s.displays[b] == nil) }
    #expect(s.displays[b]?.runId == run)
    #expect(s.displayFrame(b)?["x"]?.double == 400 && s.displayFrame(b)?["y"]?.double == 300)
  }

  /// Automatic additions leave placement to the host; a person's drop is explicit.
  @Test func aNewCardDelegatesPlacementToTheHost() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let here = CGPoint(x: 4000, y: 4000)
    var placed: [CGPoint] = []
    for template in [DefinitionTemplate.draft, .draft, .python] {
      let k = try await s.addCard(template: template, in: [], at: here, automaticPlacement: true)
      #expect(s.layout["cards"]?[k] != nil, "The host saves the place it chose")
      placed.append(s.position(k))
    }
    #expect(!placed.contains(here) && Set(placed.map { "\($0)" }).count == placed.count, "\(placed)")
    let dropped = try await s.addCard(template: .draft, in: [], at: here)
    #expect(s.position(dropped) == here)
    await s.save()
    #expect(!s.dirty, "\(s.saveError ?? "")")
  }

  /// A failed save keeps a newly dropped Card in place; retry saves that place.
  @Test func commentsAreSavedOnTheirCardWithWhoDidWhat() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let k = try await s.addCard(template: .python, in: [], at: .zero, name: "plot")
    #expect(s.comments(k).isEmpty && s.nodeData(k).openComments == 0)

    try await s.addComment(k, "  y 軸を対数に  ")
    try await s.addComment(k, "   ")
    #expect(s.comments(k).map(\.text) == ["y 軸を対数に"])
    try await s.addComment(k, "軸に名前を")
    let first = s.comments(k)[0].id
    s.edit { $0.setCommentText(k, id: first, text: "両軸を対数に") }
    s.edit { $0.setCommentText(k, id: first, text: "  ") }
    await s.save()
    #expect(s.comments(k).map(\.text) == ["両軸を対数に", "軸に名前を"])
    #expect(s.nodeData(k).openComments == 2 && s.cardsWithOpenComments(in: []) == [k])
    #expect(s.cardDetails(k).contains { $0.contains("両軸を対数に") })

    // What the Agent said goes away when the comment is opened again.
    s.edit { x in
      x.updateMeta(k) { m in
        var list = m["comments"]?.array ?? []
        list[0]["resolved"] = .bool(true)
        list[0]["resolution"] = .string("両軸を対数にした")
        m["comments"] = .array(list)
      }
    }
    await s.save()
    #expect(s.comments(k)[0].resolution == "両軸を対数にした" && s.openComments(k).map(\.text) == ["軸に名前を"])
    s.setCommentResolved(k, id: first, false)
    await s.save()
    #expect(s.comments(k)[0].resolved == false && s.comments(k)[0].resolution == nil)

    #expect(s.comments(k)[0].stateBy == "user" && s.comments(k)[0].editedBy == "user" && s.comments(k)[1].author == "user")
    // A Draft takes comments as any Card.
    let plan = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 0, y: 200), name: "plan")
    try await s.addComment(plan, "単位を考える")
    #expect(s.cardsWithOpenComments(in: []).contains(plan))
    // What the core recorded of the Agent reads back, in the words the screen uses.
    let agent = CardComment.list(in: try JSONValue.parse(#"{"comments":[{"id":"a","text":"t","author":"agent","resolved":true,"stateBy":"agent","cards":["p","q"]}]}"#))
    #expect(agent[0].cards == ["p", "q"] && agent[0].author == "agent")
    #expect(CardComment.change("resolved", by: "agent").contains("Agent") && !CardComment.change("removed", by: nil).contains("（"))

    for c in s.comments(k) { s.removeComment(k, id: c.id) }
    await s.save()
    #expect(s.comments(k).isEmpty && s.displayName(k) == "plot", "Removing all comments keeps the Card's name")
    await s.undo()
    #expect(s.comments(k).map(\.text) == ["軸に名前を"])
  }

  /// Draft wires attach to the Card itself; its content and name survive saving.
  @Test func aDraftIsWiredAndWrittenOnTheCanvas() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    func ports(_ k: CardKey) -> [[String]] {
      let p = s.portData(k)
      return [p.inputs.map(\.name), p.outputs.map(\.name)]
    }
    func wires() -> [String] {
      s.connections(in: []).map { c in
        "\(c["from"]?["card"]?.string ?? "").\(c["from"]?["port"]?.string ?? "")>\(c["to"]?["card"]?.string ?? "").\(c["to"]?["port"]?.string ?? "")"
      }
    }
    let a = try await s.addCard(template: .draft, in: [], at: .zero, name: "p")
    let b = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 300, y: 0), name: "q")
    #expect(ports(a) == [[], []])

    try await s.connect(from: (a, CardGeometry.newPort), to: (b, CardGeometry.newPort), in: [])
    #expect(wires() == ["p.>q."])
    #expect(ports(a) == [[], []] && ports(b) == [[], []])
    let c = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 600, y: 0), name: "r")
    try await s.connect(from: (a, CardGeometry.newPort), to: (c, CardGeometry.newPort), in: [])
    try await s.connect(from: (c, CardGeometry.newPort), to: (b, CardGeometry.newPort), in: [])
    #expect(ports(a) == [[], []] && ports(b) == [[], []])
    #expect(s.connections(in: []).count == 3, "no fork, nothing replaced")

    // The core agrees: no ports are created, even with many wires.
    await s.save()
    #expect(!s.dirty, "\(s.saveError ?? "")")
    for k in [a, b, c] {
      #expect(!(s.resolution[k]?.diagnostics ?? []).contains { $0.message.contains("port") }, "\(k)")
    }
    s.removeConnection(at: 0, in: [])
    #expect(ports(a) == [[], []] && ports(b) == [[], []])
    s.disconnect(c, inputs: false, in: [])
    #expect(ports(b) == [[], []])

    #expect(s.dirty, "The wire removals are still awaiting autosave when typing starts")
    await s.startDraftEdit(a)
    s.inlineDraft?.content = "count words"
    s.inlineDraft?.name = "renamed"
    #expect(CardGeometry(s.nodeData(a), flow: .topToBottom).draftRow == CardGeometry.draftEditRow)
    let to = await s.finishDraftEdit()
    #expect(to == a && s.inlineDraft == nil)
    // The new name arrives with the saved state.
    try await waitForModel { !(s.cardMeta(a) == nil) }
    #expect(s.cardMeta(a)?["content"]?.string == "count words")
    #expect(s.nodeData(a).summary == "count words")

    // No name is fine: the Card shows its content only, and keeps the id made for it.
    let plain = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 0, y: 300))
    #expect(s.draftUntitled(plain) && s.nodeData(plain).untitled)
    await s.startDraftEdit(plain)
    #expect(s.inlineDraft?.name == "" && s.draftNameProblem(plain, "") == nil)
    let same = await s.finishDraftEdit()
    #expect(same == nil && s.instance(plain) != nil)

    // Names are free text and need not be unique; exact addresses stay fixed.
    await s.startDraftEdit(b)
    s.inlineDraft?.name = "日本語"
    #expect(s.cardNameProblem(b, "日本語") == nil)
    let kept = await s.finishDraftEdit()
    #expect(kept == b && s.displayName(b) == "日本語" && s.instance(b) != nil)
    _ = try await s.applyMutation { $0.updateMeta(c) { $0["name"] = .string("日本語") } }
    #expect(s.displayName(c) == s.displayName(b) && b != c)
    #expect(wires() == ["p.>r."])
    #expect(s.cardNameProblem(b, " \n ") != nil)
    // An overlong name is still refused without changing the Card.
    let overlong = String(repeating: "あ", count: 1366)
    await s.startDraftEdit(b)
    s.inlineDraft?.name = overlong
    #expect(s.cardNameProblem(b, overlong) != nil)
    let refused = await s.finishDraftEdit()
    #expect(refused == nil && s.notice != nil && s.displayName(b) == "日本語" && s.instance(b) != nil)
    await s.close()
  }

  @Test func draftCompletionIsJoinedBeforeSwitchingRunningAndClosing() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let model = AppModel.shared
    // The model starts its session: the connection it is making reads the
    // endpoint set here, and a second start would replace it under an action.
    let s = try model.session(for: dir)
    s.endpointOverride = d.endpoint
    try await Self.connected(s)
    let a = try await s.addCard(template: .draft, in: [], at: .zero)
    let b = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 300, y: 0))

    await s.startDraftEdit(a)
    s.inlineDraft?.content = "first Draft"
    // Return/Command-Return and focus loss can submit without awaiting each
    // other. Opening the next Draft must join them before replacing the fields.
    let done = Task { await s.finishDraftEdit() }
    let blurred = Task { await s.finishDraftEdit() }
    await s.startDraftEdit(b)
    _ = await done.value
    _ = await blurred.value
    #expect(s.cardMeta(a)?["content"]?.string == "first Draft")
    #expect(s.inlineDraft?.card == b)
    s.inlineDraft?.content = "before running"
    // Running the unconnected trigger has no worker work; the Draft must be
    // committed before RunRequest, even without a focus-loss callback.
    try await s.run(card: ["start"], propagate: false)
    #expect(s.inlineDraft == nil && s.cardMeta(b)?["content"]?.string == "before running")

    await s.startDraftEdit(b)
    s.inlineDraft?.content = "before closing"
    let w = Workspace()
    model.hold(s)
    w.open(s)
    model.windowClosed(w)
    try await waitForModel { !(model.sessions[s.identity.id] != nil) }
    #expect(w.session == nil && model.sessions[s.identity.id] == nil)
    #expect(s.inlineDraft == nil && s.cardMeta(b)?["content"]?.string == "before closing")
    let saved = try JSONValue.parse(String(contentsOf: dir.appendingPathComponent("definitions/\(b)/_meta.json"), encoding: .utf8))
    #expect(saved["content"]?.string == "before closing", "Closing waits for the transaction on disk")
  }

  @Test func agentChangesAndUIEditsBothStay() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let agent = RPCClient(endpoint: d.endpoint.url)
    _ = try await agent.connect(token: d.endpoint.token, expectedProjectId: nil)

    defer { Task { await agent.close() } }
    // Start each Agent edit with UI edits pending. Either transaction may win;
    // if the Agent loses, join the UI saves before retrying its fresh snapshot.
    // No further UI writes compete with that retry, so exhaustion of an
    // arbitrary retry count cannot masquerade as an RPC timeout.
    for (batch, id) in ["a", "b", "c"].enumerated() {
      let snapshot = try await agent.inspect()
      for i in (batch * 10)..<(batch * 10 + 10) {
        s.edit { $0.setPosition(i % 2 == 0 ? "a" : "b", CGPoint(x: Double(i * 10), y: 5)) }
      }
      let revision: UInt64
      do { revision = try await Self.agentConnect(agent, to: id, snapshot: snapshot) }
      catch let failure as RPCFailure where failure.kind == .revisionConflict {
        await s.save()
        try #require(s.saveError == nil && !s.dirty, "UI save failed: \(s.saveError ?? "still dirty")")
        revision = try await Self.agentConnect(agent, to: id)
      }
      try await Self.received(revision, by: s)
      await s.save()
    }

    #expect(s.saveError == nil)
    #expect(!s.dirty)
    // The Agent's connections are shown and kept…
    let targets = Set(s.connections(in: []).compactMap { $0["to"]?["card"]?.string })
    #expect(targets == ["a", "b", "c"])
    // …and the UI's last positions are saved.
    #expect(s.confirmedLayout["cards"]?["a"]?["x"]?.double == 280)
    #expect(s.confirmedLayout["cards"]?["b"]?["x"]?.double == 290)
    let snap = try await agent.inspect()
    let proj = try JSONValue.parse(snap.projectJson)
    #expect(proj["canvas"]?["connections"]?.array?.count == 3)

    // Undo takes back the UI's last move only; the Agent's wires stay.
    await s.undo()
    await s.save()
    #expect(s.confirmedLayout["cards"]?["b"]?["x"]?.double == 270)
    #expect(Set(s.connections(in: []).compactMap { $0["to"]?["card"]?.string }) == ["a", "b", "c"])
    await s.close()
  }

  /// delegate: each helper's bird writes the Cards it owns; a Card no helper
  /// owns is the main Agent's.
  @Test func helpersSavesAreDrawnByTheirOwnBirds() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    s.startHelpers([(name: "first", cards: ["a"]), (name: "second", cards: ["b"])])
    #expect(s.presences.count == 3 && s.helperPresences.map(\.label) == ["first", "second"])
    #expect(s.helperPresences.map(\.tint) == [1, 2])
    // Each comes to its Card first.
    #expect(s.helperPresences[0].beats.map(\.beat) == [.visit("a")])
    let agent = RPCClient(endpoint: d.endpoint.url)
    _ = try await agent.connect(token: d.endpoint.token, expectedProjectId: nil)
    for id in ["b", "c"] {
      let snap = try await agent.inspect()
      _ = try await agent.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent",
                               changes: [Change.putDefinition(id: id, cardJSON: #"{"language":"bash","inputs":[{"name":"in"}],"maxRuns":3}"#, metaJSON: nil)])
    }
    try await waitForModel { s.presence.beats.contains { $0.beat == .save([AgentPresence.Written(card: "c", reveal: false)], []) } }
    #expect(s.helperPresences[1].beats.contains { $0.beat == .save([AgentPresence.Written(card: "b", reveal: false)], []) })
    #expect(!s.presence.beats.contains { $0.beat == .save([AgentPresence.Written(card: "b", reveal: false)], []) })
    #expect(s.presence.beats.contains { $0.beat == .save([AgentPresence.Written(card: "c", reveal: false)], []) })
    #expect(s.presence(forCard: "a") === s.helperPresences[0] && s.presence(forCard: "c") === s.presence)
    await s.close()
  }

  /// The returned host graph is saved and undone as one client edit.
  @Test func reloadingSettingsKeepsTheExactWorkingText() {
    let edit = RevisionEdit()
    edit.reload(0)
    edit.conflict(1)
    var text = "{\"symbol\": \"mine\",\n"
    let mine = text
    edit.keepAndReload(text) { text = "{}"; edit.reload(1) }
    #expect(text == "{}")
    #expect(edit.keptText == mine)
    #expect(edit.baseRev == 1 && edit.conflictRev == nil)
  }

  /// JSON definitions and project settings retain their own opening revision.
  /// Rejected text is not left in the session for a later autosave to send.
  @Test func importedProjectIsEditedInside() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let other = try Self.project()
    defer { try? FileManager.default.removeItem(at: other) }
    try FileManager.default.createDirectory(at: other.appendingPathComponent(".tsugumi"), withIntermediateDirectories: true)
    try Data("history".utf8).write(to: other.appendingPathComponent(".tsugumi/runs.sqlite"))
    let d = try Daemon(project: dir)
    defer { d.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)

    let k = try await s.importProject(from: other, in: [], at: CGPoint(x: 400, y: 0))
    await s.save()
    #expect(s.saveError == nil)
    let name = path(k).last!
    let ext = dir.appendingPathComponent("externals/\(name)")
    #expect(FileManager.default.fileExists(atPath: ext.appendingPathComponent("definitions/a/_card.json").path))
    #expect(!FileManager.default.fileExists(atPath: ext.appendingPathComponent(".tsugumi").path))
    #expect(s.kind(k) == .subgraph)
    try await waitForModel { !(s.cards(in: [name]).isEmpty) }
    #expect(s.cards(in: [name]).count == 4)

    // Inside: wire and move Cards, add one; the external's own files change.
    try await s.connect(from: (key([name, "start"]), "out"), to: (key([name, "a"]), "in"), in: [name])
    s.edit { $0.setPosition(key([name, "a"]), CGPoint(x: 123, y: 45)) }
    let added = try await s.addCard(template: .python, in: [name], at: CGPoint(x: 0, y: 200))
    await s.save()
    #expect(s.saveError == nil && !s.dirty)
    let proj = try JSONValue.parse(String(decoding: try Data(contentsOf: ext.appendingPathComponent("tsugumi.json")), as: UTF8.self))
    #expect(proj["canvas"]?["connections"]?.array?.count == 1)
    #expect(proj["canvas"]?["cards"]?.array?.contains { $0["id"]?.string == path(added).last } == true)
    let lay = try JSONValue.parse(String(decoding: try Data(contentsOf: ext.appendingPathComponent("layout.json")), as: UTF8.self))
    #expect(lay["cards"]?["a"]?["x"]?.double == 123)
    #expect(FileManager.default.fileExists(atPath: ext.appendingPathComponent("definitions/\(path(added).last!)/_card.json").path))
    // The core resolves the inner Cards from the edited files.
    try await waitForModel { !(s.resolution[added] == nil) }
    #expect(s.resolution[added] != nil)

    // A definition of the external, copied into this project as a new Card.
    let copy = await s.placeDefinitionCopy(prefix: "externals/\(name)/", id: "c", in: [], at: .zero)
    await s.save()
    #expect(copy == "c-2")
    #expect(s.confirmedDefinitions["c-2"]?["language"]?.string == "bash")

    // Undo of the copy, then of the inner move, one operation each.
    await s.undo()
    await s.save()
    #expect(s.confirmedDefinitions["c-2"] == nil && s.instance("c-2") == nil)
  }
}

extension SyncTests {
  @Test func deletingAWireEndWhileAllocatingAForkDoesNotAddTheWire() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let daemon = try Daemon(project: dir)
    defer { daemon.stop() }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await Self.connected(s)
    try await s.connect(from: ("start", "out"), to: ("a", "in"), in: [])
    await s.save()
    let allocating = AsyncStream<Void>.makeStream()
    var allocation: CheckedContinuation<String, Never>?
    s.cardIdProvider = { _, _ in await withCheckedContinuation { allocation = $0; allocating.continuation.yield() } }
    let connecting = Task { try await s.connect(from: ("start", "out"), to: ("b", "in"), in: []) }
    var allocationEvents = allocating.stream.makeAsyncIterator()
    _ = await allocationEvents.next()
    s.deleteCards(["b"], in: [])
    allocation?.resume(returning: "fork-delayed")
    // The host refuses the wire: its end is gone by the time the action's turn comes.
    await #expect(throws: (any Error).self) { try await connecting.value }
    #expect(s.instance("fork-delayed") == nil)
    #expect(!s.connections(in: []).contains { $0["to"]?["card"]?.string == "b" })
    await s.close()
  }

  @Test func reloadingExternalPermissionWideningCanBeDeclinedOrApproved() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let daemon = try Daemon(project: dir)
    defer { daemon.stop() }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await Self.connected(s)
    var definition = try #require(s.cardDefinition("a"))
    definition["network"] = .bool(true)
    try definition.serialized().write(to: dir.appendingPathComponent("definitions/a/_card.json"), atomically: true, encoding: .utf8)
    let base = s.rev
    await s.reloadExternal()
    #expect(s.reloadApproval?.permissions.contains("network a") == true)
    await s.answerReloadApproval(false, selected: [])
    #expect(s.rev == base && s.reloadApproval == nil)
    await s.reloadExternal()
    #expect(s.reloadApproval != nil)
    await s.answerReloadApproval(true, selected: ["network a"])
    #expect(s.saveError == nil && s.reloadApproval == nil)
    try await waitForModel { !(s.rev == base) }
    #expect(s.cardDefinition("a")?["network"]?.bool == true)
    await s.close()
  }
}

@Suite struct RunMarkTests {
  @Test func mapsExplicitRunProvenance() {
    let status = Tsugumi_RPC_RunStatusT()
    #expect(runMark(status) == nil)
    status.partialRun = true
    #expect(runMark(status) == RunMarkLabel.partial)
    status.partialRun = false
    status.usesPartialResult = true
    #expect(runMark(status) == RunMarkLabel.derived)
    status.partialRun = true
    #expect(runMark(status) == "\(RunMarkLabel.partial) · \(RunMarkLabel.derived)")
  }
}

/// Raw core mutations only set up the other writer in synchronization scenarios.
private enum FixtureChange {
  static func setProject(_ json: String) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_SetProjectT()
    v.json = json
    return change(v, .setproject)
  }

  static func removeDefinition(id: String) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_RemoveDefinitionT()
    v.id = id
    return change(v, .removedefinition)
  }

  static func putSource(path: String, bytes: Data, baseHash: String?) -> Tsugumi_RPC_DocumentChangeT {
    let v = Tsugumi_RPC_PutSourceT()
    v.path = path
    v.bytes = [UInt8](bytes)
    v.baseHash = baseHash
    return change(v, .putsource)
  }

  private static func change(_ value: NativeObject, _ type: Tsugumi_RPC_DocumentChangeValue) -> Tsugumi_RPC_DocumentChangeT {
    let result = Tsugumi_RPC_DocumentChangeT()
    result.value = Tsugumi_RPC_DocumentChangeValueUnion(value, type: type)
    return result
  }
}
