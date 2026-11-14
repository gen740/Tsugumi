import FlatBuffers
import Foundation
import TsugumiClientC

public typealias Message = Tsugumi_RPC_MessageT
public typealias RequestPayload = Tsugumi_RPC_RequestPayload
public typealias ReplyPayload = Tsugumi_RPC_ReplyPayload
public typealias FailureKind = Tsugumi_Data_FailureKind

/// The generated failure kinds are plain values (the generated file cannot
/// declare it, so it is asserted here).
extension Tsugumi_Data_FailureKind: @unchecked Sendable {}

/// A structured failure returned by tsugumid (never matched by its text).
public struct RPCFailure: Error, CustomStringConvertible, Sendable {
  public let kind: FailureKind
  public let args: [String: String]
  public let permissions: [String]
  public let diagnostic: String?
  public let currentRev: UInt64?
  public let retryAfterMs: UInt32?

  init(_ f: Tsugumi_Data_FailureT) {
    kind = f.kind
    var a: [String: String] = [:]
    for kv in f.args { if let kv { a[kv.key] = kv.value } }
    args = a
    permissions = f.args.compactMap { $0 }.filter { $0.key == "permission" }.map(\.value)
    diagnostic = f.diagnostic
    currentRev = f.currentRev
    retryAfterMs = f.retryAfterMs
  }

  public init(kind: FailureKind, diagnostic: String) {
    self.kind = kind
    args = [:]
    permissions = []
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

private func nativeRPCError(_ code: Int32, _ message: UnsafePointer<CChar>?) -> RPCError {
  let text = message.map(String.init(cString:)) ?? "RPC transport failed"
  switch code {
  case 2: return .timeout
  case 3: return .protocolViolation(text)
  case 5, 6: return .unexpectedReply(text)
  default: return .disconnected
  }
}

private final class NativeReply {
  let continuation: CheckedContinuation<Data, Error>
  init(_ continuation: CheckedContinuation<Data, Error>) { self.continuation = continuation }
}

private let nativeReplyCallback: tsugumi_client_callback = { context, bytes, count, code, message in
  guard let context else { return }
  let reply = Unmanaged<NativeReply>.fromOpaque(context).takeRetainedValue()
  if code != 0 { reply.continuation.resume(throwing: nativeRPCError(code, message)) }
  else { reply.continuation.resume(returning: bytes.map { Data(bytes: $0, count: count) } ?? Data()) }
}

private final class NativeEvents {
  var receive: ((Data) -> Void)?
  var closed: (() -> Void)?
}

/// This bridge only owns buffers and Swift continuations. The C++ library owns
/// framing, validation, request correlation, deadlines and disconnect handling.
public final class NativeByteTransport: @unchecked Sendable {
  private let handle: OpaquePointer?
  private let events = NativeEvents()

  public enum Contract: Int32 { case project = 0, agent = 1, host = 2 }
  public init(endpoint: String, contract: Contract = .project) { handle = tsugumi_client_create(endpoint, contract.rawValue) }
  deinit { tsugumi_client_destroy(handle) }

  public func observe(receive: @escaping (Data) -> Void, closed: @escaping () -> Void) {
    events.receive = receive
    events.closed = closed
    let context = Unmanaged.passUnretained(events).toOpaque()
    tsugumi_client_on_event(handle, { context, bytes, count, _, _ in
      guard let context, let bytes else { return }
      Unmanaged<NativeEvents>.fromOpaque(context).takeUnretainedValue().receive?(Data(bytes: bytes, count: count))
    }, context)
    tsugumi_client_on_closed(handle, { context, _, _, _, _ in
      guard let context else { return }
      Unmanaged<NativeEvents>.fromOpaque(context).takeUnretainedValue().closed?()
    }, context)
  }

  public func start() async throws {
    guard let handle else { throw RPCError.disconnected }
    let _: Data = try await withCheckedThrowingContinuation { continuation in
      tsugumi_client_start(handle, nativeReplyCallback, Unmanaged.passRetained(NativeReply(continuation)).toOpaque())
    }
  }

  public func call(_ data: Data, timeout: Duration, continuation: CheckedContinuation<Data, Error>) {
    guard let handle else { continuation.resume(throwing: RPCError.disconnected); return }
    let parts = timeout.components
    let milliseconds = Double(parts.seconds) * 1000 + Double(parts.attoseconds) / 1e15
    guard milliseconds > 0 else { continuation.resume(throwing: RPCError.timeout); return }
    let bounded = UInt32(min(Double(UInt32.max), max(1, milliseconds)))
    data.withUnsafeBytes { bytes in
      tsugumi_client_call(handle, bytes.bindMemory(to: UInt8.self).baseAddress, data.count, bounded,
                          nativeReplyCallback, Unmanaged.passRetained(NativeReply(continuation)).toOpaque())
    }
  }

  public func close() { tsugumi_client_close(handle) }
}

/// Typed operations on the shared C++ client. Both native socket transports and
/// the browser's Wasm build use the same connection implementation.
public actor RPCClient {
  public nonisolated let notifications: AsyncStream<NotificationEvent>
  private let notify: AsyncStream<NotificationEvent>.Continuation
  private let transport: NativeByteTransport
  private var nextId: UInt64 = 1
  private var closed = false
  private var started = false
  private var receiveTask: Task<Void, Never>?
  public private(set) var hello: Tsugumi_RPC_HelloResultT?

  public init(endpoint: URL) {
    var cont: AsyncStream<NotificationEvent>.Continuation!
    notifications = AsyncStream(bufferingPolicy: .unbounded) { cont = $0 }
    notify = cont
    transport = NativeByteTransport(endpoint: endpoint.absoluteString)
  }

  /// Opens the socket and performs SessionHello. The token never appears in URLs.
  public func connect(token: String, clientId: UUID, expectedProjectId: String?) async throws -> Tsugumi_RPC_HelloResultT {
    guard !started, !closed else { throw RPCError.disconnected }
    started = true
    // Native callbacks are ordered. A separate Task for every notification can
    // reach this actor out of order, creating false sequence gaps in clients.
    let incoming = AsyncStream<Data>.makeStream(bufferingPolicy: .unbounded)
    transport.observe(receive: { incoming.continuation.yield($0) },
                      closed: { incoming.continuation.finish() })
    receiveTask = Task.detached { [weak self, stream = incoming.stream] in
      for await data in stream {
        guard !Task.isCancelled else { break }
        await self?.receiveEvent(data)
      }
      await self?.closedByPeer()
    }
    try await transport.start()
    let req = Tsugumi_RPC_SessionHelloRequestT()
    req.protocol_ = 1
    req.clientId = clientId.uuidString.lowercased()
    req.token = token
    req.expectedProjectId = expectedProjectId
    req.language = Bundle.main.preferredLocalizations.first?.hasPrefix("en") == true ? "en" : "ja"
    let result: Tsugumi_RPC_HelloResultT = try await call(.sessionhellorequest, req)
    hello = result
    return result
  }

  public func close() {
    guard !closed else { return }
    closed = true
    transport.close()
    receiveTask?.cancel()
    receiveTask = nil
    notify.finish()
  }

  private func closedByPeer() { closed = true; receiveTask = nil; notify.finish() }

  public func call<R>(_ type: RequestPayload, _ value: NativeObject, timeout: Duration = .seconds(60)) async throws -> R {
    let reply = try await send(type, value, timeout: timeout)
    guard let value = reply.value?.value as? R else {
      throw RPCError.unexpectedReply("expected \(R.self), got \(String(describing: reply.value?.type))")
    }
    return value
  }

  private func exchange(_ data: Data, timeout: Duration) async throws -> Data {
    try await withCheckedThrowingContinuation { continuation in
      // Submit while still on this actor so IDs preserve their issue order.
      transport.call(data, timeout: timeout, continuation: continuation)
    }
  }

  private func requestId() throws -> UInt64 {
    guard !closed, nextId < UInt64.max else { throw RPCError.disconnected }
    let id = nextId
    nextId += 1
    return id
  }

  public func send(_ type: RequestPayload, _ value: NativeObject, timeout: Duration) async throws -> Tsugumi_RPC_ReplyT {
    let id = try requestId()
    let request = Tsugumi_RPC_RequestT()
    request.value = Tsugumi_RPC_RequestPayloadUnion(value, type: type)
    let message = Message()
    message.requestId = id
    message.payload = Tsugumi_RPC_PayloadUnion(request, type: .request)
    var builder = FlatBufferBuilder(initialSize: 1024)
    var object: Message? = message
    let offset = Tsugumi_RPC_Message.pack(&builder, obj: &object)
    Tsugumi_RPC_Message.finish(&builder, end: offset)
    let data = try await exchange(builder.data, timeout: timeout)
    var buffer = ByteBuffer(data: data)
    let root: Tsugumi_RPC_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSG1")
    let reply: Tsugumi_RPC_ReplyT?
    if let raw = root.payload(type: Tsugumi_RPC_Reply.self), raw.valueType == .recordresult,
       let record = raw.value(type: Tsugumi_RPC_RecordResult.self) {
      let decoded = Tsugumi_RPC_ReplyT()
      decoded.value = Tsugumi_RPC_ReplyPayloadUnion(unpackRecordResult(record), type: .recordresult)
      reply = decoded
    } else { reply = Message(root).payload?.value as? Tsugumi_RPC_ReplyT }
    guard let reply else {
      throw RPCError.unexpectedReply("no reply value")
    }
    if reply.value?.type == .failure, let failure = reply.value?.value as? Tsugumi_Data_FailureT {
      throw RPCFailure(failure)
    }
    return reply
  }

  public func callRaw(payloadType: UInt8, build: (inout FlatBufferBuilder) throws -> Offset,
                      timeout: Duration = .seconds(60)) async throws -> (UInt8, Table) {
    guard let type = Tsugumi_RPC_RequestPayload(rawValue: payloadType) else {
      throw RPCError.protocolViolation("unknown request type \(payloadType)")
    }
    let id = try requestId()
    var builder = FlatBufferBuilder(initialSize: 4096)
    let value = try build(&builder)
    let request = Tsugumi_RPC_Request.createRequest(&builder, valueType: type, valueOffset: value)
    let message = Tsugumi_RPC_Message.createMessage(&builder, requestId: id, payloadType: .request, payloadOffset: request)
    Tsugumi_RPC_Message.finish(&builder, end: message)
    let data = try await exchange(builder.data, timeout: timeout)
    var buffer = ByteBuffer(data: data)
    let root: Tsugumi_RPC_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSG1")
    guard let reply = root.payload(type: Tsugumi_RPC_Reply.self), let value = reply.rawValue else {
      throw RPCError.unexpectedReply("no reply value")
    }
    return (reply.valueType.rawValue, value.table)
  }

  private func receiveEvent(_ data: Data) {
    guard !closed else { return }
    do {
      var buffer = ByteBuffer(data: data)
      let root: Tsugumi_RPC_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSG1")
      if let note = root.payload(type: Tsugumi_RPC_Notification.self), note.valueType == .displayupdated,
         let display = note.value(type: Tsugumi_RPC_DisplayUpdated.self)?.display {
        let updated = Tsugumi_RPC_DisplayUpdatedT()
        updated.display = unpackDisplay(display)
        notify.yield(NotificationEvent(subscriptionId: note.subscriptionId ?? "", seq: note.seq ?? 0,
          payload: Tsugumi_RPC_NotificationPayloadUnion(updated, type: .displayupdated)))
        return
      }
      let message = Message(root)
      guard message.payload?.type == .notification,
            let note = message.payload?.value as? Tsugumi_RPC_NotificationT,
            let payload = note.value else { throw RPCError.protocolViolation("invalid notification") }
      notify.yield(NotificationEvent(subscriptionId: note.subscriptionId, seq: note.seq ?? 0, payload: payload))
    } catch { close() }
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

/// Decode presentation separately: invalid UTF-8 must never trap the generated
/// object API's required String initializers or disappear as replacement text.
public func unpackDisplay(_ wire: Tsugumi_Data_Display) -> Tsugumi_Data_DisplayT {
  var valid = true
  switch wire.valueType {
  case .displaytext:
    if let text = wire.value(type: Tsugumi_Data_DisplayText.self) {
      valid = (text.textSegmentArray?.count ?? Int.max) <= 1 << 20 && text.text != nil
    } else { valid = false }
  case .displayscalar:
    if let scalar = wire.value(type: Tsugumi_Data_DisplayScalar.self) {
      valid = scalar.value?.isFinite == true && scalar.label != nil
    } else { valid = false }
  case .displaytable:
    if let table = wire.value(type: Tsugumi_Data_DisplayTable.self) {
      var bytes = 0
      valid = !table.columns.isEmpty && table.columns.count <= 64 && table.rows.count <= 10000
      if valid {
        for column in table.columns {
          guard let column, column.utf8.count <= 4096 else { valid = false; break }
          bytes += column.utf8.count + 8
        }
        for row in table.rows {
          guard valid && row.cells.count == table.columns.count else { valid = false; break }
          for cell in row.cells {
            guard let cell, cell.utf8.count <= 4096 else { valid = false; break }
            bytes += cell.utf8.count + 8
            if bytes > 16 << 20 { valid = false; break }
          }
        }
      }
    } else { valid = false }
  case .displayimage:
    if let image = wire.value(type: Tsugumi_Data_DisplayImage.self) {
      valid = image.data.count <= 8 << 20 && image.mediaType != nil && (image.altSegmentArray == nil || image.alt != nil)
    } else { valid = false }
  case .displaypointcloud:
    if let cloud = wire.value(type: Tsugumi_Data_DisplayPointCloud.self) {
      valid = cloud.x.count <= 1000000 && cloud.y.count == cloud.x.count && cloud.z.count == cloud.x.count &&
        (cloud.colors.isEmpty || cloud.colors.count == cloud.x.count)
    } else { valid = false }
  case .displaymodule:
    if let module = wire.value(type: Tsugumi_Data_DisplayModule.self) {
      valid = module.module != nil && module.configJson != nil && module.blobs.count <= 64
      var bytes = 0
      if valid {
        for blob in module.blobs {
          guard blob.name != nil else { valid = false; break }
          bytes += blob.data.count
          if bytes > 8 << 20 { valid = false; break }
        }
      }
    } else { valid = false }
  default: break
  }
  if valid { return wire.unpack() }
  let value = Tsugumi_Data_DisplayT()
  value.card = wire.card!.unpack()
  value.runId = wire.runId ?? ""
  value.invocation = wire.invocation
  value.revision = wire.revision
  return value
}

private func unpackRecordResult(_ wire: Tsugumi_RPC_RecordResult) -> Tsugumi_RPC_RecordResultT {
  let value = Tsugumi_RPC_RecordResultT()
  value.exists = wire.exists
  value.selectedInvocation = wire.selectedInvocation
  value.run = wire.run!.unpack()
  value.status = wire.status!.unpack()
  value.aggregate = wire.aggregate!.unpack()
  value.recordingDisabled = wire.recordingDisabled
  value.truncated = wire.truncated
  value.orphaned = wire.orphaned
  if let record = wire.record {
    let r = Tsugumi_RPC_InvocationRecordT()
    r.summary = record.summary!.unpack()
    r.inputs = record.inputs.map { $0.unpack() }
    r.outputs = record.outputs.map { $0.unpack() }
    r.inputSources = record.inputSources.map { $0.unpack() }
    r.display = record.display.map(unpackDisplay)
    r.recordIo = record.recordIo
    r.paramsJson = record.paramsJson
    value.record = r
  }
  return value
}

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
