import Darwin
import Foundation
import Testing
import TsugumiAgentClient
import TsugumiTestSupport
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

/// Native RPC smoke tests with a real project daemon.
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

  static let sinkCard = #"{"type":"code","language":"bash","inputs":[{"name":"in"}]}"#
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

  /// A project with a trigger and a few Bash sinks (no connections yet).
  static func project() throws -> URL {
    let p = testTemporaryDirectory.appendingPathComponent("sync-\(UUID().uuidString.prefix(8))")
    do {
      let fm = FileManager.default
      let cards = ["a", "b", "c", "start"]
      for id in cards {
        let d = p.appendingPathComponent("definitions/\(id)")
        try fm.createDirectory(at: d, withIntermediateDirectories: true)
        let definition = id == "start" ? #"{"type":"trigger"}"# : sinkCard
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

  static func connected(_ s: ProjectSession) async throws {
    // A failed or abandoned connection is reported, not waited for.
    try await waitForModel("the session connects") { s.connected || s.connectionMessage != nil || s.closed }
    try #require(s.connected, "\(s.connectionMessage ?? "the session closed before it connected")")
    if s.connected, let endpoint = s.endpointOverride {
      // Direct test daemons bypass EnvironmentManager's host registration.
      _ = try await CoreHostClient.shared.call("RegisterProjectRequest", [
        "project_id": endpoint.projectId, "endpoint": endpoint.url.absoluteString,
        "token": endpoint.token, "path": s.identity.url.path])
    }
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

    let a = try await s.addCard(template: .draft, in: [], at: .zero)
    let b = try await s.addCard(template: .draft, in: [], at: CGPoint(x: 300, y: 0), name: "same name")

    try await s.connect(from: (a, CardGeometry.newPort), to: (b, CardGeometry.newPort), in: [])
    #expect(s.connections(in: []).count == 1 && s.portData(a).outputs.isEmpty && s.portData(b).inputs.isEmpty)

    await s.startDraftEdit(a)
    s.inlineDraft?.content = "first Draft"
    s.inlineDraft?.name = "same name"
    // Return/Command-Return and focus loss can submit without awaiting each
    // other. Opening the next Draft must join them before replacing the fields.
    let done = Task { await s.finishDraftEdit() }
    let blurred = Task { await s.finishDraftEdit() }
    await s.startDraftEdit(b)
    _ = await done.value
    _ = await blurred.value
    #expect(s.nodeData(a).summary == "first Draft" && s.displayName(a) == "same name")
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
    try await waitForModel("the session released its connection") {
      guard case .stopped = s.environment else { return false }
      return !s.connected && model.sessions[s.identity.id] == nil
    }
    #expect(w.session == nil)
    #expect(s.inlineDraft == nil && s.cardMeta(b)?["content"]?.string == "before closing")
    let saved = try JSONValue.parse(String(contentsOf: dir.appendingPathComponent("definitions/\(b)/_meta.json"), encoding: .utf8))
    #expect(saved["content"]?.string == "before closing", "Closing waits for the transaction on disk")
    try await checkUnreachableTsugumid(in: dir)
  }

  private func checkUnreachableTsugumid(in dir: URL) async throws {
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = Endpoint(url: URL(string: "ws://127.0.0.1:1/rpc")!, token: "t", projectId: "p")
    s.start()
    try await waitForModel { !(s.connectionMessage == nil) }
    #expect(!s.connected)
    #expect(s.connectionFailed, "\(s.connectionMessage ?? "")")
    await s.close()
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
    var finishes = ended.stream.makeAsyncIterator()
    for _ in 0..<2 {
      let accepted = try await rpc.run(baseRev: opened.snapshot.rev ?? 0, opId: UUID(), card: ["start"], propagate: false)
      let status = await finishes.next()
      try #require(status?.runId == accepted.run.runId && status?.state == .succeeded)
    }
    _ = try await rpc.save(baseRev: opened.snapshot.rev ?? 0, opId: marker, origin: "ui",
                            changes: [Change.setLayout("{\"cards\":{\"b\":{\"x\":1,\"y\":2}}}")])
    let (sequences, reachedMarker) = await received.value
    await rpc.close()
    #expect(reachedMarker, "The saved-change barrier must arrive after the run notifications")
    #expect(sequences.first == (opened.seq ?? 1) + 1)
    let outOfOrder = zip(sequences, sequences.dropFirst()).first { $1 != $0 + 1 }
    #expect(outOfOrder == nil)
  }
}

@MainActor @Test func defaultDisplayRectTransformsTheHostGeometry() throws {
  let dir = try SyncTests.project()
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

@MainActor @Test func reloadingSettingsKeepsTheExactWorkingText() {
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
