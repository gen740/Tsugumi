import Darwin
import Foundation
import Testing
@testable import TsugumiLauncher
@testable import TsugumiApp
@testable import TsugumiRPC

@MainActor @Test func executionHistoryRecoveryRequiresExplicitArchive() async throws {
  let dir = try SyncTests.project()
  defer { try? FileManager.default.removeItem(at: dir) }
  let support = dir.appendingPathComponent("test-support")
  try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
  let socket = VMHostIPC.socketPath(support: support)
  let listener = try VMHostIPC.listen(path: socket)
  defer { Darwin.close(listener); unlink(socket) }
  let host = Task.detached { () throws -> [Bool] in
    let peer = accept(listener, nil, nil)
    try #require(peer >= 0)
    defer { Darwin.close(peer) }
    var timeout = timeval(tv_sec: 5, tv_usec: 0)
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    var archives: [Bool] = []
    for attempt in 0..<3 {
      let request = try #require(try VMHostIPC.read(peer))
      #expect(request["op"] as? String == "start")
      archives.append(request["archive_incompatible_history"] as? Bool == true)
      let kind = attempt < 2 ? "incompatible_history" : "database_corrupt"
      try VMHostIPC.write([
        "id": try #require(request["id"] as? String),
        "error": "cannot open history",
        "startup_error": ["kind": kind, "found": "2", "expected": "3"],
      ], to: peer)
    }
    return archives
  }
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(support: support, image: nil))
  func waitForProblem() async throws {
    for _ in 0..<200 {
      if EnvironmentOverlay.hasProblem(session), session.connectionMessage != nil { return }
      try await Task.sleep(for: .milliseconds(10))
    }
    Issue.record("Expected a startup problem")
  }
  session.start()
  try await waitForProblem()
  guard case .incompatibleHistory(let mismatch) = session.environment else {
    Issue.record("Expected a typed history mismatch")
    await session.close()
    return
  }
  #expect(mismatch.found == "2")
  #expect(mismatch.expected == "3")
  session.start()
  try await waitForProblem()
  session.archiveHistoryAndStart()
  try await waitForProblem()
  #expect(try await host.value == [false, false, true])
  guard case .failed = session.environment else {
    Issue.record("Expected an ordinary startup failure for a corrupt database")
    await session.close()
    return
  }
  session.archiveHistoryAndStart()
  if case .failed = session.environment {} else { Issue.record("A corrupt database cannot be archived here") }
  await session.close()
}

@Test func executionHistoryFindsTheActiveRunWithoutReplacingAHistoricalSelection() {
  let runs = ["active", "stopped", "older"]
  #expect(ExecutionHistorySelection.run(nil, available: runs, active: "active") == "active")
  #expect(ExecutionHistorySelection.run("stopped", available: runs, active: "active") == "stopped")
  #expect(ExecutionHistorySelection.run("deleted", available: runs, active: "active") == "active")
  #expect(ExecutionHistorySelection.run(nil, available: ["stopped"], active: "unrelated") == "stopped")
  #expect(ExecutionHistorySelection.run(nil, available: [], active: "unrelated") == nil)
}

@Test func executionHistoryFollowsNewInvocationsButPreservesAnEarlierCall() {
  #expect(ExecutionHistorySelection.invocation(nil, previous: [], available: [1]) == 1)
  #expect(ExecutionHistorySelection.invocation(1, previous: [1], available: [1, 2]) == 2)
  #expect(ExecutionHistorySelection.invocation(1, previous: [1, 2], available: [1, 2, 3]) == 1)
  #expect(ExecutionHistorySelection.invocation(3, previous: [1, 2, 3], available: []) == nil)
}

@Test func executionHistoryDetectsSilentBuildAndTerminationTransitions() {
  var building = CardRunState(state: .running, invocation: 1, phase: .build)
  building.stages = [StageRecord(stage: .build, outcome: .running, start: 1)]
  var executing = building
  executing.phase = .execute
  executing.stages = [StageRecord(stage: .build, outcome: .cached, start: 1, end: 2),
                      StageRecord(stage: .execute, outcome: .running, start: 2)]
  #expect(building != executing)
  var finished = executing
  finished.state = .succeeded
  finished.endedAt = 3
  finished.stages[1].outcome = .done
  finished.stages[1].end = 3
  #expect(executing != finished)
  var cancelled = building
  cancelled.state = .cancelled
  cancelled.stages[0].outcome = .cancelled
  #expect(building != cancelled)
}

@MainActor @Test func executionHistoryRefreshesAfterARunWithoutStandardOutput() async throws {
  let dir = try SyncTests.project()
  let daemon = try SyncTests.Daemon(project: dir)
  defer { daemon.stop(); try? FileManager.default.removeItem(at: dir) }
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  session.endpointOverride = daemon.endpoint
  session.start()
  try await SyncTests.connected(session)
  session.deleteCards(["a", "b", "c"], in: [])
  await session.save()
  #expect(session.saveError == nil)
  let before = ExecutionHistoryRefresh(session: session, card: "start")
  try await session.run(card: ["start"], propagate: true)
  for _ in 0..<200 where session.runActive { try await Task.sleep(for: .milliseconds(25)) }
  #expect(!session.runActive)
  #expect(session.runStatus?.state == .succeeded)
  let after = ExecutionHistoryRefresh(session: session, card: "start")
  #expect(before != after)
  #expect(after.card?.state == .succeeded)
  let output = try await session.rpc!.logRange(runId: after.runId!, card: ["start"], invocation: 1,
                                                stream: .stdout, offset: 0, limit: 1024)
  #expect(output.text.isEmpty)
  let history = try await session.rpc!.history(card: ["start"], limit: 100)
  #expect(history.runs.first??.status.runId == after.runId)
  #expect(history.runs.first??.status.state == .succeeded)
  await session.close()
}
