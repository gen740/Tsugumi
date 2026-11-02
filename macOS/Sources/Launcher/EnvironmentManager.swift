import Darwin
import Foundation

/// Starts, finds and stops the per-project Apple container environment.
///
/// One environment per project identity (device + inode, so path aliases map
/// to the same environment). Start/stop is serialized across processes by a
/// host file lock. Only the project folder, the environment's 0700 control
/// folder and its persistent store volume are shared with the VM.
public actor EnvironmentManager {
  public static let imageName = "tsugumi-env"
  public static let imageTag = "1.0.0"
  public static var image: String { "\(imageName):\(imageTag)" }

  public nonisolated let cli: ContainerCLI
  public nonisolated let support: URL
  private let bundledImage: URL?
  private var observers: [String: [UUID: (EnvironmentState) -> Void]] = [:]
  private var states: [String: EnvironmentState] = [:]

  public init(cli: ContainerCLI = ContainerCLI(), support: URL? = nil, bundledImage: URL? = nil) {
    self.cli = cli
    self.support = support ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Tsugumi", isDirectory: true)
    self.bundledImage = bundledImage
  }

  // MARK: - Paths and names

  nonisolated public func containerName(_ p: ProjectIdentity) -> String { "tsugumi-\(p.id)" }
  nonisolated public func volumeName(_ p: ProjectIdentity) -> String { "tsugumi-\(p.id)-store" }
  nonisolated func envDir(_ p: ProjectIdentity) -> URL { support.appendingPathComponent("environments/\(p.id)", isDirectory: true) }
  nonisolated public func controlDir(_ p: ProjectIdentity) -> URL { envDir(p).appendingPathComponent("control", isDirectory: true) }

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
      let endpoint = try await startLocked(p, limits: limits)
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
    let lock = try HostLock(path: envDir(p).appendingPathComponent("launch.lock"))
    set(p, .starting("waiting for other windows"))
    try await waitLock(lock)
    set(p, .starting("checking the container service"))
    let version = try await cli.version()
    guard version.hasPrefix(ContainerCLI.pinnedVersion) else { throw LauncherError.unsupportedVersion(version) }
    guard await cli.serviceRunning() else { throw LauncherError.serviceStopped }

    let name = containerName(p)
    // An environment that is already running (another window or app instance) is reused.
    if let info = try await inspect(name), info.running {
      if let ep = try? readyEndpoint(p, port: info.hostPort) { return ep }
    }
    if try await inspect(name) != nil {
      // Stopped or broken container: recreate it; documents and the store volume persist.
      _ = try await cli.run(["rm", "-f", name], timeout: 60)
    }
    try await ensureImage(p)
    try await ensureVolume(p, limits: limits)
    let control = controlDir(p)
    try FileManager.default.createDirectory(at: control, withIntermediateDirectories: true)
    chmod(control.path, 0o700)
    try? FileManager.default.removeItem(at: control.appendingPathComponent("server.ready"))
    let grants = control.appendingPathComponent("grants.json")
    if !FileManager.default.fileExists(atPath: grants.path) {
      try Data(#"{"grants":{}}"#.utf8).write(to: grants, options: .atomic)
    }
    let port = try freePort()
    set(p, .starting("starting tsugumid"))
    let run = try await cli.run([
      "run", "-d", "--name", name,
      "-c", String(limits.cpus), "-m", "\(limits.memoryGiB)G",
      "-v", "\(p.url.path):/project",
      "-v", "\(control.path):/control",
      "-v", "\(volumeName(p)):/persist",
      // RPC is published on the loopback interface only.
      "-p", "127.0.0.1:\(port):7700",
      "-e", "TSUGUMI_CPUS=\(limits.cpus)",
      "-l", "tsugumi.project=\(p.id)",
      Self.image,
    ], timeout: 120)
    guard run.ok else {
      throw LauncherError.startFailed(reason: run.stderr.trimmingCharacters(in: .whitespacesAndNewlines), log: run.stderr)
    }
    // Wait for server.ready without blocking the UI.
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
  /// recorded as such; documents, history and the store are kept.
  public func stop(_ p: ProjectIdentity) async throws {
    let lock = try HostLock(path: envDir(p).appendingPathComponent("launch.lock"))
    try await waitLock(lock)
    let name = containerName(p)
    _ = try await cli.run(["stop", "--time", "20", name], timeout: 60)
    _ = try await cli.run(["rm", "-f", name], timeout: 60)
    try? FileManager.default.removeItem(at: controlDir(p).appendingPathComponent("server.ready"))
    set(p, .stopped)
  }

  /// Deletes the environment's data (the Nix store volume and control files).
  /// The project's documents and history stay in the project folder.
  public func deleteData(_ p: ProjectIdentity) async throws {
    try await stop(p)
    _ = try await cli.run(["volume", "rm", volumeName(p)], timeout: 120)
    try? FileManager.default.removeItem(at: envDir(p))
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
      s.imageLoaded = (try? await cli.run(["image", "inspect", Self.image], timeout: 60).ok) ?? false
    }
    return s
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
    let load = try await cli.run(["image", "load", "--input", bundled.path], timeout: 1800)
    guard load.ok else { throw LauncherError.startFailed(reason: "image load failed", log: load.stderr) }
  }

  // MARK: - Helpers

  private func waitLock(_ lock: HostLock) async throws {
    for _ in 0..<600 {
      if (try? lock.lock(wait: false)) != nil { return }
      try await Task.sleep(for: .milliseconds(100))
    }
    throw LauncherError.locked
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

  private func ensureImage(_ p: ProjectIdentity) async throws {
    let o = try await cli.run(["image", "inspect", Self.image], timeout: 60)
    if o.ok { return }
    guard let bundled = bundledImage, FileManager.default.fileExists(atPath: bundled.path) else {
      throw LauncherError.imageMissing
    }
    set(p, .preparing("loading the environment image (first start)"))
    let load = try await cli.run(["image", "load", "--input", bundled.path], timeout: 1800)
    guard load.ok else { throw LauncherError.startFailed(reason: "image load failed", log: load.stderr) }
  }

  private func ensureVolume(_ p: ProjectIdentity, limits: EnvironmentLimits) async throws {
    let v = volumeName(p)
    if try await cli.run(["volume", "inspect", v], timeout: 30).ok { return }
    set(p, .preparing("creating the \(limits.storeGiB) GiB store volume"))
    let o = try await cli.run(["volume", "create", "-s", "\(limits.storeGiB)G", "--label", "tsugumi.project=\(p.id)", v], timeout: 120)
    guard o.ok else { throw LauncherError.startFailed(reason: "volume create failed", log: o.stderr) }
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
