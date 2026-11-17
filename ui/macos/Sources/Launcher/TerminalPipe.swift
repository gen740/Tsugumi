import Darwin
import Foundation
@_exported import TsugumiVMTypes
import TsugumiAgentClient
import TsugumiRPC

/// The UI gets a local descriptor; terminal transport uses Host FlatBuffers.
final class TerminalPipe: @unchecked Sendable {
  let local: Int32
  private let remote: Int32
  private let lock = NSLock()
  private let output = DispatchQueue(label: "app.tsugumi.terminal-output")
  private var closed = false
  init(client: CoreHostClient, session: UInt64, ended: @escaping @Sendable () -> Void) throws {
    var pair: [Int32] = [-1, -1]
    guard socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0 else { throw LauncherError.notReady("cannot create terminal bridge") }
    local = pair[0]; remote = pair[1]
    for fd in pair { noSigPipe(fd); _ = fcntl(fd, F_SETFD, FD_CLOEXEC) }
    var timeout = timeval(tv_sec: 5, tv_usec: 0)
    setsockopt(remote, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    Thread.detachNewThread { [self] in
      defer {
        stop(); Darwin.close(remote); ended()
        Task { _ = try? await client.call("HostTerminalRequest", ["action": Tsugumi_Host_TerminalAction.close, "session": session]) }
      }
      var buffer = [UInt8](repeating: 0, count: 16384)
      while true {
        let n = Darwin.read(remote, &buffer, buffer.count)
        if n < 0, errno == EINTR { continue }
        if n <= 0 { break }
        let bytes = Array(buffer[..<n]), sent = DispatchSemaphore(value: 0)
        Task { _ = try? await client.call("HostTerminalRequest", ["action": Tsugumi_Host_TerminalAction.input, "session": session, "data": bytes]); sent.signal() }
        sent.wait()
      }
    }
  }
  func stop() { lock.withLock { if !closed { closed = true; shutdown(remote, SHUT_RDWR) } } }
  func receive(_ value: [String: Any]) {
    output.async { [self] in
      lock.withLock {
        guard !closed else { return }
        if let bytes = value["data"] as? [UInt8] { try? writeAll(remote, Data(bytes)) }
        if value["closed"] as? Bool == true { closed = true; shutdown(remote, SHUT_RDWR) }
      }
    }
  }
}

/// A write to the pipe whose peer went away fails (EPIPE) instead of killing
/// the app with SIGPIPE.
private func noSigPipe(_ fd: Int32) {
  var one: Int32 = 1
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
}

private struct TerminalWriteFailed: Error {}

private func writeAll(_ fd: Int32, _ data: Data) throws {
  try data.withUnsafeBytes { (buf: UnsafeRawBufferPointer) in
    var off = 0
    while off < buf.count {
      let n = write(fd, buf.baseAddress! + off, buf.count - off)
      if n <= 0 { throw TerminalWriteFailed() }
      off += n
    }
  }
}
