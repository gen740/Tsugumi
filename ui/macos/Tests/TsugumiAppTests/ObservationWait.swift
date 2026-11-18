import Foundation
import Observation
import Testing

/// Each predicate read subscribes to its observable dependencies.
@MainActor func waitForModel(_ reason: String = "model transition",
                            _ predicate: @escaping @MainActor () -> Bool) async throws {
  let updates = AsyncStream<Void>.makeStream(bufferingPolicy: .bufferingNewest(1))
  defer { updates.continuation.finish() }
  var iterator = updates.stream.makeAsyncIterator()
  while true {
    let ready = withObservationTracking { predicate() } onChange: { updates.continuation.yield() }
    if ready { return }
    if await iterator.next() == nil {
      Issue.record("Observation ended before \(reason)")
      throw CancellationError()
    }
  }
}
