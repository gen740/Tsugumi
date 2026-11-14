import Darwin
import Foundation
import Testing
@testable import TsugumiAgentClient
@testable import TsugumiApp

/// The environment and CoreAgentClient.shared select one host for this process.
/// Every test that reaches it (including through a session save) must hold this
/// scope until its sessions and fixture services have stopped.
struct HostServiceTrait: SuiteTrait, TestTrait, TestScoping {
  var isRecursive: Bool { true }

  func provideScope(for test: Test, testCase: Test.Case?,
                    performing function: @Sendable () async throws -> Void) async throws {
    await HostServiceLock.shared.acquire()
    // Home's startup must finish in the current environment before a fixture
    // can replace it. This also joins startup triggered by an AppModel test.
    await AppModel.shared.initialRefresh?.value
    do {
      try await function()
      await HostServiceLock.shared.release()
    } catch {
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
  private let previousState = ProcessInfo.processInfo.environment["TSUGUMI_AGENT_STATE"]
  private let previousSupport = ProcessInfo.processInfo.environment["TSUGUMI_SUPPORT_DIR"]

  func install(state: URL, support: URL) async {
    await AppModel.shared.initialRefresh?.value
    await CoreAgentClient.shared.disconnect()
    setenv("TSUGUMI_AGENT_STATE", state.path, 1)
    setenv("TSUGUMI_SUPPORT_DIR", support.path, 1)
  }

  func restore() async {
    await CoreAgentClient.shared.disconnect()
    if let previousState { setenv("TSUGUMI_AGENT_STATE", previousState, 1) }
    else { unsetenv("TSUGUMI_AGENT_STATE") }
    if let previousSupport { setenv("TSUGUMI_SUPPORT_DIR", previousSupport, 1) }
    else { unsetenv("TSUGUMI_SUPPORT_DIR") }
  }
}
