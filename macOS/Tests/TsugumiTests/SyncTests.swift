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

    init(project: URL) throws {
      dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("tsg-\(UUID().uuidString.prefix(8))")
      try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
      let build = SyncTests.root.appendingPathComponent("build")
      process.executableURL = build.appendingPathComponent("core/tsugumid")
      process.arguments = ["--project", project.path, "--unix", dir.appendingPathComponent("s").path,
                           "--control-dir", dir.appendingPathComponent("control").path,
                           "--worker", build.appendingPathComponent("sdk/tsugumi-worker").path,
                           "--sdk-dir", SyncTests.root.appendingPathComponent("sdk").path,
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

  /// A project with a trigger and a few loggers (no connections yet).
  static func project() throws -> URL {
    let p = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("sync-\(UUID().uuidString.prefix(8))")
    let fm = FileManager.default
    let cards = ["begin": "trigger", "a": "logger", "b": "logger", "c": "logger"]
    for (id, t) in cards {
      let d = p.appendingPathComponent("definitions/\(id)")
      try fm.createDirectory(at: d, withIntermediateDirectories: true)
      try Data("{\"template\": \"\(t)\"}".utf8).write(to: d.appendingPathComponent("_card.json"))
    }
    let list = cards.keys.sorted().map { "{\"id\": \"\($0)\"}" }.joined(separator: ", ")
    try Data("{\"name\": \"sync\", \"pkgsPath\": \"tsugumi:default\", \"databaseConnections\": [], \"canvas\": {\"cards\": [\(list)], \"connections\": []}}".utf8)
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
      conns.append(.object([("from", .object([("card", .string("begin")), ("port", .string("out"))])),
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

  @Test func agentChangesAndUIEditsBothStay() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
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

  @Test func cardEditorFollowsUntouchedSettings() async throws {
    let dir = try Self.project()
    let d = try Daemon(project: dir)
    defer { d.stop(); try? FileManager.default.removeItem(at: dir) }
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    s.endpointOverride = d.endpoint
    s.start()
    try await Self.connected(s)
    let m = CardEditorModel(session: s, cardKey: "a")
    m.definition?["description"] = .string("mine")
    // Another client changes another setting and the same one.
    let other = RPCClient(endpoint: d.endpoint.url)
    _ = try await other.connect(token: d.endpoint.token, clientId: UUID(), expectedProjectId: nil)
    let snap = try await other.inspect()
    _ = try await other.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "agent", changes: [
      Change.putDefinition(id: "a", cardJSON: "{\"template\": \"logger\", \"maxRuns\": 3, \"description\": \"theirs\"}", metaJSON: nil),
    ])
    for _ in 0..<100 where s.cardDefinition("a")?["maxRuns"] == nil { try await Task.sleep(for: .milliseconds(20)) }
    m.follow()
    #expect(m.definition?["maxRuns"]?.uint64 == 3)        // untouched: follows
    #expect(m.definition?["description"]?.string == "mine") // touched here: kept
    #expect(m.conflicts == ["_card.json: description"])
    #expect(await m.apply())
    #expect(s.confirmedDefinitions["a"]?["description"]?.string == "mine")
    #expect(s.confirmedDefinitions["a"]?["maxRuns"]?.uint64 == 3)
  }
}
