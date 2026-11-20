import Foundation
import Testing
import TsugumiAgentClient
import TsugumiRPC
import TsugumiTestSupport

// The auth fixture temporarily selects a host through TSUGUMI_HOST_STATE.
extension AuthenticationTests {
  @MainActor @Test(.timeLimit(.minutes(1)))
  func observesHostLaunchAndRestartAndReceivesTraceOnTheSocketThread() async throws {
    let environment = ProcessInfo.processInfo.environment
    let binary = try #require(environment["TSUGUMI_HOSTD"])
    let root = URL(fileURLWithPath: environment["TSUGUMI_SOCKET_DIR"] ?? "/private/tmp")
      .appendingPathComponent("trace-\(UUID().uuidString.prefix(8))")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let state = root.appendingPathComponent("host-core")
    let client = CoreHostClient(support: root)
    var hosts = await client.hostConnectionChanges().makeAsyncIterator()
    let initial = try #require(await hosts.next())
    #expect(initial == nil)

    func startHost() throws -> Process {
      let process = Process()
      process.executableURL = URL(fileURLWithPath: binary)
      process.arguments = ["--state-dir", state.path, "--unix", root.appendingPathComponent("rpc").path,
                           "--socket-dir", root.appendingPathComponent("b").path, "--no-websocket",
                           "--usage-scratch-dir", root.appendingPathComponent("scratch").path]
      process.environment = environment.merging(["TSUGUMI_RPC_TRACE": "1"]) { _, trace in trace }
      try process.run()
      return process
    }

    func connectAndReceive(_ process: Process) async throws -> UUID {
      let ready = state.appendingPathComponent("host.ready")
      try #require(try fixtureReadiness(ready, pid: process.processIdentifier) {
        FileManager.default.fileExists(atPath: ready.path) ? true : nil
      } == true)
      _ = try await client.activityBeforeQuit()
      let event = try #require(await hosts.next())
      let session = try #require(event)

      let trace = try await client.traceConnection()
      defer { trace.close() }
      let notices = AsyncStream<String?>.makeStream()
      try await trace.connect(changed: { notices.continuation.yield($0) },
                              closed: { notices.continuation.finish() })
      _ = try await trace.query(mode: .trace)
      _ = try await trace.query(source: "")
      _ = try await client.activityBeforeQuit()
      var events = notices.stream.makeAsyncIterator()
      let source = try #require(await events.next())
      #expect(source == "")
      let snapshot = try await trace.query(source: "").snapshot
      #expect(snapshot.entries.contains { $0?.messageType == "HostActivityRequest" })
      return session
    }

    func stop(_ process: Process) {
      if process.isRunning { process.terminate() }
      process.waitUntilExit()
    }

    let first = try startHost()
    defer { stop(first) }
    let firstSession = try await connectAndReceive(first)
    stop(first)
    let disconnected = try #require(await hosts.next())
    #expect(disconnected == nil)

    let second = try startHost()
    defer { stop(second) }
    let secondSession = try await connectAndReceive(second)
    #expect(firstSession != secondSession)
  }
}
