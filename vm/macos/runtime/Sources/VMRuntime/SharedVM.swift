import Darwin
import FlatBuffers
import Foundation
import TsugumiRPC
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
public final class SharedVM: NSObject, VZVirtualMachineDelegate, VZVirtioSocketListenerDelegate, @unchecked Sendable {
  private static let controlPort: UInt32 = 1024
  public static let rpcPort: UInt32 = 1025
  private static let terminalPort: UInt32 = 1026

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
  private var consoleFile: ConsoleFile?
  // Only used on the Virtualization queue.
  private let controlListener = VZVirtioSocketListener()
  private var control: GuestControl?
  private var idleMemory: IdleMemory?
  private var clockTask: Task<Void, Never>?
  private var projects: [String: Tsugumi_VMGuest_ProjectStatusT] = [:]
  private var bridges: [String: LoopbackBridge] = [:]
  private var projectObserver: (@Sendable (Tsugumi_VMGuest_ProjectStatusT) -> Void)?

  public init(directory: URL) {
    self.directory = directory
    executionBroker = ExecutionBrokerBridge()
    super.init()
    controlListener.delegate = self
  }

  public func configureBroker(_ socket: String) { executionBroker.configure(socket) }

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

  /// A disk: a sparse file, formatted by the guest on first boot. A larger
  /// size grows the file (the guest grows its file system on boot); a smaller
  /// one keeps it: shrinking needs a new store (reset the environments).
  private func ensureDisk(_ url: URL, gib: Int, what: String) throws {
    let wanted = UInt64(gib) << 30
    if let size = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size]) as? UInt64 {
      if size < wanted {
        console.note("growing the \(what) disk from \(size >> 30) to \(gib) GiB")
        let h = try FileHandle(forWritingTo: url)
        try h.truncate(atOffset: wanted)
        try h.close()
      } else if size > wanted {
        console.note("the \(what) disk stays \(size >> 30) GiB; a smaller one needs a new store")
      }
      return
    }
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
    // delivered nothing). ConsoleLog already created the file for this boot.
    let serial = VZVirtioConsoleDeviceSerialPortConfiguration()
    serial.attachment = try VZFileSerialPortAttachment(url: consoleURL, append: true)
    c.serialPorts = [serial]
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

  private func consoleLine(_ line: String) {
    if line.hasPrefix("TSUGUMI-STAGE "),
       let stage = VMStage(rawValue: String(line.dropFirst("TSUGUMI-STAGE ".count))) {
      if case .starting = state { setVMState(.starting(stage)) }
    } else if line.hasPrefix("TSUGUMI-FAILED ") {
      setVMState(.failed(String(line.dropFirst("TSUGUMI-FAILED ".count))))
    }
  }

  public func listener(_ listener: VZVirtioSocketListener, shouldAcceptNewConnection connection: VZVirtioSocketConnection,
                       from socketDevice: VZVirtioSocketDevice) -> Bool {
    // The guest supervisor initiates the connection before starting projects.
    guard socketDevice === socket, control == nil, case .starting = state else { return false }
    let fd = fcntl(connection.fileDescriptor, F_DUPFD_CLOEXEC, 0)
    guard fd >= 0 else {
      setVMState(.failed("cannot open the guest control stream: \(String(cString: strerror(errno)))"))
      return false
    }
    let channel = GuestControl(fd: fd, retaining: connection)
    control = channel
    channel.start(event: { [weak self, weak channel] message in
      guard let self, let channel else { return }
      try self.queue.sync {
        guard self.control === channel else { return }
        try self.guestEvent(message, channel: channel)
      }
    }, closed: { [weak self, weak channel] error in
      guard let self, let channel else { return }
      self.queue.async {
        guard self.control === channel else { return }
        self.clockTask?.cancel()
        self.stopIdleMemory()
        self.console.note("guest control: \(error)")
        if !self.lock.withLock({ self.stopping }) {
          self.setVMState(.failed("guest control: \(error)"))
        }
      }
    })
    return true
  }

  private func guestEvent(_ message: Tsugumi_VMGuest_MessageT, channel: GuestControl) throws {
    switch message.payload?.value {
    case is Tsugumi_VMGuest_ReadyT:
      guard case .starting = state else { throw LauncherError.notReady("unexpected guest readiness") }
      guard let balloon = vm?.memoryBalloonDevices.first as? VZVirtioTraditionalMemoryBalloonDevice else {
        throw LauncherError.notReady("missing VM memory balloon")
      }
      idleMemory = IdleMemory(queue: queue, fullBytes: balloon.targetVirtualMachineMemorySize, setTarget: { [console] bytes in
        balloon.targetVirtualMachineMemorySize = bytes
        console.note("VM memory target: \(bytes >> 20) MiB")
      }, snapshot: { [console] in
        Self.logMemory(try await channel.memorySnapshot(), to: console)
      })
      setVMState(.running)
      // Sync clocks periodically because the VM drifts across host sleep.
      clockTask = Task {
        while !Task.isCancelled {
          let request = Tsugumi_VMGuest_ClockT()
          request.unixMs = Int64(Date().timeIntervalSince1970 * 1000)
          do {
            _ = try await channel.request(request, type: .clock)
            try await Task.sleep(for: .seconds(30))
          } catch {
            if !Task.isCancelled { self.console.note("cannot sync the guest clock: \(error)") }
            return
          }
        }
      }
    case let project as Tsugumi_VMGuest_ProjectStatusT:
      projects[project.key] = project
      if project.phase == .exited { detachProject(project.key) }
      projectObserver?(project)
    case let memory as Tsugumi_VMGuest_MemoryT:
      Self.logMemory(memory, to: console)
    case let line as Tsugumi_VMGuest_ConsoleLineT:
      console.note(line.text)
    default: throw LauncherError.notReady("unknown guest event")
    }
  }

  private static func logMemory(_ memory: Tsugumi_VMGuest_MemoryT, to console: ConsoleLog) {
    console.note("guest memory: \(memory.totalKib >> 10) MiB total, \(memory.availableKib >> 10) MiB available, \(memory.cachedKib >> 10) MiB cached")
  }

  private func stopIdleMemory() {
    idleMemory?.stop()
    idleMemory = nil
  }

  public func observeProjects(_ handler: @escaping @Sendable (Tsugumi_VMGuest_ProjectStatusT) -> Void) {
    queue.sync { projectObserver = handler }
  }

  /// Subscribe and send the state already known on the same queue as updates.
  public func projectSnapshot(_ publish: ([Tsugumi_VMGuest_ProjectStatusT]) -> Void) {
    queue.sync { publish(Array(projects.values)) }
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
        do {
          self.consoleFile = try ConsoleFile(self.consoleURL, queue: self.queue) { [weak self = self] result in
            guard let self else { return }
            switch result {
            case .success(let data):
              for line in self.console.feed(data) { self.consoleLine(line) }
            case .failure(let error):
              self.setVMState(.failed("cannot follow the VM console: \(error)"))
            }
          }
        } catch {
          self.lock.unlock()
          c.resume(throwing: error)
          return
        }
        let vm = VZVirtualMachine(configuration: handed.value, queue: self.queue)
        vm.delegate = self
        self.vm = vm
        self.current = ObjectIdentifier(vm)
        self.lock.unlock()
        self.fileSystems = vm.directorySharingDevices.compactMap { $0 as? VZVirtioFileSystemDevice }
        self.socket = vm.socketDevices.first as? VZVirtioSocketDevice
        if let socket = self.socket {
          self.executionBroker.install(on: socket)
          socket.setSocketListener(self.controlListener, forPort: Self.controlPort)
        }
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
        do { try await maintenance(.shutdown) }
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
        self.stopIdleMemory()
        self.clockTask?.cancel()
        self.clockTask = nil
        self.control?.finish(LauncherError.notReady("the VM stopped"))
        self.control = nil
        for project in self.projects.values where project.phase != .exited {
          let exited = Tsugumi_VMGuest_ProjectStatusT()
          exited.key = project.key
          exited.pid = project.pid
          exited.phase = .exited
          self.projectObserver?(exited)
        }
        self.projects.removeAll()
        self.slots.removeAll()
        for bridge in self.bridges.values { bridge.close() }
        self.bridges.removeAll()
        self.consoleFile?.cancel()
        self.consoleFile = nil
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
      switch state {
      case .failed(let reason): setVMState(.failed(reason))
      default: setVMState(.stopped)
      }
    }
  }

  public func guestDidStop(_ virtualMachine: VZVirtualMachine) { stopped(virtualMachine, nil) }

  public func virtualMachine(_ virtualMachine: VZVirtualMachine, didStopWithError error: Error) { stopped(virtualMachine, error) }

  // MARK: - Shared folders

  /// Gives the project a slot (its own virtiofs device, `tsugumi-<slot>`)
  /// sharing `entries` (name -> folder) before the guest mounts it. The guest cannot
  /// reach the folders it had open once the device's share is replaced.
  public func attach(_ projectId: String, _ entries: [String: URL], readOnly: Set<String> = []) async throws -> Int {
    // Allocation and share replacement run on the same queue as guest exits.
    let (slot, memory, restore): (Int, IdleMemory, Task<Void, Error>?) = try await withCheckedThrowingContinuation { answer in
      queue.async {
        guard let memory = self.idleMemory, case .running = self.state else {
          answer.resume(throwing: LauncherError.notReady("the VM is not running"))
          return
        }
        guard self.slots[projectId] == nil else {
          answer.resume(throwing: LauncherError.notReady("the project already has a VM slot"))
          return
        }
        let used = Set(self.slots.values)
        guard let slot = (0..<Self.slotCount).first(where: { !used.contains($0) }) else {
          answer.resume(throwing: LauncherError.startFailed(reason: "at most \(Self.slotCount) projects can be open at once", log: ""))
          return
        }
        self.slots[projectId] = slot
        var shared: [String: VZSharedDirectory] = [:]
        for (name, url) in entries {
          shared[name] = VZSharedDirectory(url: url, readOnly: readOnly.contains(name))
        }
        self.fileSystems[slot].share = VZMultipleDirectoryShare(directories: shared)
        answer.resume(returning: (slot, memory, memory.hold()))
      }
    }
    do {
      try await restore?.value
    } catch {
      queue.sync {
        if idleMemory === memory { detachProject(projectId) }
      }
      throw error
    }
    return slot
  }

  /// Empties the slot only after the guest unmounted it.
  private func detachProject(_ projectId: String) {
    bridges.removeValue(forKey: projectId)?.close()
    guard let slot = slots.removeValue(forKey: projectId) else { return }
    fileSystems[slot].share = VZMultipleDirectoryShare(directories: [:])
    idleMemory?.release()
  }

  public func process(_ key: String) async throws -> (pid: Int64, endpoint: String?) {
    try await withCheckedThrowingContinuation { answer in
      queue.async {
        guard let project = self.projects[key], project.phase != .exited else {
          answer.resume(returning: (0, nil))
          return
        }
        do {
          if project.phase == .ready, self.bridges[key] == nil {
            self.bridges[key] = try LoopbackBridge(vm: self, projectId: key)
          }
          let endpoint = self.bridges[key].map { "ws://127.0.0.1:\($0.port)/rpc" }
          answer.resume(returning: (project.pid, endpoint))
        } catch { answer.resume(throwing: error) }
      }
    }
  }

  public func stopProject(_ key: String) async throws {
    let running = queue.sync { projects[key].map { $0.phase != .exited } ?? false }
    guard running else { return }
    let stop = Tsugumi_VMGuest_StopT()
    stop.key = key
    try await request(stop, type: .stop)
  }

  public func releaseUnstartedProject(_ key: String) {
    queue.sync {
      if projects[key] == nil || projects[key]?.phase == .exited { detachProject(key) }
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
            let fd = fcntl(conn.fileDescriptor, F_DUPFD_CLOEXEC, 0)
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

  @discardableResult
  public func request(_ value: NativeObject, type: Tsugumi_VMGuest_Command) async throws -> Tsugumi_VMGuest_ReplyT {
    guard let channel = queue.sync(execute: { control }) else {
      throw LauncherError.notReady("the guest control stream is unavailable")
    }
    return try await channel.request(value, type: type)
  }

  @discardableResult
  public func maintenance(_ action: Tsugumi_VMGuest_MaintenanceAction) async throws -> String {
    let value = Tsugumi_VMGuest_MaintenanceT()
    value.action = action
    let (channel, memory, restore): (GuestControl, IdleMemory?, Task<Void, Error>?) = try queue.sync {
      guard let control else { throw LauncherError.notReady("the guest control stream is unavailable") }
      switch action {
      case .shutdown:
        stopIdleMemory()
        return (control, nil, nil)
      case .snapshotcacheusage:
        return (control, nil, nil)
      case .collectstore, .verifystore:
        guard let idleMemory else { throw LauncherError.notReady("the VM is not running") }
        return (control, idleMemory, idleMemory.hold())
      }
    }
    defer {
      if let memory { queue.sync { memory.release() } }
    }
    try await restore?.value
    return try await channel.request(value, type: .maintenance).message ?? ""
  }

  public func openTerminal(_ request: Tsugumi_VMGuest_OpenTerminalT) async throws -> (Int32, UInt64) {
    let data = try GuestControl.requestData(request, type: .openterminal, id: 1)
    let fd = try await connect(port: Self.terminalPort)
    return try await withCheckedThrowingContinuation { answer in
      Thread.detachNewThread {
        do {
          try VMHostIPC.write(data, to: fd)
          guard let message = try GuestControl.read(fd), message.requestId == 1,
                let result = message.payload?.value as? Tsugumi_VMGuest_ReplyT else {
            throw LauncherError.notReady("no terminal reply")
          }
          answer.resume(returning: (fd, try GuestControl.checked(result).session))
        } catch {
          Darwin.close(fd)
          answer.resume(throwing: error)
        }
      }
    }
  }
}

func writeAll(_ fd: Int32, _ data: Data) throws {
  try data.withUnsafeBytes { (buf: UnsafeRawBufferPointer) in
    var off = 0
    while off < buf.count {
      let n = write(fd, buf.baseAddress! + off, buf.count - off)
      if n < 0 && errno == EINTR { continue }
      if n <= 0 { throw LauncherError.notReady("write failed") }
      off += n
    }
  }
}

/// Forwards connections from the Mac to a project's vsock RPC stream.
/// The first frame routes the connection to the project's tsugumid.
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
    _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
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
      if c >= 0 {
        _ = fcntl(c, F_SETFD, FD_CLOEXEC)
        noSigPipe(c)
      }
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
    guard (try? VMHostIPC.write(Data(projectId.utf8), to: up)) != nil else {
      Darwin.close(client)
      Darwin.close(up)
      return
    }
    VMHostIPC.relay(client, up)
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
