import Darwin
import Foundation
import TsugumiAgentClient
import Virtualization

/// Virtualization.framework adapter. No worker protocol or registry lives here.
final class ExecutionBrokerBridge: NSObject, VZVirtioSocketListenerDelegate, @unchecked Sendable {
  private let listener = VZVirtioSocketListener()
  private let lock = NSLock()
  private var endpoint: String?
  private var active = 0

  override init() {
    super.init()
    listener.delegate = self
  }

  func install(on socket: VZVirtioSocketDevice) { socket.setSocketListener(listener, forPort: 1027) }

  func authorize(_ project: ProjectIdentity, control: URL) async throws -> Bool {
    let result = try await CoreAgentClient.shared.call("ExecutionProjectAuthorizeRequest", [
      "project_id": project.id, "path": project.url.path, "control_dir": control.path, "guest": true,
    ])
    guard let value = result["endpoint"] as? String, value.hasPrefix("unix:") else {
      throw LauncherError.notReady("invalid execution broker endpoint")
    }
    lock.withLock { endpoint = String(value.dropFirst(5)) }
    return result["available"] as? Bool ?? false
  }

  func revoke(_ project: String) async throws {
    _ = try await CoreAgentClient.shared.call("ExecutionProjectRevokeRequest", ["project_id": project])
  }

  func listener(_ listener: VZVirtioSocketListener, shouldAcceptNewConnection connection: VZVirtioSocketConnection,
                from socketDevice: VZVirtioSocketDevice) -> Bool {
    let path = lock.withLock { () -> String? in
      guard active < 64, let endpoint else { return nil }
      active += 1
      return endpoint
    }
    guard let path else { return false }
    let guest = Guest(connection)
    Thread.detachNewThread { [self, guest] in
      var transferred = false
      defer { if !transferred { lock.withLock { active -= 1 } } }
      do {
        let peer = try VMHostIPC.connect(path: path)
        // VZ owns its descriptor. The generic byte relay owns only this dup.
        let fd = dup(guest.connection.fileDescriptor)
        guard fd >= 0 else { Darwin.close(peer); return }
        noSigPipe(fd)
        var timeout = timeval(tv_sec: 60, tv_usec: 0)
        for socket in [fd, peer] {
          setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
          setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        }
        transferred = true
        VMHostIPC.relay(fd, peer, retained: guest) { [self] in lock.withLock { active -= 1 } }
      } catch { Darwin.shutdown(guest.connection.fileDescriptor, SHUT_RDWR) }
    }
    return true
  }

  private final class Guest: @unchecked Sendable {
    let connection: VZVirtioSocketConnection
    init(_ connection: VZVirtioSocketConnection) { self.connection = connection }
  }
}
