import FlatBuffers
import Foundation
import Testing
import TsugumiAgent
import TsugumiRPC

/// Agent end-to-end with the local model fixture and a real tsugumid over
/// WebSocket. Only the model connection is replaced; every core operation is
/// the real RPC. Requires `build/core/tsugumid` (cmake build) and python3.
struct AgentHarness {
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../..").standardizedFileURL
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-agent-\(UUID().uuidString)")
  var daemon: Process?
  var fixture: Process?
  var fixturePort = 0
  var endpoint: URL?
  var token = ""

  static func freePort() -> Int {
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    defer { close(fd) }
    var a = sockaddr_in()
    a.sin_family = sa_family_t(AF_INET)
    a.sin_addr.s_addr = inet_addr("127.0.0.1")
    var len = socklen_t(MemoryLayout<sockaddr_in>.size)
    _ = withUnsafeMutablePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, len) } }
    _ = withUnsafeMutablePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) } }
    return Int(UInt16(bigEndian: a.sin_port))
  }

  func write(_ rel: String, _ s: String) throws {
    let u = dir.appendingPathComponent("project/\(rel)")
    try FileManager.default.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
    try Data(s.utf8).write(to: u)
  }

  mutating func start(fixtureArgs: [String] = []) throws {
    try write("definitions/trigger/_card.json", #"{"template":"trigger"}"#)
    try write("definitions/source/_card.json", #"{"template":"input","params":[{"name":"text","type":"string","data":"x"}]}"#)
    try write("definitions/source/_outputs.fbs", "namespace demo;\ntable Msg { text:string; }\ntable Outputs { msg:Msg (required); }\nroot_type Outputs;\n")
    try write("definitions/log/_card.json", #"{"template":"logger","canvasEnabled":true}"#)
    try write("tsugumi.json", """
    {"name":"agent test","pkgsPath":"tsugumi:default","databaseConnections":[],
     "canvas":{"cards":[{"id":"begin","definition":"trigger"},{"id":"src","definition":"source"},{"id":"out","definition":"log"}],
               "connections":[{"from":{"card":"src","port":"msg"},"to":{"card":"out","port":"in"}}]}}
    """)
    try write("layout.json", "{}")
    let control = dir.appendingPathComponent("control")
    let d = Process()
    d.executableURL = root.appendingPathComponent("build/core/tsugumid")
    d.arguments = ["--project", dir.appendingPathComponent("project").path, "--listen", "127.0.0.1:0",
                   "--control-dir", control.path, "--allow-missing-origin",
                   "--worker", root.appendingPathComponent("build/sdk/tsugumi-worker").path,
                   "--sdk-dir", root.appendingPathComponent("sdk").path]
    d.standardError = FileHandle.nullDevice
    try d.run()
    daemon = d
    let ready = control.appendingPathComponent("server.ready")
    for _ in 0..<200 where !FileManager.default.fileExists(atPath: ready.path) { usleep(50_000) }
    let obj = try JSONSerialization.jsonObject(with: Data(contentsOf: ready)) as! [String: Any]
    endpoint = URL(string: obj["endpoint"] as! String)
    token = obj["token"] as! String
    fixturePort = Self.freePort()
    let f = Process()
    f.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    f.arguments = ["python3", root.appendingPathComponent("tests/fixtures/model_fixture.py").path, "--port", "\(fixturePort)"] + fixtureArgs
    f.standardOutput = FileHandle.nullDevice
    try f.run()
    fixture = f
    usleep(500_000)
  }

  func stop() {
    daemon?.terminate()
    daemon?.waitUntilExit()
    fixture?.terminate()
    try? FileManager.default.removeItem(at: dir)
  }
}

struct DirectTransport: ToolTransport {
  let client: RPCClient
  let names: [UInt8: String]
  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: FlatBuffers.Table) {
    let (t, table) = try await client.callRaw(payloadType: payloadType, build: build)
    return (names[t] ?? "?", table)
  }
}

final class EventLog: @unchecked Sendable {
  private let lock = NSLock()
  private var events: [AgentEvent] = []
  func add(_ e: AgentEvent) { lock.lock(); events.append(e); lock.unlock() }
  var all: [AgentEvent] { lock.lock(); defer { lock.unlock() }; return events }
  var finished: Bool { all.contains { if case .finished = $0 { return true }; if case .error = $0 { return true }; return false } }
  var text: String { all.compactMap { if case .text(let t) = $0 { return t }; return nil }.joined() }
}

func makeAgent(_ h: AgentHarness, _ client: RPCClient) throws -> Agent {
  let bfbs = try #require(RPCSchema.bfbs())
  let schema = try BFBS(data: bfbs)
  let catalog = try ToolCatalog(schema: schema)
  var names: [UInt8: String] = [:]
  for v in schema.enums.first(where: { $0.name == "Tsugumi.RPC.ReplyPayload" })!.values { names[UInt8(v.value)] = v.name }
  return Agent(catalog: catalog, transport: DirectTransport(client: client, names: names),
               provider: .localFixture(port: h.fixturePort), storage: h.dir.appendingPathComponent("conversation.json"))
}

func waitFinished(_ log: EventLog, seconds: Double = 60) async {
  let deadline = Date().addingTimeInterval(seconds)
  while !log.finished && Date() < deadline { try? await Task.sleep(for: .milliseconds(50)) }
}

@Suite(.serialized) struct AgentFixtureTests {
  @Test func readChangeRunExplain() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    // Tool schemas come from the FBS: 64 bit fields are strings, op_id is Agent-owned.
    let save = try #require(agent.catalog.tool(named: "project_save"))
    let props = save.inputSchema["properties"] as! [String: Any]
    #expect(props["op_id"] == nil && (props["base_rev"] as? [String: Any])?["type"] as? String == "string")
    let log = EventLog()
    await agent.send("set src.text to \"Hello from the agent\" and run, then report out", context: []) { log.add($0) }
    await waitFinished(log)
    #expect(log.text.contains("succeeded"), "\(log.all)")
    let snap = try await client.inspect()
    #expect(snap.projectJson.contains("Hello from the agent"))
    let runs = try await client.history()
    #expect(runs.runs.first??.status.state == .succeeded)
    // The trace is persisted with op_ids for mutating calls.
    let conv = await agent.conversation
    #expect(conv.tools.contains { $0.name == "project_save" && $0.opId != nil && $0.state == .completed })
    await client.close()
  }

  @Test func cancelStopsModelAndUnsentTools() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--slow", "0.2"])
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send("set src.text to cancelled and run", context: []) { log.add($0) }
    try await Task.sleep(for: .milliseconds(400))
    await agent.cancel()
    await waitFinished(log, seconds: 10)
    #expect(log.all.contains { if case .error(let s) = $0 { return s == "cancelled" }; return false }, "\(log.all)")
    let runs = try await client.history()
    #expect(runs.runs.isEmpty)  // the run tool was never sent
    await client.close()
  }

  @Test func retriesAfterDroppedModelConnection() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--drop-once"])
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send("set src.text to retried and run, then report out", context: []) { log.add($0) }
    await waitFinished(log)
    #expect(log.all.contains { if case .retrying = $0 { return true }; return false }, "\(log.all)")
    #expect(log.text.contains("succeeded"))
    // Exactly one save and one run despite the retry.
    let runs = try await client.history()
    #expect(runs.runs.count == 1)
    await client.close()
  }
}

/// Draft from plan to implementation through the Agent: described and wired,
/// saved and reloaded, converted by the Agent into a code Card (types, Nix,
/// sources) that validates; the intent stays in _meta.json. A cancelled
/// generation leaves the plan untouched.
@Suite(.serialized) struct DraftGenerationTests {
  func addDraft(_ client: RPCClient) async throws {
    let snap = try await client.inspect()
    var doc = try JSONValue.parse(snap.projectJson)
    var cards = doc["canvas"]?["cards"]?.array ?? []
    cards.append(.object([("id", .string("plan")), ("definition", .string("plan"))]))
    var conns = doc["canvas"]?["connections"]?.array ?? []
    conns.append(try JSONValue.parse(#"{"from":{"card":"src","port":"msg"},"to":{"card":"plan","port":"data"}}"#))
    var canvas = doc["canvas"]!
    canvas["cards"] = .array(cards)
    canvas["connections"] = .array(conns)
    doc["canvas"] = canvas
    let meta = #"{"description":"Count words","plan":"Split the message text and count the words.","constraints":"No network","done":"summary holds the count","inputs":[{"name":"data","description":"the message"}],"outputs":[{"name":"summary","description":"the word count"}]}"#
    _ = try await client.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "test", changes: [
      Change.putDefinition(id: "plan", cardJSON: #"{"name":"Word count","template":"draft","inputs":[{"name":"data"}]}"#, metaJSON: meta),
      Change.setProject(doc.serialized()),
    ])
  }

  @Test func draftIsPlannedSavedAndImplemented() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    try await addDraft(client)
    // Reload from disk: the plan and its ports come back unchanged.
    let snap0 = try await client.inspect()
    _ = try await client.reload(baseRev: snap0.rev ?? 0, opId: UUID(), origin: "test")
    let planned = try await client.inspect()
    let draftPorts = planned.resolution.compactMap { $0 }.first { $0.card.path == ["plan"] }
    #expect(draftPorts.map { $0.outputs.compactMap { $0 }.map(\.name) } == ["summary"])
    #expect(planned.definitions.compactMap { $0 }.first { $0.id == "plan" }?.cardJson.contains("draft") == true)
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send("Implement these Draft Cards in the root workflow.\n- plan (definition plan): Count words", context: []) { log.add($0) }
    await waitFinished(log, seconds: 120)
    #expect(log.text.contains("Implemented"), "\(log.all)")
    let after = try await client.inspect()
    let def = try #require(after.definitions.compactMap { $0 }.first { $0.id == "plan" })
    let card = try JSONValue.parse(def.cardJson)
    #expect(card["language"]?.string == "bash" && card["template"] == nil)
    #expect(def.metaJson?.contains("the word count") == true && def.metaJson?.contains("Count words") == true)
    #expect(after.sources.compactMap { $0 }.map(\.path).contains("definitions/plan/_outputs.fbs"))
    // Wiring kept; types now come from the implementation.
    let resolved = after.resolution.compactMap { $0 }.first { $0.card.path == ["plan"] }
    #expect(resolved?.inputs.first??.rootType != nil)
    #expect(resolved?.outputs.first??.rootType == "generated.Text")
    let validation = try await client.validate(atRev: after.rev ?? 0, targets: [["plan"]], executable: false)
    #expect(validation.diagnostics.compactMap { $0 }.filter { $0.severity == .error }.isEmpty)
    await client.close()
  }

  @Test func cancelledGenerationKeepsThePlan() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--slow", "0.3"])
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    try await addDraft(client)
    let before = try await client.inspect()
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send("Implement these Draft Cards in the root workflow.\n- plan (definition plan): Count words", context: []) { log.add($0) }
    try await Task.sleep(for: .milliseconds(300))
    await agent.cancel()
    try await Task.sleep(for: .milliseconds(800))
    let after = try await client.inspect()
    #expect(after.rev == before.rev)
    #expect(after.definitions.compactMap { $0 }.first { $0.id == "plan" }?.cardJson.contains("draft") == true)
    await client.close()
  }
}
