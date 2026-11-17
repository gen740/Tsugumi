import Foundation

/// Replies and notifications share one revision order, including the client's echo.
@MainActor
public final class DocumentRevisions {
  public private(set) var revision: UInt64 = 0
  private var pending: [UInt64: (Tsugumi_RPC_DocumentDeltaT, Bool)] = [:]
  private var waiting: [(UInt64, CheckedContinuation<Void, Error>)] = []

  public init() {}

  public func reset(_ revision: UInt64) {
    self.revision = revision
    pending = pending.filter { ($0.value.0.rev ?? 0) > revision }
    finish()
  }

  public func receive(_ delta: Tsugumi_RPC_DocumentDeltaT, fromSave: Bool = false,
                      apply: (Tsugumi_RPC_DocumentDeltaT, Bool) -> Void) {
    guard let rev = delta.rev, let base = delta.baseRev, rev > revision else { return }
    pending[base] = (delta, fromSave)
    drain(apply: apply)
  }

  public func drain(apply: (Tsugumi_RPC_DocumentDeltaT, Bool) -> Void) {
    while let (next, saved) = pending.removeValue(forKey: revision) {
      apply(next, saved)
      revision = next.rev!
      finish()
    }
  }

  public func applied(_ revision: UInt64) async throws {
    if self.revision >= revision { return }
    try await withCheckedThrowingContinuation { waiting.append((revision, $0)) }
  }

  public func disconnect(_ error: Error) {
    pending.removeAll()
    let callbacks = waiting
    waiting.removeAll()
    for (_, callback) in callbacks { callback.resume(throwing: error) }
  }

  private func finish() {
    let callbacks = waiting.filter { $0.0 <= revision }
    waiting.removeAll { $0.0 <= revision }
    for (_, callback) in callbacks { callback.resume() }
  }
}
