// A resident owner of the shared VM. Same-user clients attach over a private
// Unix socket; losing a UI connection never interrupts project execution.
import Darwin
import Foundation
import TsugumiLauncher

signal(SIGPIPE, SIG_IGN)
_ = setsid()

var support: URL?
var imageDir: URL?
var sourceDir: URL?
var sourceEntries: [String]?
var resources = VMResources.default
var args = CommandLine.arguments.dropFirst()
while let argument = args.popFirst() {
  guard let value = args.popFirst() else { exit(2) }
  switch argument {
  case "--support": support = URL(fileURLWithPath: value, isDirectory: true)
  case "--image": imageDir = URL(fileURLWithPath: value, isDirectory: true)
  case "--source": sourceDir = URL(fileURLWithPath: value, isDirectory: true)
  case "--source-entries": sourceEntries = (try? JSONSerialization.jsonObject(with: Data(value.utf8))) as? [String]
  case "--cpus": resources.cpus = Int(value) ?? resources.cpus
  case "--memory": resources.memoryGiB = Int(value) ?? resources.memoryGiB
  case "--store": resources.storeGiB = Int(value) ?? resources.storeGiB
  default: exit(2)
  }
}
guard let support, let imageDir, let sourceDir else {
  FileHandle.standardError.write(Data("usage: tsugumi-vmhost --support DIR --image DIR --source DIR\n".utf8))
  exit(2)
}
try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
chmod(support.path, 0o700)
let hostLock = open(support.appendingPathComponent("vmhost.lock").path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0o600)
guard hostLock >= 0 else { exit(1) }
// Concurrent launchers converge on the first host. A losing launcher exits
// successfully; its client connects to the winner's socket.
guard flock(hostLock, LOCK_EX | LOCK_NB) == 0 else { exit(0) }
let socketPath = VMHostIPC.socketPath(support: support)
let listener = try VMHostIPC.listen(path: socketPath)
let manager = EnvironmentManager(support: support, image: VMImage(directory: imageDir, source: sourceDir, sourceEntries: sourceEntries), resources: resources, ownsVM: true)
let defaults = UserDefaults(suiteName: ProcessInfo.processInfo.environment["TSUGUMI_DEFAULTS_DOMAIN"] ?? "app.tsugumi.Tsugumi")!

final class Peer: @unchecked Sendable {
  let id = UUID()
  let fd: Int32
  private let lock = NSLock()
  private var closed = false
  private var requests = 0
  private var terminals: [UInt64: Int32] = [:]

  init(_ fd: Int32) { self.fd = fd }
  func begin() -> Bool { lock.withLock { if closed || requests >= 32 { return false }; requests += 1; return true } }
  func end() { lock.withLock { requests -= 1 } }
  func emit(_ object: [String: Any]) {
    lock.withLock {
      guard !closed else { return }
      do { try VMHostIPC.write(object, to: fd) } catch { shutdown(fd, SHUT_RDWR) }
    }
  }
  func disconnect() { lock.withLock { if !closed { shutdown(fd, SHUT_RDWR) } } }
  func close() {
    lock.withLock {
      guard !closed else { return }
      closed = true
      for descriptor in terminals.values { shutdown(descriptor, SHUT_RDWR) }
      shutdown(fd, SHUT_RDWR)
      Darwin.close(fd)
    }
  }
  func addTerminal(_ descriptor: Int32, _ session: UInt64) {
    let accepted = lock.withLock { if closed { return false }; terminals[session] = descriptor; return true }
    guard accepted else { Darwin.close(descriptor); return }
    Thread.detachNewThread { [self] in
      var buffer = [UInt8](repeating: 0, count: 16384)
      while true {
        let count = Darwin.read(descriptor, &buffer, buffer.count)
        if count < 0, errno == EINTR { continue }
        if count <= 0 { break }
        emit(["event": "terminal", "session": session, "data": Data(buffer[0..<count]).base64EncodedString()])
      }
      lock.withLock { terminals[session] = nil }
      Darwin.close(descriptor)
      emit(["event": "terminalExit", "session": session])
    }
  }
  func writeTerminal(_ session: UInt64, _ data: Data) {
    lock.withLock {
      guard let descriptor = terminals[session], !closed else { return }
      data.withUnsafeBytes { buffer in
        var offset = 0
        while offset < buffer.count {
          let count = Darwin.write(descriptor, buffer.baseAddress! + offset, buffer.count - offset)
          if count < 0, errno == EINTR { continue }
          if count <= 0 { break }
          offset += count
        }
      }
    }
  }
  func closeTerminal(_ session: UInt64) { lock.withLock { if let descriptor = terminals[session] { shutdown(descriptor, SHUT_RDWR) } } }
}

final class Peers: @unchecked Sendable {
  private let lock = NSLock()
  private var peers: [UUID: Peer] = [:]
  func add(_ peer: Peer) -> Bool { lock.withLock { if peers.count >= 64 { return false }; peers[peer.id] = peer; return true } }
  func remove(_ peer: Peer) { _ = lock.withLock { peers.removeValue(forKey: peer.id) }; peer.close() }
  func emit(_ object: [String: Any]) { for peer in lock.withLock({ Array(peers.values) }) { peer.emit(object) } }
}
let peers = Peers()

@Sendable func vmState(_ state: SharedVM.State) -> [String: Any] {
  switch state {
  case .stopped: return ["event": "vmState", "state": "stopped"]
  case .running: return ["event": "vmState", "state": "running"]
  case .starting(let stage): return ["event": "vmState", "state": "starting", "stage": stage.rawValue]
  case .failed(let message): return ["event": "vmState", "state": "failed", "message": message]
  }
}
_ = manager.vm.observe { state in peers.emit(vmState(state)) }
_ = manager.vm.console.observe { line in peers.emit(["event": "console", "line": line]) }

actor Observed {
  private var ids: Set<String> = []
  func first(_ id: String) -> Bool { ids.insert(id).inserted }
}
let observed = Observed()

@Sendable func project(_ request: [String: Any]) throws -> ProjectIdentity {
  guard let path = request["path"] as? String, path.hasPrefix("/") else { throw LauncherError.notReady("an absolute project path is required") }
  return try ProjectIdentity(url: URL(fileURLWithPath: path))
}

@Sendable func answer(_ request: [String: Any], _ peer: Peer) async throws -> Any {
  switch request["op"] as? String {
  case "hello": return ["pid": getpid()]
  case "start":
    let p = try project(request)
    let origins = Set(await manager.allowedOrigins).union(request["origins"] as? [String] ?? [])
    await manager.setAllowedOrigins(origins.sorted())
    let control = manager.controlDir(p)
    try FileManager.default.createDirectory(at: control, withIntermediateDirectories: true)
    chmod(control.path, 0o700)
    try JSONSerialization.data(withJSONObject: ["origins": origins.sorted()]).write(to: control.appendingPathComponent("origins.json"), options: .atomic)
    if let folders = request["folders"] as? [String] { await manager.setExtraFolders(p, folders) }
    if await observed.first(p.id) {
      _ = await manager.observe(p) { state in
        switch state {
        case .preparing(let stage), .starting(let stage): peers.emit(["event": "progress", "key": p.id, "stage": stage])
        case .stopped: peers.emit(["event": "stopped", "key": p.id])
        default: break
        }
      }
    }
    let endpoint = try await manager.start(p, archiveIncompatibleHistory: request["archive_incompatible_history"] as? Bool == true)
    return ["endpoint": endpoint.url.absoluteString, "token": endpoint.token, "projectId": endpoint.projectId,
            "serverInstanceId": endpoint.serverInstanceId, "readyFile": manager.controlDir(p).appendingPathComponent("server.ready").path]
  case "stop": try await manager.stop(try project(request)); return [:]
  case "folders": await manager.setExtraFolders(try project(request), request["folders"] as? [String] ?? []); return [:]
  case "cleanupStore":
    let result = try await manager.cleanupStore()
    return ["bytes": result.bytes, "paths": result.paths]
  case "histories":
    return ["histories": try await manager.vmHistories().map { item -> [String: Any] in
      var row: [String: Any] = ["id": item.id, "bytes": item.bytes, "running": item.running]
      if let date = item.lastStart { row["lastStart"] = date.timeIntervalSince1970 }; return row
    }]
  case "deleteVMData": try await manager.deleteVMData(id: request["projectId"] as? String ?? ""); return [:]
  case "deleteData": try await manager.deleteData(try project(request)); return [:]
  case "grantImport":
    guard let source = request["source"] as? String, source.hasPrefix("/") else { throw LauncherError.notReady("an absolute import path is required") }
    return ["grant": try await manager.grantImport(try project(request), source: URL(fileURLWithPath: source))]
  case "resetAll":
    let result = try await manager.resetAll()
    return ["environments": result.environments, "vmRemoved": result.vmRemoved, "hostPid": getpid()]
  case "shutdown": await manager.shutdown(); return [:]
  case "terminal":
    let (fd, session) = try await manager.openTerminal(try project(request), rows: request["rows"] as? Int ?? 24, cols: request["cols"] as? Int ?? 80)
    // Reply first, before terminal output can arrive.
    peer.emit(["id": request["id"]!, "result": ["session": session]])
    peer.addTerminal(fd, session)
    return NSNull()
  case "input":
    if let session = request["session"] as? UInt64, let data = Data(base64Encoded: request["data"] as? String ?? "") { peer.writeTerminal(session, data) }
    return [:]
  case "resize":
    if let session = request["session"] as? UInt64 { await manager.resizeTerminal(session: session, rows: request["rows"] as? Int ?? 24, cols: request["cols"] as? Int ?? 80) }
    return [:]
  case "closeTerminal": if let session = request["session"] as? UInt64 { peer.closeTerminal(session) }; return [:]
  case "prefs":
    var values: [String: Any] = [:]
    for key in request["keys"] as? [String] ?? [] {
      if let value = defaults.object(forKey: key), JSONSerialization.isValidJSONObject([value]) { values[key] = value }
    }
    return values
  case "setPrefs":
    for (key, value) in request["values"] as? [String: Any] ?? [:] { defaults.set(value, forKey: key) }
    return [:]
  default: throw LauncherError.notReady("unknown VM host operation")
  }
}

@Sendable func serve(_ peer: Peer, _ request: [String: Any]) {
  guard let id = request["id"] else { peer.disconnect(); return }
  guard peer.begin() else { peer.emit(["id": id, "error": "too many pending VM host operations"]); return }
  Task {
    defer { peer.end() }
    do {
      let result = try await answer(request, peer)
      if !(result is NSNull) { peer.emit(["id": id, "result": result]) }
      if request["op"] as? String == "resetAll" {
        // Reset is the explicit boundary at which the next launcher loads
        // an updated helper and VM image. Ordinary disconnects never exit.
        unlink(socketPath)
        exit(0)
      }
    } catch {
      var failure: [String: Any] = ["id": id, "error": "\(error)"]
      if let launcher = error as? LauncherError {
        failure["recovery"] = launcher.recovery
        if case .startFailed(_, let log) = launcher { failure["log"] = log }
        if case .incompatibleHistory(let issue) = launcher { failure["startup_error"] = issue.diagnostic }
      }
      peer.emit(failure)
    }
  }
}

Thread.detachNewThread {
  while true {
    let fd = accept(listener, nil, nil)
    if fd < 0 { if errno == EINTR { continue }; break }
    _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
    noSigPipe(fd)
    var timeout = timeval(tv_sec: 5, tv_usec: 0)
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    var uid: uid_t = 0, gid: gid_t = 0
    guard getpeereid(fd, &uid, &gid) == 0, uid == geteuid() else { Darwin.close(fd); continue }
    let peer = Peer(fd)
    Thread.detachNewThread {
      do {
        guard let first = try VMHostIPC.read(fd) else { peer.close(); return }
        if first["op"] as? String == "terminalStream" {
          Task {
            do {
              let terminal = try await manager.openTerminal(try project(first), rows: first["rows"] as? Int ?? 24, cols: first["cols"] as? Int ?? 80)
              do { try VMHostIPC.write(["id": first["id"] ?? "terminal", "result": ["session": terminal.session]], to: fd) }
              catch { Darwin.close(terminal.fd); throw error }
              VMHostIPC.relay(fd, terminal.fd)
            } catch { peer.emit(["error": "\(error)"]); peer.close() }
          }
          return
        }
        guard peers.add(peer) else { peer.close(); return }
        peer.emit(vmState(manager.vm.state))
        for line in manager.vm.console.snapshot.suffix(200) { peer.emit(["event": "console", "line": line]) }
        serve(peer, first)
        while let request = try VMHostIPC.read(fd) { serve(peer, request) }
      } catch {}
      peers.remove(peer)
    }
  }
}
dispatchMain()
