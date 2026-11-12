import CoreFoundation
import CryptoKit
import Darwin
import Foundation
import Network
import Security
import Virtualization

/// Bridges one framed request per connection from a project daemon to its
/// registered execution daemon. Credentials and the registry stay on the Mac.
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
      forward(fd)
      Darwin.shutdown(fd, SHUT_RDWR)
    }
    return true
  }

  private final class GuestConnection: @unchecked Sendable {
    let connection: VZVirtioSocketConnection
    init(_ connection: VZVirtioSocketConnection) { self.connection = connection }
  }

  func forward(_ fd: Int32) {
    forward(fd) { entry, request, guest in
      _ = try Self.remote(entry, request: request, guest: guest)
    }
  }

  func forward(_ fd: Int32, send: (StoredExecutionService, [String: Any], Int32) throws -> Void) {
    var forwarding = false
    do {
      var request = try Frame.read(from: fd)
      guard let token = request.removeValue(forKey: "broker_token") as? String,
            let scope = lock.withLock({ tokens.values.first(where: { $0.token == token })?.scope }) else {
        throw BrokerError("The project is not authorized to use this execution broker.")
      }
      let entries = try ExecutionServiceRegistry.loadStored(support: support)
      if request["operation"] as? String == "catalog" {
        let services: [[String: Any]] = entries.map { entry in
          let caps = entry.capabilities
          return ["id": entry.id, "native_system": caps.nativeSystem, "systems": caps.systems,
                  "available": caps.workerContract == ExecutionServiceRegistry.workerContract,
                  "server_id": caps.serverId, "executor": "daemon", "isolation": caps.isolation,
                  "resource_limits": caps.resourceLimits, "max_jobs": caps.maxConcurrentJobs, "worker_contract": caps.workerContract]
        }
        try Frame.write(["version": 1, "ok": true, "services": services], to: fd)
        return
      }
      guard let operation = request["operation"] as? String, Self.operations.contains(operation) else {
        throw BrokerError("Unknown execution service operation.")
      }
      guard let entry = entries.first(where: { $0.id == request["service"] as? String }),
            request["server_id"] as? String == entry.capabilities.serverId,
            entry.capabilities.workerContract == ExecutionServiceRegistry.workerContract else {
        throw BrokerError("The execution service registration changed. Verify its connection in settings.", code: "registration_changed")
      }
      request["project_scope"] = scope
      request["server_id"] = entry.capabilities.serverId
      forwarding = true
      try send(entry, request, fd)
    } catch {
      // EOF preserves an unconfirmed outcome after a transport interruption.
      let failure = error as? BrokerError
      if !forwarding || failure?.code == "registration_changed" {
        var reply: [String: Any] = ["version": 1, "ok": false, "error": failure?.message ?? "The execution broker could not complete the request."]
        if let code = failure?.code { reply["code"] = code }
        try? Frame.write(reply, to: fd)
      }
    }
  }

  static func probe(_ entry: ExecutionServiceRegistration, token: String) throws -> ExecutionServiceCapabilities {
    let reply = try remote(endpoint: entry.endpoint, token: token, request: ["version": 1, "operation": "probe", "service": entry.id])
    return try checkedProbe(reply)
  }

  static func checkedProbe(_ reply: [String: Any]) throws -> ExecutionServiceCapabilities {
    guard reply["ok"] as? Bool == true else {
      throw BrokerError(reply["error"] as? String ?? "The execution service probe failed.")
    }
    guard reply["executor"] as? String == "daemon" else {
      throw BrokerError(String(localized: "接続先で Tsugumi の実行デーモンを起動してください。", bundle: .main))
    }
    let expected = ExecutionServiceRegistry.workerContract
    let actual = jsonInteger(reply["worker_contract"])
    guard actual == expected else {
      throw BrokerError("Execution daemon worker contract is \(actual.map(String.init) ?? "unreported"). Expected \(expected). Rebuild the execution daemon with this Tsugumi revision.")
    }
    guard let id = reply["server_id"] as? String, let native = reply["native_system"] as? String,
          let systems = reply["systems"] as? [String], let isolation = reply["isolation"] as? String,
          let limit = reply["resource_limits"] as? NSNumber, CFGetTypeID(limit) == CFBooleanGetTypeID(),
          let maxJobs = jsonInteger(reply["max_jobs"]) else {
      throw BrokerError(String(localized: "実行デーモンが有効な実行環境情報を返しませんでした。", bundle: .main))
    }
    let caps = ExecutionServiceCapabilities(serverId: id, nativeSystem: native, systems: systems, isolation: isolation,
                                            resourceLimits: limit.boolValue, maxConcurrentJobs: maxJobs, workerContract: expected)
    try ExecutionServiceRegistry.validate(caps)
    return caps
  }

  private static func jsonInteger(_ value: Any?) -> Int? {
    guard let value = value as? NSNumber, CFGetTypeID(value) != CFBooleanGetTypeID() else { return nil }
    return value as? Int
  }

  private static func remote(_ entry: StoredExecutionService, request: [String: Any], guest: Int32) throws -> [String: Any] {
    try remote(endpoint: entry.endpoint, token: entry.token, request: request, guest: guest)
  }

  private static func remote(endpoint: String, token: String, request: [String: Any], guest: Int32? = nil) throws -> [String: Any] {
    let url = try ExecutionServiceRegistry.endpointURL(endpoint)
    let transport = try HTTPTransport(endpoint: url)
    let prefix = try Frame.encode(request)
    let size = try Frame.bodySize(request)
    var fields = "POST /rpc HTTP/1.1\r\nHost: \(transport.authority)\r\nAuthorization: Bearer \(token)\r\nContent-Type: application/octet-stream\r\nContent-Length: \(prefix.count + size)\r\nConnection: close\r\n"
    let pin = request["server_id"] as? String
    if let pin { fields += "X-Tsugumi-Server: \(pin)\r\n" }
    if size > 0 { fields += "Expect: 100-continue\r\n" }
    try transport.send(Data((fields + "\r\n").utf8))
    if size > 0 { try transport.waitForContinue(pinned: pin != nil) }
    try transport.send(prefix)
    if let guest {
      var remaining = size
      while remaining > 0 {
        let chunk = try Frame.exact(min(65536, remaining), from: guest)
        try transport.send(chunk)
        remaining -= chunk.count
      }
    } else if size != 0 {
      throw BrokerError("The execution service request body is missing.")
    }

    let length = try transport.readResponseLength(rejectedBeforeBody: pin != nil && size == 0)
    var consumed = 0
    let reply = try Frame.read { size in
      consumed += size
      return try transport.exact(size)
    }
    var remaining = try Frame.bodySize(reply)
    guard consumed + remaining == length, guest != nil || remaining == 0 else {
      throw BrokerError("Invalid execution daemon response size.")
    }
    if let guest {
      try Frame.write(reply, to: guest)
      while remaining > 0 {
        let chunk = try transport.exact(min(65536, remaining))
        try writeAll(guest, chunk)
        remaining -= chunk.count
      }
    }
    return reply
  }

  /// One fixed-length HTTP request over the system TCP/TLS transport.
  private final class HTTPTransport {
    let authority: String
    private let connection: NWConnection
    private var pending = Data()

    init(endpoint: URL) throws {
      guard let parts = URLComponents(url: endpoint, resolvingAgainstBaseURL: false),
            let host = parts.host, let port = NWEndpoint.Port(rawValue: UInt16(parts.port ?? (parts.scheme == "https" ? 443 : 80))) else {
        throw BrokerError("Invalid execution daemon endpoint.")
      }
      authority = host + (parts.port.map { ":\($0)" } ?? "")
      let hostname = host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
      let parameters: NWParameters
      if parts.scheme == "https" {
        let tls = NWProtocolTLS.Options()
        sec_protocol_options_set_tls_server_name(tls.securityProtocolOptions, hostname)
        parameters = NWParameters(tls: tls, tcp: NWProtocolTCP.Options())
      } else {
        parameters = .tcp
      }
      connection = NWConnection(host: NWEndpoint.Host(hostname), port: port, using: parameters)
      do {
        try wait(seconds: 10) { (finish: @escaping (Result<Void, Error>) -> Void) in
          connection.stateUpdateHandler = { state in
            switch state {
            case .ready: finish(.success(()))
            case .failed(let error), .waiting(let error): finish(.failure(error))
            default: break
            }
          }
          connection.start(queue: .global(qos: .utility))
        }
        connection.stateUpdateHandler = nil
      } catch {
        connection.cancel()
        throw error
      }
    }

    deinit { connection.cancel() }

    private func wait<T>(seconds: Int = 60, begin: (@escaping (Result<T, Error>) -> Void) -> Void) throws -> T {
      let semaphore = DispatchSemaphore(value: 0)
      let lock = NSLock()
      var result: Result<T, Error>?
      begin { next in
        lock.withLock {
          guard result == nil else { return }
          result = next
          semaphore.signal()
        }
      }
      guard semaphore.wait(timeout: .now() + .seconds(seconds)) == .success else {
        connection.cancel()
        throw BrokerError("The execution daemon connection timed out.")
      }
      return try lock.withLock { try result!.get() }
    }

    func send(_ bytes: Data) throws {
      try wait { (finish: @escaping (Result<Void, Error>) -> Void) in
        connection.send(content: bytes, completion: .contentProcessed { error in
          if let error { finish(.failure(error)) } else { finish(.success(())) }
        })
      }
    }

    private func receive(maximum: Int) throws -> Data {
      try wait { finish in
        connection.receive(minimumIncompleteLength: 1, maximumLength: maximum) { bytes, _, _, error in
          if let bytes, !bytes.isEmpty { finish(.success(bytes)) }
          else { finish(.failure(error ?? BrokerError("The execution daemon closed the connection early."))) }
        }
      }
    }

    func exact(_ length: Int) throws -> Data {
      var result = Data()
      if !pending.isEmpty {
        let count = min(pending.count, length)
        result.append(pending.prefix(count))
        pending = Data(pending.dropFirst(count))
      }
      while result.count < length { result.append(try receive(maximum: length - result.count)) }
      return result
    }

    func waitForContinue(pinned: Bool) throws {
      let (code, _) = try readResponse()
      guard code == 100 else { throw Self.unexpectedStatus(code, rejectedBeforeBody: pinned) }
    }

    func readResponseLength(rejectedBeforeBody: Bool) throws -> Int {
      let (code, length) = try readResponse()
      guard code == 200 else { throw Self.unexpectedStatus(code, rejectedBeforeBody: rejectedBeforeBody) }
      guard let length else { throw BrokerError("The execution daemon did not report its response length.") }
      return length
    }

    private static func unexpectedStatus(_ code: Int, rejectedBeforeBody: Bool) -> BrokerError {
      if code == 409, rejectedBeforeBody {
        return BrokerError("The execution daemon at this endpoint has changed. Verify its connection in settings.", code: "registration_changed")
      }
      return BrokerError(String(localized: "実行デーモンへの接続に失敗しました（HTTP \(code)）。", bundle: .main))
    }

    private func readResponse() throws -> (Int, Int?) {
      let end = Data([13, 10, 13, 10])
      while pending.range(of: end) == nil {
        guard pending.count < 16 * 1024 else { throw BrokerError("The execution daemon HTTP headers are too large.") }
        pending.append(try receive(maximum: 4096))
      }
      guard let boundary = pending.range(of: end), boundary.lowerBound < 16 * 1024,
            let header = String(data: pending[..<boundary.lowerBound], encoding: .utf8) else {
        throw BrokerError("Invalid execution daemon HTTP response.")
      }
      pending = Data(pending[boundary.upperBound...])
      let lines = header.components(separatedBy: "\r\n")
      let status = lines[0].split(separator: " ", maxSplits: 2)
      guard status.count >= 2, status[0] == "HTTP/1.1" || status[0] == "HTTP/1.0", let code = Int(status[1]) else {
        throw BrokerError("Invalid execution daemon HTTP status.")
      }
      if code != 200 { return (code, nil) }
      var length: Int?
      for line in lines.dropFirst() {
        guard let colon = line.firstIndex(of: ":") else { throw BrokerError("Invalid execution daemon HTTP header.") }
        switch line[..<colon].lowercased() {
        case "content-length":
          let value = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
          guard length == nil, let parsed = Int(value), parsed >= 5, value.utf8.allSatisfy({ (48...57).contains($0) }) else {
            throw BrokerError("Invalid execution daemon response length.")
          }
          length = parsed
        case "transfer-encoding", "content-encoding":
          throw BrokerError("The execution daemon must send an unencoded, fixed-length frame.")
        default: break
        }
      }
      return (code, length)
    }
  }

  private struct BrokerError: LocalizedError {
    let message: String
    let code: String?
    init(_ message: String, code: String? = nil) { self.message = message; self.code = code }
    var errorDescription: String? { message }
  }

  private enum Frame {
    static func bodySize(_ header: [String: Any]) throws -> Int {
      guard let size = jsonInteger(header["size"] ?? 0), (0...(64 << 30)).contains(size) else {
        throw BrokerError("Invalid execution service body size.")
      }
      return size
    }

    static func read(from fd: Int32) throws -> [String: Any] {
      try read { try exact($0, from: fd) }
    }

    static func read(exact: (Int) throws -> Data) throws -> [String: Any] {
      let length = try exact(4).reduce(0) { ($0 << 8) | Int($1) }
      guard (1...65536).contains(length),
            let header = try JSONSerialization.jsonObject(with: exact(length)) as? [String: Any],
            jsonInteger(header["version"]) == 1 else { throw BrokerError("Invalid execution service header.") }
      _ = try bodySize(header)
      return header
    }

    static func encode(_ header: [String: Any]) throws -> Data {
      let json = try JSONSerialization.data(withJSONObject: header)
      guard (1...65536).contains(json.count) else { throw BrokerError("Invalid execution service header size.") }
      var length = UInt32(json.count).bigEndian
      return withUnsafeBytes(of: &length) { Data($0) } + json
    }

    static func write(_ header: [String: Any], to fd: Int32) throws {
      try writeAll(fd, encode(header))
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
