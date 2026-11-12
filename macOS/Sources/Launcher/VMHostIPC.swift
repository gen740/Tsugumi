import CryptoKit
import Darwin
import Foundation

/// Local launcher transport. The resident helper owns the VM and its bridges;
/// disconnecting a launcher does not stop a project or an Agent tool call.
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

  public static func write(_ object: [String: Any], to fd: Int32) throws {
    let bytes = try JSONSerialization.data(withJSONObject: object) + Data([10])
    guard bytes.count <= 1 << 20 else { throw LauncherError.notReady("VM host message exceeds its limit") }
    try writeAll(fd, bytes)
  }

  public static func read(_ fd: Int32) throws -> [String: Any]? {
    var data = Data()
    var buffer = [UInt8](repeating: 0, count: 8192)
    while true {
      // Peek up to the delimiter so a terminalStream acknowledgement never
      // consumes the terminal bytes that immediately follow it.
      let available = recv(fd, &buffer, buffer.count, MSG_PEEK)
      if available < 0, errno == EINTR { continue }
      if available <= 0 { return nil }
      let end = buffer[0..<available].firstIndex(of: 10)
      let wanted = end.map { $0 + 1 } ?? available
      let count = Darwin.read(fd, &buffer, wanted)
      if count < 0, errno == EINTR { continue }
      if count <= 0 { return nil }
      data.append(contentsOf: buffer[0..<count])
      guard data.count <= 1 << 20 else { throw LauncherError.notReady("VM host message exceeds its limit") }
      if data.last == 10 { data.removeLast(); break }
    }
    guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
      throw LauncherError.notReady("invalid VM host message")
    }
    return object
  }

  /// Transfers ownership of both descriptors until the stream has drained.
  public static func relay(_ first: Int32, _ second: Int32) {
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
    done.notify(queue: .global()) { Darwin.close(first); Darwin.close(second) }
  }
}

actor ResidentVMHost {
  private let support: URL
  private let image: VMImage?
  private let resources: VMResources
  private var descriptor: Int32 = -1
  private var generation: UInt64 = 0
  private var connecting: Task<Int32, Error>?
  private var pending: [String: CheckedContinuation<[String: Any], Error>] = [:]
  private var timers: [String: Task<Void, Never>] = [:]
  private var handler: (@Sendable ([String: Any]) -> Void)?

  init(support: URL, image: VMImage?, resources: VMResources) {
    self.support = support; self.image = image; self.resources = resources
  }

  deinit {
    if descriptor >= 0 { shutdown(descriptor, SHUT_RDWR) }
    for timer in timers.values { timer.cancel() }
    for reply in pending.values { reply.resume(throwing: LauncherError.notReady("VM host client closed")) }
  }

  func observe(_ handler: @escaping @Sendable ([String: Any]) -> Void) { self.handler = handler }

  private func connected() async throws -> Int32 {
    if descriptor >= 0 { return descriptor }
    if let connecting { return try await connecting.value }
    let support = support, image = image, resources = resources
    let task = Task.detached { try Self.open(support: support, image: image, resources: resources) }
    connecting = task
    do {
      let fd = try await task.value
      descriptor = fd
      generation &+= 1
      let generation = generation
      connecting = nil
      Thread.detachNewThread { [weak self] in
        do {
          while let object = try VMHostIPC.read(fd) {
            guard let self else { break }
            let delivered = DispatchSemaphore(value: 0)
            Task { await self.received(object, from: fd, generation: generation); delivered.signal() }
            delivered.wait()
          }
        } catch {}
        let finished = DispatchSemaphore(value: 0)
        Task { await self?.lost(fd, generation: generation); finished.signal() }
        finished.wait()
        Darwin.close(fd)
      }
      return fd
    } catch { connecting = nil; throw error }
  }

  private nonisolated static func open(support: URL, image: VMImage?, resources: VMResources) throws -> Int32 {
    try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
    chmod(support.path, 0o700)
    let path = VMHostIPC.socketPath(support: support)
    if let fd = try? VMHostIPC.connect(path: path) { return fd }
    guard let image else { throw LauncherError.imageMissing }
    let environment = ProcessInfo.processInfo.environment
    let bundled = Bundle.main.executableURL?.deletingLastPathComponent().appendingPathComponent("tsugumi-vmhost")
    let sibling = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent().appendingPathComponent("tsugumi-vmhost")
    let binary = environment["TSUGUMI_VMHOST"].map { URL(fileURLWithPath: $0) }
      ?? [bundled, sibling].compactMap { $0 }.first { FileManager.default.isExecutableFile(atPath: $0.path) }
    guard let binary, FileManager.default.isExecutableFile(atPath: binary.path) else {
      throw LauncherError.notReady("tsugumi-vmhost is not installed")
    }
    let logURL = support.appendingPathComponent("vmhost.log")
    let logDescriptor = Darwin.open(logURL.path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0o600)
    guard logDescriptor >= 0 else { throw LauncherError.notReady("cannot open VM host log") }
    let log = FileHandle(fileDescriptor: logDescriptor, closeOnDealloc: true)
    let process = Process()
    process.executableURL = binary
    process.arguments = ["--support", support.path, "--image", image.kernel.deletingLastPathComponent().path, "--source", image.source.path,
                         "--cpus", "\(resources.cpus)", "--memory", "\(resources.memoryGiB)", "--store", "\(resources.storeGiB)"]
    if let entries = image.sourceEntries, let data = try? JSONSerialization.data(withJSONObject: entries), let json = String(data: data, encoding: .utf8) {
      process.arguments?.append(contentsOf: ["--source-entries", json])
    }
    process.standardInput = FileHandle.nullDevice
    process.standardOutput = log
    process.standardError = log
    try process.run()
    let deadline = Date().addingTimeInterval(15)
    while Date() < deadline {
      if let fd = try? VMHostIPC.connect(path: path) { return fd }
      if !process.isRunning, process.terminationStatus != 0 {
        throw LauncherError.notReady("tsugumi-vmhost failed to start; see vmhost.log")
      }
      Thread.sleep(forTimeInterval: 0.05)
    }
    throw LauncherError.notReady("tsugumi-vmhost did not become ready")
  }

  func call(_ operation: String, _ arguments: [String: Any] = [:], timeout: TimeInterval = 1800) async throws -> [String: Any] {
    let fd = try await connected()
    let id = UUID().uuidString
    var request = arguments; request["op"] = operation; request["id"] = id
    return try await withCheckedThrowingContinuation { continuation in
      pending[id] = continuation
      do { try VMHostIPC.write(request, to: fd) } catch {
        pending.removeValue(forKey: id)?.resume(throwing: error)
        return
      }
      timers[id] = Task {
        do { try await Task.sleep(for: .seconds(timeout)) } catch { return }
        self.expire(id)
      }
    }
  }

  private func expire(_ id: String) {
    timers[id] = nil
    pending.removeValue(forKey: id)?.resume(throwing: LauncherError.notReady("VM host request timed out"))
  }

  private func received(_ object: [String: Any], from fd: Int32, generation: UInt64) {
    guard descriptor == fd, self.generation == generation else { return }
    guard let id = object["id"] as? String else { handler?(object); return }
    timers.removeValue(forKey: id)?.cancel()
    guard let reply = pending.removeValue(forKey: id) else { return }
    if let error = object["error"] as? String {
      if let details = object["startup_error"] as? [String: Any], let issue = HistoryVersionMismatch(diagnostic: details) {
        reply.resume(throwing: LauncherError.incompatibleHistory(issue))
      } else {
        reply.resume(throwing: LauncherError.startFailed(reason: error, log: object["log"] as? String ?? ""))
      }
    } else { reply.resume(returning: object["result"] as? [String: Any] ?? [:]) }
  }

  func disconnect() {
    guard descriptor >= 0 else { return }
    let fd = descriptor
    descriptor = -1
    generation &+= 1
    shutdown(fd, SHUT_RDWR)
    for timer in timers.values { timer.cancel() }; timers.removeAll()
    let replies = pending; pending.removeAll()
    for reply in replies.values { reply.resume(throwing: LauncherError.notReady("VM host client disconnected")) }
  }

  private func lost(_ fd: Int32, generation: UInt64) {
    guard descriptor == fd, self.generation == generation else { return }
    descriptor = -1
    shutdown(fd, SHUT_RDWR)
    for timer in timers.values { timer.cancel() }; timers.removeAll()
    let replies = pending; pending.removeAll()
    for reply in replies.values { reply.resume(throwing: LauncherError.notReady("connection to the VM host was lost")) }
    handler?(["event": "vmState", "state": "failed", "message": "connection to the VM host was lost"])
  }

  func terminal(_ project: ProjectIdentity, rows: Int, cols: Int) async throws -> (fd: Int32, session: UInt64) {
    _ = try await connected()
    let support = support
    return try await Task.detached {
      let fd = try VMHostIPC.connect(path: VMHostIPC.socketPath(support: support))
      do {
        try VMHostIPC.write(["id": "terminal", "op": "terminalStream", "path": project.url.path, "rows": rows, "cols": cols], to: fd)
        guard let answer = try VMHostIPC.read(fd), let result = answer["result"] as? [String: Any], let session = result["session"] as? UInt64 else {
          throw LauncherError.notReady("cannot open the project's terminal")
        }
        return (fd, session)
      } catch { Darwin.close(fd); throw error }
    }.value
  }
}
