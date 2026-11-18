// Signed Virtualization.framework adapter driven exclusively by hostd.
import Darwin
import FlatBuffers
import Foundation
import TsugumiVMRuntime
import TsugumiVMTypes
import TsugumiRPC
import Virtualization

signal(SIGPIPE, SIG_IGN)
_ = setsid()
var support: URL?, imageDir: URL?
var arguments = CommandLine.arguments.dropFirst()
while let argument = arguments.popFirst() {
  guard let value = arguments.popFirst() else { exit(2) }
  switch argument {
  case "--support": support = URL(fileURLWithPath: value)
  case "--image": imageDir = URL(fileURLWithPath: value)
  default: exit(2)
  }
}
guard let support, let imageDir else { exit(2) }
try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
chmod(support.path, 0o700)
let hostLock = open(support.appendingPathComponent("vmhost.lock").path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0o600)
guard hostLock >= 0 else { exit(1) }
guard flock(hostLock, LOCK_EX | LOCK_NB) == 0 else { exit(0) }
let socketPath = VMHostIPC.socketPath(support: support)
let listener = try VMHostIPC.listen(path: socketPath)
let vm = SharedVM(directory: support.appendingPathComponent("vm"))
let image = VMImage(directory: imageDir)

// The spawning UI may quit while work continues. The host's Watch connection
// owns the resident VM; EOF (including a host crash) ends it orderly.
final class ExitState: @unchecked Sendable {
  private let lock = NSLock()
  private var ending = false
  var isEnding: Bool { lock.withLock { ending } }
  func begin() { lock.withLock { ending = true } }

}
let exitState = ExitState()
@Sendable func endVMHost(signal: Bool = false) async {
  exitState.begin()
  await vm.shutdown(signal: signal, exiting: true)
  unlink(socketPath)
  exit(0)
}
let signalQueue = DispatchQueue(label: "app.tsugumi.vmhost.signals")
let stopSignals = [SIGTERM, SIGINT, SIGHUP].map { number in
  signal(number, SIG_IGN)
  let source = DispatchSource.makeSignalSource(signal: number, queue: signalQueue)
  source.setEventHandler {
    for _ in 0..<max(1, source.data) { Task { await endVMHost(signal: true) } }
  }
  source.resume()
  return source
}

@Sendable func encode(_ value: NativeObject, reply: Tsugumi_VMHost_ReplyPayload? = nil, event: Tsugumi_VMHost_EventPayload? = nil, id: UInt64 = 0) -> Data {
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
  func add(_ peer: Peer) { lock.withLock { peers[peer.fd] = peer } }
  func remove(_ peer: Peer) -> Bool { lock.withLock {
    guard peers.removeValue(forKey: peer.fd) != nil else { return false }
    return peers.isEmpty
  } }
  var isEmpty: Bool { lock.withLock { peers.isEmpty } }
  func emit(_ data: Data) { for peer in lock.withLock({ Array(peers.values) }) { peer.emit(data) } }
}
let watchers = Watchers()
@Sendable func runtime() -> Tsugumi_Host_HostRuntimeT {
  let value = Tsugumi_Host_HostRuntimeT()
  switch vm.state {
  case .stopped: value.vmState = "stopped"
  case .running: value.vmState = "running"
  case .starting(let stage): value.vmState = "starting"; value.stage = stage.rawValue
  case .failed(let message): value.vmState = "failed"; value.message = message
  }
  value.storePath = vm.directory.appendingPathComponent("store.img").path
  value.storeExists = FileManager.default.fileExists(atPath: value.storePath ?? "")
  let size = (try? FileManager.default.attributesOfItem(atPath: value.storePath ?? "")[.size] as? UInt64) ?? 0
  value.storeDiskGib = UInt32(size >> 30)
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
  func input(_ session: UInt64, _ data: [UInt8]) async throws {
    let fd = lock.withLock { descriptors[session].map { dup($0) } ?? -1 }
    guard fd >= 0 else { return }
    try await withCheckedThrowingContinuation { (answer: CheckedContinuation<Void, Error>) in
      Thread.detachNewThread {
        defer { Darwin.close(fd) }
        do { try writeAllToTerminal(fd, Data(data)); answer.resume() }
        catch { answer.resume(throwing: error) }
      }
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
    guard !exitState.isEnding else { throw LauncherError.notReady("the VM host is stopping") }
    vm.configureBroker(request.brokerSocket)
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
    let size = request.resources
    let resources = VMResources(cpus: Int(size.cpus), memoryGiB: Int(size.memoryGib), storeGiB: Int(size.storeGib))
    try await vm.start(image: image, resources: resources, cacheConfiguration: request.cacheConfiguration ?? "", cacheNetrc: request.cacheNetrc ?? "", cacheProblem: request.cacheProblem ?? "")
  }
  func shares(_ key: String, _ mounts: [Tsugumi_VMHost_MountT?]) async throws -> Int {
    var entries: [String: URL] = [:], readOnly: Set<String> = []
    for mount in mounts.compactMap({ $0 }) {
      entries[mount.name] = URL(fileURLWithPath: mount.path)
      if mount.readOnly { readOnly.insert(mount.name) }
    }
    return try await vm.attach(key, entries, readOnly: readOnly)
  }
  func process(_ key: String) async throws -> Tsugumi_VMHost_ProcessT {
    let value = Tsugumi_VMHost_ProcessT()
    guard case .running = vm.state else { return value }
    let status = try await vm.request("status")
    for item in status.split(separator: " ") {
      let parts = item.split(separator: ":")
      if parts.count == 2, parts[0] == key, let pid = Int64(parts[1]) {
        value.pid = pid; value.running = true
        if bridges[key] == nil { bridges[key] = try LoopbackBridge(vm: vm, projectId: key) }
        value.endpoint = "ws://127.0.0.1:\(bridges[key]!.port)/rpc"
      }
    }
    if !value.running { await detach(key) }
    return value
  }
  func run(_ request: Tsugumi_VMHost_RunT) async throws -> Tsugumi_VMHost_ProcessT {
    guard !exitState.isEnding else { throw LauncherError.notReady("the VM host is stopping") }
    let slot = try await shares(request.key, request.mounts)
    let origins = request.origins.compactMap { $0 }.joined(separator: ",")
    _ = try await vm.request("start \(request.key) \(request.hostName) \(slot)" + (origins.isEmpty ? "" : " " + origins))
    return try await process(request.key)
  }
  func stop(_ key: String) async throws {
    if case .running = vm.state { _ = try await vm.request("stop \(key)") }
    await detach(key)
  }
  private func detach(_ key: String) async {
    bridges.removeValue(forKey: key)?.close()
    await vm.detach(key)
  }
  func operation(_ request: Tsugumi_Host_HostRuntimeRequestT) async throws -> Tsugumi_Host_HostRuntimeT {
    let result = runtime()
    switch request.action {
    case .state: result.console = vm.console.snapshot.suffix(200).map { Optional($0) }
    case .shutdown: exitState.begin(); await vm.shutdown(exiting: true); for bridge in bridges.values { bridge.close() }; bridges.removeAll()
    case .verifystore:
      await vm.hold(); defer { vm.release() }
      result.message = try await vm.request("verify-store")
    case .cleanupstore:
      await vm.hold(); defer { vm.release() }
      try await vm.request("gc")
    case .forgetproject:
      let key = request.projectId ?? ""
      try await vm.request("forget-stopped \(key)")
      await detach(key)
    case .command:
      guard case .running = vm.state else { throw LauncherError.notReady("the VM is not running") }
      result.message = try await vm.request(request.command ?? "")
    default: throw LauncherError.notReady("unsupported VM runtime operation")
    }
    let current = runtime(); result.vmState = current.vmState; result.storeExists = current.storeExists; result.storeDiskGib = current.storeDiskGib
    return result
  }
  func reset(_ request: Tsugumi_VMHost_ResetT) async throws -> Tsugumi_Host_HostRuntimeT {
    exitState.begin()
    await vm.shutdown(exiting: true)
    for bridge in bridges.values { bridge.close() }
    bridges.removeAll()
    let report = try VMReset.apply(directory: vm.directory,
                                  keepFiles: request.keepFiles.compactMap { $0 },
                                  discardOtherFiles: request.discardOtherFiles)
    let result = runtime()
    result.resetKeptFiles = report.kept.map(Optional.some)
    result.resetDiscardedFiles = report.discarded.map(Optional.some)
    result.vmRemoved = !report.discarded.isEmpty
    return result
  }
  func terminal(_ request: Tsugumi_Host_HostTerminalRequestT, _ peer: Peer, id: UInt64) async throws {
    let result = Tsugumi_Host_HostTerminalT(); result.session = request.session
    switch request.action {
    case .open_:
      guard let key = request.project else { throw LauncherError.notReady("the project is not running") }
      let fd = try await vm.connect(port: SharedVM.controlPort)
      do {
        try writeAllToTerminal(fd, Data("shell \(key) \(max(request.rows, 2)) \(max(request.cols, 10))\n".utf8))
        var line = Data(), byte: UInt8 = 0
        while Darwin.read(fd, &byte, 1) == 1, byte != 10 { line.append(byte) }
        let reply = String(decoding: line, as: UTF8.self)
        let fields = reply.split(separator: " ")
        guard fields.count == 2, fields[0] == "ok", let session = UInt64(fields[1]) else {
          let reason = reply.hasPrefix("error ") ? String(reply.dropFirst(6)) : "cannot open terminal"
          throw LauncherError.notReady(reason)
        }
        result.session = session
        peer.emit(encode(result, reply: .hostterminal, id: id))
        terminals.add(fd, session)
        return
      } catch { Darwin.close(fd); throw error }
    case .input: try await terminals.input(request.session, request.data)
    case .resize: _ = try await vm.request("resize \(request.session) \(max(request.rows, 2)) \(max(request.cols, 10))")
    case .close: terminals.close(request.session)
    }
    peer.emit(encode(result, reply: .hostterminal, id: id))
  }
}
let driver = Driver()
@Sendable func serve(_ message: Tsugumi_VMHost_MessageT, _ peer: Peer) async throws {
  guard let request = message.payload?.value as? Tsugumi_VMHost_RequestT, let value = request.value?.value else { throw LauncherError.notReady("invalid VM adapter request") }
  let id = message.requestId
  switch value {
  case is Tsugumi_VMHost_WatchT:
    peer.emit(encode(Tsugumi_Service_AcknowledgementT(), reply: .acknowledgement, id: id))
    watchers.add(peer)
    peer.emit(encode(runtime(), event: .hostruntime))
  case let request as Tsugumi_VMHost_BootT:
    try await driver.boot(request); peer.emit(encode(Tsugumi_Service_AcknowledgementT(), reply: .acknowledgement, id: id))
  case let request as Tsugumi_VMHost_RunT: peer.emit(encode(try await driver.run(request), reply: .process, id: id))
  case let request as Tsugumi_VMHost_ProcessRequestT:
    if request.action == .stop { try await driver.stop(request.key) }
    peer.emit(encode(try await driver.process(request.key), reply: .process, id: id))
  case let request as Tsugumi_Host_HostRuntimeRequestT:
    peer.emit(encode(try await driver.operation(request), reply: .hostruntime, id: id))
    if request.action == .shutdown { unlink(socketPath); exit(0) }
  case let request as Tsugumi_VMHost_ResetT:
    peer.emit(encode(try await driver.reset(request), reply: .hostruntime, id: id))
    unlink(socketPath); exit(0)
  case let request as Tsugumi_Host_HostTerminalRequestT: try await driver.terminal(request, peer, id: id)
  default: throw LauncherError.notReady("unknown VM adapter operation")
  }
}
Thread.detachNewThread {
  while true {
    let fd = accept(listener, nil, nil)
    if fd < 0 { if errno == EINTR { continue }; break }
    _ = fcntl(fd, F_SETFD, FD_CLOEXEC); noSigPipe(fd)
    let peer = Peer(fd)
    Thread.detachNewThread {
      defer {
        if watchers.remove(peer) { terminals.closeAll(); Task { await endVMHost() } }
        peer.close()
      }
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
