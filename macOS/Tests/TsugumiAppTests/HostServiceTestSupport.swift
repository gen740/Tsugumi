import Darwin
import Foundation
import Testing
@testable import TsugumiAgentClient
@testable import TsugumiApp

/// The environment and CoreHostClient.shared select one host for this process.
/// Every test that reaches it (including through a session save) must hold this
/// scope until its sessions and fixture services have stopped.
struct HostServiceTrait: SuiteTrait, TestTrait, TestScoping {
  var isRecursive: Bool { true }

  func provideScope(for test: Test, testCase: Test.Case?,
                    performing function: @Sendable () async throws -> Void) async throws {
    await HostServiceLock.shared.acquire()
    let directory = URL(fileURLWithPath: ProcessInfo.processInfo.environment["TSUGUMI_SOCKET_DIR"] ?? "/tmp").appendingPathComponent("hs-\(UUID().uuidString.prefix(8))")
    let environment = await HostServiceEnvironment()
    do {
      try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
      await environment.install(state: directory.appendingPathComponent("support/host-core"), support: directory.appendingPathComponent("support"), waitForHome: false, selectState: false)
      // Home's startup must finish in the current environment before a fixture
      // can replace it. This also joins startup triggered by an AppModel test.
      await AppModel.shared.initialRefresh?.value
      try await function()
      await environment.restore()
      try FileManager.default.removeItem(at: directory)
      await HostServiceLock.shared.release()
    } catch {
      await environment.restore()
      try? FileManager.default.removeItem(at: directory)
      await HostServiceLock.shared.release()
      throw error
    }
  }
}

extension Trait where Self == HostServiceTrait {
  static var hostService: Self { Self() }
}

/// Suspend waiters rather than blocking the main actor that runs UI fixtures.
private actor HostServiceLock {
  static let shared = HostServiceLock()
  private var held = false
  private var waiters: [CheckedContinuation<Void, Never>] = []

  func acquire() async {
    if !held { held = true; return }
    await withCheckedContinuation { waiters.append($0) }
  }

  func release() {
    if waiters.isEmpty { held = false }
    else { waiters.removeFirst().resume() }
  }
}

/// Used only while holding .hostService. Invalidate both sockets on each swap
/// so the next call connects to the service selected by the new environment.
@MainActor final class HostServiceEnvironment {
  private let previousState = ProcessInfo.processInfo.environment["TSUGUMI_HOST_STATE"]
  private let previousSupport = ProcessInfo.processInfo.environment["TSUGUMI_SUPPORT_DIR"]
  private let previousSocket = ProcessInfo.processInfo.environment["TSUGUMI_SOCKET_DIR"]
  private let previousScratch = ProcessInfo.processInfo.environment["TSUGUMI_RUN_SCRATCH_DIR"]
  private var installedState: URL?

  func install(state: URL, support: URL, waitForHome: Bool = true, selectState: Bool = true) async {
    if waitForHome { await AppModel.shared.initialRefresh?.value }
    await CoreHostClient.shared.disconnect()
    if selectState { setenv("TSUGUMI_HOST_STATE", state.path, 1) }
    else { unsetenv("TSUGUMI_HOST_STATE") }
    setenv("TSUGUMI_SUPPORT_DIR", support.path, 1)
    setenv("TSUGUMI_SOCKET_DIR", state.deletingLastPathComponent().path, 1)
    setenv("TSUGUMI_RUN_SCRATCH_DIR", support.resolvingSymlinksInPath().appendingPathComponent("scratch").path, 1)
    installedState = state
  }

  func restore() async {
    await CoreHostClient.shared.disconnect()
    // Read the current lock winner, including a service a reconnect started
    // after the fixture's original Process exited. Never inspect real support.
    if let state = installedState,
       let data = try? Data(contentsOf: state.appendingPathComponent("host.ready")),
       let record = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
       let pid = record["pid"] as? Int32, pid > 1, pid != getpid() {
      kill(pid, SIGTERM)
      let deadline = Date().addingTimeInterval(5)
      while kill(pid, 0) == 0 && Date() < deadline { try? await Task.sleep(for: .milliseconds(20)) }
      if kill(pid, 0) == 0 { kill(pid, SIGKILL) }
    }
    installedState = nil
    if let previousState { setenv("TSUGUMI_HOST_STATE", previousState, 1) }
    else { unsetenv("TSUGUMI_HOST_STATE") }
    if let previousSupport { setenv("TSUGUMI_SUPPORT_DIR", previousSupport, 1) }
    else { unsetenv("TSUGUMI_SUPPORT_DIR") }
    if let previousSocket { setenv("TSUGUMI_SOCKET_DIR", previousSocket, 1) }
    else { unsetenv("TSUGUMI_SOCKET_DIR") }
    if let previousScratch { setenv("TSUGUMI_RUN_SCRATCH_DIR", previousScratch, 1) }
    else { unsetenv("TSUGUMI_RUN_SCRATCH_DIR") }
  }
}
