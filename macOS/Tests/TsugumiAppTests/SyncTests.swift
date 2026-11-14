import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

/// The UI saves every edit at once and takes in what the Agent saves at the
/// same time: both changes stay, and no save conflict is shown.
@MainActor
@Suite(.serialized) struct SyncTests {
  nonisolated static let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent()

  final class Daemon {
    let process = Process()
    let dir: URL
    var ready: [String: Any] = [:]

    init(project: URL, mockedBuilds: Bool = false) throws {
      dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("tsg-\(UUID().uuidString.prefix(8))")
      try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
      let env = ProcessInfo.processInfo.environment
      let build = env["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) }
        ?? SyncTests.root.appendingPathComponent("build")
      let binary = mockedBuilds ? ("TSUGUMI_MOCK_DAEMON", "tests/core/tsugumid-mock") : ("TSUGUMID", "core/tsugumid")
      process.executableURL = env[binary.0].map { URL(fileURLWithPath: $0) } ?? build.appendingPathComponent(binary.1)
      if mockedBuilds { process.environment = env.merging(["TSUGUMI_MOCK_CONTROL": dir.path]) { _, value in value } }
      process.arguments = ["--project", project.path, "--unix", dir.appendingPathComponent("s").path,
                           "--control-dir", dir.appendingPathComponent("control").path,
                           "--worker", env["TSUGUMI_WORKER"] ?? build.appendingPathComponent("workerd/tsugumi-worker").path,
                           "--sdk-dir", env["TSUGUMI_SDK_DIR"] ?? SyncTests.root.appendingPathComponent("sdk").path,
                           "--allow-missing-origin", "--listen", "127.0.0.1:0"]
      process.standardOutput = FileHandle.nullDevice
      process.standardError = FileHandle.nullDevice
      try process.run()
      let file = dir.appendingPathComponent("control/server.ready")
      for _ in 0..<400 {
        if let d = try? Data(contentsOf: file), let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] {
          ready = j
          return
        }
        Thread.sleep(forTimeInterval: 0.025)
      }
      throw CocoaError(.fileReadNoSuchFile)
    }

    var endpoint: Endpoint {
      Endpoint(url: URL(string: ready["endpoint"] as! String)!, token: ready["token"] as! String,
               projectId: ready["project_id"] as? String ?? "", serverInstanceId: ready["server_instance_id"] as? String ?? "")
    }

    func stop() {
      process.terminate()
      process.waitUntilExit()
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
      return Change.putSource(path: "definitions/\(id)/\(name)", bytes: Data(source.utf8), baseHash: nil)
    }
  }

  /// A project with a trigger and a few Bash sinks (no connections yet).
  static func project() throws -> URL {
    let p = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("sync-\(UUID().uuidString.prefix(8))")
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
  }

  /// The Agent's side: adds a connection with its own read-modify-save, as the Agent tools do.
  static func agentConnect(_ c: RPCClient, to card: String) async throws {
    for _ in 0..<20 {
      let snap = try await c.inspect()
      var proj = try JSONValue.parse(snap.projectJson)
      var conns = proj["canvas"]?["connections"]?.array ?? []
      conns.append(.object([("from", .object([("card", .string("start")), ("port", .string("out"))])),
                            ("to", .object([("card", .string(card)), ("port", .string("in"))]))]))
      proj["canvas"]?["connections"] = .array(conns)
      do {
        _ = try await c.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent", changes: [Change.setProject(proj.serialized())])
        return
      } catch let f as RPCFailure where f.kind == .revisionConflict {
        continue
      }
    }
    throw RPCError.timeout
  }

  static func connected(_ s: ProjectSession) async throws {
    for _ in 0..<200 where !s.connected { try await Task.sleep(for: .milliseconds(25)) }
    #expect(s.connected, "\(s.connectionMessage ?? "")")
  }

  static func waitForSuccessfulRun(_ s: ProjectSession) async throws {
    for _ in 0..<200 where s.runActive { try await Task.sleep(for: .milliseconds(25)) }
    try #require(s.runStatus?.state == .succeeded)
  }

  /// A tsugumid that cannot be reached is tried again for a moment and then
  /// told as a connection failure, not as a project that could not be read.
  @Test func anUnreachableTsugumidIsToldAsAConnectionFailure() async throws {
    let dir = try Self.project()
    defer { try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.connectRetryDelays = [0.01, 0.01]
    s.endpointOverride = Endpoint(url: URL(string: "ws://127.0.0.1:1/rpc")!, token: "t", projectId: "p", serverInstanceId: "i")
    s.start()
    for _ in 0..<400 where s.connectionMessage == nil { try await Task.sleep(for: .milliseconds(25)) }
    #expect(!s.connected)
    #expect(s.connectionFailed, "\(s.connectionMessage ?? "")")
  }

  /// The Agent's rename and delete send no layout: the Card stays where it was
  /// under its new name, and the frame of a deleted Card does not come back on
  /// a new Card of that name.
  @Test func anAgentRenameKeepsThePlaceFrameAndGroup() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
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
    let group = s.addGroup([a, b], in: [])
    await s.save()
    #expect(!s.dirty, "\(s.saveError ?? "")")

    let snap = try await s.rpc!.inspect()
    let definition = try #require(snap.definitions.compactMap { $0 }.first(where: { $0.id == "b" }))
    var meta = try JSONValue.parse(definition.metaJson ?? "{}")
    meta["name"] = .string("renamed")
    _ = try await s.rpc!.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent",
                              changes: [Change.putDefinition(id: "b", cardJSON: definition.cardJson, metaJSON: meta.serialized())])
    for _ in 0..<200 where s.displayName(b) != "renamed" { try await Task.sleep(for: .milliseconds(25)) }
    #expect(s.position(renamed) == CGPoint(x: 700, y: 500))
    #expect(s.layout["displays"]?["b"]?["width"]?.double == 300)
    #expect(s.displayName(b) == "renamed")
    #expect(s.groups(in: []).first { $0["id"]?.string == group }?["cards"]?.array?.map(\.string) == ["a", "b"])
    #expect(!s.dirty)

    let again = try await s.rpc!.inspect()
    var left = try JSONValue.parse(again.projectJson)
    let kept = (left["canvas"]?["cards"]?.array ?? []).filter { $0["id"]?.string != "b" }
    left["canvas"]?["cards"] = .array(kept)
    _ = try await s.rpc!.save(baseRev: again.rev ?? 0, opId: UUID(), origin: "agent",
                              changes: [Change.setProject(left.serialized()), Change.removeDefinition(id: "b")])
    for _ in 0..<200 where s.resolution[renamed] != nil { try await Task.sleep(for: .milliseconds(25)) }
    #expect(s.layout["cards"]?["b"] == nil && s.layout["displays"]?["b"] == nil)
    #expect(s.groups(in: []).first { $0["id"]?.string == group }?["cards"]?.array?.map(\.string) == ["a"])
  }

  /// A project no window shows any more is closed: its connection ends and it
  /// does not come back on its own (its environment is given back).
  @Test func aClosedSessionLetsGo() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    await s.close()
    #expect(!s.connected)
    guard case .stopped = s.environment else { Issue.record("still \(s.environment)"); return }
    try await Task.sleep(for: .milliseconds(600))
    #expect(!s.connected)
  }

  /// A frame still drawn beside its Card has no place of its own yet. Arrange gives it
  /// the whole frame, or the core refuses the layout and nothing can be saved.
  @Test func arrangingAFrameWithoutAPlaceCanBeSaved() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
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
    for _ in 0..<200 where s.displays[b] == nil { try await Task.sleep(for: .milliseconds(25)) }
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
  @Test func discardingWhatCannotBeSavedGoesBackAndSavingWorksAgain() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let a = key(["a"])
    s.setPosition(a, CGPoint(x: 100, y: 100))
    await s.save()
    #expect(!s.dirty, "\(s.saveError ?? "")")

    // A layout the core refuses, however it came to be.
    s.edit { $0.updateLayout(of: []) { l in l["displays"] = .object([("b", .object([("x", .real(1))]))]) } }
    await s.save()
    #expect(s.saveError?.contains("display b") == true, "\(s.saveError ?? "")")
    #expect(s.dirty)

    // The Agent's save reaches the window meanwhile, and the refused edit stays refused.
    let rev = s.rev
    try await Self.agentConnect(s.rpc!, to: "c")
    for _ in 0..<200 where s.rev == rev { try await Task.sleep(for: .milliseconds(25)) }
    await s.save()
    #expect(s.saveError != nil)

    s.discardDraft()
    #expect(!s.dirty)
    #expect(s.saveError == nil)
    #expect(s.layout == s.confirmedLayout)
    #expect(s.project == s.confirmedProject)
    #expect(s.connections(in: []).contains { $0["to"]?["card"]?.string == "c" }, "what the Agent saved stays")
    #expect(s.position(a) == CGPoint(x: 100, y: 100))

    s.setPosition(a, CGPoint(x: 300, y: 300))
    await s.save()
    #expect(s.saveError == nil, "\(s.saveError ?? "")")
    #expect(!s.dirty)
    #expect(s.confirmedLayout["cards"]?["a"]?["x"]?.double == 300)
  }

  /// The refused edit undone: nothing is left to save, so the failure does not stay.
  @Test func undoingWhatCannotBeSavedClearsTheFailure() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    s.edit { $0.updateLayout(of: []) { l in l["displays"] = .object([("b", .object([("x", .real(1))]))]) } }
    await s.save()
    #expect(s.saveError != nil)
    s.undo()
    await s.save()
    #expect(!s.dirty)
    #expect(s.saveError == nil, "\(s.saveError ?? "")")
    #expect(s.syncState == .saved)
  }

  /// Home with what cannot be saved: the window says why and stays until the user
  /// gives the changes up; then it goes back to Home with nothing kept.
  @Test func leavingWithWhatCannotBeSavedAsksAndDiscardingLeaves() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let model = AppModel.shared, w = Workspace()
    model.hold(s)
    w.open(s)
    s.edit { $0.updateLayout(of: []) { l in l["displays"] = .object([("b", .object([("x", .real(1))]))]) } }

    model.leaveProject(in: w)
    for _ in 0..<200 where w.unsavedReason == nil { try await Task.sleep(for: .milliseconds(25)) }
    #expect(w.unsavedReason?.contains("display b") == true, "\(w.unsavedReason ?? "")")
    #expect(w.alert == nil, "not a dead end")
    #expect(w.session === s && s.dirty, "it stays, with the changes, until the user answers")

    model.discardAndLeave(in: w)
    #expect(w.session == nil)
    #expect(!s.dirty)
    #expect(s.saveError == nil)
  }

  /// Arrange changes the layout only, frames and notes too, and is one step of undo.
  @Test func arrangingIsOneUndo() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let a = try await s.addCard(template: .trigger, in: [], at: CGPoint(x: 500, y: 500), name: "a")
    let b = try await s.addCard(template: .python, in: [], at: .zero, name: "b")
    let note = s.addNote(in: [], at: CGPoint(x: 3, y: 4))
    s.updateDisplayFrame(b) { d in
      d["width"] = .real(300)
      d["height"] = .real(200)
    }
    let before = s.layout
    let project = s.project
    await s.arrange([])
    #expect(s.layout != before)
    #expect(s.project == project, "the layout only")
    #expect(s.position(a) != CGPoint(x: 500, y: 500))
    #expect(s.displayFrame(b)?["width"]?.double == 300)
    #expect(s.displayFrame(b)?["x"]?.double != nil)
    #expect(s.note(note)?["x"]?.double != 3 || s.note(note)?["y"]?.double != 4)
    let arranged = s.layout
    s.undo()
    #expect(s.layout == before)
    await s.arrange([])
    #expect(s.layout == arranged, "from the same graph, the same layout")
    await s.arrange([])
    #expect(s.layout == arranged, "arranging again changes nothing")
    // Moving one Card afterwards: it goes by whole grids and nothing else moves.
    let was = s.position(b), other = s.position(a), frame = s.displayFrame(b)
    s.moveSelection(cards: [b], notes: [], frames: [], by: CGSize(width: 47, height: 33), snap: 20)
    #expect(s.position(b) == CGPoint(x: was.x + 40, y: was.y + 40))
    #expect(s.position(a) == other)
    #expect(s.displayFrame(b) == frame)
  }

  /// A marquee catches Cards, sticky notes and output frames, at the top and
  /// inside a Card in Card. They move together in one edit and one save, and go
  /// together: a frame alone is closed and its Card stays.
  @Test func aMarqueeCatchesNotesAndFramesWhichMoveAndGoWithTheCards() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)

    func check(in scope: [String]) async throws {
      let near = try await s.addCard(template: .draft, in: scope, at: .zero, name: "near")
      let framed = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 3000, y: 0), name: "framed")
      let outside = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 6000, y: 0), name: "outside")
      let beside = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 9000, y: 0), name: "beside")
      let note = s.addNote(in: scope, at: CGPoint(x: 100, y: 300))
      let outsideNote = s.addNote(in: scope, at: CGPoint(x: 6000, y: 3000))
      s.updateDisplayFrame(framed) {
        $0["x"] = .real(400)
        $0["y"] = .real(400)
        $0["width"] = .real(300)
        $0["height"] = .real(200)
      }
      s.updateDisplayFrame(near) { $0["hidden"] = .bool(false) }  // beside its Card
      s.updateDisplayFrame(beside) { $0["hidden"] = .bool(false) }  // beside its Card, to be closed
      s.closeDisplayFrame(outside)

      let items = s.marqueeItems(CGRect(x: -500, y: -500, width: 1500, height: 1500), in: scope)
      #expect(items.cards.contains(near) && !items.cards.contains(framed) && !items.cards.contains(outside), "\(scope)")
      #expect(items.notes == [note])
      #expect(items.frames == [near, framed], "the frame caught, though its Card is outside")
      #expect(!items.notes.contains(outsideNote))
      #expect(s.marqueeItems(CGRect(x: 5900, y: -500, width: 1000, height: 1000), in: scope).frames.isEmpty, "a closed frame is not caught")

      let before = (card: s.position(near), note: s.noteRect(s.note(note)!), beside: s.displayRect(near), undos: s.undoStack.count)
      s.moveSelection(cards: items.cards, notes: items.notes, frames: items.frames, by: CGSize(width: 30, height: 40), snap: nil)
      #expect(s.position(near) == CGPoint(x: before.card.x + 30, y: before.card.y + 40))
      #expect(s.noteRect(s.note(note)!).origin == CGPoint(x: before.note.minX + 30, y: before.note.minY + 40))
      #expect(s.displayRect(framed) == CGRect(x: 430, y: 440, width: 300, height: 200), "its Card stays, its size too")
      #expect(s.position(framed) == CGPoint(x: 3000, y: 0))
      #expect(s.displayFrame(near)?["x"]?.double == nil, "beside its Card: it goes with the Card and keeps no place")
      #expect(s.displayRect(near).origin == CGPoint(x: before.beside.minX + 30, y: before.beside.minY + 40), "not moved twice")
      #expect(s.undoStack.count == before.undos + 1, "one edit")
      s.undo()
      #expect(s.position(near) == before.card && s.noteRect(s.note(note)!).origin == before.note.origin)
      #expect(s.displayRect(framed).origin == CGPoint(x: 400, y: 400), "one undo brings all back")

      // With the grid, all go by the same whole grids, so the arrangement stays:
      // the Card goes from where it is drawn, what moves with it by the same step.
      let grid = 20.0
      let offGrid = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 3010, y: 950), name: "offgrid")
      let noteBefore = s.noteRect(s.note(note)!).origin
      s.moveSelection(cards: [offGrid], notes: [note], frames: [framed], by: CGSize(width: 33, height: 47), snap: grid)
      #expect(s.position(offGrid) == CGPoint(x: snapToGrid(3010, grid) + 40, y: snapToGrid(950, grid) + 40))
      #expect(s.noteRect(s.note(note)!).origin == CGPoint(x: noteBefore.x + 40, y: noteBefore.y + 40))
      #expect(s.displayRect(framed).origin == CGPoint(x: 440, y: 440))
      s.undo()
      // Without a Card there is nothing to keep to the grid: they land where dropped.
      s.moveSelection(cards: [], notes: [note], frames: [framed], by: CGSize(width: 33, height: 47), snap: grid)
      #expect(s.noteRect(s.note(note)!).origin == CGPoint(x: noteBefore.x + 33, y: noteBefore.y + 47))
      #expect(s.displayRect(framed).origin == CGPoint(x: 433, y: 447))
      s.undo()

      // A frame alone is closed and its Card stays; notes go; one undo.
      let undos = s.undoStack.count
      s.deleteSelection(cards: [], notes: [note], frames: [framed], in: scope)
      #expect(s.displayFrame(framed)?["hidden"]?.bool == true && s.instance(framed) != nil)
      #expect(s.note(note) == nil && s.undoStack.count == undos + 1)
      s.undo()
      #expect(s.note(note) != nil && s.displayFrame(framed)?["hidden"]?.bool != true)

      // A Card takes its frame with it. A frame beside its Card is closed with
      // its place and size, which the core wants: the document still saves.
      s.deleteSelection(cards: [near], notes: [], frames: [near, beside], in: scope)
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
    let daemon = try Daemon(project: dir)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
    let rpc = RPCClient(endpoint: daemon.endpoint.url)
    _ = try await rpc.connect(token: daemon.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    let opened = try await rpc.open(viewId: UUID())
    let marker = UUID()
    // A final saved change is the barrier: every run notification before it
    // must be delivered in order, including when replies arrive concurrently.
    let received = Task {
      var sequences: [UInt64] = []
      for await event in rpc.notifications {
        sequences.append(event.seq)
        if let patch = event.payload.value as? Tsugumi_RPC_DocPatchedT,
           patch.delta.opId == marker.uuidString.lowercased() { return (sequences, true) }
      }
      return (sequences, false)
    }
    let deadline = Task {
      do { try await Task.sleep(for: .seconds(30)); received.cancel() } catch {}
    }
    defer { deadline.cancel(); received.cancel() }
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
                                   changes: [Change.setProject(project.serialized())])
    for _ in 0..<20 {
      let accepted = try await rpc.run(baseRev: saved.rev ?? 0, opId: UUID(), card: ["start"], propagate: true)
      var status = try await rpc.runStatus(runId: accepted.run.runId)
      for _ in 0..<200 where status.endedAt == nil {
        try await Task.sleep(for: .milliseconds(5))
        status = try await rpc.runStatus(runId: accepted.run.runId)
      }
      try #require(status.state == .succeeded)
    }
    _ = try await rpc.save(baseRev: saved.rev ?? 0, opId: marker, origin: "ui",
                            changes: [Change.setLayout("{\"cards\":{\"b\":{\"x\":1,\"y\":2}}}")])
    let (sequences, reachedMarker) = await received.value
    await rpc.close()
    #expect(reachedMarker, "The saved-change barrier must arrive after the run notifications")
    #expect(sequences.count > 200)
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
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let b = key(["b"])
    s.deleteCards([key(["a"]), key(["c"])], in: [])
    try await s.connect(from: (key(["start"]), "out"), to: (b, "in"), in: [])
    s.updateDisplayFrame(b) { $0["width"] = .real(300); $0["height"] = .real(200) }
    await s.save()
    try await s.run(card: ["start"], propagate: true)
    try await Self.waitForSuccessfulRun(s)
    for _ in 0..<200 where s.displays[b] == nil { try await Task.sleep(for: .milliseconds(25)) }
    let run = s.displays[b]?.runId
    #expect(run != nil)
    try await s.renameCard(b, to: "同じ名前")
    #expect(s.displayName(b) == "同じ名前")
    #expect(s.nodeData(b).title == "同じ名前")
    #expect(s.nodeData(b, meta: .object([("name", .string("Preview"))])).title == "Preview")
    #expect(s.nodeData(b, meta: .object([("name", .string(""))])).title == b)
    #expect(s.displays[b]?.runId == run && s.displayFrame(b)?["width"]?.double == 300)
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
    s.undo()
    await s.save()
    for _ in 0..<200 where s.displays[b] == nil { try await Task.sleep(for: .milliseconds(25)) }
    #expect(s.displays[b]?.runId == run)
  }

  /// A new Card put on the canvas covers nothing: not a Card, a frame or a note,
  /// also when several are put in the same place. A free place is taken as it is.
  @Test func aNewCardCoversNothing() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    func covers(_ k: CardKey, in scope: [String]) -> [String] {
      let r = CGRect(origin: s.placedPosition(k), size: s.geometry(k).size)
      var hit: [String] = []
      for c in s.cards(in: scope).compactMap({ $0["id"]?.string }).map({ key(scope + [$0]) }) where c != k {
        if r.intersects(CGRect(origin: s.placedPosition(c), size: s.geometry(c).size)) { hit.append(c) }
      }
      for c in s.framedCards(in: scope) where c != k && r.intersects(s.displayRect(c, origin: s.placedPosition(c))) { hit.append("frame \(c)") }
      for n in s.notes(in: scope) where r.intersects(s.noteRect(n)) { hit.append("note") }
      return hit
    }
    func check(in scope: [String]) async throws {
      let here = CGPoint(x: 4000, y: 4000)
      let first = try await s.addCard(template: .draft, in: scope, at: here, clearOfOthers: true)
      #expect(s.placedPosition(first).x.distance(to: here.x).magnitude < 40, "a free place is taken")
      let second = try await s.addCard(template: .draft, in: scope, at: here, clearOfOthers: true)
      let third = try await s.addCard(template: .python, in: scope, at: here, clearOfOthers: true)
      // Near a frame and a note too.
      let framed = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 9000, y: 0))
      s.updateDisplayFrame(framed) {
        $0["x"] = .real(6000)
        $0["y"] = .real(6000)
        $0["width"] = .real(320)
        $0["height"] = .real(170)
      }
      let onFrame = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 6100, y: 6050), clearOfOthers: true)
      _ = s.addNote(in: scope, at: CGPoint(x: 8000, y: 8000))
      let onNote = try await s.addCard(template: .draft, in: scope, at: CGPoint(x: 8050, y: 8020), clearOfOthers: true)
      for k in [first, second, third, onFrame, onNote] { #expect(covers(k, in: scope).isEmpty, "\(k): \(covers(k, in: scope))") }
      // Aimed at a spot without asking for room: stays there (a drop).
      let dropped = try await s.addCard(template: .draft, in: scope, at: here)
      #expect(s.position(dropped) == here)
    }
    try await check(in: [])
    let inner = await s.makeSubgraph([key(["a"])], in: [])
    try await check(in: path(try #require(inner)))
    await s.save()
    #expect(!s.dirty, "\(s.saveError ?? "")")
  }

  /// A deleted Card takes its files with it: saving the removal works, for a
  /// Card with files on disk and for one whose files were just written.
  @Test func deletingACardWithFilesSaves() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let code = try await s.addCard(template: .python, in: [], at: .zero, name: "code")
    await s.save()
    #expect(!s.dirty, "\(s.saveError ?? "")")
    let folder = dir.appendingPathComponent("definitions/code")
    #expect(FileManager.default.fileExists(atPath: folder.appendingPathComponent("_outputs.fbs").path))

    s.deleteCards([code], in: [])
    await s.save()
    #expect(!s.dirty && s.saveError == nil, "\(s.saveError ?? "")")
    #expect(!FileManager.default.fileExists(atPath: folder.path))

    // Undone, it is back with its files; deleted with an unsaved edit, it goes too.
    s.undo()
    await s.save()
    #expect(!s.dirty && s.saveError == nil, "\(s.saveError ?? "")")
    #expect(s.instance(code) != nil)
    s.putSource("definitions/code/extra.txt", "unsaved")
    s.deleteCards([code], in: [])
    await s.save()
    #expect(!s.dirty && s.saveError == nil, "\(s.saveError ?? "")")
    #expect(!FileManager.default.fileExists(atPath: folder.path))
  }

  /// A Draft's wires make its ports and take them away, and what is written on
  /// its Card is kept, under a new name too.
  @Test func commentsAreSavedOnTheirCardWithWhoDidWhat() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let k = try await s.addCard(template: .python, in: [], at: .zero, name: "plot")
    #expect(s.comments(k).isEmpty && s.nodeData(k).openComments == 0)

    s.addComment(k, "  y 軸を対数に  ")
    s.addComment(k, "   ")
    #expect(s.comments(k).map(\.text) == ["y 軸を対数に"])
    s.addComment(k, "軸に名前を")
    let first = s.comments(k)[0].id
    s.edit { $0.setCommentText(k, id: first, text: "両軸を対数に") }
    s.edit { $0.setCommentText(k, id: first, text: "  ") }
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
    #expect(s.comments(k)[0].resolution == "両軸を対数にした" && s.openComments(k).map(\.text) == ["軸に名前を"])
    s.setCommentResolved(k, id: first, false)
    #expect(s.comments(k)[0].resolved == false && s.comments(k)[0].resolution == nil)

    #expect(s.comments(k)[0].stateBy == "user" && s.comments(k)[0].editedBy == "user" && s.comments(k)[1].author == "user")
    // A Draft takes comments as any Card.
    let plan = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 0, y: 200), name: "plan")
    s.addComment(plan, "単位を考える")
    #expect(s.cardsWithOpenComments(in: []).contains(plan))
    // A copy keeps what the Card says, comments too, as comments of its own.
    let copy = await s.duplicate([k], in: [])
    #expect(copy.count == 1 && s.comments(copy.first!).map(\.text) == s.comments(k).map(\.text))
    #expect(s.comments(copy.first!)[0].id != s.comments(k)[0].id)
    // What the core recorded of the Agent reads back, in the words the screen uses.
    let agent = CardComment.list(in: try JSONValue.parse(#"{"comments":[{"id":"a","text":"t","author":"agent","resolved":true,"stateBy":"agent","cards":["p","q"]}]}"#))
    #expect(agent[0].cards == ["p", "q"] && agent[0].author == "agent")
    #expect(CardComment.change("resolved", by: "agent").contains("Agent") && !CardComment.change("removed", by: nil).contains("（"))

    for c in s.comments(k) { s.removeComment(k, id: c.id) }
    #expect(s.cardMeta(k) == nil, "no comments left, no _meta.json made for them")
    s.undo()
    #expect(s.comments(k).map(\.text) == ["軸に名前を"])
  }

  @Test func aDraftIsWiredAndWrittenOnTheCanvas() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
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
    #expect(wires() == ["p.out>q.in"])
    #expect(ports(a) == [[], ["out"]] && ports(b) == [["in"], []])
    let c = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 600, y: 0), name: "r")
    try await s.connect(from: (a, CardGeometry.newPort), to: (c, CardGeometry.newPort), in: [])
    try await s.connect(from: (c, CardGeometry.newPort), to: (b, CardGeometry.newPort), in: [])
    #expect(ports(a) == [[], ["out", "out2"]] && ports(b) == [["in", "in2"], []])
    #expect(s.connections(in: []).count == 3, "no fork, nothing replaced")

    // The core agrees (no unknown_port), and takes the ports away with the wires.
    await s.save()
    #expect(!s.dirty, "\(s.saveError ?? "")")
    for k in [a, b, c] {
      #expect(!(s.resolution[k]?.diagnostics ?? []).contains { $0.message.contains("port") }, "\(k)")
    }
    s.removeConnection(at: 0, in: [])
    #expect(ports(a) == [[], ["out2"]] && ports(b) == [["in2"], []])
    s.disconnect(c, inputs: false, in: [])
    #expect(ports(b) == [[], []])

    s.startDraftEdit(a)
    s.inlineDraft?.content = "count words"
    s.inlineDraft?.name = "renamed"
    #expect(CardGeometry(s.nodeData(a), flow: .topToBottom).draftRow == CardGeometry.draftEditRow)
    let to = await s.finishDraftEdit()
    #expect(to == a && s.inlineDraft == nil)
    // The new name arrives with the saved state.
    for _ in 0..<200 where s.cardMeta(a) == nil { try await Task.sleep(for: .milliseconds(25)) }
    #expect(s.cardMeta(a)?["content"]?.string == "count words")
    #expect(s.nodeData(a).summary == "count words")

    // No name is fine: the Card shows its content only, and keeps the id made for it.
    let plain = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 0, y: 300))
    #expect(s.draftUntitled(plain) && s.nodeData(plain).untitled)
    s.startDraftEdit(plain)
    #expect(s.inlineDraft?.name == "" && s.draftNameProblem(plain, "") == nil)
    let same = await s.finishDraftEdit()
    #expect(same == nil && s.instance(plain) != nil)

    // A name the Card cannot take is reported and not kept.
    s.startDraftEdit(b)
    s.inlineDraft?.name = "日本語"
    #expect(s.cardNameProblem(b, "日本語") != nil)
    let kept = await s.finishDraftEdit()
    #expect(kept == nil && s.notice != nil && s.instance(b) != nil)
    await s.close()
  }

  @Test func agentChangesAndUIEditsBothStay() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let agent = RPCClient(endpoint: d.endpoint.url)
    _ = try await agent.connect(token: d.endpoint.token, clientId: UUID(), expectedProjectId: nil)

    // The Agent wires Cards while the UI moves them (each move is saved at once).
    let wiring = Task { for id in ["a", "b", "c"] { try await Self.agentConnect(agent, to: id); try await Task.sleep(for: .milliseconds(15)) } }
    for i in 0..<30 {
      s.edit { $0.setPosition(i % 2 == 0 ? "a" : "b", CGPoint(x: Double(i * 10), y: 5)) }
      try await Task.sleep(for: .milliseconds(5))
    }
    try await wiring.value
    await s.save()
    for _ in 0..<100 where s.connections(in: []).count < 3 || s.dirty { try await Task.sleep(for: .milliseconds(20)) }

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
    s.undo()
    await s.save()
    #expect(s.confirmedLayout["cards"]?["b"]?["x"]?.double == 270)
    #expect(Set(s.connections(in: []).compactMap { $0["to"]?["card"]?.string }) == ["a", "b", "c"])
  }

  /// delegate: each helper's bird writes the Cards it owns; a Card no helper
  /// owns is the main Agent's.
  @Test func helpersSavesAreDrawnByTheirOwnBirds() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
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
    _ = try await agent.connect(token: d.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    for id in ["b", "c"] {
      let snap = try await agent.inspect()
      _ = try await agent.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent",
                               changes: [Change.putDefinition(id: id, cardJSON: #"{"language":"bash","inputs":[{"name":"in"}],"maxRuns":3}"#, metaJSON: nil)])
    }
    for _ in 0..<100 where !s.presence.beats.contains(where: { $0.beat == .write("c", reveal: false) }) {
      try await Task.sleep(for: .milliseconds(20))
    }
    #expect(s.helperPresences[1].beats.contains { $0.beat == .write("b", reveal: false) })
    #expect(!s.presence.beats.contains { $0.beat == .write("b", reveal: false) })
    #expect(s.presence.beats.contains { $0.beat == .write("c", reveal: false) })
    #expect(s.presence(forCard: "a") === s.helperPresences[0] && s.presence(forCard: "c") === s.presence)
    await s.close()
  }

  /// One wire per output: a second wire from an output goes through a fork
  /// Card inserted for it; more wires become more branches of that fork.
  @Test func aSecondWireFromAnOutputGoesThroughAFork() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    func wires() -> Set<String> {
      Set(s.connections(in: []).compactMap { c in
        guard let f = c["from"]?["card"]?.string, let fp = c["from"]?["port"]?.string, let t = c["to"]?["card"]?.string else { return nil }
        return "\(f).\(fp)>\(t)"
      })
    }
    try await s.connect(from: ("start", "out"), to: ("a", "in"), in: [])
    #expect(wires() == ["start.out>a"])
    try await s.connect(from: ("start", "out"), to: ("b", "in"), in: [])
    #expect(wires() == ["start.out>fork", "fork.a>a", "fork.b>b"])
    #expect(s.kind("fork") == .fork)
    // From the trigger again, or from the fork: one more branch, no second fork.
    try await s.connect(from: ("start", "out"), to: ("c", "in"), in: [])
    #expect(wires() == ["start.out>fork", "fork.a>a", "fork.b>b", "fork.c>c"])
    #expect(s.cards(in: []).count == 5)
    await s.save()
    for _ in 0..<100 where s.dirty { try await Task.sleep(for: .milliseconds(20)) }
    // The core agrees: no output has two wires.
    let snap = try await s.rpc!.inspect()
    let codes = snap.resolution.compactMap { $0 }.flatMap { $0.diagnostics.compactMap { $0?.code } }
    #expect(!codes.contains("fan_out"), "\(codes)")
    await s.close()
  }

  @Test func aCardsSummaryIsInItsMetaWhateverItsKind() async throws {
    let dir = try Self.project(), other = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir); try? FileManager.default.removeItem(at: other) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    func written(_ id: String) throws -> String? {
      let url = dir.appendingPathComponent("definitions/\(id)/_meta.json")
      guard FileManager.default.fileExists(atPath: url.path) else { return nil }
      return try JSONValue.parse(String(decoding: Data(contentsOf: url), as: UTF8.self))["description"]?.string
    }

    let code = CardEditorModel(session: s, cardKey: "a")
    code.meta = .object([("description", .string("logs the value"))])
    #expect(await code.apply())
    #expect(try written("a") == "logs the value")
    #expect(s.nodeData("a").summary == "logs the value")
    #expect(s.cardDescription("a") == "logs the value")
    #expect(s.cardDefinition("a")?["description"] == nil)

    // A Card in Card has no _card.json, and its summary is saved all the same.
    let k = try await s.importProject(from: other, in: [], at: CGPoint(x: 400, y: 0))
    await s.save()
    #expect(s.saveError == nil)
    let name = path(k).last!
    let link = CardEditorModel(session: s, cardKey: k)
    link.meta = .object([("description", .string("the imported graph"))])
    #expect(await link.apply())
    #expect(try written(name) == "the imported graph")
    #expect(s.nodeData(k).summary == "the imported graph")
    await s.close()
  }

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
  @Test func staleSettingsApplyNeedsAnExplicitRevisionChoice() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let other = RPCClient(endpoint: d.endpoint.url)
    _ = try await other.connect(token: d.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    for projectDocument in [false, true] {
      let editor = RevisionEdit()
      editor.reload(s.rev)
      func agentSave(_ value: String) async throws {
        let snap = try await other.inspect()
        var project = try JSONValue.parse(snap.projectJson)
        project["name"] = .string(value)
        _ = try await other.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent", changes: projectDocument
          ? [Change.setProject(project.serialized())]
          : [Change.putDefinition(id: "a", cardJSON: #"{"template":"draft","symbol":""# + value + #""}"#, metaJSON: nil)])
        for _ in 0..<100 where s.rev <= (snap.rev ?? 0) { try await Task.sleep(for: .milliseconds(20)) }
        #expect(s.rev > (snap.rev ?? 0))
      }
      func apply(_ overwrite: Bool = false) async -> Bool {
        await editor.apply(s, overwrite: overwrite) { x in
          if projectDocument { x.project["name"] = .string("mine") }
          else { x.setCardDefinition("a", DefinitionDraft(card: .object([("template", .string("draft")), ("symbol", .string("mine"))]), meta: nil)) }
        }
      }
      try await agentSave("newer")
      #expect(!(await apply()))
      #expect(editor.conflictRev == s.rev)
      #expect(!s.dirty)
      #expect(!(await apply()))
      try await agentSave("newest")
      #expect(!(await apply(true)))
      #expect(!s.dirty)
      #expect(await apply(true))
      #expect(!s.dirty)
      #expect(projectDocument ? s.project["name"]?.string == "mine" : s.cardDefinition("a")?["symbol"]?.string == "mine")
    }
    await other.close()
    await s.close()
  }

  @Test func cardEditorSourceConflictsNeedAnExplicitChoice() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let other = RPCClient(endpoint: d.endpoint.url)
    _ = try await other.connect(token: d.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    for removing in [false, true] {
      let rel = removing ? "remove.txt" : "create.txt"
      let p = "definitions/a/" + rel
      func agentSave(_ text: String) async throws {
        let snap = try await other.inspect()
        let hash = snap.sources.compactMap { $0 }.first { $0.path == p }?.hash
        _ = try await other.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent", changes: [
          Change.putSource(path: p, bytes: Data(text.utf8), baseHash: hash),
        ])
        for _ in 0..<100 where s.rev <= (snap.rev ?? 0) { try await Task.sleep(for: .milliseconds(20)) }
        #expect(s.rev > (snap.rev ?? 0))
      }
      if removing { try await agentSave("base") }
      let model = CardEditorModel(session: s, cardKey: "a")
      if removing { model.removeFile(p) } else { model.createFile(rel, text: "mine") }
      try await agentSave("agent")
      #expect(!(await model.apply()))
      #expect(model.conflicts == [rel])
      #expect(!s.dirty)
      #expect(await model.apply(overwrite: true))
      #expect(!s.dirty)
      if removing { #expect(!FileManager.default.fileExists(atPath: dir.appendingPathComponent(p).path)) }
      else { #expect(try String(contentsOf: dir.appendingPathComponent(p), encoding: .utf8) == "mine") }
    }
    await other.close()
    await s.close()
  }

  @Test func cardEditorFollowsUntouchedSettings() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = withTestCardIds(ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil)))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let m = CardEditorModel(session: s, cardKey: "a")
    m.definition?["symbol"] = .string("mine")
    // Another client changes another setting and the same one.
    let other = RPCClient(endpoint: d.endpoint.url)
    _ = try await other.connect(token: d.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    let snap = try await other.inspect()
    _ = try await other.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent", changes: [
      Change.putDefinition(id: "a", cardJSON: #"{"language":"bash","inputs":[{"name":"in"}],"maxRuns":3,"symbol":"theirs"}"#, metaJSON: nil),
    ])
    for _ in 0..<100 where s.cardDefinition("a")?["maxRuns"] == nil { try await Task.sleep(for: .milliseconds(20)) }
    m.follow()
    #expect(m.definition?["maxRuns"]?.uint64 == 3)        // untouched: follows
    #expect(m.definition?["symbol"]?.string == "mine") // touched here: kept
    #expect(m.conflicts == ["_card.json: symbol"])
    #expect(!(await m.apply()))
    #expect(s.confirmedDefinitions["a"]?["symbol"]?.string == "theirs")
    #expect(await m.apply(overwrite: true))
    #expect(s.confirmedDefinitions["a"]?["symbol"]?.string == "mine")
    #expect(s.confirmedDefinitions["a"]?["maxRuns"]?.uint64 == 3)
  }

  /// Another project taken in as a Card in Card is a copy of its own: its
  /// Cards are edited from inside, and its definitions are copied as new Cards.
  @Test func importedProjectIsEditedInside() async throws {
    let dir = try Self.project()
    let other = try Self.project()
    try FileManager.default.createDirectory(at: other.appendingPathComponent(".tsugumi"), withIntermediateDirectories: true)
    try Data("history".utf8).write(to: other.appendingPathComponent(".tsugumi/runs.sqlite"))
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir); try? FileManager.default.removeItem(at: other) }
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
    for _ in 0..<100 where s.cards(in: [name]).isEmpty { try await Task.sleep(for: .milliseconds(20)) }
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
    for _ in 0..<100 where s.resolution[added] == nil { try await Task.sleep(for: .milliseconds(20)) }
    #expect(s.resolution[added] != nil)

    // A definition of the external, copied into this project as a new Card.
    let copy = await s.placeDefinitionCopy(prefix: "externals/\(name)/", id: "c", in: [], at: .zero)
    await s.save()
    #expect(copy == "c-2")
    #expect(s.confirmedDefinitions["c-2"]?["language"]?.string == "bash")

    // Undo of the copy, then of the inner move, one operation each.
    s.undo()
    await s.save()
    #expect(s.confirmedDefinitions["c-2"] == nil && s.instance("c-2") == nil)
  }
}

extension SyncTests {
  @Test func deletingAWireEndWhileAllocatingAForkDoesNotAddTheWire() async throws {
    let dir = try Self.project()
    let daemon = try Daemon(project: dir)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await Self.connected(s)
    try await s.connect(from: ("start", "out"), to: ("a", "in"), in: [])
    await s.save()
    var allocation: CheckedContinuation<String, Never>?
    s.cardIdProvider = { _, _ in await withCheckedContinuation { allocation = $0 } }
    let connecting = Task { try await s.connect(from: ("start", "out"), to: ("b", "in"), in: []) }
    while allocation == nil { await Task.yield() }
    s.deleteCards(["b"], in: [])
    allocation?.resume(returning: "fork-delayed")
    try await connecting.value
    #expect(s.instance("fork-delayed") == nil)
    #expect(!s.connections(in: []).contains { $0["to"]?["card"]?.string == "b" })
    await s.close()
  }

  @Test func cardEditorKeepsTypingAfterSendingSettingsAndSources() async throws {
    let dir = try Self.project()
    let p = "definitions/a/later.txt"
    try "base".write(to: dir.appendingPathComponent(p), atomically: true, encoding: .utf8)
    let daemon = try Daemon(project: dir)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await Self.connected(s)
    let model = CardEditorModel(session: s, cardKey: "a")
    _ = try await model.text(p)
    model.texts[p] = "sent"
    model.definition?["symbol"] = .string("sent")
    var finished = false
    let saving = Task { let ok = await model.apply(); finished = true; return ok }
    while !model.applying && !finished { await Task.yield() }
    #expect(model.applying)
    model.texts[p] = "later"
    model.definition?["symbol"] = .string("later")
    model.createFile("new.txt", text: "new")
    #expect(await saving.value)
    #expect(model.loadedText(p) == "later" && model.isEdited(p) && model.unapplied)
    #expect(model.definition?["symbol"]?.string == "later")
    #expect(model.created.contains("definitions/a/new.txt"))
    #expect(try String(contentsOf: dir.appendingPathComponent(p), encoding: .utf8) == "sent")
    #expect(s.cardDefinition("a")?["symbol"]?.string == "sent")
    #expect(await model.apply())
    #expect(!model.unapplied)
    #expect(try String(contentsOf: dir.appendingPathComponent(p), encoding: .utf8) == "later")
    await s.close()
  }

  @Test func inlineValueKeepsItsOpeningRevisionAndSkipsAnUnchangedBlur() async throws {
    let dir = try Self.project()
    let daemon = try Daemon(project: dir)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = daemon.endpoint
    s.start()
    try await Self.connected(s)
    let edit = InlineParamEdit()
    edit.begin("1", rev: s.rev)
    let other = RPCClient(endpoint: daemon.endpoint.url)
    _ = try await other.connect(token: daemon.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    var project = s.project
    project["name"] = .string("newer")
    _ = try await other.save(baseRev: s.rev, opId: UUID(), origin: "ui", changes: [Change.setProject(project.serialized())])
    for _ in 0..<100 where s.rev == edit.revision.baseRev { try await Task.sleep(for: .milliseconds(20)) }
    #expect(s.rev > edit.revision.baseRev)
    var wrote = false
    #expect(await edit.commit(s, text: "1") { _ in wrote = true })
    #expect(!wrote)
    #expect(!(await edit.commit(s, text: "3") { $0.project["name"] = .string("stale") }))
    #expect(edit.revision.conflictRev == s.rev && !s.dirty)
    #expect(s.project["name"]?.string == "newer")
    await other.close()
    await s.close()
  }

  @Test func reloadingExternalPermissionWideningCanBeDeclinedOrApproved() async throws {
    let dir = try Self.project()
    let daemon = try Daemon(project: dir)
    defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
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
    await s.answerReloadApproval(false)
    #expect(s.rev == base && s.reloadApproval == nil)
    await s.reloadExternal()
    #expect(s.reloadApproval != nil)
    await s.answerReloadApproval(true)
    #expect(s.saveError == nil && s.reloadApproval == nil)
    for _ in 0..<100 where s.rev == base { try await Task.sleep(for: .milliseconds(20)) }
    #expect(s.cardDefinition("a")?["network"]?.bool == true)
    await s.close()
  }
}
