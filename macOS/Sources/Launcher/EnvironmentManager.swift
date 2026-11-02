import Darwin
import Foundation

/// Steps of starting an environment. The raw values are internal names
/// (shown as details); the UI says each in words.
public enum EnvironmentStage: String, Sendable {
  case waitingForOthers = "waiting for other windows"
  case checkingService = "checking the container service"
  case loadingImage = "loading the environment image"
  case creatingStore = "creating the Nix store disk image"
  case startingDaemon = "starting tsugumid"
  case connecting = "waiting for tsugumid to become ready"
}

/// Starts, finds and stops the per-project Apple container environment.
///
/// One environment per project identity (device + inode, so path aliases map
/// to the same environment). Start, stop and deletion of an environment are
/// serialized across processes by one host file lock per environment.
///
/// Everything an environment owns lives under the Tsugumi support folder
/// (`~/Library/Application Support/Tsugumi`): its 0700 control folder and its
/// Nix store, a sparse disk image (`store/store.img`) that the VM mounts as
/// `/persist`. Deleting that folder removes the stores; the container image
/// itself is kept by the container runtime and is replaced from the app's
/// bundled image on the next start (`runtime/image.json` records what was
/// loaded), and containers left by a deleted folder are removed then.
/// Project documents, history and artifacts stay in the project folder.
public actor EnvironmentManager {
  public static let imageName = "tsugumi-env"
  public static let imageTag = "1.0.0"
  public static var image: String { "\(imageName):\(imageTag)" }
  static let projectLabel = "tsugumi.project"

  public nonisolated let cli: ContainerCLI
  public nonisolated let support: URL
  private let bundledImage: URL?
  /// Guest kernel for Tsugumi's VMs (kernel.nix: the recommended kernel
  /// without SELinux, whose oops leaves VMs unable to stop). Passed with
  /// `--kernel` to Tsugumi's containers only; nil uses the system default.
  private let bundledKernel: URL?
  private var observers: [String: [UUID: (EnvironmentState) -> Void]] = [:]
  private var states: [String: EnvironmentState] = [:]

  public init(cli: ContainerCLI = ContainerCLI(), support: URL? = nil, bundledImage: URL? = nil, bundledKernel: URL? = nil) {
    self.cli = cli
    self.support = support ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Tsugumi", isDirectory: true)
    self.bundledImage = bundledImage
    self.bundledKernel = bundledKernel.flatMap { FileManager.default.fileExists(atPath: $0.path) ? $0 : nil }
  }

  // MARK: - Paths and names

  nonisolated public func containerName(_ p: ProjectIdentity) -> String { "tsugumi-\(p.id)" }
  /// Named volume used by earlier versions for the store (removed on deletion).
  nonisolated public func legacyVolumeName(_ p: ProjectIdentity) -> String { "tsugumi-\(p.id)-store" }
  nonisolated var environmentsDir: URL { support.appendingPathComponent("environments", isDirectory: true) }
  nonisolated func envDir(_ p: ProjectIdentity) -> URL { envDir(id: p.id) }
  nonisolated func envDir(id: String) -> URL { environmentsDir.appendingPathComponent(id, isDirectory: true) }
  nonisolated public func controlDir(_ p: ProjectIdentity) -> URL { envDir(p).appendingPathComponent("control", isDirectory: true) }
  nonisolated public func storeDir(_ p: ProjectIdentity) -> URL { envDir(p).appendingPathComponent("store", isDirectory: true) }
  nonisolated var runtimeDir: URL { support.appendingPathComponent("runtime", isDirectory: true) }
  /// Locks live outside the environment folders, so deleting a folder never
  /// races with another process waiting on its lock.
  nonisolated func lockURL(_ id: String) -> URL { support.appendingPathComponent("locks/\(id).lock") }

  // MARK: - Observation

  public func observe(_ p: ProjectIdentity, _ handler: @escaping @Sendable (EnvironmentState) -> Void) -> UUID {
    let id = UUID()
    observers[p.id, default: [:]][id] = handler
    handler(states[p.id] ?? .stopped)
    return id
  }

  public func stopObserving(_ p: ProjectIdentity, _ token: UUID) {
    observers[p.id]?[token] = nil
  }

  private func set(_ p: ProjectIdentity, _ s: EnvironmentState) {
    states[p.id] = s
    for (_, h) in observers[p.id] ?? [:] { h(s) }
  }

  public func state(_ p: ProjectIdentity) -> EnvironmentState { states[p.id] ?? .stopped }

  // MARK: - Lifecycle

  /// Returns the endpoint of a running environment, starting it when needed.
  @discardableResult
  public func start(_ p: ProjectIdentity, limits: EnvironmentLimits = EnvironmentLimits()) async throws -> Endpoint {
    do {
      let endpoint = try await withLock(p.id, stage: { self.set(p, .starting(EnvironmentStage.waitingForOthers.rawValue)) }) {
        try await self.startLocked(p, limits: limits)
      }
      set(p, .ready(endpoint))
      return endpoint
    } catch let e as LauncherError {
      var log = ""
      if case .startFailed(_, let l) = e { log = l }
      set(p, .failed(message: e.description, recovery: e.recovery, log: log))
      throw e
    } catch {
      set(p, .failed(message: "\(error)", recovery: "Retry; if it persists, stop the environment and start it again.", log: ""))
      throw error
    }
  }

  private func startLocked(_ p: ProjectIdentity, limits: EnvironmentLimits) async throws -> Endpoint {
    set(p, .starting(EnvironmentStage.checkingService.rawValue))
    let version = try await cli.version()
    guard version.hasPrefix(ContainerCLI.pinnedVersion) else { throw LauncherError.unsupportedVersion(version) }
    guard await cli.serviceRunning() else { throw LauncherError.serviceStopped }

    let name = containerName(p)
    // An environment that is already running (another window or app instance) is reused.
    if let info = try await inspect(name), info.running {
      if let ep = try? readyEndpoint(p, port: info.hostPort) { return ep }
    }
    if try await inspect(name) != nil {
      // Stopped or broken container: recreate it; documents and the store persist.
      try await removeContainer(name)
    }
    try await ensureImage(p)
    try ensureStore(p, limits: limits)
    let control = controlDir(p)
    try FileManager.default.createDirectory(at: control, withIntermediateDirectories: true)
    chmod(control.path, 0o700)
    try? FileManager.default.removeItem(at: control.appendingPathComponent("server.ready"))
    let grants = control.appendingPathComponent("grants.json")
    if !FileManager.default.fileExists(atPath: grants.path) {
      try Data(#"{"grants":{}}"#.utf8).write(to: grants, options: .atomic)
    }
    try writeEnvironmentInfo(p)
    let port = try freePort()
    set(p, .starting(EnvironmentStage.startingDaemon.rawValue))
    let run = try await cli.run([
      "run", "-d", "--name", name] + (bundledKernel.map { ["--kernel", $0.path] } ?? []) + [
      "-c", String(limits.cpus), "-m", "\(limits.memoryGiB)G",
      "-v", "\(p.url.path):/project",
      "-v", "\(control.path):/control",
      // The store disk image, mounted inside the VM as /persist.
      "-v", "\(storeDir(p).path):/hoststore",
      // RPC is published on the loopback interface only.
      "-p", "127.0.0.1:\(port):7700",
      "-e", "TSUGUMI_CPUS=\(limits.cpus)",
      "-l", "\(Self.projectLabel)=\(p.id)",
      Self.image,
    ], timeout: 120)
    guard run.ok else {
      throw LauncherError.startFailed(reason: run.stderr.trimmingCharacters(in: .whitespacesAndNewlines), log: run.stderr)
    }
    // Wait for server.ready without blocking the UI.
    set(p, .starting(EnvironmentStage.connecting.rawValue))
    let deadline = Date().addingTimeInterval(90)
    while Date() < deadline {
      if let ep = try? readyEndpoint(p, port: port) { return ep }
      if let info = try await inspect(name), !info.running {
        let log = (try? await cli.run(["logs", name], timeout: 20).stdout) ?? ""
        throw LauncherError.startFailed(reason: "tsugumid exited during startup", log: String(log.suffix(4000)))
      }
      try await Task.sleep(for: .milliseconds(100))
    }
    let log = (try? await cli.run(["logs", name], timeout: 20).stdout) ?? ""
    throw LauncherError.startFailed(reason: "no readiness signal within 90 s", log: String(log.suffix(4000)))
  }

  /// Stops the environment. Active runs are interrupted by tsugumid and
  /// recorded as such; documents, history and the store are kept. Throws
  /// when the container is still there afterwards.
  public func stop(_ p: ProjectIdentity) async throws {
    try await withLock(p.id) { try await self.stopLocked(p) }
    set(p, .stopped)
  }

  private func stopLocked(_ p: ProjectIdentity) async throws {
    let name = containerName(p)
    if try await inspect(name) != nil {
      // `stop` may time out on a VM that does not shut down; the forced
      // removal below then ends it, and is checked.
      _ = try await cli.run(["stop", "--time", "20", name], timeout: 60)
      try await removeContainer(name)
    }
    try? FileManager.default.removeItem(at: controlDir(p).appendingPathComponent("server.ready"))
  }

  /// Deletes the environment's data: its container, its Nix store and its
  /// control files (and the named volume of earlier versions). Stop and
  /// deletion hold the environment's lock throughout, so no start can slip
  /// in between. On any failure the folder is kept and the error says what
  /// is left, so the deletion can be retried.
  public func deleteData(_ p: ProjectIdentity) async throws {
    try await withLock(p.id) {
      try await self.stopLocked(p)
      try await self.removeLegacyVolume(p.id)
      try self.removeFolder(self.envDir(p))
    }
    set(p, .stopped)
  }

  /// What `resetAll` removed and what it left.
  public struct ResetReport: Sendable {
    public var containers: [String] = []
    public var environments = 0
    public var volumes: [String] = []
    public var imageRemoved = false
    public var imageKept: String?
    public init() {}
  }

  /// Removes every Tsugumi environment: their containers (only those Tsugumi
  /// labelled), stores, control folders and old volumes, and the environment
  /// image unless a container that is not Tsugumi's still uses it. The
  /// recent-project list, project folders and other containers, images and
  /// volumes are not touched. The next start sets up everything anew.
  public func resetAll() async throws -> ResetReport {
    var report = ResetReport()
    let all = try await listContainers()
    let ours = all.filter { $0.label != nil }
    for c in ours {
      try await withLock(c.label!) {
        if c.running { _ = try await self.cli.run(["stop", "--time", "20", c.id], timeout: 60) }
        try await self.removeContainer(c.id)
      }
      report.containers.append(c.id)
    }
    for v in try await listVolumes() where v.label != nil {
      try await withLock(v.label!) {
        let o = try await self.cli.run(["volume", "rm", v.name], timeout: 120)
        guard o.ok || Self.notFound(o) else { throw LauncherError.deleteFailed(what: "volume \(v.name)", log: o.stderr) }
      }
      report.volumes.append(v.name)
    }
    let envs = (try? FileManager.default.contentsOfDirectory(atPath: environmentsDir.path)) ?? []
    for id in envs where !id.hasPrefix(".") {
      try await withLock(id) { try self.removeFolder(self.envDir(id: id)) }
      report.environments += 1
    }
    for id in Set(ours.compactMap(\.label)) { set(id: id, .stopped) }
    // The image goes too, unless something that is not Tsugumi's runs on it.
    let others = try await listContainers().filter { $0.image == Self.image }
    if others.isEmpty {
      let o = try await cli.run(["image", "rm", Self.image], timeout: 120)
      guard o.ok || Self.notFound(o) else { throw LauncherError.deleteFailed(what: "image \(Self.image)", log: o.stderr) }
      report.imageRemoved = o.ok
    } else {
      report.imageKept = others.map(\.id).joined(separator: ", ")
    }
    try? FileManager.default.removeItem(at: runtimeDir)
    return report
  }

  private func set(id: String, _ s: EnvironmentState) {
    states[id] = s
    for (_, h) in observers[id] ?? [:] { h(s) }
  }

  /// Registers a read-only snapshot of an external project for ProjectImport.
  /// The copy is fixed at this moment; core only reads granted copies.
  public func grantImport(_ p: ProjectIdentity, source: URL) throws -> String {
    let grant = UUID().uuidString.lowercased()
    let control = controlDir(p)
    let dest = control.appendingPathComponent("grants/\(grant)", isDirectory: true)
    try FileManager.default.createDirectory(at: dest, withIntermediateDirectories: true)
    for item in ["tsugumi.json", "layout.json", "definitions"] {
      let src = source.appendingPathComponent(item)
      if FileManager.default.fileExists(atPath: src.path) {
        try FileManager.default.copyItem(at: src, to: dest.appendingPathComponent(item))
      }
    }
    let grantsURL = control.appendingPathComponent("grants.json")
    var grants = (try? JSONSerialization.jsonObject(with: Data(contentsOf: grantsURL))) as? [String: [String: String]] ?? [:]
    grants["grants", default: [:]][grant] = "/control/grants/\(grant)"
    try JSONSerialization.data(withJSONObject: grants).write(to: grantsURL, options: .atomic)
    return grant
  }

  // MARK: - Initial setup

  /// What the host still needs before projects can start.
  public struct SetupStatus: Sendable, Equatable {
    public var containerInstalled = false
    public var version: String?
    public var versionSupported = false
    public var serviceRunning = false
    public var imageLoaded = false
    public var bundledImageAvailable = false
    public var ready: Bool { containerInstalled && versionSupported && serviceRunning && imageLoaded }
    public init() {}
  }

  public func setupStatus() async -> SetupStatus {
    var s = SetupStatus()
    s.bundledImageAvailable = bundledImage.map { FileManager.default.fileExists(atPath: $0.path) } ?? false
    guard FileManager.default.isExecutableFile(atPath: cli.executable.path) else { return s }
    s.containerInstalled = true
    s.version = try? await cli.version()
    s.versionSupported = s.version?.hasPrefix(ContainerCLI.pinnedVersion) ?? false
    s.serviceRunning = await cli.serviceRunning()
    if s.serviceRunning {
      await removeOrphans()
      // A missing stamp (the support folder was deleted or never set up)
      // means the image is set up again from the bundle on the first start.
      s.imageLoaded = (try? await cli.run(["image", "inspect", Self.image], timeout: 60).ok) ?? false
      if s.imageLoaded, s.bundledImageAvailable, !stampMatchesBundle() { s.imageLoaded = false }
    }
    return s
  }

  /// Containers labelled by Tsugumi whose environment folder no longer
  /// exists (the support folder was deleted) are removed when stopped.
  private func removeOrphans() async {
    guard let all = try? await listContainers() else { return }
    for c in all {
      guard let id = c.label, !c.running, !FileManager.default.fileExists(atPath: envDir(id: id).path) else { continue }
      try? await withLock(id) { try await self.removeContainer(c.id) }
    }
  }

  /// Runs `container system start` (the user confirms in the setup screen).
  public func startService() async throws {
    let o = try await cli.run(["system", "start", "--enable-kernel-install"], timeout: 600)
    guard o.ok else { throw LauncherError.startFailed(reason: "container system start failed", log: o.stderr + o.stdout) }
  }

  /// Loads the bundled environment image ahead of the first project start.
  public func loadBundledImage() async throws {
    guard let bundled = bundledImage, FileManager.default.fileExists(atPath: bundled.path) else {
      throw LauncherError.imageMissing
    }
    // `image load` keeps an existing tag; replace it so the bundled one is used.
    _ = try? await cli.run(["image", "rm", Self.image], timeout: 120)
    let load = try await cli.run(["image", "load", "--input", bundled.path], timeout: 1800)
    guard load.ok else { throw LauncherError.startFailed(reason: "image load failed", log: load.stderr) }
    try writeStamp()
  }

  // MARK: - Helpers

  /// Runs `body` holding the environment's host lock (waits up to 60 s).
  private func withLock<T: Sendable>(_ id: String, stage: (() -> Void)? = nil, _ body: () async throws -> T) async throws -> T {
    let lock = try HostLock(path: lockURL(id))
    stage?()
    for _ in 0..<600 {
      if (try? lock.lock(wait: false)) != nil { return try await body() }
      try await Task.sleep(for: .milliseconds(100))
    }
    throw LauncherError.locked
  }

  static func notFound(_ o: ContainerCLI.Output) -> Bool { o.stderr.contains("notFound") || o.stderr.contains("not found") }

  /// Removes a container and confirms it is gone ("already gone" is fine).
  private func removeContainer(_ name: String) async throws {
    let o = try await cli.run(["rm", "-f", name], timeout: 60)
    if !o.ok && !Self.notFound(o) {
      throw LauncherError.deleteFailed(what: "container \(name)", log: o.stderr)
    }
    if try await inspect(name) != nil {
      throw LauncherError.deleteFailed(what: "container \(name)", log: "the container still exists after removal")
    }
  }

  private func removeLegacyVolume(_ id: String) async throws {
    let v = "tsugumi-\(id)-store"
    guard try await cli.run(["volume", "inspect", v], timeout: 30).ok else { return }
    let o = try await cli.run(["volume", "rm", v], timeout: 120)
    guard o.ok || Self.notFound(o) else { throw LauncherError.deleteFailed(what: "volume \(v)", log: o.stderr) }
  }

  private nonisolated func removeFolder(_ url: URL) throws {
    guard FileManager.default.fileExists(atPath: url.path) else { return }
    do { try FileManager.default.removeItem(at: url) } catch {
      throw LauncherError.deleteFailed(what: url.path, log: "\(error)")
    }
  }

  struct ContainerInfo {
    let running: Bool
    let hostPort: Int?
  }

  private func inspect(_ name: String) async throws -> ContainerInfo? {
    let o = try await cli.run(["inspect", name], timeout: 30)
    guard o.ok, let data = o.stdout.data(using: .utf8),
          let list = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]], let first = list.first else {
      return nil
    }
    let status = (first["status"] as? String) ?? ""
    let config = first["configuration"] as? [String: Any]
    let ports = config?["publishedPorts"] as? [[String: Any]]
    let port = ports?.first?["hostPort"] as? Int
    return ContainerInfo(running: status == "running", hostPort: port)
  }

  struct Listed { let id: String; let running: Bool; let image: String?; let label: String? }

  /// Every container; `label` is the Tsugumi project id when Tsugumi made it.
  private func listContainers() async throws -> [Listed] {
    let o = try await cli.run(["ls", "-a", "--format", "json"], timeout: 30)
    guard o.ok, let data = o.stdout.data(using: .utf8),
          let list = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
      throw LauncherError.deleteFailed(what: "container list", log: o.stderr)
    }
    return list.compactMap { c in
      let config = c["configuration"] as? [String: Any]
      guard let id = config?["id"] as? String else { return nil }
      let labels = config?["labels"] as? [String: String]
      let image = (config?["image"] as? [String: Any])?["reference"] as? String
      return Listed(id: id, running: (c["status"] as? String) == "running", image: image, label: labels?[Self.projectLabel])
    }
  }

  private func listVolumes() async throws -> [(name: String, label: String?)] {
    let o = try await cli.run(["volume", "ls", "--format", "json"], timeout: 30)
    guard o.ok, let data = o.stdout.data(using: .utf8),
          let list = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return [] }
    return list.compactMap { v in
      guard let n = v["name"] as? String else { return nil }
      return (n, (v["labels"] as? [String: String])?[Self.projectLabel])
    }
  }

  /// Loads the bundled image when none is there or when it is not the one
  /// this support folder recorded (a deleted folder sets everything up anew).
  private func ensureImage(_ p: ProjectIdentity) async throws {
    let present = try await cli.run(["image", "inspect", Self.image], timeout: 60).ok
    let bundled = bundledImage.map { FileManager.default.fileExists(atPath: $0.path) } ?? false
    if present && (!bundled || stampMatchesBundle()) { return }
    guard bundled else { throw LauncherError.imageMissing }
    set(p, .preparing(EnvironmentStage.loadingImage.rawValue))
    try await loadBundledImage()
  }

  /// A sparse disk image for the environment's Nix store (formatted by the
  /// VM on first use).
  private func ensureStore(_ p: ProjectIdentity, limits: EnvironmentLimits) throws {
    let dir = storeDir(p), img = dir.appendingPathComponent("store.img")
    if FileManager.default.fileExists(atPath: img.path) { return }
    set(p, .preparing(EnvironmentStage.creatingStore.rawValue))
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    guard FileManager.default.createFile(atPath: img.path, contents: nil) else {
      throw LauncherError.startFailed(reason: "cannot create the store disk image", log: img.path)
    }
    let h = try FileHandle(forWritingTo: img)
    try h.truncate(atOffset: UInt64(limits.storeGiB) << 30)
    try h.close()
  }

  private nonisolated func writeEnvironmentInfo(_ p: ProjectIdentity) throws {
    let info: [String: Any] = ["project": p.url.path, "id": p.id]
    try JSONSerialization.data(withJSONObject: info, options: [.prettyPrinted, .sortedKeys])
      .write(to: envDir(p).appendingPathComponent("environment.json"), options: .atomic)
  }

  /// What identifies the bundled image file (size and modification date).
  private nonisolated func bundleIdentity() -> String? {
    guard let b = bundledImage, let a = try? FileManager.default.attributesOfItem(atPath: b.path) else { return nil }
    let size = (a[.size] as? NSNumber)?.int64Value ?? 0
    let date = (a[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
    return "\(size)-\(Int64(date))"
  }

  private nonisolated var stampURL: URL { runtimeDir.appendingPathComponent("image.json") }

  private nonisolated func stampMatchesBundle() -> Bool {
    guard let id = bundleIdentity(), let data = try? Data(contentsOf: stampURL),
          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: String] else { return false }
    return obj["bundle"] == id && obj["image"] == Self.image
  }

  private nonisolated func writeStamp() throws {
    try FileManager.default.createDirectory(at: runtimeDir, withIntermediateDirectories: true)
    let obj = ["bundle": bundleIdentity() ?? "", "image": Self.image]
    try JSONSerialization.data(withJSONObject: obj, options: .sortedKeys).write(to: stampURL, options: .atomic)
  }

  private nonisolated func readyEndpoint(_ p: ProjectIdentity, port: Int?) throws -> Endpoint {
    let url = controlDir(p).appendingPathComponent("server.ready")
    let data = try Data(contentsOf: url)
    guard let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any],
          let token = obj["token"] as? String, let pid = obj["project_id"] as? String,
          let sid = obj["server_instance_id"] as? String, let port else {
      throw LauncherError.notReady("server.ready is incomplete")
    }
    return Endpoint(url: URL(string: "ws://127.0.0.1:\(port)/rpc")!, token: token, projectId: pid, serverInstanceId: sid)
  }

  private nonisolated func freePort() throws -> Int {
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    defer { close(fd) }
    var addr = sockaddr_in()
    addr.sin_family = sa_family_t(AF_INET)
    addr.sin_addr.s_addr = inet_addr("127.0.0.1")
    addr.sin_port = 0
    var len = socklen_t(MemoryLayout<sockaddr_in>.size)
    let ok = withUnsafeMutablePointer(to: &addr) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
        bind(fd, $0, len) == 0 && getsockname(fd, $0, &len) == 0
      }
    }
    guard ok else { throw LauncherError.startFailed(reason: "no free local port", log: "") }
    return Int(UInt16(bigEndian: addr.sin_port))
  }
}
