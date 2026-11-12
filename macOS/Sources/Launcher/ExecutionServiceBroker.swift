import CoreFoundation
import CryptoKit
import Darwin
import Foundation
import Virtualization

/// Bridges one framed request per connection from a project daemon to its
/// registered SSH gateway. SSH and the registry stay on the Mac.
final class ExecutionServiceBroker: NSObject, VZVirtioSocketListenerDelegate, @unchecked Sendable {
  private let support: URL
  private let listener = VZVirtioSocketListener()
  private let lock = NSLock()
  private struct Authorization {
    let token: String
    let scope: String
  }
  private var tokens: [String: Authorization] = [:]
  private var active = 0
  private static let operations = Set(["probe", "put_blob", "get_blob", "submit", "status", "cancel"])

  init(support: URL) {
    self.support = support
    super.init()
    listener.delegate = self
  }

  func install(on socket: VZVirtioSocketDevice) { socket.setSocketListener(listener, forPort: 1027) }

  func authorize(_ project: ProjectIdentity, control: URL) throws {
    let file = control.appendingPathComponent("execution-broker-token")
    guard let identity = project.executionScopeIdentity else {
      revoke(project.id)
      unlink(file.path)
      return
    }
    let token = UUID().uuidString + UUID().uuidString
    let secret = try scopeKey()
    let scope = HMAC<SHA256>.authenticationCode(for: Data(identity.utf8), using: secret)
      .map { String(format: "%02x", $0) }.joined()
    // The directory is private on the Mac and only this project sees its
    // virtiofs share. The daemon has a different uid inside the guest.
    try Data(token.utf8).write(to: file, options: .atomic)
    guard chmod(file.path, 0o644) == 0 else { throw POSIXError(.EACCES) }
    lock.withLock { tokens[project.id] = Authorization(token: token, scope: scope) }
  }

  func revoke(_ project: String) { lock.withLock { tokens[project] = nil } }
  func revokeAll() { lock.withLock { tokens.removeAll() } }

  private func scopeKey() throws -> SymmetricKey {
    let file = support.appendingPathComponent(".execution-broker-key")
    func readKey() throws -> SymmetricKey? {
      let fd = open(file.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
      if fd < 0, errno == ENOENT { return nil }
      guard fd >= 0 else { throw BrokerError("The execution broker key cannot be opened.") }
      defer { close(fd) }
      var st = stat()
      guard fstat(fd, &st) == 0, st.st_mode & S_IFMT == S_IFREG, st.st_uid == getuid(),
            st.st_mode & 0o077 == 0, st.st_size == 32 else {
        throw BrokerError("The execution broker key must be a private host file.")
      }
      return SymmetricKey(data: try Frame.exact(32, from: fd))
    }
    if let key = try readKey() { return key }
    let temporary = support.appendingPathComponent(".execution-broker-key-\(UUID().uuidString)")
    let fd = open(temporary.path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600)
    guard fd >= 0 else { throw BrokerError("The execution broker key cannot be created.") }
    defer { close(fd); unlink(temporary.path) }
    let key = SymmetricKey(size: .bits256)
    try writeAll(fd, key.withUnsafeBytes { Data($0) })
    guard fsync(fd) == 0 else { throw BrokerError("The execution broker key cannot be saved.") }
    guard link(temporary.path, file.path) == 0 || errno == EEXIST, let stored = try readKey() else {
      throw BrokerError("The execution broker key cannot be saved.")
    }
    return stored
  }

  func listener(_ listener: VZVirtioSocketListener, shouldAcceptNewConnection connection: VZVirtioSocketConnection,
                from socketDevice: VZVirtioSocketDevice) -> Bool {
    let admitted = lock.withLock {
      guard active < 64 else { return false }
      active += 1
      return true
    }
    guard admitted else { return false }
    let client = GuestConnection(connection)
    Thread.detachNewThread { [self, client] in
      defer { lock.withLock { active -= 1 } }
      let fd = client.connection.fileDescriptor
      noSigPipe(fd)
      var timeout = timeval(tv_sec: 60, tv_usec: 0)
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
      forward(fd) { entry, request, guest in
        _ = try Self.remote(entry, request: request, guest: guest)
      }
      Darwin.shutdown(fd, SHUT_RDWR)
    }
    return true
  }

  private final class GuestConnection: @unchecked Sendable {
    let connection: VZVirtioSocketConnection
    init(_ connection: VZVirtioSocketConnection) { self.connection = connection }
  }

  func forward(_ fd: Int32, send: (ExecutionServiceConnection, [String: Any], Int32) throws -> Void) {
    var forwarding = false
    do {
      var request = try Frame.read(from: fd)
      guard let token = request.removeValue(forKey: "broker_token") as? String,
            let scope = lock.withLock({ tokens.values.first(where: { $0.token == token })?.scope }) else {
        throw BrokerError("The project is not authorized to use this execution broker.")
      }
      let entries = try ExecutionServiceRegistry.load(support: support)
      if request["operation"] as? String == "catalog" {
        let services: [[String: Any]] = entries.map { entry in
          ["id": entry.id, "native_system": entry.nativeSystem ?? "", "systems": entry.systems ?? [],
           "available": entry.nativeSystem != nil]
        }
        try Frame.write(["version": 1, "ok": true, "services": services], to: fd)
        return
      }
      guard let operation = request["operation"] as? String, Self.operations.contains(operation) else {
        throw BrokerError("Unknown execution service operation.")
      }
      guard let entry = entries.first(where: { $0.id == request["service"] as? String }) else {
        throw BrokerError("The execution service is not registered.")
      }
      request["project_scope"] = scope
      forwarding = true
      try send(entry, request, fd)
    } catch {
      // EOF preserves an unconfirmed outcome after an SSH interruption.
      if !forwarding {
        let message = (error as? BrokerError)?.message ?? "The execution broker could not complete the request."
        try? Frame.write(["version": 1, "ok": false, "error": message], to: fd)
      }
    }
  }

  static func probe(_ entry: ExecutionServiceConnection) throws -> ExecutionServiceConnection {
    try ExecutionServiceRegistry.validate([entry])
    let reply = try remote(entry, request: ["version": 1, "operation": "probe", "service": entry.id])
    return try checkedProbe(entry, reply: reply)
  }

  static func checkedProbe(_ entry: ExecutionServiceConnection, reply: [String: Any]) throws -> ExecutionServiceConnection {
    guard reply["ok"] as? Bool == true, let native = reply["native_system"] as? String,
          let systems = reply["systems"] as? [String] else {
      throw BrokerError(reply["error"] as? String ?? "The execution service probe failed.")
    }
    let workerContract = 2
    let reported = reply["worker_contract"] as? NSNumber
    let actual = reported.flatMap { CFGetTypeID($0) == CFBooleanGetTypeID() ? nil : ($0 as? Int) }
    guard actual == workerContract else {
      throw BrokerError("SSH gateway worker contract is \(actual.map(String.init) ?? "unreported"). Expected \(workerContract). Rebuild the gateway with this Tsugumi revision.")
    }
    var checked = entry
    checked.nativeSystem = native
    checked.systems = systems
    try ExecutionServiceRegistry.validate([checked])
    return checked
  }

  static func sshArguments(_ entry: ExecutionServiceConnection) -> [String] {
    // The executable is a fixed name or a validated absolute path from
    // app settings. OpenSSH invokes the remote shell. Requests use stdin.
    let command = entry.gateway.map { "'\($0)'" } ?? "tsugumi-remote-gateway"
    return ["-T", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=10",
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
            "-o", "ClearAllForwardings=yes", "-o", "ForwardAgent=no", "--", entry.sshHost, command]
  }

  private static func remote(_ entry: ExecutionServiceConnection, request: [String: Any],
                             guest: Int32? = nil) throws -> [String: Any] {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/ssh")
    process.arguments = sshArguments(entry)
    let stdin = Pipe(), stdout = Pipe()
    process.standardInput = stdin
    process.standardOutput = stdout
    process.standardError = FileHandle.nullDevice
    try process.run()
    let timer = DispatchSource.makeTimerSource()
    // Bound idle operations while allowing an archive transfer to continue
    // as long as bytes are moving.
    func progress() { timer.schedule(deadline: .now() + 60) }
    progress()
    timer.setEventHandler { if process.isRunning { process.terminate() } }
    timer.resume()
    defer {
      timer.cancel()
      try? stdin.fileHandleForWriting.close()
      try? stdout.fileHandleForReading.close()
      if process.isRunning { process.terminate() }
      process.waitUntilExit()
    }
    let to = stdin.fileHandleForWriting.fileDescriptor
    _ = fcntl(to, F_SETNOSIGPIPE, 1)
    let from = stdout.fileHandleForReading.fileDescriptor
    try Frame.write(request, to: to)
    if let guest { try Frame.copy(try Frame.bodySize(request), from: guest, to: to, progress: progress) }
    try stdin.fileHandleForWriting.close()
    let reply: [String: Any]
    do { reply = try Frame.read(from: from) }
    catch { throw BrokerError("The SSH execution gateway did not reply.") }
    if let guest {
      progress()
      try Frame.write(reply, to: guest)
      try Frame.copy(try Frame.bodySize(reply), from: from, to: guest, progress: progress)
    }
    return reply
  }

  private struct BrokerError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
  }

  private enum Frame {
    static func bodySize(_ header: [String: Any]) throws -> Int {
      guard let size = (header["size"] ?? 0) as? Int, size >= 0 else {
        throw BrokerError("Invalid execution service body size.")
      }
      return size
    }

    static func read(from fd: Int32) throws -> [String: Any] {
      let length = try exact(4, from: fd).reduce(0) { ($0 << 8) | Int($1) }
      guard (1...65536).contains(length),
            let header = try JSONSerialization.jsonObject(with: exact(length, from: fd)) as? [String: Any],
            header["version"] as? Int == 1 else { throw BrokerError("Invalid execution service header.") }
      _ = try bodySize(header)
      return header
    }

    static func write(_ header: [String: Any], to fd: Int32) throws {
      let json = try JSONSerialization.data(withJSONObject: header)
      guard (1...65536).contains(json.count) else { throw BrokerError("Invalid execution service header size.") }
      var length = UInt32(json.count).bigEndian
      try writeAll(fd, withUnsafeBytes(of: &length) { Data($0) })
      try writeAll(fd, json)
    }

    static func copy(_ count: Int, from: Int32, to: Int32, progress: () -> Void) throws {
      var remaining = count
      while remaining > 0 {
        let size = min(65536, remaining)
        try writeAll(to, exact(size, from: from))
        remaining -= size
        progress()
      }
    }

    static func exact(_ count: Int, from fd: Int32) throws -> Data {
      var data = Data(count: count)
      try data.withUnsafeMutableBytes { (buffer: UnsafeMutableRawBufferPointer) in
        var offset = 0
        while offset < count {
          let size = Darwin.read(fd, buffer.baseAddress! + offset, count - offset)
          if size < 0, errno == EINTR { continue }
          guard size > 0 else { throw BrokerError("The execution service closed the connection before replying.") }
          offset += size
        }
      }
      return data
    }
  }
}

public extension ExecutionServiceRegistry {
  static func probe(_ entry: ExecutionServiceConnection) async throws -> ExecutionServiceConnection {
    try await Task.detached { try ExecutionServiceBroker.probe(entry) }.value
  }
}
