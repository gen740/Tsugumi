import Foundation

/// One shutdown shared by RPC callers and signals. The deadline and a repeated
/// signal can force the VM even when the guest request itself is stuck.
public actor VMShutdown {
  private enum Phase { case idle, graceful, forcing }
  private var phase = Phase.idle
  private var signals = 0
  private var generation = UUID()
  private var work: Task<Void, Never>?
  private var deadline: Task<Void, Never>?
  private var waiters: [CheckedContinuation<Void, Never>] = []

  public init() {}

  public func stop(signal: Bool = false, timeout: Duration = .seconds(40),
                   graceful: @escaping @Sendable () async -> Void,
                   force: @escaping @Sendable () async -> Void) async {
    await withCheckedContinuation { continuation in
      if signal { signals += 1 }
      waiters.append(continuation)
      switch phase {
      case .idle:
        phase = .graceful
        generation = UUID()
        let id = generation
        work = Task { await graceful(); self.finished(id) }
        deadline = Task {
          do { try await Task.sleep(for: timeout) } catch { return }
          self.force(id, force)
        }
      case .graceful:
        if signals >= 2 { self.force(generation, force) }
      case .forcing: break
      }
    }
  }

  private func force(_ id: UUID, _ action: @escaping @Sendable () async -> Void) {
    guard generation == id, phase == .graceful else { return }
    phase = .forcing
    work?.cancel()
    deadline?.cancel()
    // Invalidate a late completion of the graceful task.
    generation = UUID()
    let forced = generation
    work = Task { await action(); self.finished(forced) }
  }

  private func finished(_ id: UUID) {
    guard generation == id else { return }
    deadline?.cancel()
    deadline = nil
    work = nil
    phase = .idle
    signals = 0
    let pending = waiters
    waiters.removeAll()
    for waiter in pending { waiter.resume() }
  }
}
