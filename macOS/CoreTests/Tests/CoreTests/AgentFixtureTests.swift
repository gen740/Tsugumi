import FlatBuffers
import Foundation
import Testing
import TsugumiAgent
import TsugumiRPC

/// Agent end-to-end with the local model fixture and a real tsugumid over
/// WebSocket. Only the model connection is replaced; every core operation is
/// the real RPC. Requires `build/core/tsugumid` (cmake build) and python3.
struct AgentHarness {
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../..").standardizedFileURL
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
    try write("definitions/start/_card.json", #"{"template":"trigger"}"#)
    try write("definitions/src/_card.json", #"{"template":"input","params":[{"name":"text","type":"string","data":"x"}]}"#)
    try write("definitions/src/_outputs.fbs", "namespace demo;\ntable Msg { text:string; }\ntable Outputs { msg:Msg (required); }\nroot_type Outputs;\n")
    try write("definitions/out/_card.json", #"{"template":"logger"}"#)
    try write("tsugumi.json", """
    {"name":"agent test","pkgsPath":"tsugumi:default","databaseConnections":[],
     "canvas":{"cards":[{"id":"start"},{"id":"src"},{"id":"out"}],
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
    // Python starts slowly on a CI runner: wait until the fixture accepts
    // connections (a fixed pause let the first requests fail there).
    let deadline = Date().addingTimeInterval(30)
    while !Self.accepts(port: fixturePort) && Date() < deadline && f.isRunning { usleep(50_000) }
  }

  /// Something listens on 127.0.0.1:port.
  static func accepts(port: Int) -> Bool {
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    defer { close(fd) }
    var a = sockaddr_in()
    a.sin_family = sa_family_t(AF_INET)
    a.sin_addr.s_addr = inet_addr("127.0.0.1")
    a.sin_port = UInt16(port).bigEndian
    return withUnsafePointer(to: &a) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
    } == 0
  }

  /// Ends the daemon and the fixture, polled with a deadline (waitUntilExit
  /// can miss the exit inside an async test and wait for ever).
  func stop() {
    for p in [daemon, fixture].compactMap({ $0 }) where p.isRunning {
      p.terminate()
      let deadline = Date().addingTimeInterval(10)
      while p.isRunning && Date() < deadline { usleep(20_000) }
      if p.isRunning { kill(p.processIdentifier, SIGKILL) }
    }
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
  var finished: Bool {
    all.contains {
      switch $0 {
      case .finished, .error, .stepLimit: return true
      default: return false
      }
    }
  }
  var text: String { all.compactMap { if case .text(let t) = $0 { return t }; return nil }.joined() }
}

func makeAgent(_ h: AgentHarness, _ client: RPCClient, provider: ProviderConfig? = nil, references: [String: URL] = [:],
               role: Agent.Role = .worker) throws -> Agent {
  let bfbs = try #require(RPCSchema.bfbs())
  let schema = try BFBS(data: bfbs)
  let catalog = try ToolCatalog(schema: schema)
  var names: [UInt8: String] = [:]
  for v in schema.enums.first(where: { $0.name == "Tsugumi.RPC.ReplyPayload" })!.values { names[UInt8(v.value)] = v.name }
  return try Agent(catalog: catalog, transport: DirectTransport(client: client, names: names),
               provider: provider ?? .localFixture(port: h.fixturePort), storage: h.dir.appendingPathComponent("conversation.json"),
               references: references, role: role)
}

final class Asked: @unchecked Sendable {
  private let lock = NSLock()
  private var asked: [[String]] = []
  func add(_ a: [String]) { lock.lock(); asked.append(a); lock.unlock() }
  var all: [[String]] { lock.lock(); defer { lock.unlock() }; return asked }
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
    #expect(log.all.contains { if case .toolInput = $0 { return true }; return false }, "the arguments reach the screen as they are written")
    let snap = try await client.inspect()
    // The value lives in the Card's own definition.
    #expect(snap.definitions.compactMap { $0 }.first { $0.id == "src" }?.cardJson.contains("Hello from the agent") == true)
    let runs = try await client.history()
    #expect(runs.runs.first??.status.state == .succeeded)
    // The trace is persisted with op_ids for mutating calls.
    let conv = await agent.conversation
    #expect(conv.tools.contains { $0.name == "project_save" && $0.opId != nil && $0.state == .completed })
    // run_status waited for the run to end: the model looked at most once.
    #expect(conv.tools.filter { $0.name == "run_status" }.count <= 1, "\(conv.tools.map(\.name))")
    await client.close()
  }

  /// A save widening what a Card may do waits for the user: approved, it is
  /// saved; declined, nothing is, and the model is told so.
  @Test func cardPermissionsWaitForTheUser() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let asked = Asked()
    func network() async throws -> Bool {
      let json = try await client.inspect().definitions.compactMap { $0 }.first { $0.id == "src" }?.cardJson ?? "{}"
      return (try JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any])?["network"] as? Bool == true
    }
    await agent.setApprover { a in asked.add(a); return [] }
    let declined = EventLog()
    await agent.send("give src network access", context: []) { declined.add($0) }
    await waitFinished(declined)
    #expect(asked.all == [["network src"]], "\(asked.all)")
    #expect(try await network() == false)
    #expect(declined.text.contains("declined"), "\(declined.all)")
    await agent.setApprover { a in asked.add(a); return Set(a) }
    let approved = EventLog()
    await agent.send("give src network access", context: []) { approved.add($0) }
    await waitFinished(approved)
    #expect(try await network() == true, "\(approved.all)")
    await client.close()
  }

  /// The same conversation over the OpenAI Responses API wire format, with the
  /// key read from the Keychain (a test account, removed afterwards).
  @Test func openAIResponsesReadChangeRun() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let account = "Tsugumi test OpenAI \(UUID().uuidString)"
    try Keychain.save("fixture-key", account: account)
    defer { Keychain.delete(account: account) }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let config = ProviderConfig(name: account, baseURL: URL(string: "http://127.0.0.1:\(h.fixturePort)")!, model: "fixture",
                                auth: .apiKey, api: .openAIResponses)
    let agent = try makeAgent(h, client, provider: config)
    let log = EventLog()
    await agent.send("set src.text to \"Hello over Responses\" and run, then report out", context: []) { log.add($0) }
    await waitFinished(log)
    #expect(log.text.contains("succeeded"), "\(log.all)")
    #expect(log.all.contains { if case .toolInput = $0 { return true }; return false }, "the arguments reach the screen as they are written")
    #expect(try await client.inspect().definitions.compactMap { $0 }.first { $0.id == "src" }?.cardJson.contains("Hello over Responses") == true)
    #expect(Keychain.load(account: account) == "fixture-key")
    Keychain.delete(account: account)
    #expect(Keychain.load(account: account) == nil)
    await client.close()
  }

  /// Sign in with ChatGPT against the fixture's issuer, then the same
  /// conversation on the ChatGPT plan: the browser is played by following the
  /// authorize redirect to the loopback callback; the ID token is verified
  /// with the issuer's JWKS; every request follows the plan's rules (the
  /// fixture refuses others); access tokens that live one second are
  /// refreshed (rotating the refresh token) before each request; the usage
  /// limit stops the turn without retries; signing out revokes the session.
  @Test func chatGPTPlanSignInRunRefreshLimitSignOut() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--chatgpt-expires", "1"])
    defer { h.stop() }
    let issuer = "http://127.0.0.1:\(h.fixturePort)"
    let store = "Tsugumi test ChatGPT \(UUID().uuidString)"
    defer { Keychain.delete(account: store) }
    let auth = ChatGPTAuth.shared(endpoints: ChatGPTEndpoints(issuer: issuer, resource: "https://api.openai.com/v1"), store: store)
    let browser = URLSession(configuration: .ephemeral)
    let signedIn = try await auth.signIn { url in _ = try await browser.data(from: url) }
    #expect(signedIn.welcome && signedIn.account.planEnabled && signedIn.account.email == "fixture@example.com")
    #expect(signedIn.account.clientId == "oaiapp_fixture_1")
    try await auth.markWelcomed(clientId: signedIn.account.clientId)

    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    var config = ProviderConfig(name: "ChatGPT", baseURL: URL(string: issuer)!, model: "fixture", auth: .chatGPT, api: .openAIResponses)
    config.chatGPTEndpoints = ChatGPTEndpoints(issuer: issuer, resource: "https://api.openai.com/v1")
    config.chatGPTStore = store
    let models = try await MessagesProvider(config: config).modelChoices()
    #expect(models.map(\.id) == ["fixture"] && models.first?.label == "Fixture model")
    // The model menu's read: the same list, with the record read without asking.
    #expect(try await MessagesProvider(config: config).modelChoices(asking: false).map(\.id) == ["fixture"])
    #expect(await auth.accountsWithoutAsking()?.map(\.signedIn) == [true])
    let agent = try makeAgent(h, client, provider: config)
    let log = EventLog()
    await agent.send("set src.text to \"Hello on the plan\" and run, then report out", context: []) { log.add($0) }
    await waitFinished(log)
    #expect(log.text.contains("succeeded"), "\(log.all)")
    #expect(try await client.inspect().definitions.compactMap { $0 }.first { $0.id == "src" }?.cardJson.contains("Hello on the plan") == true)
    let stats = try JSONSerialization.jsonObject(with: try await browser.data(from: URL(string: "\(issuer)/fixture/chatgpt")!).0) as! [String: Any]
    #expect((stats["violations"] as? [String])?.isEmpty == true, "\(stats)")
    #expect((stats["refreshes"] as? Int ?? 0) >= 3, "\(stats)")
    let authorize = (stats["authorize"] as? [[String: Any]])?.first
    #expect(authorize?["client_id"] as? String == "dynamic_agent_client" && authorize?["agent_name_hint"] as? String == "Tsugumi")
    #expect((authorize?["ext_agent_host_id"] as? String)?.hasPrefix("urn:uuid:") == true)

    let limited = EventLog()
    await agent.send("now exceed the usage limit", context: []) { limited.add($0) }
    await waitFinished(limited)
    #expect(limited.all.contains { if case .error(let s) = $0 { return s == ChatGPTError.usageLimitText }; return false }, "\(limited.all)")
    #expect(!limited.all.contains { if case .retrying = $0 { return true }; return false })

    // Signing in again to the saved registration keeps its client and host.
    _ = try await auth.signIn(clientId: signedIn.account.clientId) { url in _ = try await browser.data(from: url) }
    let again = (try JSONSerialization.jsonObject(with: try await browser.data(from: URL(string: "\(issuer)/fixture/chatgpt")!).0) as! [String: Any])["authorize"] as! [[String: Any]]
    #expect(again.last?["client_id"] as? String == "oaiapp_fixture_1" && again.last?["agent_name_hint"] == nil)
    #expect(again.last?["ext_agent_host_id"] as? String == authorize?["ext_agent_host_id"] as? String)
    #expect(again.last?["id_token_hint"] as? Bool == true)

    #expect(try await auth.signOut(clientId: signedIn.account.clientId))
    #expect(await auth.accounts().map(\.signedIn) == [false])
    await #expect(throws: ChatGPTError.self) { _ = try await auth.accessToken() }
    // Nothing of the tokens is left after signing out.
    let record = try #require(Keychain.load(account: store))
    #expect(!record.contains("chatgpt-access-") && !record.contains("chatgpt-refresh-"))
    await client.close()
  }

  /// The model menu lists models with a key it read without asking: from an
  /// item this program saved; an item another program saved would need the
  /// keychain's dialog, so the listing fails at once instead of showing it.
  @Test func modelListsNeverAskForTheKeychain() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let port = URL(string: "http://127.0.0.1:\(h.fixturePort)")!
    let own = "Tsugumi test own \(UUID().uuidString)", other = "Tsugumi test other \(UUID().uuidString)"
    defer {
      Keychain.delete(account: own)
      Keychain.delete(account: other)
    }
    try Keychain.save("fixture-key", account: own)
    #expect(Keychain.loadWithoutAsking(account: own) == "fixture-key")
    let listed = try await MessagesProvider(config: ProviderConfig(name: own, baseURL: port, model: "fixture", auth: .apiKey, api: .openAIResponses))
      .modelChoices(asking: false)
    #expect(listed.map(\.id) == ["fixture"])

    // Saved by `security`: only that program may read it without a dialog.
    let add = Process()
    add.executableURL = URL(fileURLWithPath: "/usr/bin/security")
    add.arguments = ["add-generic-password", "-s", "app.tsugumi.agent", "-a", other, "-w", "not-for-this-program"]
    try add.run()
    while add.isRunning { usleep(20_000) }
    #expect(add.terminationStatus == 0)
    #expect(Keychain.exists(account: other))
    let started = Date()
    #expect(Keychain.loadWithoutAsking(account: other) == nil)
    await #expect(throws: (any Error).self) {
      _ = try await MessagesProvider(config: ProviderConfig(name: other, baseURL: port, model: "fixture", auth: .apiKey, api: .openAIResponses))
        .modelChoices(asking: false)
    }
    // A dialog would wait for someone to answer it.
    #expect(Date().timeIntervalSince(started) < 5)
    // The switch is back: this program's own items read as ever.
    #expect(Keychain.load(account: own) == "fixture-key")
  }

  /// The step limit is said, with the run the Agent started and did not
  /// check; "continue" goes on from there and checks it.
  @Test func stepLimitIsReportedAndContinues() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    await agent.setMaxSteps(3)  // inspect, save, run
    let log = EventLog()
    await agent.send("set src.text to \"limited\" and run, then report out", context: []) { log.add($0) }
    await waitFinished(log)
    let limit = log.all.compactMap { e -> (Int, String?)? in if case .stepLimit(let n, let r) = e { return (n, r) }; return nil }.first
    #expect(limit?.0 == 3, "\(log.all)")
    #expect(limit?.1 != nil, "the started run is named")
    #expect(!log.all.contains { if case .finished = $0 { return true }; return false })
    await agent.setMaxSteps(60)
    let more = EventLog()
    await agent.continueAfterLimit { more.add($0) }
    await waitFinished(more)
    #expect(more.all.contains { if case .finished = $0 { return true }; return false }, "\(more.all)")
    #expect(more.text.contains("succeeded"))
    await client.close()
  }

  /// A reply cut at the output limit inside a tool call runs nothing and is
  /// asked again in smaller steps; the work still finishes.
  @Test func cutReplyIsRetriedInSmallerSteps() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--cut-once"])
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send("set src.text to \"after a cut\" and run, then report out", context: []) { log.add($0) }
    await waitFinished(log)
    #expect(log.all.contains { if case .retrying(let s) = $0 { return s.contains("output limit") }; return false }, "\(log.all)")
    #expect(log.all.contains { if case .finished = $0 { return true }; return false })
    #expect(log.text.contains("succeeded"))
    let conv = await agent.conversation
    #expect(conv.tools.filter { $0.name == "project_save" }.count == 1, "the cut call never ran")
    #expect(conv.messages.contains { $0.display?.contains("途中で切れた") == true })
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

  /// A new request while a turn runs: the old turn ends first and adds
  /// nothing afterwards, so the conversation stays well formed (the fixture
  /// answers 400 to a tool_use without its result, as the Messages API does).
  @Test func aNewRequestWaitsForTheCancelledTurn() async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--slow", "0.05"])
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let first = EventLog(), second = EventLog()
    await agent.send("set src.text to first and run", context: []) { first.add($0) }
    try await Task.sleep(for: .milliseconds(300))
    await agent.send("set src.text to second and run, then report out", context: []) { second.add($0) }
    await waitFinished(second)
    #expect(second.text.contains("succeeded"), "\(second.all)")
    let conv = await agent.conversation
    let i = try #require(conv.messages.lastIndex { m in m.content.contains { if case .text(let t) = $0 { return t.contains("to second") }; return false } })
    // After the second request only its own turn: every tool result answers
    // a tool call made after it.
    var asked = Set<String>()
    for m in conv.messages[(i + 1)...] {
      for b in m.content {
        if case .toolUse(let id, _, _) = b { asked.insert(id) }
        if case .toolResult(let id, _, _) = b { #expect(asked.contains(id), "a result of the cancelled turn: \(id)") }
      }
    }
    await client.close()
  }

  /// A turn stopped while a tool ran leaves a tool_use without its result;
  /// a retry sends the recorded result (or "not run") first, so the request is
  /// valid (the fixture answers 400 otherwise, as the Messages API does).
  @Test func retryAfterAStoppedToolSendsItsResult() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    var stopped = Conversation()
    stopped.messages = [
      ChatMessage(role: "user", content: [.text("set src.text to resumed and run")]),
      ChatMessage(role: "assistant", content: [.toolUse(id: "toolu_done", name: "project_inspect", input: "{}"),
                                               .toolUse(id: "toolu_never", name: "project_inspect", input: "{}")]),
    ]
    stopped.tools = [ToolRecord(id: "toolu_done", name: "project_inspect", arguments: "{}", state: .completed,
                                result: "{\"reply\":\"Snapshot\"}")]
    try stopped.save(h.dir.appendingPathComponent("conversation.json"))
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.retry { log.add($0) }
    await waitFinished(log)
    #expect(log.all.contains { if case .finished = $0 { return true }; return false }, "\(log.all)")
    let conv = await agent.conversation
    let answered = conv.messages[2].content.compactMap { b -> (String, Bool)? in
      if case .toolResult(let id, _, let e) = b { return (id, e) }
      return nil
    }
    #expect(answered.map(\.0) == ["toolu_done", "toolu_never"] && answered.map(\.1) == [false, true], "\(conv.messages[2])")
    #expect(try await client.inspect().definitions.compactMap { $0 }.first { $0.id == "src" }?.cardJson.contains("resumed") == true)
    await client.close()
  }

  /// System prompt, tools and the conversation so far are marked for prompt
  /// caching; what the provider read from and wrote to its cache is counted.
  @Test func promptCacheIsAskedForAndCounted() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send("set src.text to cached and run", context: []) { log.add($0) }
    await waitFinished(log)
    let usage = await agent.conversation.usage
    #expect(usage.cacheWriteTokens == 400 && usage.cacheReadTokens >= 400, "\(usage)")
    await client.close()
  }

  /// Compaction replaces the earlier turns by a summary (they stay in the
  /// archive) and the conversation goes on: the next task runs to the end.
  @Test func compactionKeepsTheConversationGoing() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let first = EventLog()
    await agent.send("set src.text to first and run", context: []) { first.add($0) }
    await waitFinished(first)
    let compacted = EventLog()
    await agent.compact { compacted.add($0) }
    await waitFinished(compacted)
    #expect(compacted.all.contains { if case .compacted = $0 { return true }; return false }, "\(compacted.all)")
    let conv = await agent.conversation
    #expect(conv.summary?.isEmpty == false && !conv.archive.isEmpty)
    let second = EventLog()
    await agent.send("set src.text to second and run", context: []) { second.add($0) }
    await waitFinished(second)
    #expect(second.text.contains("succeeded"), "\(second.all)")
    #expect(try await client.inspect().definitions.compactMap { $0 }.first { $0.id == "src" }?.cardJson.contains("second") == true)
    await client.close()
  }

  /// The real model (opt in: TSUGUMI_LIVE_API=1, the Anthropic key from the
  /// Keychain as the app stores it; this costs a few cents): a setting is
  /// changed, the graph runs and the result is reported, with no tool error.
  @Test(.enabled(if: ProcessInfo.processInfo.environment["TSUGUMI_LIVE_API"] == "1"))
  func liveModelChangesRunsAndReports() async throws {
    var h = AgentHarness()
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client, provider: .anthropic)
    let log = EventLog()
    await agent.send("Set the text param of the src Card to \"live check\", run the workflow from its trigger and tell me "
                     + "what the out Card logged.", context: []) { log.add($0) }
    await waitFinished(log, seconds: 600)
    #expect(log.all.contains { if case .finished = $0 { return true }; return false }, "\(log.all)")
    let conv = await agent.conversation
    #expect(!conv.tools.contains { $0.isError }, "\(conv.tools.filter(\.isError).map { ($0.name, $0.result ?? "") })")
    #expect(try await client.inspect().definitions.compactMap { $0 }.first { $0.id == "src" }?.cardJson.contains("live check") == true)
    #expect(try await client.history().runs.first??.status.state == .succeeded)
    print("live: \(conv.messages.filter { $0.role == "assistant" }.count) replies, \(conv.tools.count) tool calls, usage \(conv.usage)")
    await client.close()
  }

  @Test func longToolResultsSayTheyWereCut() {
    let long = String(repeating: "x", count: Agent.resultLimit + 10)
    #expect(Agent.clip("short") == "short")
    #expect(Agent.clip(long).hasPrefix(String(repeating: "x", count: Agent.resultLimit)))
    #expect(Agent.clip(long).contains("[cut: showing the first \(Agent.resultLimit) of \(Agent.resultLimit + 10) characters"))
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
    cards.append(.object([("id", .string("plan"))]))
    var conns = doc["canvas"]?["connections"]?.array ?? []
    conns.append(try JSONValue.parse(#"{"from":{"card":"src","port":"msg"},"to":{"card":"plan","port":"data"}}"#))
    var canvas = doc["canvas"]!
    canvas["cards"] = .array(cards)
    canvas["connections"] = .array(conns)
    doc["canvas"] = canvas
    let meta = #"{"description":"Count words","content":"Input: the message. Output: summary with the word count. Split the message text and count the words; no network.","inputs":[{"name":"data","description":"the message"}],"outputs":[{"name":"summary","description":"the word count"}]}"#
    _ = try await client.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "test", changes: [
      Change.putDefinition(id: "plan", cardJSON: #"{"template":"draft","inputs":[{"name":"data"}]}"#, metaJSON: meta),
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

/// The examples in the Agent's rules are the bundled demos' own files, and
/// the references it reads are the real ones.
@Suite struct AgentReferenceTests {
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../..").standardizedFileURL

  func indented(_ rel: String) throws -> String {
    let text = try String(contentsOf: root.appendingPathComponent(rel), encoding: .utf8)
    return text.split(separator: "\n", omittingEmptySubsequences: false).dropLast(text.hasSuffix("\n") ? 1 : 0)
      .map { $0.isEmpty ? "" : "      " + $0 }.joined(separator: "\n")
  }

  @Test func promptExamplesAreTheDemoFiles() throws {
    for rel in ["demos/report/definitions/generate/default.nix", "demos/collatz/externals/collatz-cpp/definitions/halve/default.nix",
                "demos/report/externals/archive/definitions/copy/default.nix", "demos/report/definitions/generate/_outputs.fbs"] {
      #expect(Agent.systemPrompt.contains(try indented(rel)), "\(rel) differs from the example in the rules")
    }
    // Every demo path the rules and the guide name exists.
    let guide = try String(contentsOf: root.appendingPathComponent("agent/Reference/card-guide.md"), encoding: .utf8)
    let named = (Agent.systemPrompt + guide).matches(of: try Regex("demos/[A-Za-z0-9_/.-]+[A-Za-z0-9]")).map { String((Agent.systemPrompt + guide)[$0.range]) }
    #expect(!named.isEmpty)
    for n in Set(named) { #expect(FileManager.default.fileExists(atPath: root.appendingPathComponent(n).path), "\(n)") }
  }

  @Test func referenceReadListsAndReadsOnlyTheReferences() async throws {
    let refs = Agent.referenceDemos(in: root.appendingPathComponent("demos", isDirectory: true)).merging([
      "sdk/python/tsugumi/__init__.py": root.appendingPathComponent("sdk/python/tsugumi/__init__.py"),
      "card-guide.md": root.appendingPathComponent("agent/Reference/card-guide.md")]) { a, _ in a }
    let bfbs = try #require(RPCSchema.bfbs())
    let agent = try Agent(catalog: try ToolCatalog(schema: try BFBS(data: bfbs)), transport: NoTransport(), provider: .localFixture(port: 1),
                      storage: FileManager.default.temporaryDirectory.appendingPathComponent("ref-\(UUID()).json"), references: refs)
    #expect(await agent.tools.contains { $0.name == "reference_read" })
    let (top, e1) = await agent.readReference("")
    #expect(!e1 && top.contains("demos/") && top.contains("card-guide.md"))
    let (main, e2) = await agent.readReference("demos/report/definitions/generate/src/main.py")
    #expect(!e2 && main.contains("ctx.output_directory"))
    let (sdk, e3) = await agent.readReference("sdk/python/tsugumi/__init__.py")
    #expect(!e3 && sdk.contains("def display_table"))
    let (_, e4) = await agent.readReference("demos/../../core/src/store.cc")
    #expect(e4)
    // The Draft demos are tasks and the finished demo of each is its answer:
    // neither is listed nor read (implementing casa-draft must not copy casa).
    let (demos, e5) = await agent.readReference("demos")
    let listed = demos.split(separator: "\n").map(String.init)
    #expect(!e5 && Set(["collatz/", "live-stream/", "plots/", "report/"]).isSubset(of: listed))
    #expect(Set(["casa/", "casa-draft/", "gw150914/", "dynamics-draft/"]).isDisjoint(with: listed))
    for hidden in ["demos/casa", "demos/casa/definitions/bands/src/main.py", "demos/casa-draft", "demos/gw150914/tsugumi.json"] {
      let (_, e) = await agent.readReference(hidden)
      #expect(e, "\(hidden) is readable")
    }
  }
}

struct NoTransport: ToolTransport {
  func call(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset) async throws -> (replyType: String, table: FlatBuffers.Table) {
    throw CancellationError()
  }
}
