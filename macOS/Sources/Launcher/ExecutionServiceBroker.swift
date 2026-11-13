import FlatBuffers
import TsugumiRPC
import CoreFoundation
import CryptoKit
import Darwin
import Foundation
import Network
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
      try Frame.write(Self.remote(entry, request: request), to: guest)
    }
  }

  func forward(_ fd: Int32, send: (StoredExecutionService, Tsugumi_RemoteWorker_RequestT, Int32) throws -> Void) {
    var forwarding = false
    do {
      let message = try Frame.read(from: fd)
      guard message.payload?.type == .request,
            let request = message.payload?.value as? Tsugumi_RemoteWorker_RequestT,
            let token = request.token,
            let scope = lock.withLock({ tokens.values.first(where: { $0.token == token })?.scope }) else {
        throw BrokerError("The project is not authorized to use this execution broker.")
      }
      let entries = try ExecutionServiceRegistry.loadStored(support: support)
      if request.body?.type == .catalogrequest {
        let catalog = Tsugumi_RemoteWorker_CatalogT()
        catalog.services = entries.map { entry in
          let target = Tsugumi_RemoteWorker_TargetT()
          target.id = entry.id
          target.available = entry.capabilities.workerContract == ExecutionServiceRegistry.workerContract
          target.capabilities = Self.wireCapabilities(entry.capabilities)
          return target
        }
        try Frame.write(Self.reply(catalog, type: .catalog), to: fd)
        return
      }
      guard request.body != nil, request.body?.type != .none_ else { throw BrokerError("Missing remote worker request.") }
      guard let entry = entries.first(where: { $0.id == request.service }),
            request.serverId == entry.capabilities.serverId,
            entry.capabilities.workerContract == ExecutionServiceRegistry.workerContract else {
        throw BrokerError("The execution service registration changed. Verify its connection in settings.", code: .registrationchanged)
      }
      request.projectScope = scope
      request.serverId = entry.capabilities.serverId
      forwarding = true
      try send(entry, request, fd)
    } catch {
      let error = error as? BrokerError ?? BrokerError("The execution broker could not complete the request.")
      if !forwarding || error.code == .registrationchanged {
        let failure = Tsugumi_RemoteWorker_FailureT()
        failure.code = error.code
        failure.message = error.message
        try? Frame.write(Self.reply(failure, type: .failure), to: fd)
      }
    }
  }

  private static func reply(_ body: NativeObject, type: Tsugumi_RemoteWorker_ReplyBody) -> Tsugumi_RemoteWorker_MessageT {
    let reply = Tsugumi_RemoteWorker_ReplyT()
    reply.body = Tsugumi_RemoteWorker_ReplyBodyUnion(body, type: type)
    let message = Tsugumi_RemoteWorker_MessageT()
    message.payload = Tsugumi_RemoteWorker_PayloadUnion(reply, type: .reply)
    return message
  }

  private static func wireCapabilities(_ c: ExecutionServiceCapabilities) -> Tsugumi_RemoteWorker_CapabilitiesT {
    let result = Tsugumi_RemoteWorker_CapabilitiesT()
    result.serverId = c.serverId
    result.nativeSystem = c.nativeSystem
    result.systems = c.systems
    result.resourceLimits = c.resourceLimits
    result.maxJobs = UInt32(c.maxConcurrentJobs)
    result.workerContract = UInt32(c.workerContract)
    return result
  }

  static func probe(_ entry: ExecutionServiceRegistration, token: String) throws -> ExecutionServiceCapabilities {
    let request = Tsugumi_RemoteWorker_RequestT()
    request.body = Tsugumi_RemoteWorker_RequestBodyUnion(Tsugumi_RemoteWorker_ProbeRequestT(), type: .proberequest)
    return try checkedProbe(remote(endpoint: entry.endpoint, token: token, request: request))
  }

  static func checkedProbe(_ message: Tsugumi_RemoteWorker_MessageT) throws -> ExecutionServiceCapabilities {
    guard message.payload?.type == .reply, let reply = message.payload?.value as? Tsugumi_RemoteWorker_ReplyT else {
      throw BrokerError("Expected remote worker reply.")
    }
    if let failure = reply.body?.value as? Tsugumi_RemoteWorker_FailureT { throw BrokerError(failure.message, code: failure.code) }
    guard reply.body?.type == .capabilities, let c = reply.body?.value as? Tsugumi_RemoteWorker_CapabilitiesT else {
      throw BrokerError("The remote worker did not report its capabilities.")
    }
    let expected = ExecutionServiceRegistry.workerContract
    guard c.workerContract == expected else { throw BrokerError("Rebuild the remote worker with this Tsugumi revision (expected contract \(expected)).") }
    let caps = ExecutionServiceCapabilities(serverId: c.serverId, nativeSystem: c.nativeSystem, systems: c.systems.compactMap { $0 },
                                            isolation: "pid_namespace", resourceLimits: c.resourceLimits,
                                            maxConcurrentJobs: Int(c.maxJobs), workerContract: expected)
    try ExecutionServiceRegistry.validate(caps)
    return caps
  }

  private static func remote(_ entry: StoredExecutionService, request: Tsugumi_RemoteWorker_RequestT) throws -> Tsugumi_RemoteWorker_MessageT {
    try remote(endpoint: entry.endpoint, token: entry.token, request: request)
  }

  private static func remote(endpoint: String, token: String, request: Tsugumi_RemoteWorker_RequestT) throws -> Tsugumi_RemoteWorker_MessageT {
    let url = try ExecutionServiceRegistry.endpointURL(endpoint)
    let transport = try StreamTransport(endpoint: url)
    request.token = token
    let message = Tsugumi_RemoteWorker_MessageT()
    message.payload = Tsugumi_RemoteWorker_PayloadUnion(request, type: .request)
    try transport.send(Frame.encode(message))
    return try Frame.read { try transport.exact($0) }
  }

  /// The same framed stream can later run over a secure carrier.
  private final class StreamTransport {
    private let connection: NWConnection
    private var pending = Data()

    init(endpoint: URL) throws {
      let destination: NWEndpoint
      if endpoint.scheme == "unix" {
        destination = .unix(path: endpoint.path)
      } else {
        guard let parts = URLComponents(url: endpoint, resolvingAgainstBaseURL: false),
              let host = parts.host, let number = parts.port, let port = NWEndpoint.Port(rawValue: UInt16(exactly: number) ?? 0) else {
          throw BrokerError("Invalid remote worker endpoint.")
        }
        destination = .hostPort(host: .init(host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))), port: port)
      }
      connection = NWConnection(to: destination, using: .tcp)
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

  }

  private struct BrokerError: LocalizedError {
    let message: String
    let code: Tsugumi_RemoteWorker_ErrorCode
    init(_ message: String, code: Tsugumi_RemoteWorker_ErrorCode = .invalid) { self.message = message; self.code = code }
    var errorDescription: String? { message }
  }

  enum Frame {
    static func read(from fd: Int32) throws -> Tsugumi_RemoteWorker_MessageT { try read { try exact($0, from: fd) } }
    static func read(exact: (Int) throws -> Data) throws -> Tsugumi_RemoteWorker_MessageT {
      let prefix = try exact(4)
      let length = prefix.enumerated().reduce(0) { $0 | (Int($1.element) << (8 * $1.offset)) }
      guard (12...(2 << 20)).contains(length) else { throw BrokerError("Invalid remote worker frame length.") }
      var buffer = ByteBuffer(data: try exact(length))
      let root: Tsugumi_RemoteWorker_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSR1")
      return root.unpack()
    }
    static func encode(_ message: Tsugumi_RemoteWorker_MessageT) throws -> Data {
      var builder = FlatBufferBuilder(initialSize: 1024)
      var object: Tsugumi_RemoteWorker_MessageT? = message
      let offset = Tsugumi_RemoteWorker_Message.pack(&builder, obj: &object)
      Tsugumi_RemoteWorker_Message.finish(&builder, end: offset)
      let bytes = Data(builder.sizedByteArray)
      guard bytes.count <= 2 << 20 else { throw BrokerError("Remote worker frame exceeds 2 MiB.") }
      var length = UInt32(bytes.count).littleEndian
      return withUnsafeBytes(of: &length) { Data($0) } + bytes
    }
    static func write(_ message: Tsugumi_RemoteWorker_MessageT, to fd: Int32) throws { try writeAll(fd, encode(message)) }
    static func exact(_ count: Int, from fd: Int32) throws -> Data {
      var data = Data(count: count)
      try data.withUnsafeMutableBytes { (buffer: UnsafeMutableRawBufferPointer) in
        var offset = 0
        while offset < count {
          let n = Darwin.read(fd, buffer.baseAddress! + offset, count - offset)
          if n < 0, errno == EINTR { continue }
          guard n > 0 else { throw BrokerError("The remote worker closed the stream before replying.") }
          offset += n
        }
      }
      return data
    }
  }
}
