import Foundation
import XCTest
@testable import TsugumiVMRuntime

final class IdleMemoryTests: XCTestCase {
  func testLastUserReleasesMemoryAndNewUsersShareTheRestore() async throws {
    let queue = DispatchQueue(label: "idle memory test")
    let fullBytes: UInt64 = 4 << 30
    let firstIdle = expectation(description: "memory returned on initial idle")
    let secondIdle = expectation(description: "memory returned after all users release it")
    let sampleRequested = expectation(description: "guest memory requested")
    sampleRequested.assertForOverFulfill = true
    let (samples, guest) = AsyncStream<Void>.makeStream()
    var targets: [UInt64] = []
    let memory = queue.sync {
      IdleMemory(queue: queue, fullBytes: fullBytes, delay: .seconds(0), setTarget: { target in
        targets.append(target)
        if targets == [IdleMemory.idleBytes] { firstIdle.fulfill() }
        if targets == [IdleMemory.idleBytes, fullBytes, IdleMemory.idleBytes] { secondIdle.fulfill() }
      }, snapshot: {
        sampleRequested.fulfill()
        for await _ in samples { return }
      })
    }
    defer { guest.finish(); queue.sync { memory.stop() } }
    // The deadline bounds the test if a transition never arrives.
    await fulfillment(of: [firstIdle], timeout: 5)
    let (project, store) = queue.sync { (memory.hold(), memory.hold()) }
    let projectRestore = try XCTUnwrap(project)
    let storeRestore = try XCTUnwrap(store)
    await fulfillment(of: [sampleRequested], timeout: 5)
    queue.sync {
      XCTAssertEqual(targets, [IdleMemory.idleBytes, fullBytes])
    }
    guest.yield()
    try await projectRestore.value
    try await storeRestore.value
    queue.sync { memory.release() }
    // Another use before the last release still has full memory.
    queue.sync {
      XCTAssertNil(memory.hold())
      memory.release()
      memory.release()
    }
    await fulfillment(of: [secondIdle], timeout: 5)
  }
}
