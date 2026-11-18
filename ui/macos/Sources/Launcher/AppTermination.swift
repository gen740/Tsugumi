import Foundation
@_exported import TsugumiVMTypes

/// AppKit owns the final reply. Coalesce repeated Quit requests while the
/// activity snapshot, the person's decision and the stop request are pending.
@MainActor
public final class AppTermination {
  private var pending = false
  public init() {}

  public func begin(activity: @escaping () async throws -> Bool,
                    localActivity: @escaping () -> Bool,
                    confirm: @escaping () -> Bool,
                    settleEdits: @escaping () async -> Bool,
                    stop: @escaping () async -> Void,
                    reply: @escaping (Bool) -> Void) {
    guard !pending else { return }
    pending = true
    Task {
      // A restarting or unresponsive host cannot establish that other clients
      // are idle. Ask once in that case too; never wait indefinitely on a read.
      let observed = await Self.briefly(timeout: .seconds(3)) {
        (try? await activity()) ?? true
      } ?? true
      let busy = localActivity() || observed
      let allowed: Bool
      if busy { allowed = confirm() }
      else { allowed = await settleEdits() }
      if allowed { await stop() }
      pending = false
      reply(allowed)
    }
  }

  /// An unstructured task bounds the wait even when an RPC ignores cancellation.
  /// A structured task group would wait for that RPC when leaving its scope.
  public static func briefly<T>(timeout: Duration,
                                wait: @escaping (Duration) async throws -> Void = { try await Task.sleep(for: $0) },
                                operation: @escaping @MainActor () async -> T) async -> T? {
    await withCheckedContinuation { continuation in
      var finished = false
      let work = Task { @MainActor in
        let value = await operation()
        guard !finished else { return }
        finished = true
        continuation.resume(returning: value)
      }
      Task { @MainActor in
        try? await wait(timeout)
        guard !finished else { return }
        finished = true
        work.cancel()
        continuation.resume(returning: nil)
      }
    }
  }
}
