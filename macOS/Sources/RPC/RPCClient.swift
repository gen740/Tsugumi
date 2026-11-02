import FlatBuffers
import Foundation

public typealias Message = Tsugumi_RPC_MessageT
public typealias RequestPayload = Tsugumi_RPC_RequestPayload
public typealias ReplyPayload = Tsugumi_RPC_ReplyPayload
public typealias FailureKind = Tsugumi_Data_FailureKind

/// A structured failure returned by tsugumid (never matched by its text).
public struct RPCFailure: Error, CustomStringConvertible, Sendable {
  public let kind: FailureKind
  public let args: [String: String]
  public let diagnostic: String?
  public let currentRev: UInt64?
  public let retryAfterMs: UInt32?

  init(_ f: Tsugumi_Data_FailureT) {
    kind = f.kind
    var a: [String: String] = [:]
    for kv in f.args { if let kv { a[kv.key] = kv.value } }
    args = a
    diagnostic = f.diagnostic
    currentRev = f.currentRev
    retryAfterMs = f.retryAfterMs
  }

  public init(kind: FailureKind, diagnostic: String) {
    self.kind = kind
    args = [:]
    self.diagnostic = diagnostic
    currentRev = nil
    retryAfterMs = nil
  }

  public var description: String { "\(kind): \(diagnostic ?? "")" }
}

public enum RPCError: Error, CustomStringConvertible, Sendable {
  case disconnected
  case protocolViolation(String)
  case timeout
  case unexpectedReply(String)

  public var description: String {
    switch self {
    case .disconnected: return "the connection to tsugumid was lost"
    case .protocolViolation(let s): return "protocol violation: \(s)"
    case .timeout: return "no reply within the time limit"
    case .unexpectedReply(let s): return "unexpected reply: \(s)"
    }
  }
}

/// An incoming notification, already unpacked.
public struct NotificationEvent: @unchecked Sendable {
  public let subscriptionId: String
  public let seq: UInt64
  public let payload: Tsugumi_RPC_NotificationPayloadUnion
}

/// Connection to one tsugumid over WebSocket (`/rpc`, subprotocol tsugumi.rpc.v1).
/// Requests are multiplexed by request_id; replies resume the awaiting caller,
/// notifications are delivered through `notifications`.
public actor RPCClient {
  public nonisolated let notifications: AsyncStream<NotificationEvent>
  private let notify: AsyncStream<NotificationEvent>.Continuation
  private let task: URLSessionWebSocketTask
  private let session: URLSession
  private var nextId: UInt64 = 1
  private var waiting: [UInt64: CheckedContinuation<Tsugumi_RPC_ReplyT, Error>] = [:]
  private var rawWaiting: [UInt64: CheckedContinuation<Data, Error>] = [:]
  private var closed = false
  public private(set) var hello: Tsugumi_RPC_HelloResultT?

  public init(endpoint: URL) {
    var cont: AsyncStream<NotificationEvent>.Continuation!
    notifications = AsyncStream(bufferingPolicy: .unbounded) { cont = $0 }
    notify = cont
    let config = URLSessionConfiguration.ephemeral
    config.timeoutIntervalForRequest = 30
    session = URLSession(configuration: config)
    task = session.webSocketTask(with: endpoint, protocols: ["tsugumi.rpc.v1"])
    task.maximumMessageSize = 16 << 20
  }

  /// Opens the socket and performs SessionHello. The token never appears in URLs.
  public func connect(token: String, clientId: UUID, expectedProjectId: String?) async throws -> Tsugumi_RPC_HelloResultT {
    task.resume()
    Task { await self.receiveLoop() }
    let req = Tsugumi_RPC_SessionHelloRequestT()
    req.protocol_ = 1
    req.clientId = clientId.uuidString.lowercased()
    req.token = token
    req.expectedProjectId = expectedProjectId
    let result: Tsugumi_RPC_HelloResultT = try await call(.sessionhellorequest, req)
    hello = result
    return result
  }

  public func close() {
    guard !closed else { return }
    closed = true
    task.cancel(with: .normalClosure, reason: nil)
    failAll(RPCError.disconnected)
    notify.finish()
  }

  public var isClosed: Bool { closed }

  /// Sends one request and waits for its reply. Failure replies throw RPCFailure.
  public func call<R>(_ type: RequestPayload, _ value: NativeObject, timeout: Duration = .seconds(60)) async throws -> R {
    let reply = try await send(type, value, timeout: timeout)
    guard let v = reply.value?.value as? R else {
      throw RPCError.unexpectedReply("expected \(R.self), got \(String(describing: reply.value?.type))")
    }
    return v
  }

  public func send(_ type: RequestPayload, _ value: NativeObject, timeout: Duration) async throws -> Tsugumi_RPC_ReplyT {
    if closed { throw RPCError.disconnected }
    let id = nextId
    nextId += 1
    let req = Tsugumi_RPC_RequestT()
    req.value = Tsugumi_RPC_RequestPayloadUnion(value, type: type)
    let msg = Message()
    msg.requestId = id
    msg.payload = Tsugumi_RPC_PayloadUnion(req, type: .request)
    var builder = FlatBufferBuilder(initialSize: 1024)
    var m: Message? = msg
    let off = Tsugumi_RPC_Message.pack(&builder, obj: &m)
    Tsugumi_RPC_Message.finish(&builder, end: off)
    let data = builder.data
    let reply: Tsugumi_RPC_ReplyT = try await withCheckedThrowingContinuation { cont in
      waiting[id] = cont
      task.send(.data(data)) { error in
        if let error { Task { await self.resolve(id, .failure(error)) } }
      }
      Task {
        try? await Task.sleep(for: timeout)
        await self.resolve(id, .failure(RPCError.timeout))
      }
    }
    if reply.value?.type == .failure, let f = reply.value?.value as? Tsugumi_Data_FailureT {
      throw RPCFailure(f)
    }
    return reply
  }

  /// Sends a request whose table is written by `build` (used by the Agent's
  /// schema-driven tool adapter). Returns the reply union type and table.
  public func callRaw(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset,
                      timeout: Duration = .seconds(60)) async throws -> (UInt8, Table) {
    if closed { throw RPCError.disconnected }
    guard let type = Tsugumi_RPC_RequestPayload(rawValue: payloadType) else {
      throw RPCError.protocolViolation("unknown request type \(payloadType)")
    }
    let id = nextId
    nextId += 1
    var builder = FlatBufferBuilder(initialSize: 4096)
    let value = try build(&builder)
    let req = Tsugumi_RPC_Request.createRequest(&builder, valueType: type, valueOffset: value)
    let msg = Tsugumi_RPC_Message.createMessage(&builder, requestId: id, payloadType: .request, payloadOffset: req)
    Tsugumi_RPC_Message.finish(&builder, end: msg)
    let data = builder.data
    let raw: Data = try await withCheckedThrowingContinuation { cont in
      rawWaiting[id] = cont
      task.send(.data(data)) { error in
        if let error { Task { await self.resolveRaw(id, .failure(error)) } }
      }
      Task {
        try? await Task.sleep(for: timeout)
        await self.resolveRaw(id, .failure(RPCError.timeout))
      }
    }
    var bb = ByteBuffer(data: raw)
    let m: Tsugumi_RPC_Message = try getCheckedRoot(byteBuffer: &bb, fileId: "TSG1")
    guard let reply = m.payload(type: Tsugumi_RPC_Reply.self), let t: Tsugumi_RPC_Failure_or_Table = reply.rawValue else {
      throw RPCError.unexpectedReply("no reply value")
    }
    return (reply.valueType.rawValue, t.table)
  }

  private func resolveRaw(_ id: UInt64, _ result: Result<Data, Error>) {
    guard let cont = rawWaiting.removeValue(forKey: id) else { return }
    cont.resume(with: result)
  }

  private func resolve(_ id: UInt64, _ result: Result<Tsugumi_RPC_ReplyT, Error>) {
    guard let cont = waiting.removeValue(forKey: id) else { return }
    cont.resume(with: result)
  }

  private func failAll(_ error: Error) {
    let all = waiting
    waiting.removeAll()
    for (_, c) in all { c.resume(throwing: error) }
    let raw = rawWaiting
    rawWaiting.removeAll()
    for (_, c) in raw { c.resume(throwing: error) }
  }

  private func receiveLoop() async {
    while !closed {
      let message: URLSessionWebSocketTask.Message
      do {
        message = try await task.receive()
      } catch {
        closed = true
        failAll(RPCError.disconnected)
        notify.finish()
        return
      }
      guard case .data(let data) = message else {
        protocolError("text frames are not part of the protocol")
        return
      }
      var bb = ByteBuffer(data: data)
      let root: Tsugumi_RPC_Message
      do {
        root = try getCheckedRoot(byteBuffer: &bb, fileId: "TSG1")
      } catch {
        protocolError("reply does not verify: \(error)")
        return
      }
      let msg = Message(root)
      switch msg.payload?.type {
      case .reply:
        guard let id = msg.requestId, let reply = msg.payload?.value as? Tsugumi_RPC_ReplyT else {
          protocolError("reply without request_id")
          return
        }
        if rawWaiting[id] != nil {
          resolveRaw(id, .success(data))
          continue
        }
        if waiting[id] == nil {
          // A reply after our own timeout is dropped; an id we never sent is a violation.
          if id >= nextId { protocolError("reply to unknown request \(id)"); return }
          continue
        }
        resolve(id, .success(reply))
      case .notification:
        guard let n = msg.payload?.value as? Tsugumi_RPC_NotificationT, let p = n.value else { continue }
        notify.yield(NotificationEvent(subscriptionId: n.subscriptionId, seq: n.seq ?? 0, payload: p))
      default:
        protocolError("unexpected message type")
        return
      }
    }
  }

  private func protocolError(_ s: String) {
    closed = true
    task.cancel(with: .protocolError, reason: nil)
    failAll(RPCError.protocolViolation(s))
    notify.finish()
  }
}

public extension Tsugumi_Data_CardPathT {
  convenience init(_ segments: [String]) {
    self.init()
    self.segments = segments
  }

  var path: [String] { segments.compactMap { $0 } }
}

public func cardPath(_ segments: [String]) -> Tsugumi_Data_CardPathT { Tsugumi_Data_CardPathT(segments) }

/// Reads the reply union's table without unpacking it.
struct Tsugumi_RPC_Failure_or_Table: FlatbuffersInitializable {
  let table: Table
  init(_ bb: ByteBuffer, o: Int32) { table = Table(bb: bb, position: o) }
}

extension Tsugumi_RPC_Reply {
  var rawValue: Tsugumi_RPC_Failure_or_Table? { value(type: Tsugumi_RPC_Failure_or_Table.self) }
}

/// The published RPC schema (doc comments included) for schema-driven clients.
public enum RPCSchema {
  public static func bfbs() -> Data? {
    Bundle.module.url(forResource: "tsugumi", withExtension: "bfbs").flatMap { try? Data(contentsOf: $0) }
  }
}
