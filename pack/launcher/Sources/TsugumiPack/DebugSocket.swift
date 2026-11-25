import CoreFoundation
import Darwin
import Foundation

/// A debug export's local, newline-delimited JSON RPC endpoint.
@MainActor final class DebugSocket {
  typealias Handler = @MainActor (String, [String: Any]) async throws -> [String: Any]

  private let path: URL
  private let handler: Handler
  private var listener: DispatchSourceRead?
  private var socketIdentity: stat?
  private var peers: [UUID: Peer] = [:]
  private var requests: [UUID: Task<Void, Never>] = [:]
  private nonisolated static let maximumMessage = 16 << 20
  private static let maximumPeers = 8

  init(path: URL, handler: @escaping Handler) {
    self.path = path
    self.handler = handler
  }

  func start() throws {
    guard listener == nil else { return }
    var address = sockaddr_un()
    let name = Array(path.path.utf8CString)
    let offset = MemoryLayout<sockaddr_un>.offset(of: \.sun_path)!
    guard path.isFileURL, name.count <= MemoryLayout.size(ofValue: address.sun_path),
          !name.dropLast().contains(0) else {
      throw SocketError("Invalid debug socket path")
    }
    address.sun_family = sa_family_t(AF_UNIX)
    address.sun_len = UInt8(offset + name.count)
    withUnsafeMutableBytes(of: &address.sun_path) { $0.copyBytes(from: name.map(UInt8.init(bitPattern:))) }

    let directory = path.deletingLastPathComponent().path
    if mkdir(directory, 0o700) != 0, errno != EEXIST { throw Self.systemError() }
    var info = stat()
    guard lstat(directory, &info) == 0, info.st_mode & S_IFMT == S_IFDIR,
          info.st_uid == geteuid(), info.st_mode & 0o777 == 0o700 else {
      throw SocketError("Debug socket requires a user-owned directory with mode 0700")
    }

    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    guard fd >= 0 else { throw Self.systemError() }
    var started = false
    defer {
      if !started {
        close(fd)
        removeSocket()
      }
    }
    guard fcntl(fd, F_SETFD, FD_CLOEXEC) != -1, fcntl(fd, F_SETFL, O_NONBLOCK) != -1 else {
      throw Self.systemError()
    }
    let result = withUnsafePointer(to: &address) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
        bind(fd, $0, socklen_t(offset + name.count))
      }
    }
    // bind refuses an existing path, preserving an active listener or unrelated file.
    guard result == 0 else { throw Self.systemError() }
    guard lstat(path.path, &info) == 0 else { throw Self.systemError() }
    socketIdentity = info
    guard chmod(path.path, 0o600) == 0, listen(fd, Int32(Self.maximumPeers)) == 0 else {
      throw Self.systemError()
    }

    let source = DispatchSource.makeReadSource(fileDescriptor: fd, queue: .main)
    source.setCancelHandler { close(fd) }
    source.setEventHandler { [weak self] in
      MainActor.assumeIsolated { self?.acceptPeer(fd) }
    }
    listener = source
    started = true
    source.activate()
  }

  func stop() {
    listener?.cancel()
    listener = nil
    removeSocket()
    for id in Array(peers.keys) { disconnect(id) }
  }

  private func removeSocket() {
    guard var info = socketIdentity else { return }
    socketIdentity = nil
    let expected = info
    if lstat(path.path, &info) == 0, info.st_mode & S_IFMT == S_IFSOCK,
       info.st_dev == expected.st_dev, info.st_ino == expected.st_ino {
      unlink(path.path)
    }
  }

  private func acceptPeer(_ listener: Int32) {
    let fd = accept(listener, nil, nil)
    guard fd >= 0 else { return }
    var uid: uid_t = 0
    var gid: gid_t = 0
    var noSignal: Int32 = 1
    guard peers.count < Self.maximumPeers, getpeereid(fd, &uid, &gid) == 0, uid == geteuid(),
          fcntl(fd, F_SETFD, FD_CLOEXEC) != -1, fcntl(fd, F_SETFL, 0) != -1,
          setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, socklen_t(MemoryLayout<Int32>.size)) == 0 else {
      close(fd)
      return
    }
    let id = UUID()
    let peer = Peer(fd: fd)
    peers[id] = peer
    exchange(id, peer: peer)
  }

  private func exchange(_ id: UUID, peer: Peer, reply: Data? = nil) {
    peer.exchange(reply: reply) { [weak self] line in
      Task { @MainActor in
        guard let self, self.peers[id] != nil else { return }
        guard let line else { self.disconnect(id); return }
        self.requests[id] = Task { [weak self] in
          guard let self else { return }
          let reply = await self.reply(to: line)
          guard self.peers[id] != nil else { return }
          self.requests[id] = nil
          guard let reply else { self.disconnect(id); return }
          self.exchange(id, peer: peer, reply: reply)
        }
      }
    }
  }

  private func disconnect(_ id: UUID) {
    requests.removeValue(forKey: id)?.cancel()
    peers.removeValue(forKey: id)?.shutdownSocket()
  }

  private func reply(to line: Data) async -> Data? {
    guard let frame = (try? JSONSerialization.jsonObject(with: line)) as? [String: Any],
          let id = frame["request_id"] as? NSNumber, CFGetTypeID(id) != CFBooleanGetTypeID(),
          UInt64(id.stringValue) != nil, frame["payload_type"] as? String == "Request",
          let payload = frame["payload"] as? [String: Any],
          let method = payload["value_type"] as? String, method.hasSuffix("Request"),
          let body = payload["value"] as? [String: Any] else { return nil }

    func encode(_ kind: String, _ value: [String: Any]) -> Data? {
      let frame: [String: Any] = [
        "payload_type": "Reply", "request_id": id,
        "payload": ["value_type": kind, "value": value],
      ]
      guard JSONSerialization.isValidJSONObject(frame),
            var data = try? JSONSerialization.data(withJSONObject: frame),
            data.count <= Self.maximumMessage else { return nil }
      data.append(10)
      return data
    }

    do {
      let result = try await handler(method, body)
      let kind = method == "ViewActionRequest" ? "ViewActionResult" : String(method.dropLast("Request".count))
      if let reply = encode(kind, result) { return reply }
      throw SocketError("Debug RPC reply exceeded the message limit or is invalid JSON")
    } catch {
      return encode("Failure", ["code": "debug_rpc", "diagnostic": error.localizedDescription,
                                "message": error.localizedDescription])
    }
  }

  private static func systemError() -> POSIXError {
    POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
  }

  struct SocketError: LocalizedError {
    let errorDescription: String?
    init(_ message: String) { errorDescription = message }
  }

  /// The queue owns framing state. The immutable handle keeps the descriptor
  /// open until any read woken by shutdown has finished.
  private final class Peer: @unchecked Sendable {
    private let handle: FileHandle
    private let queue = DispatchQueue(label: "org.tsugumi.pack.debug.peer")
    private var pending = Data()

    init(fd: Int32) { handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true) }

    func shutdownSocket() { shutdown(handle.fileDescriptor, SHUT_RDWR) }

    func exchange(reply: Data?, complete: @escaping @Sendable (Data?) -> Void) {
      queue.async {
        if let reply, !self.write(reply) { complete(nil); return }
        complete(self.readLine())
      }
    }

    private func write(_ data: Data) -> Bool {
      data.withUnsafeBytes { bytes in
        guard let base = bytes.baseAddress else { return true }
        var offset = 0
        while offset < bytes.count {
          let count = Darwin.write(handle.fileDescriptor, base + offset, bytes.count - offset)
          if count < 0, errno == EINTR { continue }
          guard count > 0 else { return false }
          offset += count
        }
        return true
      }
    }

    private func readLine() -> Data? {
      var buffer = [UInt8](repeating: 0, count: 64 << 10)
      var scanStart = pending.startIndex
      while true {
        if let newline = pending[scanStart...].firstIndex(of: 10) {
          guard newline - pending.startIndex <= DebugSocket.maximumMessage else { return nil }
          let line = Data(pending[..<newline])
          pending.removeSubrange(...newline)
          return line
        }
        guard pending.count <= DebugSocket.maximumMessage else { return nil }
        scanStart = pending.endIndex
        let count = Darwin.read(handle.fileDescriptor, &buffer, buffer.count)
        if count < 0, errno == EINTR { continue }
        guard count > 0 else { return nil }
        pending.append(contentsOf: buffer.prefix(count))
      }
    }
  }
}
