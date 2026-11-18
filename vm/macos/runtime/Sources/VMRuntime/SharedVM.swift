import Darwin
import Foundation
// (its configuration types are not Sendable; they are handed to the VM queue only)
@preconcurrency import Virtualization
@_exported import TsugumiVMTypes

/// The shared VM's boot disk (vm/guest/vm-boot.nix: kernel and a
/// release-built root file system with Nix, Tsugumi and project tools).
/// How the VM runs x86_64-linux binaries (Cards with "system": "x86_64-linux"):
/// with Rosetta when it is installed on this Mac, with qemu only when
/// TSUGUMI_VM_X86=qemu asks for it. The guest's supervisor (vm-system.nix)
/// registers the one named on the kernel command line (tsugumi.x86=...).
enum X86Emulation: String {
  case rosetta, qemu, none

  static var chosen: X86Emulation {
    if ProcessInfo.processInfo.environment["TSUGUMI_VM_X86"] == "qemu" { return .qemu }
    // Never installed from here: that is a system dialog.
    return VZLinuxRosettaDirectoryShare.availability == .installed ? .rosetta : .none
  }
}

/// The one Linux VM of the app (Virtualization.framework). It boots from the
/// bundled kernel and read-only root file system, keeps the shared Nix store
/// on a sparse disk image in the support
/// folder, and runs one tsugumid per
/// open project (tsugumi-vmd). Each open project gets one of `slotCount`
/// virtiofs devices for its folders: replacing one device's share while the
/// VM runs leaves the others alone (replacing a share that other projects
/// had mounted made their files disappear).
public final class SharedVM: NSObject, VZVirtualMachineDelegate, @unchecked Sendable {
  public static let controlPort: UInt32 = 1024
  public static let rpcPort: UInt32 = 1025

  public let directory: URL
  public let console = ConsoleLog()
  private let queue = DispatchQueue(label: "app.tsugumi.vm")
  private let lock = NSLock()
  private let shutdownController = VMShutdown()
  private var stopping = false
  private var ending = false
  private var bootGeneration: UInt64 = 0
  private var vm: VZVirtualMachine?
  public static let slotCount = 16
  private var fileSystems: [VZVirtioFileSystemDevice] = []
  private var socket: VZVirtioSocketDevice?
  let executionBroker: ExecutionBrokerBridge
  private var slots: [String: Int] = [:]  // project id -> slot
  /// The VM that runs now (under `lock`): a stop reported for an earlier one
  /// changes nothing.
  private var current: ObjectIdentifier?
  private var _state: VMState = .stopped
  private var stateObservers: [UUID: @Sendable (VMState) -> Void] = [:]
  private var stopWaiters: [CheckedContinuation<Void, Never>] = []
  private var readyWaiters: [CheckedContinuation<Void, Error>] = []

  public init(directory: URL) {
    self.directory = directory
    executionBroker = ExecutionBrokerBridge(support: directory.deletingLastPathComponent())
  }

  public func configureBroker(_ endpoint: String) throws { try executionBroker.configure(endpoint) }

  public var state: VMState {
    lock.lock()
    defer { lock.unlock() }
    return _state
  }

  public func observe(_ handler: @escaping @Sendable (VMState) -> Void) -> UUID {
    let id = UUID()
    lock.lock()
    stateObservers[id] = handler
    let s = _state
    lock.unlock()
    handler(s)
    return id
  }

  public func stopObserving(_ id: UUID) {
    lock.lock()
    stateObservers[id] = nil
    lock.unlock()
  }

  private func setVMState(_ s: VMState) {
    lock.lock()
    _state = s
    let obs = Array(stateObservers.values)
    var waiters: [CheckedContinuation<Void, Error>] = []
    if case .running = s { waiters = readyWaiters; readyWaiters = [] }
    if case .failed = s { waiters = readyWaiters; readyWaiters = [] }
    if case .stopped = s { waiters = readyWaiters; readyWaiters = [] }
    let stopped = current == nil ? stopWaiters : []
    if current == nil { stopWaiters.removeAll() }
    lock.unlock()
    for waiter in stopped { waiter.resume() }
    for o in obs { o(s) }
    for w in waiters {
      switch s {
      case .running: w.resume()
      case .failed(let m): w.resume(throwing: LauncherError.startFailed(reason: m, log: console.snapshot.suffix(80).joined(separator: "\n")))
      default: w.resume(throwing: LauncherError.startFailed(reason: "the VM stopped", log: console.snapshot.suffix(80).joined(separator: "\n")))
      }
    }
  }

  var storeImage: URL { directory.appendingPathComponent("store.img") }
  var consoleURL: URL { directory.appendingPathComponent("console.log") }

  // MARK: - Start and stop

  /// Starts the VM (first start: copies the image and creates the store
  /// disk) and returns once tsugumi-vmd is ready. Concurrent callers wait
  /// for the same start.
  public func start(image: VMImage, resources: VMResources, cacheConfiguration: String, cacheNetrc: String, cacheProblem: String) async throws {
    let begin: Bool = try {
      lock.lock()
      defer { lock.unlock() }
      guard !stopping, !ending else { throw LauncherError.notReady("the VM is shutting down") }
      switch _state {
      case .running, .starting: return false
      default:
        _state = .starting(.image)
        return true
      }
    }()
    if !begin {
      if case .running = state { return }
      try await waitReady()
      return
    }
    // A VM of a failed start may still be going down: never two on the same disks.
    if queue.sync(execute: { self.vm != nil }) { await shutdown() }
    lock.withLock { stopping = false }
    let generation = lock.withLock { bootGeneration += 1; _state = .starting(.image); return bootGeneration }
    do {
      try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
      console.open(console: consoleURL)
      setVMState(.starting(.image))
      let files = try installImage(image)
      setVMState(.starting(.disk))
      try ensureDisk(storeImage, gib: resources.storeGiB, what: "store")
      setVMState(.starting(.boot))
      lock.withLock { _bootCount += 1 }
      console.note("starting the VM: \(resources.cpus) CPUs, \(resources.memoryGiB) GiB memory, store \(resources.storeGiB) GiB (image \(image.version)), x86_64-linux: \(X86Emulation.chosen.rawValue)")
      let config = try configuration(files: files, resources: resources, cacheConfiguration: cacheConfiguration, cacheNetrc: cacheNetrc, cacheProblem: cacheProblem)
      try await boot(config, generation: generation)
    } catch {
      console.note("start failed: \(error)")
      await shutdown()
      setVMState(.failed("\(error)"))
      throw error
    }
    do {
      try await waitReady()
    } catch {
      // A failed start leaves no VM behind (a retry starts one afresh).
      let state = self.state
      await shutdown()
      if case .failed = state { setVMState(state) }
      throw error
    }
  }

  private func waitReady() async throws {
    try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
      lock.lock()
      switch _state {
      case .running:
        lock.unlock()
        c.resume()
      case .failed(let m):
        lock.unlock()
        c.resume(throwing: LauncherError.startFailed(reason: m, log: console.snapshot.suffix(80).joined(separator: "\n")))
      case .stopped:
        lock.unlock()
        c.resume(throwing: LauncherError.startFailed(reason: "the VM is not running", log: ""))
      case .starting:
        readyWaiters.append(c)
        lock.unlock()
      }
    }
  }

  private struct BootFiles {
    let kernel: URL
    let rootfs: URL
  }

  /// Extract next to the store so app replacement cannot remove a running VM's files.
  private func installImage(_ image: VMImage) throws -> BootFiles {
    let fm = FileManager.default
    let dest = directory.appendingPathComponent("image", isDirectory: true)
    let stamp = dest.appendingPathComponent("vm-version")
    let have = (try? String(contentsOf: stamp, encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines)
    let files = ["vmlinux", "rootfs.erofs"].map { dest.appendingPathComponent($0) }
    if have != image.version {
      let stage = directory.appendingPathComponent("image-\(UUID().uuidString)", isDirectory: true)
      try fm.createDirectory(at: stage, withIntermediateDirectories: true)
      defer { try? fm.removeItem(at: stage) }
      let process = Process()
      process.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
      process.arguments = ["-xzf", image.archive.path, "-C", stage.path, "vmlinux", "rootfs.erofs", "vm-version"]
      try process.run()
      process.waitUntilExit()
      let version = (try? String(contentsOf: stage.appendingPathComponent("vm-version"), encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines)
      guard process.terminationStatus == 0, version == image.version else {
        throw LauncherError.imageMissing
      }
      if fm.fileExists(atPath: dest.path) { try fm.removeItem(at: dest) }
      try fm.moveItem(at: stage, to: dest)
    }
    return BootFiles(kernel: files[0], rootfs: files[1])
  }

  /// A disk: a sparse file, formatted by the guest on first boot.
  private func ensureDisk(_ url: URL, gib: Int, what: String) throws {
    if FileManager.default.fileExists(atPath: url.path) { return }
    console.note("creating the \(what) disk (\(gib) GiB, sparse)")
    guard FileManager.default.createFile(atPath: url.path, contents: nil) else {
      throw LauncherError.startFailed(reason: "cannot create the \(what) disk", log: url.path)
    }
    let h = try FileHandle(forWritingTo: url)
    try h.truncate(atOffset: UInt64(gib) << 30)
    try h.close()
  }

  private func configuration(files: BootFiles, resources: VMResources, cacheConfiguration: String, cacheNetrc: String, cacheProblem: String) throws -> VZVirtualMachineConfiguration {
    let c = VZVirtualMachineConfiguration()
    let boot = VZLinuxBootLoader(kernelURL: files.kernel)
    let x86 = X86Emulation.chosen
    boot.commandLine = ProcessInfo.processInfo.environment["TSUGUMI_VM_CMDLINE"]
      ?? "console=hvc0 root=/dev/vda rootfstype=erofs ro init=/init loglevel=4 panic=-1 tsugumi.x86=\(x86.rawValue)"
    c.bootLoader = boot
    c.platform = VZGenericPlatformConfiguration()
    c.cpuCount = resources.cpus
    c.memorySize = UInt64(resources.memoryGiB) << 30
    let root = try VZDiskImageStorageDeviceAttachment(url: files.rootfs, readOnly: true)
    let store = try VZDiskImageStorageDeviceAttachment(url: storeImage, readOnly: false,
                                                       cachingMode: .automatic, synchronizationMode: .full)
    c.storageDevices = [root, store].map { VZVirtioBlockDeviceConfiguration(attachment: $0) }
    let net = VZVirtioNetworkDeviceConfiguration()
    net.attachment = VZNATNetworkDeviceAttachment()
    net.macAddress = macAddress()
    c.networkDevices = [net]
    lock.withLock { slots.removeAll() }
    let cacheDirectory = directory.appendingPathComponent("binary-cache-config", isDirectory: true)
    try FileManager.default.createDirectory(at: cacheDirectory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    for (name, contents) in [("nix.conf", cacheConfiguration), ("netrc", cacheNetrc)] {
      let file = cacheDirectory.appendingPathComponent(name)
      let temporary = cacheDirectory.appendingPathComponent(".cache-\(UUID().uuidString)")
      let fd = Darwin.open(temporary.path, O_WRONLY | O_CREAT | O_EXCL, 0o600)
      guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
      let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
      defer { try? handle.close(); try? FileManager.default.removeItem(at: temporary) }
      try handle.write(contentsOf: Data(contents.utf8))
      try handle.close()
      guard rename(temporary.path, file.path) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
    }
    if !cacheProblem.isEmpty { console.note(cacheProblem) }
    let caches = VZVirtioFileSystemDeviceConfiguration(tag: "tsugumi-binary-caches")
    caches.share = VZSingleDirectoryShare(directory: VZSharedDirectory(url: cacheDirectory, readOnly: true))
    c.directorySharingDevices = (0..<Self.slotCount).map { i in
      let fs = VZVirtioFileSystemDeviceConfiguration(tag: "tsugumi-\(i)")
      fs.share = VZMultipleDirectoryShare(directories: [:])
      return fs
    } + [caches] + (x86 == .rosetta ? [try rosettaShare()] : [])
    c.socketDevices = [VZVirtioSocketDeviceConfiguration()]
    c.entropyDevices = [VZVirtioEntropyDeviceConfiguration()]
    c.memoryBalloonDevices = [VZVirtioTraditionalMemoryBalloonDeviceConfiguration()]
    // The guest console goes straight to console.log (a pipe attachment
    // delivered nothing); a reader follows the file for stages and the view.
    let serial = VZVirtioConsoleDeviceSerialPortConfiguration()
    serial.attachment = try VZFileSerialPortAttachment(url: consoleURL, append: false)
    c.serialPorts = [serial]
    followConsole(consoleURL, boot: bootCount)
    try c.validate()
    return c
  }

  /// Rosetta's directory, for the guest to register its binary with binfmt_misc.
  /// After the slots: setShare finds a slot by its index among these devices.
  private func rosettaShare() throws -> VZVirtioFileSystemDeviceConfiguration {
    let fs = VZVirtioFileSystemDeviceConfiguration(tag: "rosetta")
    fs.share = try VZLinuxRosettaDirectoryShare()
    return fs
  }

  private var _bootCount = 0
  private var bootCount: Int { lock.withLock { _bootCount } }

  /// One MAC address per support folder, kept across boots: the Mac's DHCP
  /// gives the VM the same lease again instead of a new one at every start
  /// (a new random address each time used up the pool).
  private func macAddress() -> VZMACAddress {
    let file = directory.appendingPathComponent("mac-address")
    if let text = try? String(contentsOf: file, encoding: .utf8),
       let mac = VZMACAddress(string: text.trimmingCharacters(in: .whitespacesAndNewlines)) { return mac }
    let mac = VZMACAddress.randomLocallyAdministered()
    try? mac.string.write(to: file, atomically: true, encoding: .utf8)
    return mac
  }

  /// The VM's clock follows the Mac's (it drifts, e.g. across sleep), so
  /// times recorded by tsugumid agree with the app's clock.
  private func keepClock() {
    let boot = bootCount
    Task.detached { [weak self] in
      while let self, self.bootCount == boot, case .running = self.state {
        let ms = Int64(Date().timeIntervalSince1970 * 1000)
        _ = try? await self.request("clock \(ms)")
        try? await Task.sleep(for: .seconds(30))
      }
    }
  }

  /// Reads what the guest appends to the console file until the next boot.
  private func followConsole(_ url: URL, boot: Int) {
    Thread.detachNewThread { [weak self] in
      // By path and offset: the attachment may replace the file when it opens it.
      var offset: UInt64 = 0
      var idle = 0.05
      while let self, self.bootCount == boot {
        var data = Data()
        if let h = try? FileHandle(forReadingFrom: url) {
          let size = (try? h.seekToEnd()) ?? 0
          if size < offset { offset = 0 }
          if size > offset {
            try? h.seek(toOffset: offset)
            data = (try? h.read(upToCount: Int(size - offset))) ?? Data()
            offset += UInt64(data.count)
          }
          try? h.close()
        }
        if data.isEmpty {
          if case .stopped = self.state { return }
          if case .failed = self.state { return }
          Thread.sleep(forTimeInterval: idle)
          idle = min(idle * 2, 0.5)
          continue
        }
        idle = 0.05
        for line in self.console.feed(data) { self.consoleLine(line) }
      }
    }
  }

  private func consoleLine(_ line: String) {
    if line.hasPrefix("TSUGUMI-STAGE ") {
      let name = String(line.dropFirst("TSUGUMI-STAGE ".count)).trimmingCharacters(in: .whitespaces)
      if name == "ready" {
        setVMState(.running)
        keepClock()
      } else if let s = VMStage(rawValue: name) { setVMState(.starting(s)) }
    } else if line.hasPrefix("TSUGUMI-FAILED ") {
      setVMState(.failed(String(line.dropFirst("TSUGUMI-FAILED ".count))))
    }
  }

  private func boot(_ config: VZVirtualMachineConfiguration, generation: UInt64) async throws {
    let handed = OnVMQueue(config)
    try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
      queue.async {
        self.lock.lock()
        guard !self.stopping, !self.ending, self.bootGeneration == generation else {
          self.lock.unlock()
          c.resume(throwing: LauncherError.notReady("the VM is shutting down"))
          return
        }
        let vm = VZVirtualMachine(configuration: handed.value, queue: self.queue)
        vm.delegate = self
        self.vm = vm
        self.current = ObjectIdentifier(vm)
        self.lock.unlock()
        self.fileSystems = vm.directorySharingDevices.compactMap { $0 as? VZVirtioFileSystemDevice }
        self.socket = vm.socketDevices.first as? VZVirtioSocketDevice
        if let socket = self.socket { self.executionBroker.install(on: socket) }
        vm.start { result in
          switch result {
          case .success:
            self.console.note("the VM is running; waiting for the guest")
            c.resume()
          case .failure(let e):
            self.stopped(vm, e)
            c.resume(throwing: LauncherError.startFailed(reason: "the VM did not start: \(e.localizedDescription)", log: ""))
          }
        }
      }
    }
  }

  /// Waits for guest poweroff. A second stop signal explicitly forces it.
  public func shutdown(signal: Bool = false, exiting: Bool = false) async {
    lock.withLock { stopping = true; ending = ending || exiting; bootGeneration += 1 }
    await shutdownController.stop(signal: signal, graceful: { [self] in
      guard lock.withLock({ current != nil }) else { return }
      console.note("shutting down; waiting for guest sync and poweroff")
      // A boot still preparing the store announces when its control socket is ready.
      if (try? await waitReady()) != nil, !Task.isCancelled {
        do { try await request("shutdown") }
        catch { console.note("shutdown request: \(error)") }
      }
      await withCheckedContinuation { continuation in
        lock.withLock {
          if current == nil { continuation.resume() }
          else { stopWaiters.append(continuation) }
        }
      }
    }, force: { [self] in
      console.note("forcing the VM off; the next boot will check the store")
      await forceStop()
    })
    // A stopped instance may be started again by a later explicit request.
    lock.withLock { stopping = false }
  }

  private func forceStop() async {
    let vm: VZVirtualMachine? = await withCheckedContinuation { (c: CheckedContinuation<VZVirtualMachine?, Never>) in
      queue.async {
        guard let vm = self.vm else { c.resume(returning: nil); return }
        guard vm.canStop else { c.resume(returning: vm); return }
        vm.stop { _ in c.resume(returning: vm) }
      }
    }
    if let vm { stopped(vm, nil) }
  }

  /// `which` stopped: the state follows only when it is the current VM.
  private func stopped(_ which: VZVirtualMachine, _ error: Error?) {
    let id = ObjectIdentifier(which)
    let isCurrent: Bool = lock.withLock {
      guard current == id else { return false }
      current = nil
      return true
    }
    guard isCurrent else { return }
    let handed = OnVMQueue(which)
    queue.async {
      if self.vm === handed.value {
        self.vm = nil
        self.fileSystems = []
        self.socket = nil
      }
    }
    if let error {
      console.note("the VM stopped with an error: \(error.localizedDescription)")
      setVMState(.failed("the VM stopped: \(error.localizedDescription)"))
    } else {
      console.note("the VM stopped")
      setVMState(.stopped)
    }
  }

  public func guestDidStop(_ virtualMachine: VZVirtualMachine) { stopped(virtualMachine, nil) }

  public func virtualMachine(_ virtualMachine: VZVirtualMachine, didStopWithError error: Error) { stopped(virtualMachine, error) }

  // MARK: - Shared folders

  /// Gives the project a slot (its own virtiofs device, `tsugumi-<slot>`)
  /// sharing `entries` (name -> folder); the same slot again for a project
  /// that has one. Only before the guest mounts the slot: the guest cannot
  /// reach the folders it had open once the device's share is replaced.
  public func attach(_ projectId: String, _ entries: [String: URL], readOnly: Set<String> = []) async throws -> Int {
    let slot: Int? = lock.withLock {
      if let s = slots[projectId] { return s }
      let used = Set(slots.values)
      guard let free = (0..<Self.slotCount).first(where: { !used.contains($0) }) else { return nil }
      slots[projectId] = free
      return free
    }
    guard let slot else {
      throw LauncherError.startFailed(reason: "at most \(Self.slotCount) projects can be open at once", log: "")
    }
    var dirs: [String: VZSharedDirectory] = [:]
    for (name, url) in entries { dirs[name] = VZSharedDirectory(url: url, readOnly: readOnly.contains(name)) }
    await setShare(slot, dirs)
    return slot
  }

  /// Empties the project's slot (after its tsugumid stopped and unmounted it).
  public func detach(_ projectId: String) async {
    guard let slot = lock.withLock({ slots.removeValue(forKey: projectId) }) else { return }
    await setShare(slot, [:])
  }

  private func setShare(_ slot: Int, _ dirs: [String: VZSharedDirectory]) async {
    let handed = OnVMQueue(dirs)
    await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
      queue.async {
        if slot < self.fileSystems.count { self.fileSystems[slot].share = VZMultipleDirectoryShare(directories: handed.value) }
        c.resume()
      }
    }
  }

  // MARK: - vsock

  /// A connected socket to a guest vsock port (the caller closes it).
  public func connect(port: UInt32) async throws -> Int32 {
    try await withCheckedThrowingContinuation { (c: CheckedContinuation<Int32, Error>) in
      queue.async {
        guard let socket = self.socket else {
          c.resume(throwing: LauncherError.notReady("the VM is not running"))
          return
        }
        socket.connect(toPort: port) { result in
          switch result {
          case .success(let conn):
            let fd = dup(conn.fileDescriptor)
            conn.close()
            if fd >= 0 { noSigPipe(fd) }
            if fd < 0 { c.resume(throwing: LauncherError.notReady("vsock dup failed")) } else { c.resume(returning: fd) }
          case .failure(let e):
            c.resume(throwing: LauncherError.notReady("vsock port \(port): \(e.localizedDescription)"))
          }
        }
      }
    }
  }

  /// One request line to tsugumi-vmd; returns the reply after "ok" or throws
  /// with the reason after "error".
  @discardableResult
  public func request(_ line: String) async throws -> String {
    let fd = try await connect(port: Self.controlPort)
    let reply = try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<String, Error>) in
      Thread.detachNewThread {
        defer { close(fd) }
        do {
          try writeAll(fd, Data((line + "\n").utf8))
          continuation.resume(returning: readLine(fd))
        } catch {
          continuation.resume(throwing: error)
        }
      }
    }
    if reply == "ok" { return "" }
    if reply.hasPrefix("ok ") { return String(reply.dropFirst(3)) }
    throw LauncherError.startFailed(reason: reply.hasPrefix("error ") ? String(reply.dropFirst(6)) : "no reply from the VM", log: "")
  }
}

func writeAll(_ fd: Int32, _ data: Data) throws {
  try data.withUnsafeBytes { (buf: UnsafeRawBufferPointer) in
    var off = 0
    while off < buf.count {
      let n = write(fd, buf.baseAddress! + off, buf.count - off)
      if n <= 0 { throw LauncherError.notReady("write failed") }
      off += n
    }
  }
}

/// One reply line. Lists (histories, status) grow with the projects, so the
/// line may be long; a cut one would leave its rest for the next reply.
func readLine(_ fd: Int32) -> String {
  var bytes: [UInt8] = []
  var c: UInt8 = 0
  while bytes.count < 1 << 20 {
    let n = read(fd, &c, 1)
    if n <= 0 || c == 10 { break }
    bytes.append(c)
  }
  return String(decoding: bytes, as: UTF8.self)
}

/// Forwards connections to 127.0.0.1:<port> on the Mac to one project's
/// tsugumid in the VM (vsock RPC port, first line = project id), so the
/// RPC client keeps using a plain local WebSocket URL.
public final class LoopbackBridge: @unchecked Sendable {
  public let port: Int
  private let listener: Int32
  private let projectId: String
  private weak var vm: SharedVM?
  private let stopFlag = NSLock()
  private var closed = false

  public init(vm: SharedVM, projectId: String) throws {
    self.vm = vm
    self.projectId = projectId
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    guard fd >= 0 else { throw LauncherError.startFailed(reason: "no local socket", log: "") }
    var one: Int32 = 1
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
    var addr = sockaddr_in()
    addr.sin_family = sa_family_t(AF_INET)
    addr.sin_addr.s_addr = inet_addr("127.0.0.1")
    addr.sin_port = 0
    var len = socklen_t(MemoryLayout<sockaddr_in>.size)
    let ok = withUnsafeMutablePointer(to: &addr) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
        bind(fd, $0, len) == 0 && listen(fd, 64) == 0 && getsockname(fd, $0, &len) == 0
      }
    }
    guard ok else {
      Darwin.close(fd)
      throw LauncherError.startFailed(reason: "no free local port", log: "")
    }
    listener = fd
    port = Int(UInt16(bigEndian: addr.sin_port))
    Thread.detachNewThread { [self] in acceptLoop() }
  }

  public func close() {
    stopFlag.lock()
    let was = closed
    closed = true
    stopFlag.unlock()
    if !was {
      shutdown(listener, SHUT_RDWR)
      Darwin.close(listener)
    }
  }

  deinit { close() }

  private func acceptLoop() {
    while true {
      let c = accept(listener, nil, nil)
      if c >= 0 { noSigPipe(c) }
      if c < 0 {
        stopFlag.lock()
        let done = closed
        stopFlag.unlock()
        if done { return }
        continue
      }
      Task { [weak self] in await self?.forward(c) }
    }
  }

  private func forward(_ client: Int32) async {
    guard let vm, let up = try? await vm.connect(port: SharedVM.rpcPort) else {
      Darwin.close(client)
      return
    }
    guard (try? writeAll(up, Data((projectId + "\n").utf8))) != nil else {
      Darwin.close(client)
      Darwin.close(up)
      return
    }
    let done = DispatchGroup()
    for (from, to) in [(client, up), (up, client)] {
      done.enter()
      Thread.detachNewThread {
        var buf = [UInt8](repeating: 0, count: 65536)
        while true {
          let n = read(from, &buf, buf.count)
          if n <= 0 { break }
          var off = 0
          while off < n {
            let m = buf.withUnsafeBytes { write(to, $0.baseAddress! + off, n - off) }
            if m <= 0 { break }
            off += m
          }
          if off < n { break }
        }
        shutdown(to, SHUT_WR)
        shutdown(from, SHUT_RD)
        done.leave()
      }
    }
    done.notify(queue: .global()) {
      Darwin.close(client)
      Darwin.close(up)
    }
  }
}

/// A write to a socket whose peer went away fails (EPIPE) instead of
/// killing the process with SIGPIPE.
public func noSigPipe(_ fd: Int32) {
  var one: Int32 = 1
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
}

/// Virtualization's objects are not Sendable: one is handed to the VM's queue
/// and used only there (the queue Virtualization requires for them).
private struct OnVMQueue<T>: @unchecked Sendable {
  let value: T
  init(_ value: T) { self.value = value }
}
