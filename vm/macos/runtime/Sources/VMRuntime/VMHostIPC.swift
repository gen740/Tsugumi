import TsugumiVMTypes
import CryptoKit
import Darwin
import Foundation
import TsugumiRPC

/// Private adapter transport, used by the signed helper and its byte bridges.
/// App and Web clients use hostd Host requests.
public enum VMHostIPC {
  public static func socketPath(support: URL) -> String {
    let canonical = realpath(support.path, nil).map { pointer in
      defer { free(pointer) }
      return String(cString: pointer)
    } ?? support.standardizedFileURL.path
    let digest = SHA256.hash(data: Data(canonical.utf8)).prefix(12).map { String(format: "%02x", $0) }.joined()
    let selected = ProcessInfo.processInfo.environment["TSUGUMI_SOCKET_DIR"]
    let prefix = selected == nil ? "tsugumi-vmhost-\(geteuid())-" : "v-"
    return URL(fileURLWithPath: selected ?? "/private/tmp").appendingPathComponent("\(prefix)\(digest).sock").path
  }

  /// The address of the socket file at `path`. As in the core's
  /// `unix_socket::Address`, a path that does not fit in sun_path is reached
  /// through a short link to its folder in TSUGUMI_SOCKET_DIR (else /tmp)
  /// `/tsugumi-<uid>/`, a folder only this user can enter.
  static func address(_ path: String) throws -> sockaddr_un {
    var address = sockaddr_un()
    address.sun_family = sa_family_t(AF_UNIX)
    let limit = MemoryLayout.size(ofValue: address.sun_path) - 1
    func failure(_ why: String) -> LauncherError {
      .notReady("Unix socket path \(path) is \(path.utf8.count) bytes; this system allows \(limit): \(why)")
    }
    var text = path
    if text.utf8.count > limit {
      guard path.hasPrefix("/"), let slash = path.lastIndex(of: "/") else { throw failure("it must be absolute to be shortened") }
      let folder = slash == path.startIndex ? "/" : String(path[..<slash])
      text = try shortFolder(folder, failure) + String(path[slash...])
      guard text.utf8.count <= limit else { throw failure("the shortened path \(text) is still too long") }
    }
    let bytes = Array(text.utf8) + [0]
    withUnsafeMutableBytes(of: &address.sun_path) { target in target.copyBytes(from: bytes) }
    return address
  }

  private static func shortFolder(_ folder: String, _ failure: (String) -> LauncherError) throws -> String {
    let selected = ProcessInfo.processInfo.environment["TSUGUMI_SOCKET_DIR"].flatMap { $0.isEmpty ? nil : $0 }
    let root = (selected ?? "/tmp") + "/tsugumi-\(geteuid())"
    if mkdir(root, 0o700) != 0 && errno != EEXIST { throw failure("cannot create \(root): \(String(cString: strerror(errno)))") }
    var status = stat()
    guard lstat(root, &status) == 0, status.st_mode & S_IFMT == S_IFDIR, status.st_uid == geteuid(),
          status.st_mode & 0o077 == 0 else { throw failure("\(root) must be a folder only this user can access") }
    // FNV-1a, the same link name the core computes.
    var hash: UInt64 = 14695981039346656037
    for byte in folder.utf8 { hash = (hash ^ UInt64(byte)) &* 1099511628211 }
    let link = root + "/" + String(format: "%016llx", hash)
    if symlink(folder, link) == 0 { return link }
    guard errno == EEXIST else { throw failure("cannot create \(link): \(String(cString: strerror(errno)))") }
    if (try? FileManager.default.destinationOfSymbolicLink(atPath: link)) == folder { return link }
    // A different folder with the same name: replace the link atomically.
    let pending = link + ".\(getpid())"
    unlink(pending)
    guard symlink(folder, pending) == 0, rename(pending, link) == 0 else {
      let error = errno
      unlink(pending)
      throw failure("cannot replace \(link): \(String(cString: strerror(error)))")
    }
    return link
  }

  public static func connect(path: String) throws -> Int32 {
    var addr = try address(path)
    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    guard fd >= 0 else { throw LauncherError.notReady("cannot create VM host socket") }
    noSigPipe(fd)
    _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
    let connected = withUnsafePointer(to: &addr) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
    }
    guard connected == 0 else {
      Darwin.close(fd)
      throw LauncherError.notReady("the resident VM host is not reachable")
    }
    return fd
  }

  public static func listen(path: String) throws -> Int32 {
    // Publish the socket pathname only after it can accept a connection.
    let pending = path.hasSuffix(".sock") ? String(path.dropLast(5)) + ".new" : path + ".new"
    unlink(pending)
    defer { unlink(pending) }
    var addr = try address(pending)
    let fd = socket(AF_UNIX, SOCK_STREAM, 0)
    guard fd >= 0 else { throw LauncherError.notReady("cannot create VM host listener") }
    _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
    let mask = umask(0o177)
    let bound = withUnsafePointer(to: &addr) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
    }
    umask(mask)
    guard bound == 0, Darwin.listen(fd, 64) == 0, rename(pending, path) == 0 else {
      Darwin.close(fd)
      throw LauncherError.notReady("cannot bind VM host listener")
    }
    return fd
  }

  /// The largest frame on the helper link, as hostd's side of it
  /// (hostd/src/vm_process.cc): the Host RPC frame limit, because the helper
  /// relays Host requests (terminal input) and replies (runtime snapshots)
  /// that hostd's clients exchange in Host RPC frames.
  public static let maxFrameBytes = 16 << 20

  public static func write(_ bytes: Data, to fd: Int32, maxSize: Int = maxFrameBytes) throws {
    do { try RPCFrame.write(bytes, to: fd, maxBytes: UInt32(maxSize)) } catch {
      throw LauncherError.notReady("VM host frame: \(error)")
    }
  }

  /// The next frame, or nil when the peer closed the stream between frames.
  public static func read(_ fd: Int32, maxSize: Int = maxFrameBytes) throws -> Data? {
    do { return try RPCFrame.read(fd, maxBytes: UInt32(maxSize)) } catch {
      throw LauncherError.notReady("VM host frame: \(error)")
    }
  }

  /// Transfers ownership of both descriptors until the stream has drained.
  public static func relay(_ first: Int32, _ second: Int32, retained: AnyObject? = nil, closeOnEOF: Bool = false, completed: @escaping () -> Void = {}) {
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
        shutdown(destination, closeOnEOF ? SHUT_RDWR : SHUT_WR)
        shutdown(source, closeOnEOF ? SHUT_RDWR : SHUT_RD)
        done.leave()
      }
    }
    done.notify(queue: .global()) { withExtendedLifetime(retained) { Darwin.close(first); Darwin.close(second); completed() } }
  }
}
