import AppKit
import Darwin
import Foundation
import TsugumiViewPresentation

/// Presents the native core's views and carries runtime RPCs over a private
/// JSON adapter with inherited standard streams.
@MainActor final class PackViewRelay {
  let presentation: CardViewPresentation
  private var stream: FileHandle?
  private var process: Process?
  private var appKitStarted = false
  private var task: Task<Void, Never>?
  private var nextId: UInt64 = 0
  private var replies: [UInt64: CheckedContinuation<[String: Any], Error>] = [:]
  private var subscription: String?
  private var lastSequence: UInt64?
  private let writer = DispatchQueue(label: "org.tsugumi.pack.views")

  init(title: String, debugClock: Bool) {
    presentation = CardViewPresentation(packedTitle: title, debugClock: debugClock)
  }

  func connect(adapter: URL, schema: URL, socket: URL) async throws {
    let child = Process()
    let inbound = Pipe()
    let outbound = Pipe()
    child.executableURL = adapter
    child.arguments = ["--endpoint", "unix:\(socket.path)", "--schema", schema.path,
                       "--timeout-ms", "60000"]
    child.standardInput = outbound
    child.standardOutput = inbound
    try child.run()
    process = child
    stream = outbound.fileHandleForWriting
    task = Task {
      for await line in Self.frames(inbound.fileHandleForReading) { apply(line) }
      disconnect()
    }
    _ = try await call("SessionHelloRequest", ["client": "MacOS"])
    let opened = try await call("ProjectOpenRequest")
    guard let id = opened["subscription_id"] as? String else {
      throw PackError("Missing packed view subscription")
    }
    subscription = id
    lastSequence = (opened["seq"] as? NSNumber)?.uint64Value
  }

  func disconnect() {
    let handle = stream
    stream = nil
    try? handle?.close()
    if let process { stopChild(process) }
    process = nil
    task?.cancel()
    task = nil
    for reply in replies.values { reply.resume(throwing: PackError("The packed core connection closed")) }
    replies.removeAll()
  }

  func call(_ kind: String, _ body: [String: Any] = [:]) async throws -> [String: Any] {
    guard let stream else { throw PackError("The packed workflow has ended") }
    nextId += 1
    let id = nextId
    var bytes = try JSONSerialization.data(withJSONObject: [
      "payload_type": "Request", "request_id": id,
      "payload": ["value_type": kind, "value": body],
    ])
    bytes.append(10)
    let line = bytes
    return try await withCheckedThrowingContinuation { reply in
      replies[id] = reply
      writer.async {
        do { try stream.write(contentsOf: line) }
        catch {
          Task { @MainActor in self.disconnect() }
        }
      }
    }
  }

  private func apply(_ line: Data) {
    guard let json = (try? JSONSerialization.jsonObject(with: line)) as? [String: Any],
          let payload = json["payload"] as? [String: Any],
          let value = payload["value"] as? [String: Any] else {
      disconnect()
      return
    }
    if json["payload_type"] as? String == "Reply", let id = json["request_id"] as? NSNumber,
       let reply = replies.removeValue(forKey: id.uint64Value) {
      if payload["value_type"] as? String == "Failure" {
        reply.resume(throwing: PackError(value["diagnostic"] as? String ?? "Packed core RPC failed"))
      } else {
        reply.resume(returning: value)
      }
      return
    }
    let decoder = JSONDecoder()
    decoder.keyDecodingStrategy = .convertFromSnakeCase
    guard let frame = try? decoder.decode(PackViewFrame.self, from: line) else {
      disconnect()
      return
    }
    applyNotification(frame)
  }

  private func applyNotification(_ frame: PackViewFrame) {
    guard let payload = frame.payload, let value = payload.value else { return }
    guard frame.payloadType == "Notification", let subscription, payload.subscriptionId == subscription else { return }
    if let lastSequence, payload.seq != lastSequence + 1 {
      disconnect()
      return
    }
    lastSequence = payload.seq
    switch payload.valueType {
    case "ViewOpened":
      guard let id = value.session, let name = value.name, let type = value.type,
            let mode = CardViewMode(rawValue: value.mode ?? 0) else { return }
      // Initialize AppKit only when the packed Workflow opens a window.
      if !appKitStarted {
        appKitStarted = true
        NSApplication.shared.setActivationPolicy(.regular)
        NSApplication.shared.finishLaunching()
      }
      let opening = CardViewOpening(id: id, card: value.card?.segments ?? [], name: name, type: type,
                                    mode: mode, width: value.width ?? 0, height: value.height ?? 0, flags: value.flags ?? 0)
      print("View \(name)")
      let session = id.uuidString.lowercased()
      let files = CardViewFiles { [weak self] _, offset in
        guard let self else { throw PackError("The packed workflow has ended") }
        let page = try await call("CardViewReadRequest", ["session": session, "offset": offset, "limit_bytes": 1 << 20])
        let bytes = (page["bytes"] as? [UInt8]) ?? []
        let nextOffset = (page["next_offset"] as? NSNumber)?.uint64Value
        return CardViewFilePage(bytes: Data(bytes), eof: page["eof"] as? Bool == true, nextOffset: nextOffset)
      }
      presentation.open(opening, files: files, send: { [weak self] route, bytes in
        _ = try await self?.call("ViewSendRequest", [
          "session": session, "route": route.uuidString.lowercased(), "bytes": Array(bytes),
        ])
      }, close: { [weak self] in
        _ = try await self?.call("ViewCloseRequest", ["session": session])
      })
    case "ViewRouteRegistered":
      guard let session = value.session, let route = value.route else { return }
      presentation.registerRoute(route, session: session)
    case "ViewClosed":
      if let session = value.session { presentation.sessionClosed(session) }
    case "DisplayUpdated":
      guard let d = value.display, let id = d.session else { return }
      let bytes = Data(d.bytes)
      presentation.receive(id, bytes: bytes) { [weak self] in
        guard let credit = d.creditId else { return }
        Task {
          _ = try? await self?.call("ViewCreditRequest", [
            "subscription_id": subscription, "session": id.uuidString.lowercased(),
            "bytes": bytes.count, "credit_id": credit,
          ])
        }
      }
    default: break
    }
  }

  private nonisolated static func frames(_ handle: FileHandle) -> AsyncStream<Data> {
    AsyncStream { output in
      let read = Task.detached {
        defer { output.finish() }
        var pending = Data()
        var chunk = Data(count: 65536)
        while !Task.isCancelled {
          // FileHandle fills its requested byte count on a live pipe. Each
          // RPC reply must be read even when it is shorter than a chunk.
          let count = chunk.withUnsafeMutableBytes {
            Darwin.read(handle.fileDescriptor, $0.baseAddress, $0.count)
          }
          if count < 0, errno == EINTR { continue }
          guard count > 0 else { return }
          pending.append(chunk.prefix(count))
          guard pending.count <= 32 << 20 else { return }
          while let newline = pending.firstIndex(of: 10) {
            let frame = Data(pending.prefix(upTo: newline))
            pending.removeSubrange(...newline)
            output.yield(frame)
          }
        }
      }
      output.onTermination = { _ in read.cancel() }
    }
  }

  func pollWindows() {
    guard let app = NSApp else { return }
    autoreleasepool {
      while let event = app.nextEvent(matching: .any, until: .distantPast, inMode: .default, dequeue: true) {
        app.sendEvent(event)
      }
      app.updateWindows()
    }
  }
}

private struct PackViewFrame: Decodable {
  let payloadType: String?
  let payload: PackViewPayload?
}

private struct PackViewPayload: Decodable {
  let valueType: String?
  let value: PackViewValue?
  let subscriptionId: String?
  let seq: UInt64?
}

private struct PackViewValue: Decodable {
  struct Card: Decodable { let segments: [String] }
  struct Display: Decodable {
    let session: UUID?
    let bytes: [UInt8]
    let creditId: UInt64?
  }

  let session: UUID?
  let route: UUID?
  let card: Card?
  let name: String?
  let type: String?
  let mode: UInt32?
  let flags: UInt32?
  let width: UInt32?
  let height: UInt32?
  let display: Display?
}
