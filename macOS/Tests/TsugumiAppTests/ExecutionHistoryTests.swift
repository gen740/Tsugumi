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
  let host = Task {
    try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<[Bool], Error>) in
      // The fake host blocks on its socket, independently of Swift test tasks.
      Thread.detachNewThread {
        continuation.resume(with: Result {
          var incoming = pollfd(fd: listener, events: Int16(POLLIN), revents: 0)
          guard poll(&incoming, 1, 60_000) == 1 else { throw POSIXError(.ETIMEDOUT) }
          let peer = accept(listener, nil, nil)
          guard peer >= 0 else { throw POSIXError(.ECONNABORTED) }
          defer { Darwin.close(peer) }
          // The application suite can hold the main actor while it launches other test daemons.
          var timeout = timeval(tv_sec: 60, tv_usec: 0)
          setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
          var archives: [Bool] = []
          for attempt in 0..<3 {
            guard let request = try VMHostIPC.read(peer), request["op"] as? String == "start",
                  let id = request["id"] as? String else { throw POSIXError(.EPROTO) }
            archives.append(request["archive_incompatible_history"] as? Bool == true)
            let kind = attempt < 2 ? "incompatible_history" : "database_corrupt"
            try VMHostIPC.write([
              "id": id,
              "error": "cannot open history",
              "startup_error": ["kind": kind, "found": attempt == 1 ? "pending" : "2", "expected": "3"],
            ], to: peer)
          }
          return archives
        })
      }
    }
  }
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(support: support, image: nil))
  func waitForProblem(found: String?) async throws {
    let deadline = ContinuousClock.now + .seconds(60)
    while true {
      if EnvironmentOverlay.hasProblem(session), session.connectionMessage != nil {
        switch session.environment {
        case .incompatibleHistory(let issue) where issue.found == found: return
        case .failed where found == nil: return
        default: break
        }
      }
      guard ContinuousClock.now < deadline else {
        Issue.record("Expected history startup state \(found ?? "failed"), got \(session.environment.label)")
        throw POSIXError(.ETIMEDOUT)
      }
      try await Task.sleep(for: .milliseconds(10))
    }
  }
  session.start()
  try await waitForProblem(found: "2")
  guard case .incompatibleHistory(let mismatch) = session.environment else {
    Issue.record("Expected a typed history mismatch")
    await session.close()
    return
  }
  #expect(mismatch.found == "2")
  #expect(mismatch.expected == "3")
  #expect(!mismatch.archiveInterrupted)
  session.start()
  try await waitForProblem(found: "pending")
  if case .incompatibleHistory(let pending) = session.environment {
    #expect(pending.archiveInterrupted)
    #expect(!LauncherError.incompatibleHistory(pending).description.contains("pending"))
  } else {
    Issue.record("Expected an interrupted archive")
  }
  session.archiveHistoryAndStart()
  try await waitForProblem(found: nil)
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
