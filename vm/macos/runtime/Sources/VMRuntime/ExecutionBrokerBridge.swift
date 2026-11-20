import Darwin
import Foundation
import Virtualization

/// Virtualization.framework adapter. No worker protocol or registry lives here.
final class ExecutionBrokerBridge: NSObject, VZVirtioSocketListenerDelegate, @unchecked Sendable {
  private let transferListener = VZVirtioSocketListener()
  private let cancelListener = VZVirtioSocketListener()
  private var transfers = 0
  private var cancellations = 0
  private let lock = NSLock()
  private var socketPath: String?

  override init() {
    super.init()
    transferListener.delegate = self
    cancelListener.delegate = self
  }

  func install(on socket: VZVirtioSocketDevice) {
    socket.setSocketListener(transferListener, forPort: 1027)
    socket.setSocketListener(cancelListener, forPort: 1028)
  }

  func configure(_ socket: String) {
    lock.withLock { socketPath = socket }
  }

  func listener(_ listener: VZVirtioSocketListener, shouldAcceptNewConnection connection: VZVirtioSocketConnection,
                from socketDevice: VZVirtioSocketDevice) -> Bool {
    let cancel = listener === cancelListener
    let path: String? = lock.withLock {
      guard let socketPath else { return nil }
      // Match the broker slots to bound relay threads, including partial requests.
      if cancel {
        guard cancellations < 2 else { return nil }
        cancellations += 1
        return socketPath + ".c"
      }
      guard transfers < 8 else { return nil }
      transfers += 1
      return socketPath
    }
    guard let path else { return false }
    let guest = Guest(connection)
    let completed: @Sendable () -> Void = { [self] in
      lock.withLock {
        if cancel { cancellations -= 1 } else { transfers -= 1 }
      }
    }
    Thread.detachNewThread { [guest] in
      do {
        let peer = try VMHostIPC.connect(path: path)
        // VZ owns its descriptor. The generic byte relay owns only this dup.
        let fd = dup(guest.connection.fileDescriptor)
        guard fd >= 0 else {
          Darwin.close(peer)
          Darwin.shutdown(guest.connection.fileDescriptor, SHUT_RDWR)
          completed()
          return
        }
        noSigPipe(fd)
        VMHostIPC.relay(fd, peer, retained: guest, closeOnEOF: true, completed: completed)
      } catch {
        Darwin.shutdown(guest.connection.fileDescriptor, SHUT_RDWR)
        completed()
      }
    }
    return true
  }

  private final class Guest: @unchecked Sendable {
    let connection: VZVirtioSocketConnection
    init(_ connection: VZVirtioSocketConnection) { self.connection = connection }
  }
}
