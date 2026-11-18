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
      // A disconnected host cannot confirm that other clients are idle.
      let observed = (try? await activity()) ?? true
      let busy = localActivity() || observed
      let allowed: Bool
      if busy { allowed = confirm() }
      else { allowed = await settleEdits() }
      if allowed { await stop() }
      pending = false
      reply(allowed)
    }
  }
}
