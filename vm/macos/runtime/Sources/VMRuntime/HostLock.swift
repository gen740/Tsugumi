import TsugumiVMTypes
import Foundation
import Darwin

/// Host-side exclusive lock serializing start/stop of one environment.
final class HostLock {
  private var fd: Int32 = -1
  init(path: URL) throws {
    try FileManager.default.createDirectory(at: path.deletingLastPathComponent(), withIntermediateDirectories: true)
    fd = open(path.path, O_RDWR | O_CREAT | O_CLOEXEC, 0o600)
    guard fd >= 0 else { throw LauncherError.locked }
  }
  func lock(wait: Bool) throws {
    if flock(fd, LOCK_EX | (wait ? 0 : LOCK_NB)) != 0 { throw LauncherError.locked }
  }
  deinit {
    if fd >= 0 { close(fd) }
  }
}
