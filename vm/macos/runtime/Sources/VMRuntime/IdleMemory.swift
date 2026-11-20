import Foundation

/// One VM's balloon policy. State and target changes run on the VM queue.
final class IdleMemory: @unchecked Sendable {
  static let idleBytes: UInt64 = 1536 << 20

  private let queue: DispatchQueue
  private let delay: DispatchTimeInterval
  private let fullBytes: UInt64
  private let setTarget: (UInt64) -> Void
  private let snapshot: @Sendable () async throws -> Void
  private var uses = 0
  private var deadline: DispatchWorkItem?
  private var needsSnapshot = false
  private var restoring: Task<Void, Error>?
  private var stopped = false

  init(queue: DispatchQueue, fullBytes: UInt64, delay: DispatchTimeInterval = .seconds(120),
       setTarget: @escaping (UInt64) -> Void, snapshot: @escaping @Sendable () async throws -> Void) {
    self.queue = queue
    self.delay = delay
    self.fullBytes = fullBytes
    self.setTarget = setTarget
    self.snapshot = snapshot
    schedule()
  }

  /// After idling, all new users wait for the same response from the guest.
  func hold() -> Task<Void, Error>? {
    uses += 1
    deadline?.cancel()
    deadline = nil
    if let restoring { return restoring }
    guard needsSnapshot else { return nil }
    setTarget(fullBytes)
    let restore = Task { [weak self, queue, snapshot] in
      defer { queue.sync { self?.restoring = nil } }
      try await snapshot()
      queue.sync { self?.needsSnapshot = false }
    }
    restoring = restore
    return restore
  }

  func release() {
    uses -= 1
    schedule()
  }

  func stop() {
    stopped = true
    deadline?.cancel()
    deadline = nil
  }

  private func schedule() {
    // Keep the page cache over a quick close and reopen of a project.
    guard uses == 0, !stopped else { return }
    let item = DispatchWorkItem { [weak self] in
      guard let self else { return }
      self.deadline = nil
      self.needsSnapshot = true
      self.setTarget(Self.idleBytes)
    }
    deadline = item
    queue.asyncAfter(deadline: .now() + delay, execute: item)
  }
}
