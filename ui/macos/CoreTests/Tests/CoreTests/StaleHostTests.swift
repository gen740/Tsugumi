import Darwin
import Foundation
import Testing
import TsugumiAgentClient
import TsugumiRPC
import TsugumiTestSupport

// The auth fixture temporarily selects a host through TSUGUMI_HOST_STATE.
extension AuthenticationTests {
  /// A machine stopped without quitting the app leaves the host's publication
  /// behind. Nobody holds host.lock any more, so the files describe no host:
  /// quitting finds no activity and the next connection starts a new hostd.
  @Test(.timeLimit(.minutes(1)))
  func publicationLeftWithoutALockOwnerDoesNotBlockStartup() async throws {
    let environment = ProcessInfo.processInfo.environment
    _ = try #require(environment["TSUGUMI_HOSTD"])
    let root = URL(fileURLWithPath: environment["TSUGUMI_SOCKET_DIR"] ?? "/private/tmp")
      .appendingPathComponent("stale-\(UUID().uuidString.prefix(8))")
    let state = root.appendingPathComponent("host-core")
    try FileManager.default.createDirectory(at: state, withIntermediateDirectories: true,
                                            attributes: [.posixPermissions: 0o700])
    defer { try? FileManager.default.removeItem(at: root) }
    // After a reboot the recorded PID can belong to an unrelated live process.
    let reused = getpid()
    let stale: [String: Any] = [
      "pid": reused, "user_token": "stale", "endpoint": "",
      "unix_socket": root.appendingPathComponent("gone/agent.sock").path,
      "host_socket": root.appendingPathComponent("gone/host.sock").path,
    ]
    try JSONSerialization.data(withJSONObject: stale).write(to: state.appendingPathComponent("host.ready"))
    try Data().write(to: state.appendingPathComponent("host.starting"))
    try Data().write(to: state.appendingPathComponent("host.lock"))
    try JSONSerialization.data(withJSONObject: [
      "application": "Tsugumi Dev", "hostname": ProcessInfo.processInfo.hostName, "pid": reused,
    ]).write(to: state.appendingPathComponent("host.lock-owner"))

    let client = CoreHostClient(support: root)
    await client.configureRuntime([])
    #expect(try await client.activityBeforeQuit() == false)
    await #expect {
      _ = try await client.traceConnection()
    } throws: { error in
      // The failing connection is the host service's, not a project's tsugumid.
      (error as? CoreHostFailure)?.code == "disconnected"
        && !error.localizedDescription.contains("tsugumid")
    }

    let activity: Tsugumi_Host_HostActivityT = try await client.hostCall(
      .hostactivityrequest, Tsugumi_Host_HostActivityRequestT())
    let ready = try #require(try JSONSerialization.jsonObject(
      with: Data(contentsOf: state.appendingPathComponent("host.ready"))) as? [String: Any])
    let pid = try #require(ready["pid"] as? Int32)
    defer { try? finishFixtureProcess(pid, signal: SIGTERM) }
    #expect(pid != reused)
    #expect(ready["user_token"] as? String != "stale")
    #expect(!activity.busy)
    #expect(try await client.activityBeforeQuit() == false)
  }
}
