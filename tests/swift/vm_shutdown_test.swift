// Standalone test: swiftc -parse-as-library macOS/Sources/Launcher/VMShutdown.swift
// tests/swift/vm_shutdown_test.swift -o /tmp/vm-shutdown-test && /tmp/vm-shutdown-test
import Foundation

actor FakeVM {
  var gracefulCalls = 0
  var forceCalls = 0
  private var parked: [CheckedContinuation<Void, Never>] = []
  func graceful() async {
    gracefulCalls += 1
    await withCheckedContinuation { parked.append($0) }
  }
  func force() { forceCalls += 1 }
  func release() { let pending = parked; parked = []; for p in pending { p.resume() } }
  func counts() -> (Int, Int) { (gracefulCalls, forceCalls) }
}

@main struct ShutdownTests {
  static func wait(_ predicate: () async -> Bool) async {
    let deadline = ContinuousClock.now + .seconds(3)
    while !(await predicate()) {
      precondition(ContinuousClock.now < deadline, "shutdown test timed out")
      await Task.yield()
    }
  }
  static func stop(_ controller: VMShutdown, _ vm: FakeVM, signal: Bool = false,
                   timeout: Duration = .seconds(5)) -> Task<Void, Never> {
    Task { await controller.stop(signal: signal, timeout: timeout,
                                 graceful: { await vm.graceful() }, force: { await vm.force() }) }
  }
  static func main() async {
    // Concurrent RPCs share a single graceful operation.
    do {
      let controller = VMShutdown(), vm = FakeVM()
      let first = stop(controller, vm)
      await wait { await vm.counts().0 == 1 }
      let second = stop(controller, vm)
      try? await Task.sleep(for: .milliseconds(20))
      await vm.release()
      await first.value; await second.value
      let counts = await vm.counts()
      precondition(counts.0 == 1 && counts.1 == 0)
    }
    // The first signal joins an RPC shutdown; only the second signal forces.
    do {
      let controller = VMShutdown(), vm = FakeVM()
      let rpc = stop(controller, vm)
      await wait { await vm.counts().0 == 1 }
      let signal = stop(controller, vm, signal: true)
      try? await Task.sleep(for: .milliseconds(20))
      let beforeSecondSignal = await vm.counts()
      precondition(beforeSecondSignal.1 == 0)
      let again = stop(controller, vm, signal: true)
      await wait { await vm.counts().1 == 1 }
      await rpc.value; await signal.value; await again.value
      await vm.release() // late completion must not finish a newer generation
      let counts = await vm.counts()
      precondition(counts.0 == 1)
    }
    // A stuck guest request cannot hold the deadline open.
    do {
      let controller = VMShutdown(), vm = FakeVM()
      let timed = stop(controller, vm, timeout: .milliseconds(20))
      await timed.value
      let counts = await vm.counts()
      precondition(counts.0 == 1 && counts.1 == 1)
      let restart = stop(controller, vm)
      await wait { await vm.counts().0 == 2 }
      await vm.release()
      await restart.value
      let after = await vm.counts()
      precondition(after.1 == 1)
    }
    print("VMShutdown: coalescing, repeated signals, deadline and restart passed")
  }
}
