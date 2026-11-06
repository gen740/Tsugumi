import Foundation
import Testing
@testable import TsugumiAgent
import TsugumiRPC

/// delegate: helpers implement independent Drafts at the same time against a
/// real tsugumid. Their saves start from stale revisions and their runs meet
/// each other's (revision_conflict, busy); the harness resolves both, and
/// each helper changes only its own Card.
@Suite(.serialized) struct TeamTests {
  static let drafts = ["alpha", "beta", "gamma"]

  func addDrafts(_ client: RPCClient) async throws {
    let snap = try await client.inspect()
    var doc = try JSONValue.parse(snap.projectJson)
    var cards = doc["canvas"]?["cards"]?.array ?? []
    var conns = doc["canvas"]?["connections"]?.array ?? []
    // The message goes to the three Drafts through a fork (an output has one wire).
    let branches = (["out"] + Self.drafts).map { "\"\($0)\"" }.joined(separator: ",")
    var changes: [Tsugumi_RPC_DocumentChangeT] = [
      Change.putDefinition(id: "share", cardJSON: #"{"template":"fork","params":[{"name":"branches","type":"string[]","data":[\#(branches)]}]}"#, metaJSON: nil)]
    cards.append(.object([("id", .string("share"))]))
    // (the logger it already fed is fed by the fork too)
    conns = conns.filter { $0["from"]?["card"]?.string != "src" }
    conns.append(try JSONValue.parse(#"{"from":{"card":"src","port":"msg"},"to":{"card":"share","port":"in"}}"#))
    conns.append(try JSONValue.parse(#"{"from":{"card":"share","port":"out"},"to":{"card":"out","port":"in"}}"#))
    for d in Self.drafts {
      cards.append(.object([("id", .string(d))]))
      conns.append(try JSONValue.parse(#"{"from":{"card":"share","port":"\#(d)"},"to":{"card":"\#(d)","port":"data"}}"#))
      let meta = #"{"description":"\#(d)","content":"Say done.","inputs":[{"name":"data"}],"outputs":[{"name":"result"}]}"#
      changes.append(Change.putDefinition(id: d, cardJSON: #"{"template":"draft","inputs":[{"name":"data"}]}"#, metaJSON: meta))
    }
    var canvas = doc["canvas"]!
    canvas["cards"] = .array(cards)
    canvas["connections"] = .array(conns)
    doc["canvas"] = canvas
    _ = try await client.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "test", changes: changes + [Change.setProject(doc.serialized())])
  }

  @Test func helpersImplementDraftsAtTheSameTime() async throws {
    var h = AgentHarness()
    // The helpers' Cards run, so the worker must be built (not only tsugumid).
    try #require(FileManager.default.fileExists(atPath: h.root.appendingPathComponent("build/sdk/tsugumi-worker").path),
                 "build/sdk/tsugumi-worker is missing: ninja -C build tsugumi-worker")
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    try await addDrafts(client)
    let agent = try makeAgent(h, client)
    #expect(await agent.tools.contains { $0.name == "delegate" })
    let log = EventLog()
    await agent.send("delegate the drafts alpha beta gamma", context: []) { log.add($0) }
    await waitFinished(log, seconds: 180)
    // Every helper reported, and its events came tagged with its name.
    for d in Self.drafts { #expect(log.text.contains("\(d): \(d) implemented, its run succeeded"), "\(log.text)") }
    let helpers = Set(log.all.compactMap { e -> String? in if case .helper(let n, _) = e { return n }; return nil })
    #expect(helpers == Set(Self.drafts))
    let after = try await client.inspect()
    for d in Self.drafts {
      let def = try #require(after.definitions.compactMap { $0 }.first { $0.id == d })
      #expect(try JSONValue.parse(def.cardJson)["language"]?.string == "bash", "\(d) is still a Draft")
    }
    // Each kept its conversation next to the main one.
    let dir = h.dir.appendingPathComponent("conversation.helpers")
    #expect(Set((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []) == Set(Self.drafts.map { $0 + ".json" }))
    await client.close()
  }

  /// "runs": "together": the helpers only write, whatever they try, and the
  /// main Agent runs once when all have reported. No helper's run holds up
  /// another's save, and there is one run in the history.
  @Test func helpersThatRunTogetherOnlyWrite() async throws {
    var h = AgentHarness()
    try #require(FileManager.default.fileExists(atPath: h.root.appendingPathComponent("build/sdk/tsugumi-worker").path),
                 "build/sdk/tsugumi-worker is missing: ninja -C build tsugumi-worker")
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    try await addDrafts(client)
    let before = try await client.history().runs.count
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send("delegate the drafts alpha beta gamma, and run them together", context: []) { log.add($0) }
    await waitFinished(log, seconds: 180)
    #expect(log.text.contains("Ran once: succeeded"), "\(log.text)")
    // Each helper tried to run and was told no.
    let refused = log.all.filter { e in
      if case .helper(_, .toolFinished(_, "run", false, let summary)) = e { return summary.contains("only writes") }
      return false
    }
    #expect(refused.count == Self.drafts.count, "\(log.all)")
    #expect(try await client.history().runs.count == before + 1, "one run, the main Agent's")
    let after = try await client.inspect()
    for d in Self.drafts {
      let def = try #require(after.definitions.compactMap { $0 }.first { $0.id == d })
      #expect(try JSONValue.parse(def.cardJson)["language"]?.string == "bash", "\(d) is still a Draft")
    }
    await client.close()
  }

  @Test func howHelpersMeetIsChosenInTheCall() {
    let tasks = #""tasks":[{"name":"a","cards":["a"],"instruction":"x"},{"name":"b","cards":["b"],"instruction":"y"}]"#
    func together(_ json: String) -> [Bool]? {
      if case .success(let h) = Agent.tasks(json) { return h.map(\.runsTogether) }
      return nil
    }
    #expect(together("{\(tasks)}") == [false, false])
    #expect(together(#"{"runs":"each","# + tasks + "}") == [false, false])
    #expect(together(#"{"runs":"together","# + tasks + "}") == [true, true])
    #expect(together(#"{"runs":"sometimes","# + tasks + "}") == nil)
  }

  @Test func aDelegateCallIsCheckedBeforeAnyHelperStarts() {
    func why(_ json: String) -> String? {
      if case .failure(let e) = Agent.tasks(json) { return "\(e)" }
      return nil
    }
    let ok = #"{"tasks":[{"name":"a","cards":["a"],"instruction":"x"},{"name":"b","cards":["b","c"],"instruction":"y"}]}"#
    #expect(why(ok) == nil)
    #expect(why(#"{"tasks":[{"name":"a","cards":["a"],"instruction":"x"}]}"#) != nil)  // one task
    #expect(why(#"{"tasks":[{"name":"a","cards":["a"],"instruction":"x"},{"name":"b","cards":["a"],"instruction":"y"}]}"#) != nil)  // shared Card
    #expect(why(#"{"tasks":[{"name":"a/..","cards":["a"],"instruction":"x"},{"name":"b","cards":["b"],"instruction":"y"}]}"#) != nil)
    #expect(why(#"{"tasks":[{"name":"a","cards":["x/y"],"instruction":"x"},{"name":"b","cards":["b"],"instruction":"y"}]}"#) != nil)
  }

  @Test func aHelperSavesOnlyItsOwnCards() {
    let h = Agent.Helper(name: "lorenz", cards: ["lorenz"], instruction: "")
    let own: [[String: Any]] = [
      ["value_type": "PutDefinition", "value": ["id": "lorenz"]],
      ["value_type": "PutSource", "value": ["path": "definitions/lorenz/src/main.py"]],
    ]
    #expect(h.outside(own) == nil)
    #expect(h.outside([["value_type": "PutDefinition", "value": ["id": "life"]]]) == "the definition life")
    #expect(h.outside([["value_type": "SetProject", "value": ["json": "{}"]]]) == "SetProject")
    #expect(h.outside([["value_type": "PutSource", "value": ["path": "definitions/lorenz/../life/x"]]]) != nil)
    #expect(h.outside([["value_type": "PutSource", "value": ["path": "tsugumi.json"]]]) == "tsugumi.json")
  }

  /// While the Agent works, the app tidies the canvas (a layout save): the
  /// Agent's save from the revision it inspected goes on, since it touches
  /// nothing of it. A change to the definition it rewrites goes to the model.
  @Test(arguments: [false, true]) func aSaveMeetingOthersChanges(definition: Bool) async throws {
    var h = AgentHarness()
    try h.start(fixtureArgs: ["--slow", "0.05"])
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let other = RPCClient(endpoint: h.endpoint!)
    _ = try await other.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let log = EventLog()
    let once = Once()
    await agent.send("set src.text to \"tidied\" and run, then report out", context: []) { e in
      log.add(e)
      // Right after the Agent inspected: the other client saves.
      if case .toolFinished(_, "project_inspect", true, _) = e, once.first() {
        // (the test's expectations show whether this save went in)
        _ = Task {
          let snap = try await other.inspect()
          let change = definition
            ? Change.putDefinition(id: "src", cardJSON: #"{"template":"input","params":[{"name":"text","type":"string","data":"theirs"}]}"#, metaJSON: nil)
            : Change.setLayout(#"{"cards":{"src":{"x":40,"y":80}}}"#)
          _ = try await other.save(baseRev: snap.rev ?? 0, opId: UUID(), origin: "test", changes: [change])
        }
      }
    }
    await waitFinished(log, seconds: 60)
    let tools = await agent.conversation.tools
    let saves = tools.filter { $0.name == "project_save" }
    if definition {
      // Theirs was not overwritten: the model is told of the conflict.
      #expect(saves.first?.isError == true && saves.first?.result?.contains("revision_conflict") == true, "\(tools.map(\.name))")
      let snap = try await client.inspect()
      #expect(snap.definitions.compactMap { $0 }.first { $0.id == "src" }?.cardJson.contains("theirs") == true)
    } else {
      #expect(log.text.contains("succeeded"), "\(log.all)")
      #expect(saves.count == 1 && saves[0].isError == false, "\(saves.map(\.result))")
      #expect(tools.filter { $0.name == "project_inspect" }.count == 1)
      let snap = try await client.inspect()
      #expect(snap.layoutJson.contains("80"))
    }
    await client.close()
    await other.close()
  }
}

final class Once: @unchecked Sendable {
  private let lock = NSLock()
  private var done = false
  func first() -> Bool { lock.withLock { defer { done = true }; return !done } }
}
