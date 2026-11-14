import CryptoKit
import Darwin
import Foundation

/// Private adapter transport, used by the signed helper and its byte bridges.
/// App and Web clients use agentd Host requests.
public enum VMHostIPC {
  public static func socketPath(support: URL) -> String {
    let canonical = realpath(support.path, nil).map { pointer in
      defer { free(pointer) }
      return String(cString: pointer)
    } ?? support.standardizedFileURL.path
    let digest = SHA256.hash(data: Data(canonical.utf8)).prefix(12).map { String(format: "%02x", $0) }.joined()
    return "/private/tmp/tsugumi-vmhost-\(geteuid())-\(digest).sock"
  }

  private static func address(_ path: String) throws -> sockaddr_un {
    var address = sockaddr_un()
    address.sun_family = sa_family_t(AF_UNIX)
    let bytes = Array(path.utf8) + [0]
    guard bytes.count <= MemoryLayout.size(ofValue: address.sun_path) else { throw LauncherError.notReady("VM host socket path is too long") }
    withUnsafeMutableBytes(of: &address.sun_path) { target in target.copyBytes(from: bytes) }
    return address
  }

  public static func connect(path: String) throws -> Int32 {
    var addr = try address(path)
    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    guard fd >= 0 else { throw LauncherError.notReady("cannot create VM host socket") }
    noSigPipe(fd)
    var timeout = timeval(tv_sec: 5, tv_usec: 0)
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
    let connected = withUnsafePointer(to: &addr) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
    }
    var uid: uid_t = 0
    var gid: gid_t = 0
    guard connected == 0, getpeereid(fd, &uid, &gid) == 0, uid == geteuid() else {
      Darwin.close(fd)
      throw LauncherError.notReady("the resident VM host is not reachable")
    }
    return fd
  }

  public static func listen(path: String) throws -> Int32 {
    var old = stat()
    if lstat(path, &old) == 0 {
      guard old.st_uid == geteuid(), old.st_mode & S_IFMT == S_IFSOCK else {
        throw LauncherError.notReady("unsafe VM host socket path")
      }
      guard unlink(path) == 0 else { throw LauncherError.notReady("cannot replace VM host socket") }
    }
    var addr = try address(path)
    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    guard fd >= 0 else { throw LauncherError.notReady("cannot create VM host listener") }
    _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
    let mask = umask(0o177)
    let bound = withUnsafePointer(to: &addr) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
    }
    umask(mask)
    guard bound == 0, Darwin.listen(fd, 64) == 0 else {
      Darwin.close(fd)
      throw LauncherError.notReady("cannot bind VM host listener")
    }
    return fd
  }

  public static func write(_ bytes: Data, to fd: Int32) throws {
    guard !bytes.isEmpty, bytes.count <= 16 << 20 else { throw LauncherError.notReady("VM host message exceeds its limit") }
    var size = UInt32(bytes.count).littleEndian
    try withUnsafeBytes(of: &size) { try writeAll(fd, Data($0)) }
    try writeAll(fd, bytes)
  }

  public static func read(_ fd: Int32) throws -> Data? {
    func exact(_ size: Int) throws -> Data? {
      var result = Data(count: size)
      return try result.withUnsafeMutableBytes { buffer in
        var offset = 0
        while offset < size {
          let count = Darwin.read(fd, buffer.baseAddress! + offset, size - offset)
          if count < 0, errno == EINTR { continue }
          if count == 0, offset == 0 { return nil }
          guard count > 0 else { throw LauncherError.notReady("VM host stream closed mid-frame") }
          offset += count
        }
        return Data(buffer)
      }
    }
    guard let prefix = try exact(4) else { return nil }
    let size = prefix.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(as: UInt32.self)) }
    guard size > 0, size <= 16 << 20 else { throw LauncherError.notReady("Invalid VM host frame length") }
    guard let data = try exact(Int(size)) else { throw LauncherError.notReady("Truncated VM host frame") }
    return data
  }

  /// Transfers ownership of both descriptors until the stream has drained.
  public static func relay(_ first: Int32, _ second: Int32, retained: AnyObject? = nil, completed: @escaping () -> Void = {}) {
    let done = DispatchGroup()
    for (source, destination) in [(first, second), (second, first)] {
      done.enter()
      Thread.detachNewThread {
        var buffer = [UInt8](repeating: 0, count: 65536)
        while true {
          let count = Darwin.read(source, &buffer, buffer.count)
          if count < 0, errno == EINTR { continue }
          if count <= 0 { break }
          do { try writeAll(destination, Data(buffer[0..<count])) } catch { break }
        }
        shutdown(destination, SHUT_WR)
        shutdown(source, SHUT_RD)
        done.leave()
      }
    }
    done.notify(queue: .global()) { withExtendedLifetime(retained) { Darwin.close(first); Darwin.close(second); completed() } }
  }
}
