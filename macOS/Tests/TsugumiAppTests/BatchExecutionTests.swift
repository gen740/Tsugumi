import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

@MainActor @Test(.hostService, arguments: [0, 1, 2])
func batchExecutionUsesRecordedWireCountsAndSelectsExactInvocations(count: Int) async throws {
  let directory = try SyncTests.project()
  defer { try? FileManager.default.removeItem(at: directory) }
  let card = directory.appendingPathComponent("definitions/a")
  try Data(#"{"language":"bash","sandbox":false,"recordIO":true}"#.utf8)
    .write(to: card.appendingPathComponent("_card.json"))
  try Data("table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;".utf8)
    .write(to: card.appendingPathComponent("_outputs.fbs"))
  let implementations = (0..<count).map { index in
    ["bash": "printf '%s' '{\"out\":{\"value\":\(index)}}' > \"$TSUGUMI_OUTPUT\"; printf 'member \(index)'"]
  }
  let mock = try JSONSerialization.data(withJSONObject: ["manifest": ["runs": implementations]])
  try mock.write(to: card.appendingPathComponent("mock-build.json"))

  let daemon = try SyncTests.Daemon(project: directory, mockedBuilds: true)
  defer { daemon.stop() }
  let identity = try ProjectIdentity(url: directory)
  let session = ProjectSession(identity: identity, environments: EnvironmentManager(image: nil))
  session.endpointOverride = daemon.endpoint
  session.start()
  try await SyncTests.connected(session)
  try await session.run(card: ["a"], propagate: false)
  // Run status and batch metadata can arrive in separate UI notification frames.
  for _ in 0..<200 {
    if !session.runActive, session.executionBatch("a")?.complete == true { break }
    try await Task.sleep(for: .milliseconds(25))
  }
  #expect(session.runStatus?.state == .succeeded)
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
    let log = try await rpc.logRange(runId: runId, card: ["a"], invocation: invocation, stream: .stdout)
    #expect(log.text.contains("member 0"))
  }
  await session.close()
}
