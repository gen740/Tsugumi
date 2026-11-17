// Standalone: swiftc -parse-as-library macOS/Sources/Launcher/AppTermination.swift
// tests/swift/app_termination_test.swift -o /tmp/app-termination-test
import Foundation

@MainActor final class Gate {
  var continuation: CheckedContinuation<Void, Never>?
  func wait() async { await withCheckedContinuation { continuation = $0 } }
  func release() { continuation?.resume(); continuation = nil }
}

@main struct TerminationTests {
  @MainActor static func until(_ predicate: () -> Bool) async {
    let deadline = ContinuousClock.now + .seconds(5)
    while !predicate() {
      precondition(ContinuousClock.now < deadline, "termination test timed out")
      await Task.yield()
    }
  }
  @MainActor static func main() async {
    // Idle quit asks nothing and waits for the stop acknowledgement.
    do {
      let controller = AppTermination(), stop = Gate()
      var stops = 0, replies = 0
      controller.begin(activity: { false }, localActivity: { false },
        confirm: { preconditionFailure("Idle quit asked") }, settleEdits: { true },
        stop: { stops += 1; await stop.wait() }, reply: { precondition($0); replies += 1 })
      await until { stops == 1 && stop.continuation != nil }
      precondition(replies == 0)
      // Repeated AppKit termination requests share the pending decision/stop.
      controller.begin(activity: { preconditionFailure("Duplicate quit queried") },
        localActivity: { false }, confirm: { true }, settleEdits: { true },
        stop: { preconditionFailure("Duplicate stop") }, reply: { _ in preconditionFailure("Duplicate reply") })
      stop.release()
      await until { replies == 1 }
    }
    // Work reported by another client, or a local save, gets exactly one alert.
    for externalWork in [true, false] {
      let controller = AppTermination()
      var questions = 0, stops = 0, replies = 0
      controller.begin(activity: { externalWork }, localActivity: { !externalWork },
        confirm: { questions += 1; return true },
        settleEdits: { preconditionFailure("A second edit alert after Quit and Stop") },
        stop: { stops += 1 }, reply: { precondition($0); replies += 1 })
      await until { replies == 1 }
      precondition(questions == 1 && stops == 1)
    }
    // Cancel does not stop anything, and the next Quit can be reconsidered.
    do {
      let controller = AppTermination()
      var replies = 0
      controller.begin(activity: { true }, localActivity: { false }, confirm: { false },
        settleEdits: { preconditionFailure("Cancel settled edits") },
        stop: { preconditionFailure("Cancel stopped runtime") }, reply: { precondition(!$0); replies += 1 })
      await until { replies == 1 }
      controller.begin(activity: { false }, localActivity: { false }, confirm: { true },
        settleEdits: { true }, stop: {}, reply: { precondition($0); replies += 1 })
      await until { replies == 2 }
    }
    // The existing unsaved-editor choice remains available when no work runs.
    do {
      let controller = AppTermination()
      var replies = 0
      controller.begin(activity: { false }, localActivity: { false },
        confirm: { preconditionFailure("Idle editor got work alert") }, settleEdits: { false },
        stop: { preconditionFailure("Cancelled editor stopped runtime") },
        reply: { precondition(!$0); replies += 1 })
      await until { replies == 1 }
    }
    // A host that went away during observation cannot establish idle state.
    do {
      let controller = AppTermination()
      var questions = 0, replies = 0
      controller.begin(activity: { throw CocoaError(.fileReadUnknown) }, localActivity: { false },
        confirm: { questions += 1; return false }, settleEdits: { true },
        stop: { preconditionFailure("Cancelled failed query stopped runtime") },
        reply: { precondition(!$0); replies += 1 })
      await until { replies == 1 }
      precondition(questions == 1)
    }
    // A blocked cancellation/stop RPC cannot extend a brief wait. Its late
    // acknowledgement must not complete the AppKit continuation twice.
    do {
      let blocked = Gate(), deadline = Gate()
      var completions = 0, operationReturned = false
      let wait = Task { @MainActor in
        let result: Bool? = await AppTermination.briefly(timeout: .seconds(3), wait: { _ in await deadline.wait() }) {
          await blocked.wait(); operationReturned = true; return true
        }
        precondition(result == nil)
        completions += 1
      }
      await until { blocked.continuation != nil && deadline.continuation != nil }
      deadline.release()
      await wait.value
      precondition(completions == 1)
      blocked.release()
      await until { operationReturned }
      precondition(completions == 1)
    }
    print("app termination: idle, activity, cancel, coalescing and bounded wait passed")
  }
}
