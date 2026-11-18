import Darwin
import Foundation
import Virtualization

/// Virtualization.framework adapter. No worker protocol or registry lives here.
final class ExecutionBrokerBridge: NSObject, VZVirtioSocketListenerDelegate, @unchecked Sendable {
  private let listener = VZVirtioSocketListener()
  private let lock = NSLock()
  private var socketPath: String?

  override init() {
    super.init()
    listener.delegate = self
  }

  func install(on socket: VZVirtioSocketDevice) { socket.setSocketListener(listener, forPort: 1027) }

  func configure(_ socket: String) {
    lock.withLock { socketPath = socket }
  }

  func listener(_ listener: VZVirtioSocketListener, shouldAcceptNewConnection connection: VZVirtioSocketConnection,
                from socketDevice: VZVirtioSocketDevice) -> Bool {
    guard let path = lock.withLock({ socketPath }) else { return false }
    let guest = Guest(connection)
    Thread.detachNewThread { [guest] in
      do {
        let peer = try VMHostIPC.connect(path: path)
        // VZ owns its descriptor. The generic byte relay owns only this dup.
        let fd = dup(guest.connection.fileDescriptor)
        guard fd >= 0 else { Darwin.close(peer); return }
        noSigPipe(fd)
        VMHostIPC.relay(fd, peer, retained: guest)
      } catch { Darwin.shutdown(guest.connection.fileDescriptor, SHUT_RDWR) }
    }
    return true
  }

  private final class Guest: @unchecked Sendable {
    let connection: VZVirtioSocketConnection
    init(_ connection: VZVirtioSocketConnection) { self.connection = connection }
  }
}
