import AppKit
import Foundation
import TsugumiViewPresentation
import Virtualization

/// The pack guest exposes its existing JSON RPC adapter on a private virtio socket.
@MainActor final class PackViewRelay {
  static let port: UInt32 = 7417
  let presentation: CardViewPresentation
  private let ready: URL
  private let done: URL
  private var runFinished = false
  private var stream: FileHandle?
  private var appKitStarted = false
  private var task: Task<Void, Never>?
  private var nextId: UInt64 = 0
  private var replies: [UInt64: CheckedContinuation<PackViewValue, Error>] = [:]
  private var subscription: String?
  private var lastSequence: UInt64?
  private let writer = DispatchQueue(label: "org.tsugumi.pack.views")

  init(title: String, share: URL) {
    presentation = CardViewPresentation(packedTitle: title)
    ready = share.appendingPathComponent("views.ready")
    done = share.appendingPathComponent("views.done")
  }

  func connect(_ device: VZVirtioSocketDevice) {
    guard task == nil else { return }
    task = Task {
      do {
        let connection = try await device.connect(toPort: Self.port)
        let fd = dup(connection.fileDescriptor)
        guard fd >= 0 else { throw PackError("Cannot duplicate the packed view connection") }
        var yes: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))
        let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        stream = handle
        let reader = Task {
          for await frame in Self.frames(handle, retaining: connection) { apply(frame) }
          disconnect()
        }
        defer { reader.cancel() }
        _ = try await call("SessionHelloRequest", ["client": "MacOS"])
        let opened = try await call("ProjectOpenRequest")
        guard let id = opened.subscriptionId else { throw PackError("Missing packed view subscription") }
        subscription = id
        lastSequence = opened.seq
        try Data().write(to: ready, options: .atomic)
        await reader.value
      } catch {
        disconnect()
        FileHandle.standardError.write(Data("Packed view connection: \(error.localizedDescription)\n".utf8))
      }
    }
  }

  func runEnded(_ runId: String) {
    // Observe the end on the subscribed connection before settling the guest.
    Task {
      _ = try? await call("RunStatusRequest", ["run_id": runId])
      runFinished = true
    }
  }

  func disconnect() {
    if let stream { shutdown(stream.fileDescriptor, SHUT_RDWR) }
    stream = nil
    for reply in replies.values { reply.resume(throwing: PackError("The packed view connection closed")) }
    replies.removeAll()
  }

  private func call(_ kind: String, _ body: [String: Any] = [:]) async throws -> PackViewValue {
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
        catch { shutdown(stream.fileDescriptor, SHUT_RDWR) }
      }
    }
  }

  private func apply(_ frame: PackViewFrame) {
    guard let payload = frame.payload, let value = payload.value else {
      disconnect()
      return
    }
    if frame.payloadType == "Reply", let id = frame.requestId, let reply = replies.removeValue(forKey: id) {
      if payload.valueType == "Failure" {
        reply.resume(throwing: PackError(value.diagnostic ?? "Packed view RPC failed"))
      } else { reply.resume(returning: value) }
      return
    }
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
      let session = id.uuidString.lowercased()
      let files = CardViewFiles { [weak self] _, offset in
        guard let self else { throw PackError("The packed workflow has ended") }
        let page = try await call("CardViewReadRequest", ["session": session, "offset": offset, "limit_bytes": 1 << 20])
        return CardViewFilePage(bytes: Data(page.bytes ?? []), eof: page.eof == true, nextOffset: page.nextOffset)
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

  private nonisolated static func frames(_ handle: FileHandle, retaining connection: VZVirtioSocketConnection)
    -> AsyncStream<PackViewFrame> {
    AsyncStream { output in
      let read = Task.detached {
        defer { withExtendedLifetime(connection) {}; output.finish() }
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        var pending = Data()
        while !Task.isCancelled, let chunk = try? handle.read(upToCount: 65536), !chunk.isEmpty {
          pending.append(chunk)
          while let newline = pending.firstIndex(of: 10) {
            guard let frame = try? decoder.decode(PackViewFrame.self, from: pending.prefix(upTo: newline)) else { return }
            pending.removeSubrange(...newline)
            output.yield(frame)
          }
        }
      }
      output.onTermination = { _ in
        read.cancel()
        shutdown(handle.fileDescriptor, SHUT_RDWR)
      }
    }
  }

  func pollWindows() {
    if runFinished, presentation.isIdle {
      runFinished = false
      do { try Data().write(to: done, options: .atomic) }
      catch { disconnect() }
    }
    guard let app = NSApp else { return }
    autoreleasepool {
      while let event = app.nextEvent(matching: .any, until: .distantPast, inMode: .default, dequeue: true) {
        app.sendEvent(event)
      }
      app.updateWindows()
    }
  }

  func waitForWindows() {
    while !presentation.isEmpty {
      _ = RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.016))
      pollWindows()
    }
  }
}

private struct PackViewFrame: Decodable {
  let requestId: UInt64?
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

  let subscriptionId: String?
  let seq: UInt64?
  let diagnostic: String?
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
  let bytes: [UInt8]?
  let eof: Bool?
  let nextOffset: UInt64?
}
