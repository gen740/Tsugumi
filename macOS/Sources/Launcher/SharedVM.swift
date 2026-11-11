import Darwin
import Foundation
// (its configuration types are not Sendable; they are handed to the VM queue only)
@preconcurrency import Virtualization

/// The shared VM's boot disk (macOS/Container/vm-boot.nix: kernel and a
/// small root file system with Nix) and Tsugumi's source, from which the VM
/// builds Tsugumi itself on its first boot (vm-system.nix). From the app
/// bundle, or dist/tsugumi-vm and the repository when run from the source tree.
public struct VMImage: Sendable, Equatable {
  static let files = ["vmlinux", "rootfs.erofs"]
  public let kernel: URL
  public let rootfs: URL
  /// The source folder; `sourceEntries` lists what of it the VM gets (all of
  /// it when nil, as in the bundle, which holds only those).
  public let source: URL
  public let sourceEntries: [String]?
  public let version: String

  public init?(directory: URL, source: URL, sourceEntries: [String]? = nil) {
    let urls = Self.files.map { directory.appendingPathComponent($0) }
    guard urls.allSatisfy({ FileManager.default.fileExists(atPath: $0.path) }),
          FileManager.default.fileExists(atPath: source.appendingPathComponent("flake.nix").path) else { return nil }
    kernel = urls[0]
    rootfs = urls[1]
    self.source = source
    self.sourceEntries = sourceEntries
    version = (try? String(contentsOf: directory.appendingPathComponent("vm-version"), encoding: .utf8))?
      .trimmingCharacters(in: .whitespacesAndNewlines) ?? "dev"
  }

  public static func find(bundle: Bundle = .main) -> VMImage? {
    if let dir = bundle.url(forResource: "tsugumi-vm", withExtension: nil),
       let source = bundle.url(forResource: "tsugumi-source", withExtension: nil),
       let image = VMImage(directory: dir, source: source) { return image }
    #if DEBUG
    // A development build (tsugumi-vmctl, tests) uses the repository.
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<4 { repo.deleteLastPathComponent() }
    return VMImage(directory: repo.appendingPathComponent("dist/tsugumi-vm"), source: repo,
                   sourceEntries: sourceList(repo.appendingPathComponent("macOS/Container/vm-source.txt")))
    #else
    return nil
    #endif
  }

  /// macOS/Container/vm-source.txt: one path per line, `#` comments.
  static func sourceList(_ url: URL) -> [String]? {
    guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
    return text.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty && !$0.hasPrefix("#") }
  }
}

/// Size of the shared VM (all projects share it).
public struct VMResources: Sendable, Equatable {
  public var cpus: Int
  public var memoryGiB: Int
  public var storeGiB: Int

  /// Half the Mac's memory (4–16 GiB), all but two cores (2–8).
  public static var `default`: VMResources {
    let mem = Int(ProcessInfo.processInfo.physicalMemory >> 30)
    let cores = ProcessInfo.processInfo.activeProcessorCount
    return VMResources(cpus: min(8, max(2, cores - 2)), memoryGiB: min(16, max(4, mem / 2)), storeGiB: 64)
  }
}

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

/// The VM console: the guest writes `console.log` in the VM folder (the
/// previous boot's is `console.prev.log`); the app's own notes about the VM
/// go to `launcher.log`. Both are kept in memory, in order, for the console view.
public final class ConsoleLog: @unchecked Sendable {
  private let lock = NSLock()
  private var lines: [String] = []
  private var partial = Data()
  private var observers: [UUID: @Sendable (String) -> Void] = [:]
  private var file: FileHandle?
  private let limit = 5000

  init() {}

  /// Starts a new boot: the guest's console file and the app's note file.
  func open(console url: URL) {
    lock.lock()
    defer { lock.unlock() }
    let dir = url.deletingLastPathComponent()
    let prev = dir.appendingPathComponent("console.prev.log")
    try? FileManager.default.removeItem(at: prev)
    try? FileManager.default.moveItem(at: url, to: prev)
    FileManager.default.createFile(atPath: url.path, contents: nil)
    let notes = dir.appendingPathComponent("launcher.log")
    if !FileManager.default.fileExists(atPath: notes.path) { FileManager.default.createFile(atPath: notes.path, contents: nil) }
    file = try? FileHandle(forWritingTo: notes)
    _ = try? file?.seekToEnd()
    lines.removeAll()
    partial = Data()
  }

  /// Adds a line written by the app (not the guest), marked as such.
  public func note(_ text: String) {
    let line = "[tsugumi] " + text
    lock.lock()
    file?.write(Data((ISO8601DateFormatter().string(from: Date()) + " " + text + "\n").utf8))
    lock.unlock()
    append(line: line)
  }

  func feed(_ data: Data) -> [String] {
    // Split on bytes: the guest ends lines with "\r\n", which a Swift String
    // holds as one Character (searching it for "\n" finds nothing).
    var complete: [String] = []
    lock.lock()
    partial.append(data)
    while let nl = partial.firstIndex(of: 10) {
      var line = partial[partial.startIndex..<nl]
      if line.last == 13 { line = line.dropLast() }
      complete.append(String(decoding: line, as: UTF8.self))
      partial.removeSubrange(partial.startIndex...nl)
    }
    lock.unlock()
    for l in complete { append(line: l) }
    return complete
  }

  private func append(line: String) {
    lock.lock()
    lines.append(line)
    if lines.count > limit { lines.removeFirst(lines.count - limit) }
    let obs = Array(observers.values)
    lock.unlock()
    for o in obs { o(line) }
  }

  public var snapshot: [String] {
    lock.lock()
    defer { lock.unlock() }
    return lines
  }

  public func observe(_ handler: @escaping @Sendable (String) -> Void) -> UUID {
    let id = UUID()
    lock.lock()
    observers[id] = handler
    lock.unlock()
    return id
  }

  public func stopObserving(_ id: UUID) {
    lock.lock()
    observers[id] = nil
    lock.unlock()
  }
}

/// The one Linux VM of the app (Virtualization.framework). It boots from the
/// bundled kernel and read-only root file system, keeps the shared Nix store
/// and the projects' run history on two sparse disk images in the support
/// folder, and runs one tsugumid per
/// open project (tsugumi-vmd). Each open project gets one of `slotCount`
/// virtiofs devices for its folders: replacing one device's share while the
/// VM runs leaves the others alone (replacing a share that other projects
/// had mounted made their files disappear).
public final class SharedVM: NSObject, VZVirtualMachineDelegate, @unchecked Sendable {
  /// Steps of starting the VM; the guest reports the later ones on the console.
  public enum Stage: String, Sendable, CaseIterable {
    case image, disk, boot, store, nix, network, system, supervisor, ready
  }

  public enum State: Sendable, Equatable {
    case stopped
    case starting(Stage)
    case running
    case failed(String)
  }

  public static let controlPort: UInt32 = 1024
  public static let rpcPort: UInt32 = 1025

  public let directory: URL
  public let console = ConsoleLog()
  private let queue = DispatchQueue(label: "app.tsugumi.vm")
  private let lock = NSLock()
  private var vm: VZVirtualMachine?
  public static let slotCount = 16
  private var fileSystems: [VZVirtioFileSystemDevice] = []
  private var socket: VZVirtioSocketDevice?
  private var slots: [String: Int] = [:]  // project id -> slot
  /// Held while this app has the VM (under `lock`).
  private var hostLock: HostLock?
  /// The VM that runs now (under `lock`): a stop reported for an earlier one
  /// changes nothing.
  private var current: ObjectIdentifier?
  private var _state: State = .stopped
  private var stateObservers: [UUID: @Sendable (State) -> Void] = [:]
  private var readyWaiters: [CheckedContinuation<Void, Error>] = []

  public init(directory: URL) {
    self.directory = directory
  }

  public var state: State {
    lock.lock()
    defer { lock.unlock() }
    return _state
  }

  public func observe(_ handler: @escaping @Sendable (State) -> Void) -> UUID {
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

  private func setState(_ s: State) {
    lock.lock()
    _state = s
    let obs = Array(stateObservers.values)
    var waiters: [CheckedContinuation<Void, Error>] = []
    if case .running = s { waiters = readyWaiters; readyWaiters = [] }
    if case .failed = s { waiters = readyWaiters; readyWaiters = [] }
    if case .stopped = s { waiters = readyWaiters; readyWaiters = [] }
    lock.unlock()
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
  /// The run history disk: its own image, so that the store and the history
  /// can be reset apart, and synchronized with fsync (an SQLite commit is not
  /// a full flush of the Mac's drive, as on the project share).
  var historyImage: URL { directory.appendingPathComponent("history.img") }
  static let historyGiB = 32
  var consoleURL: URL { directory.appendingPathComponent("console.log") }

  // MARK: - Start and stop

  /// Starts the VM (first start: copies the image and creates the store
  /// disk) and returns once tsugumi-vmd is ready. Concurrent callers wait
  /// for the same start.
  public func start(image: VMImage, resources: VMResources) async throws {
    let begin: Bool = {
      lock.lock()
      defer { lock.unlock() }
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
    if queue.sync(execute: { self.vm != nil }) { await forceStop() }
    lock.withLock { _state = .starting(.image) }
    do {
      try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
      // One VM per support folder: a second app instance does not get it (and
      // leaves the console file of the VM that runs alone).
      let hl = try HostLock(path: directory.appendingPathComponent("vm.lock"))
      do { try hl.lock(wait: false) } catch {
        throw LauncherError.startFailed(reason: "another Tsugumi is using the execution environment", log: "")
      }
      lock.withLock { hostLock = hl }
      console.open(console: consoleURL)
      setState(.starting(.image))
      let files = try installImage(image)
      setState(.starting(.disk))
      try ensureDisk(storeImage, gib: resources.storeGiB, what: "store")
      try ensureDisk(historyImage, gib: Self.historyGiB, what: "run history")
      setState(.starting(.boot))
      lock.withLock { _bootCount += 1 }
      console.note("starting the VM: \(resources.cpus) CPUs, \(resources.memoryGiB) GiB memory, store \(resources.storeGiB) GiB (image \(image.version)), x86_64-linux: \(X86Emulation.chosen.rawValue)")
      let config = try configuration(files: files, resources: resources)
      try await boot(config)
    } catch {
      console.note("start failed: \(error)")
      await forceStop()
      setState(.failed("\(error)"))
      throw error
    }
    // A guest that never reports in (kernel or console trouble) fails the
    // start. Once it does, it reports its own failures: building Tsugumi on
    // the first boot takes as long as the downloads take.
    let watchdog = Task { [weak self] in
      try await Task.sleep(for: .seconds(240))
      guard let self, case .starting(let stage) = self.state, [.image, .disk, .boot].contains(stage) else { return }
      self.console.note("no report from the guest within 240 s")
      self.setState(.failed("the VM did not report within 240 s"))
    }
    defer { watchdog.cancel() }
    do {
      try await waitReady()
    } catch {
      // A failed start leaves no VM behind (a retry starts one afresh).
      let state = self.state
      await forceStop()
      if case .failed = state { setState(state) }
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

  /// The bundled files, copied (APFS clone) next to the store so an app
  /// update never pulls them away from a running VM. The source is copied
  /// on every start (it is small); the VM rebuilds Tsugumi only when its
  /// content changed.
  private func installImage(_ image: VMImage) throws -> VMImage {
    let dest = directory.appendingPathComponent("image", isDirectory: true)
    let fm = FileManager.default
    let stamp = dest.appendingPathComponent("vm-version")
    let have = try? String(contentsOf: stamp, encoding: .utf8)
    if have != image.version || image.version == "dev" || Self.files(in: dest) == nil {
      console.note("installing the VM image \(image.version)")
      try? fm.removeItem(at: dest)
      try fm.createDirectory(at: dest, withIntermediateDirectories: true)
      for (src, name) in zip([image.kernel, image.rootfs], VMImage.files) {
        try copy(src, dest.appendingPathComponent(name), name)
      }
      try image.version.write(to: stamp, atomically: true, encoding: .utf8)
    }
    let source = directory.appendingPathComponent("source", isDirectory: true)
    try? fm.removeItem(at: source)
    try fm.createDirectory(at: source, withIntermediateDirectories: true)
    let entries = try image.sourceEntries ?? fm.contentsOfDirectory(atPath: image.source.path)
    for entry in entries {
      let to = source.appendingPathComponent(entry)
      try fm.createDirectory(at: to.deletingLastPathComponent(), withIntermediateDirectories: true)
      try copy(image.source.appendingPathComponent(entry), to, entry)
    }
    guard let installed = VMImage(directory: dest, source: source) else { throw LauncherError.imageMissing }
    return installed
  }

  private static func files(in dir: URL) -> [URL]? {
    let urls = VMImage.files.map { dir.appendingPathComponent($0) }
    return urls.allSatisfy { FileManager.default.fileExists(atPath: $0.path) } ? urls : nil
  }

  /// A file or folder (cloned where APFS can).
  private func copy(_ from: URL, _ to: URL, _ name: String) throws {
    do { try FileManager.default.copyItem(at: from, to: to) } catch {
      throw LauncherError.startFailed(reason: "cannot copy \(name) into \(to.deletingLastPathComponent().path)", log: "\(error)")
    }
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

  private func configuration(files: VMImage, resources: VMResources) throws -> VZVirtualMachineConfiguration {
    let c = VZVirtualMachineConfiguration()
    let boot = VZLinuxBootLoader(kernelURL: files.kernel)
    let x86 = X86Emulation.chosen
    boot.commandLine = ProcessInfo.processInfo.environment["TSUGUMI_VM_CMDLINE"]
      ?? "console=hvc0 root=/dev/vda rootfstype=erofs ro init=/init loglevel=4 panic=-1 tsugumi.x86=\(x86.rawValue)"
    c.bootLoader = boot
    c.platform = VZGenericPlatformConfiguration()
    c.cpuCount = max(VZVirtualMachineConfiguration.minimumAllowedCPUCount,
                     min(resources.cpus, VZVirtualMachineConfiguration.maximumAllowedCPUCount))
    c.memorySize = max(VZVirtualMachineConfiguration.minimumAllowedMemorySize,
                       min(UInt64(resources.memoryGiB) << 30, VZVirtualMachineConfiguration.maximumAllowedMemorySize))
    let root = try VZDiskImageStorageDeviceAttachment(url: files.rootfs, readOnly: true)
    let store = try VZDiskImageStorageDeviceAttachment(url: storeImage, readOnly: false,
                                                       cachingMode: .automatic, synchronizationMode: .full)
    let history = try VZDiskImageStorageDeviceAttachment(url: historyImage, readOnly: false,
                                                         cachingMode: .automatic, synchronizationMode: .fsync)
    c.storageDevices = [root, store, history].map { VZVirtioBlockDeviceConfiguration(attachment: $0) }
    let net = VZVirtioNetworkDeviceConfiguration()
    net.attachment = VZNATNetworkDeviceAttachment()
    net.macAddress = macAddress()
    c.networkDevices = [net]
    lock.withLock { slots.removeAll() }
    let source = VZVirtioFileSystemDeviceConfiguration(tag: "tsugumi-source")
    source.share = VZSingleDirectoryShare(directory: VZSharedDirectory(url: files.source, readOnly: true))
    c.directorySharingDevices = (0..<Self.slotCount).map { i in
      let fs = VZVirtioFileSystemDeviceConfiguration(tag: "tsugumi-\(i)")
      fs.share = VZMultipleDirectoryShare(directories: [:])
      return fs
    } + [source] + (x86 == .rosetta ? [try rosettaShare()] : [])
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
        _ = try? await self.request("clock \(ms)", timeout: 5)
        try? await Task.sleep(for: .seconds(30))
      }
    }
  }

  /// Reads what the guest appends to the console file until the next boot.
  private func followConsole(_ url: URL, boot: Int) {
    Thread.detachNewThread { [weak self] in
      // By path and offset: the attachment may replace the file when it opens it.
      var offset: UInt64 = 0
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
          Thread.sleep(forTimeInterval: 0.05)
          continue
        }
        for line in self.console.feed(data) { self.consoleLine(line) }
      }
    }
  }

  private func consoleLine(_ line: String) {
    if line.hasPrefix("TSUGUMI-STAGE ") {
      let name = String(line.dropFirst("TSUGUMI-STAGE ".count)).trimmingCharacters(in: .whitespaces)
      if name == "ready" {
        setState(.running)
        keepClock()
      } else if let s = Stage(rawValue: name) { setState(.starting(s)) }
    } else if line.hasPrefix("TSUGUMI-FAILED ") {
      setState(.failed(String(line.dropFirst("TSUGUMI-FAILED ".count))))
    }
  }

  private func boot(_ config: VZVirtualMachineConfiguration) async throws {
    let handed = OnVMQueue(config)
    try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
      queue.async {
        let vm = VZVirtualMachine(configuration: handed.value, queue: self.queue)
        vm.delegate = self
        self.vm = vm
        self.lock.withLock { self.current = ObjectIdentifier(vm) }
        self.fileSystems = vm.directorySharingDevices.compactMap { $0 as? VZVirtioFileSystemDevice }
        self.socket = vm.socketDevices.first as? VZVirtioSocketDevice
        vm.start { result in
          switch result {
          case .success:
            self.console.note("the VM is running; waiting for the guest")
            c.resume()
          case .failure(let e): c.resume(throwing: LauncherError.startFailed(reason: "the VM did not start: \(e.localizedDescription)", log: ""))
          }
        }
      }
    }
  }

  /// Stops every project (tsugumid records interrupted runs) and powers the
  /// VM off; forced after `timeout`.
  public func shutdown(timeout: TimeInterval = 40) async {
    guard case .running = state else {
      await forceStop()
      return
    }
    console.note("shutting down")
    _ = try? await request("shutdown", timeout: 5)
    let deadline = Date().addingTimeInterval(timeout)
    while Date() < deadline {
      if case .stopped = state { return }
      try? await Task.sleep(for: .milliseconds(100))
    }
    await forceStop()
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
    else { lock.withLock { hostLock = nil } }
  }

  /// `which` stopped: the state follows only when it is the current VM.
  private func stopped(_ which: VZVirtualMachine, _ error: Error?) {
    let id = ObjectIdentifier(which)
    let isCurrent: Bool = lock.withLock {
      guard current == id else { return false }
      current = nil
      hostLock = nil
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
      setState(.failed("the VM stopped: \(error.localizedDescription)"))
    } else {
      console.note("the VM stopped")
      setState(.stopped)
    }
  }

  public func guestDidStop(_ virtualMachine: VZVirtualMachine) { stopped(virtualMachine, nil) }

  public func virtualMachine(_ virtualMachine: VZVirtualMachine, didStopWithError error: Error) { stopped(virtualMachine, error) }

  // MARK: - Shared folders

  /// Gives the project a slot (its own virtiofs device, `tsugumi-<slot>`)
  /// sharing `entries` (name -> folder); the same slot again for a project
  /// that has one.
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
  public func request(_ line: String, timeout: TimeInterval = 60) async throws -> String {
    let fd = try await connect(port: Self.controlPort)
    defer { close(fd) }
    var tv = timeval(tv_sec: Int(timeout), tv_usec: 0)
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
    try writeAll(fd, Data((line + "\n").utf8))
    let reply = readLine(fd)
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
