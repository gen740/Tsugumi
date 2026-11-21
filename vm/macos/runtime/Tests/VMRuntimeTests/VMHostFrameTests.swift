import Darwin
import Foundation
import XCTest
@testable import TsugumiVMRuntime
import TsugumiVMTypes

/// Frames on the helper link are bounded before anything is allocated.
final class VMHostFrameTests: XCTestCase {
  private var reader: Int32 = -1
  private var writer: Int32 = -1

  override func setUpWithError() throws {
    var pair: [Int32] = [-1, -1]
    guard socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0 else { throw POSIXError(.EIO) }
    (reader, writer) = (pair[0], pair[1])
    for fd in pair { var on: Int32 = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size)) }
  }

  override func tearDown() {
    if reader >= 0 { Darwin.close(reader) }
    if writer >= 0 { Darwin.close(writer) }
  }

  private func send(prefix size: UInt32, _ body: [UInt8] = []) {
    var little = size.littleEndian
    let bytes = withUnsafeBytes(of: &little) { Array($0) } + body
    XCTAssertEqual(Darwin.write(writer, bytes, bytes.count), bytes.count)
  }

  private func closeWriter() {
    Darwin.close(writer)
    writer = -1
  }

  func testAFrameExactlyAtTheLimitPasses() throws {
    let payload = Data((0..<64).map { UInt8($0) })
    try VMHostIPC.write(payload, to: writer, maxSize: 64)
    XCTAssertEqual(try VMHostIPC.read(reader, maxSize: 64), payload)
    closeWriter()
    XCTAssertNil(try VMHostIPC.read(reader, maxSize: 64))
  }

  func testALengthOneByteOverTheLimitIsRejectedWithoutReadingItsBody() throws {
    // The writer stays open with no body: reading one would block.
    send(prefix: 65)
    XCTAssertThrowsError(try VMHostIPC.read(reader, maxSize: 64)) { error in
      guard case LauncherError.notReady(let message) = error else { return XCTFail("\(error)") }
      XCTAssertTrue(message.contains("limit of 64"), message)
    }
    XCTAssertThrowsError(try VMHostIPC.write(Data(count: 65), to: writer, maxSize: 64))
  }

  func testTheDefaultLimitRejectsAFourGiBLength() throws {
    send(prefix: UInt32.max)
    XCTAssertThrowsError(try VMHostIPC.read(reader))
    send(prefix: UInt32(VMHostIPC.maxFrameBytes + 1))
    XCTAssertThrowsError(try VMHostIPC.read(reader))
  }

  func testATruncatedFrameIsAnError() throws {
    send(prefix: 10, [1, 2, 3])
    closeWriter()
    XCTAssertThrowsError(try VMHostIPC.read(reader, maxSize: 64))
  }

  func testATruncatedPrefixIsAnError() throws {
    XCTAssertEqual(Darwin.write(writer, [7, 0], 2), 2)
    closeWriter()
    XCTAssertThrowsError(try VMHostIPC.read(reader, maxSize: 64))
  }

  func testAnEmptyFrameIsAnError() throws {
    send(prefix: 0)
    XCTAssertThrowsError(try VMHostIPC.read(reader, maxSize: 64))
    XCTAssertThrowsError(try VMHostIPC.write(Data(), to: writer, maxSize: 64))
  }
}
