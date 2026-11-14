// Signed Virtualization.framework adapter driven exclusively by agentd.
import Darwin
import FlatBuffers
import Foundation
import TsugumiLauncher
import TsugumiRPC
import Virtualization

signal(SIGPIPE, SIG_IGN)
_ = setsid()
var support: URL?, imageDir: URL?, sourceDir: URL?
var sourceEntries: [String] = []
var resources = VMResources.default
var arguments = CommandLine.arguments.dropFirst()
while let argument = arguments.popFirst() {
  guard let value = arguments.popFirst() else { exit(2) }
  switch argument {
  case "--support": support = URL(fileURLWithPath: value)
  case "--image": imageDir = URL(fileURLWithPath: value)
  case "--source": sourceDir = URL(fileURLWithPath: value)
  case "--source-entry": sourceEntries.append(value)
  case "--cpus": if let n = Int(value), n > 0 { resources.cpus = n }
  case "--memory": if let n = Int(value), n > 0 { resources.memoryGiB = n }
  case "--store": if let n = Int(value), n > 0 { resources.storeGiB = n }
  default: exit(2)
  }
}
guard let support, let imageDir, let sourceDir else { exit(2) }
try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
chmod(support.path, 0o700)
let hostLock = open(support.appendingPathComponent("vmhost.lock").path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0o600)
guard hostLock >= 0 else { exit(1) }
guard flock(hostLock, LOCK_EX | LOCK_NB) == 0 else { exit(0) }
let socketPath = VMHostIPC.socketPath(support: support)
let listener = try VMHostIPC.listen(path: socketPath)
let vm = SharedVM(directory: support.appendingPathComponent("vm"))
let image = VMImage(directory: imageDir, source: sourceDir, sourceEntries: sourceEntries.isEmpty ? nil : sourceEntries)

func encode(_ value: NativeObject, reply: Tsugumi_VMHost_ReplyPayload? = nil, event: Tsugumi_VMHost_EventPayload? = nil, id: UInt64 = 0) -> Data {
  let message = Tsugumi_VMHost_MessageT()
  message.requestId = id
  if let reply {
    let body = Tsugumi_VMHost_ReplyT(); body.value = Tsugumi_VMHost_ReplyPayloadUnion(value, type: reply)
    message.payload = Tsugumi_VMHost_PayloadUnion(body, type: .reply)
  } else if let event {
    let body = Tsugumi_VMHost_EventT(); body.value = Tsugumi_VMHost_EventPayloadUnion(value, type: event)
    message.payload = Tsugumi_VMHost_PayloadUnion(body, type: .event)
  }
  var builder = FlatBufferBuilder(initialSize: 1024)
  var object: Tsugumi_VMHost_MessageT? = message
  let offset = Tsugumi_VMHost_Message.pack(&builder, obj: &object)
  Tsugumi_VMHost_Message.finish(&builder, end: offset)
  return builder.data
}
final class Peer: @unchecked Sendable {
  let fd: Int32
  private let lock = NSLock()
  private var closed = false
  init(_ fd: Int32) { self.fd = fd }
  func emit(_ data: Data) { lock.withLock { if !closed { do { try VMHostIPC.write(data, to: fd) } catch { shutdown(fd, SHUT_RDWR) } } } }
  func close() { lock.withLock { if !closed { closed = true; shutdown(fd, SHUT_RDWR); Darwin.close(fd) } } }
}
final class Watchers: @unchecked Sendable {
  private let lock = NSLock()
  private var peers: [Int32: Peer] = [:]
  func add(_ peer: Peer) -> Bool { lock.withLock { guard peers.count < 8 else { return false }; peers[peer.fd] = peer; return true } }
  func remove(_ peer: Peer) -> Bool { lock.withLock {
    guard peers.removeValue(forKey: peer.fd) != nil else { return false }
    return peers.isEmpty
  } }
  func emit(_ data: Data) { for peer in lock.withLock({ Array(peers.values) }) { peer.emit(data) } }
}
let watchers = Watchers()
func runtime() -> Tsugumi_Host_HostRuntimeT {
  let value = Tsugumi_Host_HostRuntimeT()
  switch vm.state {
  case .stopped: value.vmState = "stopped"
  case .running: value.vmState = "running"
  case .starting(let stage): value.vmState = "starting"; value.stage = stage.rawValue
  case .failed(let message): value.vmState = "failed"; value.message = message
  }
  value.storePath = vm.directory.appendingPathComponent("store.img").path
  value.storeExists = FileManager.default.fileExists(atPath: value.storePath ?? "")
  value.virtualizationSupported = VZVirtualMachine.isSupported
  value.imageAvailable = image != nil
  value.imageVersion = image?.version
  return value
}
_ = vm.observe { _ in watchers.emit(encode(runtime(), event: .hostruntime)) }
_ = vm.console.observe { line in let value = runtime(); value.console = [line]; watchers.emit(encode(value, event: .hostruntime)) }

final class Terminals: @unchecked Sendable {
  private let lock = NSLock()
  private var descriptors: [UInt64: Int32] = [:]
  func add(_ fd: Int32, _ session: UInt64) {
    lock.withLock { descriptors[session] = fd }
    Thread.detachNewThread { [self] in
      var buffer = [UInt8](repeating: 0, count: 16384)
      while true {
        let n = Darwin.read(fd, &buffer, buffer.count)
        if n < 0, errno == EINTR { continue }
        if n <= 0 { break }
        let value = Tsugumi_Host_HostTerminalDataT(); value.session = session; value.data = Array(buffer[..<n])
        watchers.emit(encode(value, event: .hostterminaldata))
      }
      lock.withLock { descriptors[session] = nil }
      Darwin.close(fd)
      let value = Tsugumi_Host_HostTerminalDataT(); value.session = session; value.closed = true
      watchers.emit(encode(value, event: .hostterminaldata))
    }
  }
  func closeAll() { lock.withLock { for fd in descriptors.values { shutdown(fd, SHUT_RDWR) } } }
  func input(_ session: UInt64, _ data: [UInt8]) throws {
    try lock.withLock {
      guard let fd = descriptors[session] else { return }
      try writeAllToTerminal(fd, Data(data))
    }
  }
  func close(_ session: UInt64) { lock.withLock { if let fd = descriptors[session] { shutdown(fd, SHUT_RDWR) } } }
}
func writeAllToTerminal(_ fd: Int32, _ data: Data) throws {
  try data.withUnsafeBytes { buffer in
    var offset = 0
    while offset < buffer.count {
      let n = Darwin.write(fd, buffer.baseAddress! + offset, buffer.count - offset)
      if n < 0, errno == EINTR { continue }
      guard n > 0 else { throw LauncherError.notReady("terminal stream closed") }
      offset += n
    }
  }
}
let terminals = Terminals()
actor Driver {
  private var bridges: [String: LoopbackBridge] = [:]
  func boot(_ request: Tsugumi_VMHost_BootT) async throws {
    try vm.configureBroker(request.brokerEndpoint)
    guard let image else { throw LauncherError.imageMissing }
    let preparing = !FileManager.default.fileExists(atPath: vm.directory.appendingPathComponent("store.img").path)
    let token = vm.observe { state in
      if case .starting(let stage) = state {
        let value = Tsugumi_VMHost_ProgressT(); value.key = request.key ?? ""; value.stage = EnvironmentStage(vmStage: stage).rawValue; value.preparing = preparing
        value.fraction = Double(EnvironmentStage.allCases.firstIndex(of: EnvironmentStage(vmStage: stage)) ?? 0) / Double(EnvironmentStage.allCases.count)
        watchers.emit(encode(value, event: .progress))
      }
    }
    defer { vm.stopObserving(token) }
    try await vm.start(image: image, resources: resources)
  }
  func validate(_ key: String) throws {
    guard key.count == 16, key.allSatisfy({ $0.isHexDigit }) else { throw LauncherError.notReady("invalid project identity") }
  }
  func shares(_ key: String, _ mounts: [Tsugumi_VMHost_MountT?]) async throws -> Int {
    try validate(key)
    var entries: [String: URL] = [:], readOnly: Set<String> = []
    for mount in mounts.compactMap({ $0 }) {
      guard !mount.name.contains("/"), mount.path.hasPrefix("/") else { throw LauncherError.notReady("invalid VM mount") }
      entries[mount.name] = URL(fileURLWithPath: mount.path)
      if mount.readOnly { readOnly.insert(mount.name) }
    }
    return try await vm.attach(key, entries, readOnly: readOnly)
  }
  func process(_ key: String) async throws -> Tsugumi_VMHost_ProcessT {
    try validate(key)
    let value = Tsugumi_VMHost_ProcessT()
    guard case .running = vm.state else { return value }
    let status = try await vm.request("status", timeout: 10)
    for item in status.split(separator: " ") {
      let parts = item.split(separator: ":")
      if parts.count == 2, parts[0] == key, let pid = Int64(parts[1]) {
        value.pid = pid; value.running = true
        if bridges[key] == nil { bridges[key] = try LoopbackBridge(vm: vm, projectId: key) }
        value.endpoint = "ws://127.0.0.1:\(bridges[key]!.port)/rpc"
      }
    }
    return value
  }
  func run(_ request: Tsugumi_VMHost_RunT) async throws -> Tsugumi_VMHost_ProcessT {
    let slot = try await shares(request.key, request.mounts)
    let command = request.archiveIncompatibleHistory ? "archive-history" : "start"
    var last: Error = LauncherError.notReady("mount is not ready")
    for _ in 0..<50 {
      do {
        let origins = request.origins.compactMap { $0 }.joined(separator: ",")
        _ = try await vm.request("\(command) \(request.key) \(request.cpus > 0 ? request.cpus : UInt32(resources.cpus)) \(request.hostName) \(slot)" + (origins.isEmpty ? "" : " " + origins))
        return try await process(request.key)
      } catch { last = error; try await Task.sleep(for: .milliseconds(100)) }
    }
    throw last
  }
  func stop(_ key: String) async throws {
    try validate(key)
    if case .running = vm.state { _ = try await vm.request("stop \(key)") }
    bridges.removeValue(forKey: key)?.close()
    await vm.detach(key)
  }
  func operation(_ request: Tsugumi_Host_HostRuntimeRequestT) async throws -> Tsugumi_Host_HostRuntimeT {
    let result = runtime()
    switch request.action {
    case .state: result.console = vm.console.snapshot.suffix(200).map { Optional($0) }
    case .shutdown: await vm.shutdown(); for bridge in bridges.values { bridge.close() }; bridges.removeAll()
    case .cleanupstore:
      let fields = try await vm.request("gc", timeout: 3600).split(separator: " ")
      result.bytes = UInt64(fields.first ?? "") ?? 0; result.paths = UInt32(fields.dropFirst().first ?? "") ?? 0
    case .histories:
      result.histories = try await vm.request("histories").split(separator: " ").compactMap { item in
        let fields = item.split(separator: ":")
        guard fields.count == 4 else { return nil }
        let row = Tsugumi_Host_RuntimeHistoryT(); row.id = String(fields[0]); row.bytes = UInt64(fields[1]) ?? 0; row.lastStartMs = Int64(fields[2]) ?? 0; row.running = fields[3] == "1"; return row
      }
    case .deletehistory:
      let key = request.projectId ?? ""
      guard key.count == 16, key.allSatisfy({ $0.isHexDigit }) else { throw LauncherError.notReady("invalid project identity") }
      guard !(try await process(key)).running else { throw LauncherError.notReady("the project is running; stop it first") }
      _ = try await vm.request("forget \(key)")
    case .reset:
      await vm.shutdown(); for bridge in bridges.values { bridge.close() }; bridges.removeAll()
      for name in (try? FileManager.default.contentsOfDirectory(atPath: vm.directory.path)) ?? [] where name != "history.img" {
        try FileManager.default.removeItem(at: vm.directory.appendingPathComponent(name)); result.vmRemoved = true
      }
    case .command:
      guard case .running = vm.state else { throw LauncherError.notReady("the VM is not running") }
      result.message = try await vm.request(request.command ?? "")
    default: throw LauncherError.notReady("unsupported VM runtime operation")
    }
    let current = runtime(); result.vmState = current.vmState; result.storeExists = current.storeExists
    return result
  }
  func terminal(_ request: Tsugumi_Host_HostTerminalRequestT, _ peer: Peer, id: UInt64) async throws {
    let result = Tsugumi_Host_HostTerminalT(); result.session = request.session
    switch request.action {
    case .open_:
      guard let key = request.project, (try await process(key)).running else { throw LauncherError.notReady("the project is not running") }
      let fd = try await vm.connect(port: SharedVM.controlPort)
      var timeout = timeval(tv_sec: 5, tv_usec: 0)
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
      do {
        try writeAllToTerminal(fd, Data("shell \(key) \(max(request.rows, 2)) \(max(request.cols, 10))\n".utf8))
        var line = Data(), byte: UInt8 = 0
        while Darwin.read(fd, &byte, 1) == 1, byte != 10 { line.append(byte) }
        let fields = String(decoding: line, as: UTF8.self).split(separator: " ")
        guard fields.count == 2, fields[0] == "ok", let session = UInt64(fields[1]) else { throw LauncherError.notReady("cannot open terminal") }
        result.session = session
        peer.emit(encode(result, reply: .hostterminal, id: id))
        terminals.add(fd, session)
        return
      } catch { Darwin.close(fd); throw error }
    case .input: try terminals.input(request.session, request.data)
    case .resize: _ = try await vm.request("resize \(request.session) \(max(request.rows, 2)) \(max(request.cols, 10))", timeout: 5)
    case .close: terminals.close(request.session)
    }
    peer.emit(encode(result, reply: .hostterminal, id: id))
  }
}
let driver = Driver()
func serve(_ message: Tsugumi_VMHost_MessageT, _ peer: Peer) async throws {
  guard message.requestId > 0, let request = message.payload?.value as? Tsugumi_VMHost_RequestT, let value = request.value?.value else { throw LauncherError.notReady("invalid VM adapter request") }
  let id = message.requestId
  switch value {
  case is Tsugumi_VMHost_WatchT:
    peer.emit(encode(Tsugumi_Service_AcknowledgementT(), reply: .acknowledgement, id: id))
    guard watchers.add(peer) else { throw LauncherError.notReady("too many VM watchers") }
    peer.emit(encode(runtime(), event: .hostruntime))
  case let request as Tsugumi_VMHost_BootT:
    try await driver.boot(request); peer.emit(encode(Tsugumi_Service_AcknowledgementT(), reply: .acknowledgement, id: id))
  case let request as Tsugumi_VMHost_RunT: peer.emit(encode(try await driver.run(request), reply: .process, id: id))
  case let request as Tsugumi_VMHost_ProcessRequestT:
    if request.action == .stop { try await driver.stop(request.key) }
    peer.emit(encode(try await driver.process(request.key), reply: .process, id: id))
  case let request as Tsugumi_VMHost_SharesT:
    _ = try await driver.shares(request.key, request.mounts); peer.emit(encode(Tsugumi_Service_AcknowledgementT(), reply: .acknowledgement, id: id))
  case let request as Tsugumi_Host_HostRuntimeRequestT:
    peer.emit(encode(try await driver.operation(request), reply: .hostruntime, id: id))
    if request.action == .reset { unlink(socketPath); exit(0) }
  case let request as Tsugumi_Host_HostTerminalRequestT: try await driver.terminal(request, peer, id: id)
  default: throw LauncherError.notReady("unknown VM adapter operation")
  }
}
Thread.detachNewThread {
  while true {
    let fd = accept(listener, nil, nil)
    if fd < 0 { if errno == EINTR { continue }; break }
    _ = fcntl(fd, F_SETFD, FD_CLOEXEC); noSigPipe(fd)
    var uid: uid_t = 0, gid: gid_t = 0
    guard getpeereid(fd, &uid, &gid) == 0, uid == geteuid() else { Darwin.close(fd); continue }
    var timeout = timeval(tv_sec: 5, tv_usec: 0)
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
    let peer = Peer(fd)
    Thread.detachNewThread {
      defer { if watchers.remove(peer) { terminals.closeAll() }; peer.close() }
      do {
        while let bytes = try VMHostIPC.read(fd) {
          var buffer = ByteBuffer(data: bytes)
          let root: Tsugumi_VMHost_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSV1")
          let message = Tsugumi_VMHost_MessageT(root)
          let finished = DispatchSemaphore(value: 0)
          Task {
            defer { finished.signal() }
            do { try await serve(message, peer) }
            catch {
              let failure = Tsugumi_VMHost_FailureT(); failure.message = "\(error)"; failure.recovery = (error as? LauncherError)?.recovery ?? "Retry the virtual machine"
              peer.emit(encode(failure, reply: .failure, id: message.requestId))
            }
          }
          finished.wait()
        }
      } catch {}
    }
  }
}
dispatchMain()
