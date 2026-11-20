import Darwin
import FlatBuffers
import Foundation
import TsugumiRPC
import TsugumiVMTypes

/// One reader for replies and events. Writes are serialized so concurrent
/// requests can share the same stream.
final class GuestControl: @unchecked Sendable {
  // Store verification returns at most a 64 KiB diagnostic tail.
  private static let maxFrameSize = 1 << 20

  // Keep the Virtualization connection alive while the duplicate is in use.
  private let retained: AnyObject?
  private let fd: Int32
  private let writer = DispatchQueue(label: "app.tsugumi.vm.control.write")
  private let lock = NSLock()
  private var nextID: UInt64 = 1
  private var pending: [UInt64: CheckedContinuation<Tsugumi_VMGuest_ReplyT, Error>] = [:]
  private var failure: Error?

  init(fd: Int32, retaining retained: AnyObject? = nil) {
    self.fd = fd
    self.retained = retained
    noSigPipe(fd)
  }

  deinit { Darwin.close(fd) }

  func start(event: @escaping @Sendable (Tsugumi_VMGuest_MessageT) throws -> Void,
             closed: @escaping @Sendable (Error) -> Void) {
    Thread.detachNewThread { [self] in
      do {
        while let message = try Self.read(fd) {
          if let reply = message.payload?.value as? Tsugumi_VMGuest_ReplyT {
            guard let answer = lock.withLock({ pending.removeValue(forKey: message.requestId) }) else {
              throw LauncherError.notReady("unexpected guest reply")
            }
            do { answer.resume(returning: try Self.checked(reply)) }
            catch { answer.resume(throwing: error) }
          } else {
            guard message.requestId == 0 else { throw LauncherError.notReady("invalid guest event ID") }
            try event(message)
          }
        }
        throw LauncherError.notReady("guest control stream closed")
      } catch {
        finish(error)
        closed(error)
      }
    }
  }

  func request(_ value: NativeObject, type: Tsugumi_VMGuest_Command) async throws -> Tsugumi_VMGuest_ReplyT {
    let id = lock.withLock {
      defer { nextID += 1 }
      return nextID
    }
    let data = try Self.requestData(value, type: type, id: id)
    return try await withCheckedThrowingContinuation { answer in
      writer.async { [self] in
        lock.lock()
        if let failure {
          lock.unlock()
          answer.resume(throwing: failure)
          return
        }
        pending[id] = answer
        lock.unlock()
        do { try VMHostIPC.write(data, to: fd) }
        catch { finish(error) }
      }
    }
  }

  func memorySnapshot() async throws -> Tsugumi_VMGuest_MemoryT {
    let reply = try await request(Tsugumi_VMGuest_ReadMemoryT(), type: .readmemory)
    guard let memory = reply.memory else {
      throw LauncherError.notReady("missing guest memory snapshot")
    }
    return memory
  }

  func finish(_ error: Error) {
    let answers = lock.withLock {
      guard failure == nil else { return [] as [CheckedContinuation<Tsugumi_VMGuest_ReplyT, Error>] }
      failure = error
      let answers = Array(pending.values)
      pending.removeAll()
      return answers
    }
    shutdown(fd, SHUT_RDWR)
    for answer in answers { answer.resume(throwing: error) }
  }

  static func requestData(_ value: NativeObject, type: Tsugumi_VMGuest_Command, id: UInt64) throws -> Data {
    let request = Tsugumi_VMGuest_RequestT()
    request.command = Tsugumi_VMGuest_CommandUnion(value, type: type)
    var message: Tsugumi_VMGuest_MessageT? = Tsugumi_VMGuest_MessageT()
    message?.requestId = id
    message?.payload = Tsugumi_VMGuest_PayloadUnion(request, type: .request)
    var builder = FlatBufferBuilder(initialSize: 256)
    let offset = Tsugumi_VMGuest_Message.pack(&builder, obj: &message)
    Tsugumi_VMGuest_Message.finish(&builder, end: offset)
    guard builder.size <= maxFrameSize else {
      throw LauncherError.startFailed(reason: "guest request is too large", log: "")
    }
    return builder.data
  }

  static func read(_ fd: Int32) throws -> Tsugumi_VMGuest_MessageT? {
    guard let bytes = try VMHostIPC.read(fd, maxSize: maxFrameSize) else { return nil }
    var buffer = ByteBuffer(data: bytes)
    let root: Tsugumi_VMGuest_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TVG1")
    return Tsugumi_VMGuest_MessageT(root)
  }

  static func checked(_ reply: Tsugumi_VMGuest_ReplyT) throws -> Tsugumi_VMGuest_ReplyT {
    if let error = reply.error {
      throw LauncherError.startFailed(reason: error, log: "")
    }
    return reply
  }
}
