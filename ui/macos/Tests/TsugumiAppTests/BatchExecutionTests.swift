import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

@MainActor @Test func emptyBatchKeepsEarlierInvocationInTimeline() throws {
  let directory = try SyncTests.project()
  defer { try? FileManager.default.removeItem(at: directory) }
  let session = ProjectSession(identity: try ProjectIdentity(url: directory), environments: EnvironmentManager(image: nil))
  let runId = "run"
  let card = cardPath(["a"])
  let status = Tsugumi_RPC_RunStatusT()
  status.runId = runId
  status.executionRev = session.executionRev
  let accepted = Tsugumi_RPC_RunAcceptedEventT()
  accepted.status = status
  let event = Tsugumi_RPC_RunEventT()
  event.runId = runId
  event.value = Tsugumi_RPC_RunEventValueUnion(accepted, type: .runacceptedevent)
  session.applyRunEvent(event)

  let invocation = Tsugumi_RPC_InvocationSummaryT()
  invocation.card = card
  invocation.invocation = 1
  invocation.batchActivation = 1
  invocation.state = .succeeded
  let changed = Tsugumi_RPC_RunCardChangedT()
  changed.invocation = invocation
  event.value = Tsugumi_RPC_RunEventValueUnion(changed, type: .runcardchanged)
  session.applyRunEvent(event)

  let batch = Tsugumi_RPC_RunBatchT()
  batch.card = card
  batch.activation = 2
  batch.executionList = true
  batch.complete = true
  let empty = Tsugumi_RPC_RunBatchChangedT()
  empty.batch = batch
  event.value = Tsugumi_RPC_RunEventValueUnion(empty, type: .runbatchchanged)
  session.applyRunEvent(event)

  #expect(session.executionBatch("a")?.invocationCount == 0)
  #expect(session.canvasCardState("a") == .succeeded)
  #expect(session.timeline.map(\.invocation) == [1])
}

@MainActor @Test(.hostService, arguments: [0, 1, 2])
func batchExecutionUsesRecordedWireCountsAndSelectsExactInvocations(count: Int) async throws {
  let directory = try SyncTests.project()
  defer { try? FileManager.default.removeItem(at: directory) }
  let card = directory.appendingPathComponent("definitions/a")
  // The daemon double runs Cards natively on this Mac: name its system so the
  // run needs no emulator.
  #if arch(arm64)
  let native = "aarch64-darwin"
  #else
  let native = "x86_64-darwin"
  #endif
  var definition = implementedCard(.code, symbol: CardKind.code.symbol, systems: [native])
  // The scripted worker reads its test package directly from the project.
  definition["sandbox"]?["filesystem"] = .bool(false)
  definition["sandbox"]?["identity"] = .bool(false)
  definition["recordIO"] = .string("on")
  try Data(definition.serialized().utf8)
    .write(to: card.appendingPathComponent("_card.json"))
  try Data("table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;".utf8)
    .write(to: card.appendingPathComponent("_outputs.fbs"))
  // Each implementation is a package the scripted worker double plays.
  let implementations = try (0..<count).map { index in
    let package = directory.appendingPathComponent("packages/\(index)")
    try FileManager.default.createDirectory(at: package, withIntermediateDirectories: true)
    let actions: [[String: Any]] = [["op": "log", "text": "member \(index)"],
                                    ["op": "output", "port": "out", "type": "uint64", "value": index]]
    try JSONSerialization.data(withJSONObject: ["actions": actions])
      .write(to: package.appendingPathComponent("scenario.json"))
    return package.path
  }
  let mock = try JSONSerialization.data(withJSONObject: [
    "product": ["runs": implementations, "views": [] as [Any]], "executionList": true,
  ] as [String: Any])
  try mock.write(to: card.appendingPathComponent("mock-build.json"))
  try Data("{ params, inputs }: { }".utf8).write(to: card.appendingPathComponent("default.nix"))
  let worker = try scriptedWorker(in: directory)

  let daemon = try SyncTests.Daemon(project: directory, mockedBuilds: true, worker: worker)
  defer { daemon.stop() }
  let identity = try ProjectIdentity(url: directory)
  let session = ProjectSession(identity: identity, environments: EnvironmentManager(image: nil))
  session.endpointOverride = daemon.endpoint
  session.start()
  try await SyncTests.connected(session)
  try await session.run(card: ["a"], propagate: false)
  // Run status and batch metadata can arrive in separate UI notification frames.
  try await waitForModel { !session.runActive && session.executionBatch("a")?.complete == true }
  #expect(session.runStatus?.state == .succeeded, "\(session.runStatus?.failure?.diagnostic ?? "No run diagnostic")")
  let runId = try #require(session.currentRunId)
  let batch = try #require(session.executionBatch("a"))
  #expect(batch.complete)
  #expect(batch.executionList)
  #expect(batch.invocationCount == UInt32(count))
  #expect(session.canvasCardState("a") == .succeeded)
  let wire = session.executionWire("a", "out")
  #expect(wire.executionList)
  #expect(wire.count == UInt32(count))
  if count == 0 {
    #expect(session.timeline.allSatisfy { $0.card != "a" })
    #expect(session.cardStates["a"] == nil)
  }

  let rpc = try #require(session.rpc)
  let page = try await rpc.invocations(runId: runId, card: ["a"])
  let members = page.records.compactMap { $0 }
  #expect(members.count == count)
  #expect(members.compactMap(\.batchIndex) == (0..<count).map { UInt32($0 + 1) })
  #expect(members.allSatisfy { $0.batchSize == UInt32(count) && $0.batchActivation == batch.activation })
  if let first = members.first, let invocation = first.invocation {
    let workspace = Workspace()
    workspace.logRunId = "a different historical run"
    workspace.showLog(for: "a", invocation: invocation, runId: runId)
    #expect(workspace.logRunId == runId)
    #expect(workspace.logInvocation == invocation)
    let record = try await rpc.record(runId: runId, card: ["a"], invocation: invocation)
    #expect(record.selectedInvocation == invocation)
    let log = try await rpc.logRange(runId: runId, card: ["a"], invocation: invocation, stream: .output)
    #expect(log.text.contains("member 0"))
  }
  await session.close()
}

/// tests/core/doubles/worker.py with absolute paths: a worker gets no PATH.
private func scriptedWorker(in directory: URL) throws -> URL {
  let env = ProcessInfo.processInfo.environment
  let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
  let python = try #require((env["PATH"] ?? "").split(separator: ":").map { URL(fileURLWithPath: String($0)).appendingPathComponent("python3") }
    .first { FileManager.default.isExecutableFile(atPath: $0.path) })
  let bindings = env["TSUGUMI_PY_BINDINGS"] ?? repo.appendingPathComponent("build/generated/python").path
  let double = repo.appendingPathComponent("tests/core/doubles/worker.py").path
  let launcher = directory.appendingPathComponent("worker")
  try Data("#!\(python.path)\nimport sys, runpy\nsys.path.insert(0, \(bindings.debugDescription))\nrunpy.run_path(\(double.debugDescription), run_name='__main__')\n".utf8)
    .write(to: launcher)
  try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: launcher.path)
  return launcher
}

@MainActor @Test func comparisonPairsBatchMembersByName() {
  func member(_ invocation: UInt64, _ label: String?, implementation: UInt32 = 1) -> Tsugumi_RPC_InvocationSummaryT {
    let item = Tsugumi_RPC_InvocationSummaryT()
    item.invocation = invocation; item.batchActivation = 1; item.batchIndex = UInt32(invocation); item.batchSize = 4
    item.implementationIndex = implementation; item.implementationLabel = label
    return item
  }
  // An inserted threshold does not shift the pairs, and a name repeated once
  // per arriving input pairs by its occurrence.
  let left = [member(1, "threshold 10"), member(2, "threshold 14"), member(3, "threshold 10"), member(4, "threshold 14")]
  let right = [member(1, "threshold 10"), member(2, "threshold 12"), member(3, "threshold 14"),
               member(4, "threshold 10"), member(5, "threshold 12"), member(6, "threshold 14")]
  let pairs = RunComparison.pairMembers(left, right)
  #expect(pairs.map(\.name) == ["threshold 10 (1)", "threshold 14 (1)", "threshold 10 (2)", "threshold 14 (2)",
                                "threshold 12 (1)", "threshold 12 (2)"])
  #expect(pairs.map { $0.left?.invocation } == [1, 2, 3, 4, nil, nil])
  #expect(pairs.map { $0.right?.invocation } == [1, 3, 4, 6, 2, 5])
  #expect(RunComparison.memberName(member(1, nil, implementation: 2)) == String(localized: "実装 \(UInt32(2))"))
  let single = member(1, "value=5")
  single.batchSize = 1
  #expect(RunComparison.inBatch(single))
  #expect(!RunComparison.inBatch(Tsugumi_RPC_InvocationSummaryT()))
}
