import Foundation
import XCTest
@testable import TsugumiVMRuntime

final class ConsoleFileTests: XCTestCase {
  func testDeliversAnAppendWithExistingBytes() throws {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try Data("early ".utf8).write(to: url)
    defer { try? FileManager.default.removeItem(at: url) }
    let initial = expectation(description: "read existing bytes")
    let complete = expectation(description: "read both parts of the line")
    let log = ConsoleLog()
    let reader = try ConsoleFile(url, queue: DispatchQueue(label: "console test")) { result in
      switch result {
      case .success(let bytes):
        if bytes == Data("early ".utf8) { initial.fulfill() }
        if !log.feed(bytes).isEmpty { complete.fulfill() }
      case .failure(let error):
        XCTFail("\(error)")
      }
    }
    defer { reader.cancel() }
    wait(for: [initial], timeout: 5)
    let writer = try FileHandle(forWritingTo: url)
    defer { try? writer.close() }
    try writer.seekToEnd()
    try writer.write(contentsOf: Data("boot\r\n".utf8))
    // The deadline only bounds a test failure when delivery never occurs.
    wait(for: [complete], timeout: 5)
    XCTAssertEqual(log.snapshot, ["early boot"])
  }
}
