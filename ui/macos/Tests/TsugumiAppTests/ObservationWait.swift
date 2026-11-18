import Foundation
import Observation
import Testing

/// Wait for a model change, with a watchdog only for a missing event. No tick
/// budget or settling delay: each predicate read subscribes to its dependencies.
@MainActor func waitForModel(_ reason: String = "model transition",
                            _ predicate: @escaping @MainActor () -> Bool) async throws {
  let updates = AsyncStream<Void>.makeStream(bufferingPolicy: .bufferingNewest(1))
  let watchdog = Task {
    try? await Task.sleep(for: .seconds(60))
    if !Task.isCancelled { updates.continuation.finish() }
  }
  defer { watchdog.cancel(); updates.continuation.finish() }
  var iterator = updates.stream.makeAsyncIterator()
  while true {
    let ready = withObservationTracking { predicate() } onChange: { updates.continuation.yield() }
    if ready { return }
    if await iterator.next() == nil {
      Issue.record("Timed out waiting for \(reason)")
      throw ModelWaitError.timeout
    }
  }
}
private enum ModelWaitError: Error { case timeout }
