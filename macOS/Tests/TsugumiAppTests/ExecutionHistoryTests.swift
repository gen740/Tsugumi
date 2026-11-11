import Foundation
import Testing
@testable import TsugumiLauncher
@testable import TsugumiApp
@testable import TsugumiRPC

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
