import Darwin
import Foundation
import TsugumiLauncher
import XCTest

final class ExecutionBrokerBridgeTests: XCTestCase {
  func testByteRelayPreservesBinaryPayloadAndHalfClose() throws {
    var guest: [Int32] = [-1, -1]
    var host: [Int32] = [-1, -1]
    XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &guest), 0)
    XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &host), 0)
    defer { Darwin.close(guest[0]); Darwin.close(host[0]) }
    for fd in [guest[0], host[0]] {
      var timeout = timeval(tv_sec: 2, tv_usec: 0)
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    }
    // The VZ adapter uses this byte relay; it never decodes remote_worker.fbs.
    VMHostIPC.relay(guest[1], host[1])
    let request = Data([0, 255, 1, 0, 128])
    XCTAssertEqual(request.withUnsafeBytes { Darwin.write(guest[0], $0.baseAddress, $0.count) }, request.count)
    Darwin.shutdown(guest[0], SHUT_WR)
    XCTAssertEqual(try readToEnd(host[0]), request)
    let reply = Data([255, 0, 2])
    XCTAssertEqual(reply.withUnsafeBytes { Darwin.write(host[0], $0.baseAddress, $0.count) }, reply.count)
    Darwin.shutdown(host[0], SHUT_WR)
    XCTAssertEqual(try readToEnd(guest[0]), reply)
  }

  private func readToEnd(_ fd: Int32) throws -> Data {
    var result = Data()
    var buffer = [UInt8](repeating: 0, count: 1024)
    while true {
      let n = Darwin.read(fd, &buffer, buffer.count)
      if n < 0, errno == EINTR { continue }
      if n < 0 { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
      if n == 0 { return result }
      result.append(contentsOf: buffer.prefix(n))
    }
  }
}
