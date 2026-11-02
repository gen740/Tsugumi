import Darwin
import Foundation
import Virtualization

/// Steps of starting a project's environment. The raw values are internal
/// names (shown as details); the UI says each in words.
public enum EnvironmentStage: String, Sendable, CaseIterable {
  case vmImage = "installing the VM image"
  case vmDisk = "creating the Nix store disk"
  case vmBoot = "booting the VM"
  case vmStore = "mounting the Nix store disk"
  case vmNix = "starting the Nix daemon"
  case vmNetwork = "configuring the network"
  case vmSupervisor = "starting the supervisor"
  case startingDaemon = "starting tsugumid"
  case connecting = "waiting for tsugumid to become ready"

  public init(vmStage vm: SharedVM.Stage) {
    switch vm {
    case .image: self = .vmImage
    case .disk: self = .vmDisk
    case .boot: self = .vmBoot
    case .store: self = .vmStore
    case .nix: self = .vmNix
    case .network: self = .vmNetwork
    case .supervisor, .ready: self = .vmSupervisor
    }
  }
}

/// Starts, finds and stops project environments in the app's one shared VM.
///
/// All projects run in one Linux VM (SharedVM) with one Nix store, kept as a
/// sparse disk image in `~/Library/Application Support/Tsugumi/vm/`. Each
/// open project has its own tsugumid there (its own uid, mount and PID
/// namespaces and cgroup; tsugumi-vmd), sees only its own folder and control
/// folder, and stopping it never stops another project. Project documents,
/// history and artifacts stay in the project folder; the control folder
/// (server.ready, import grants) is `environments/<id>/control` here.
public actor EnvironmentManager {
  public nonisolated let support: URL
  public nonisolated let vm: SharedVM
  private let image: VMImage?
  public nonisolated let resources: VMResources
  private var observers: [String: [UUID: (EnvironmentState) -> Void]] = [:]
  private var states: [String: EnvironmentState] = [:]
  private var bridges: [String: LoopbackBridge] = [:]
  private var starting: [String: Task<Endpoint, Error>] = [:]

  public init(support: URL? = nil, image: VMImage? = VMImage.find(), resources: VMResources = .default) {
    self.support = support ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Tsugumi", isDirectory: true)
    self.image = image
    self.resources = resources
    vm = SharedVM(directory: self.support.appendingPathComponent("vm", isDirectory: true))
  }

  // MARK: - Paths

  nonisolated var environmentsDir: URL { support.appendingPathComponent("environments", isDirectory: true) }
  nonisolated func envDir(_ p: ProjectIdentity) -> URL { environmentsDir.appendingPathComponent(p.id, isDirectory: true) }
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

  private func set(_ id: String, _ s: EnvironmentState) {
    states[id] = s
    for (_, h) in observers[id] ?? [:] { h(s) }
  }

  public func state(_ p: ProjectIdentity) -> EnvironmentState { states[p.id] ?? .stopped }

  // MARK: - Lifecycle

  /// Returns the endpoint of a running environment, starting the VM and the
  /// project's tsugumid when needed. Concurrent callers share one start.
  @discardableResult
  public func start(_ p: ProjectIdentity) async throws -> Endpoint {
    // A ready project is reused only while its tsugumid still runs (it may
    // have exited or crashed since; then it is started again).
    if case .ready(let ep) = states[p.id], bridges[p.id] != nil {
      if let status = try? await vm.request("status", timeout: 10), status.split(separator: " ").contains(where: { $0.hasPrefix("\(p.id):") }) {
        return ep
      }
      bridges[p.id]?.close()
      bridges[p.id] = nil
    }
    if let t = starting[p.id] { return try await t.value }
    let t = Task { try await self.startProject(p) }
    starting[p.id] = t
    defer { starting[p.id] = nil }
    do {
      let ep = try await t.value
      set(p.id, .ready(ep))
      return ep
    } catch let e as LauncherError {
      var log = ""
      if case .startFailed(_, let l) = e { log = l }
      set(p.id, .failed(message: e.description, recovery: e.recovery, log: log))
      throw e
    } catch {
      set(p.id, .failed(message: "\(error)", recovery: "Retry; if it persists, quit Tsugumi and open the project again.", log: ""))
      throw error
    }
  }

  private func startProject(_ p: ProjectIdentity) async throws -> Endpoint {
    guard let image else { throw LauncherError.imageMissing }
    // The VM's own progress is the project's progress until it runs.
    let first = !FileManager.default.fileExists(atPath: vm.directory.appendingPathComponent("store.img").path)
    let token = vm.observe { [weak self] s in
      guard case .starting(let stage) = s else { return }
      let raw = EnvironmentStage(vmStage: stage).rawValue
      Task { await self?.set(p.id, first ? .preparing(raw) : .starting(raw)) }
    }
    defer { vm.stopObserving(token) }
    try await vm.start(image: image, resources: resources)

    set(p.id, .starting(EnvironmentStage.startingDaemon.rawValue))
    let control = controlDir(p)
    try FileManager.default.createDirectory(at: control, withIntermediateDirectories: true)
    chmod(control.path, 0o700)
    let ready = control.appendingPathComponent("server.ready")
    try? FileManager.default.removeItem(at: ready)
    let grants = control.appendingPathComponent("grants.json")
    if !FileManager.default.fileExists(atPath: grants.path) {
      try Data(#"{"grants":{}}"#.utf8).write(to: grants, options: .atomic)
    }
    try writeEnvironmentInfo(p)
    let slot = try await vm.attach(p.id, ["project": p.url, "control": control])
    // The share may take a moment to show new folders in the VM.
    var started = false
    var lastError = ""
    for _ in 0..<50 {
      do {
        try await vm.request("start \(p.id) \(resources.cpus) \(hostName(p)) \(slot)")
        started = true
        break
      } catch {
        lastError = "\(error)"
        try await Task.sleep(for: .milliseconds(100))
      }
    }
    guard started else { throw LauncherError.startFailed(reason: lastError, log: tsugumidLog(p)) }
    bridges[p.id]?.close()
    let bridge = try LoopbackBridge(vm: vm, projectId: p.id)
    bridges[p.id] = bridge

    set(p.id, .starting(EnvironmentStage.connecting.rawValue))
    let deadline = Date().addingTimeInterval(90)
    while Date() < deadline {
      if let ep = try? readyEndpoint(p, port: bridge.port) { return ep }
      if let status = try? await vm.request("status"), !status.contains("\(p.id):") {
        throw LauncherError.startFailed(reason: "tsugumid exited during startup", log: tsugumidLog(p))
      }
      try await Task.sleep(for: .milliseconds(100))
    }
    throw LauncherError.startFailed(reason: "no readiness signal within 90 s", log: tsugumidLog(p))
  }

  /// Stops the project's tsugumid (active runs are recorded as interrupted)
  /// and nothing else; the VM and other projects keep running.
  public func stop(_ p: ProjectIdentity) async throws {
    if case .running = vm.state {
      try await vm.request("stop \(p.id)")
      await vm.detach(p.id)
    }
    bridges[p.id]?.close()
    bridges[p.id] = nil
    try? FileManager.default.removeItem(at: controlDir(p).appendingPathComponent("server.ready"))
    set(p.id, .stopped)
  }

  /// Deletes the project's environment data: its control folder and its
  /// build roots in the shared store (the builds become garbage). Documents,
  /// history and artifacts in the project folder stay.
  public func deleteData(_ p: ProjectIdentity) async throws {
    try await stop(p)
    if case .running = vm.state { try await vm.request("forget \(p.id)") }
    try removeFolder(envDir(p))
  }

  /// A terminal in the project's environment (its uid and namespaces, in
  /// /project): the returned socket carries the terminal's bytes both ways.
  public func openTerminal(_ p: ProjectIdentity, rows: Int, cols: Int) async throws -> (fd: Int32, session: UInt64) {
    guard case .running = vm.state, bridges[p.id] != nil else { throw LauncherError.notReady("the project's environment is not running") }
    let fd = try await vm.connect(port: SharedVM.controlPort)
    do {
      try writeAll(fd, Data("shell \(p.id) \(max(rows, 2)) \(max(cols, 10))\n".utf8))
      let reply = readLine(fd)
      guard reply.hasPrefix("ok "), let sid = UInt64(reply.dropFirst(3)) else {
        throw LauncherError.notReady(reply.hasPrefix("error ") ? String(reply.dropFirst(6)) : "no terminal")
      }
      return (fd, sid)
    } catch {
      close(fd)
      throw error
    }
  }

  public nonisolated func resizeTerminal(session: UInt64, rows: Int, cols: Int) async {
    _ = try? await vm.request("resize \(session) \(max(rows, 2)) \(max(cols, 10))", timeout: 5)
  }

  /// Stops every project and the VM (app quit).
  public func shutdown() async {
    for (_, b) in bridges { b.close() }
    bridges.removeAll()
    await vm.shutdown()
    for id in states.keys { set(id, .stopped) }
  }

  /// What `resetAll` removed.
  public struct ResetReport: Sendable {
    public var environments = 0
    public var vmRemoved = false
    public var legacyContainers: [String] = []
    public init() {}
  }

  /// Stops the VM and removes the shared store, the VM image copy and every
  /// project's control folder. Containers made by earlier versions (Apple
  /// container, labelled by Tsugumi) are removed too when the CLI is there.
  /// Project folders are not touched. The next start sets everything up anew.
  public func resetAll() async throws -> ResetReport {
    var report = ResetReport()
    await shutdown()
    if FileManager.default.fileExists(atPath: vm.directory.path) {
      try removeFolder(vm.directory)
      report.vmRemoved = true
    }
    let envs = (try? FileManager.default.contentsOfDirectory(atPath: environmentsDir.path)) ?? []
    for id in envs where !id.hasPrefix(".") {
      try removeFolder(environmentsDir.appendingPathComponent(id))
      report.environments += 1
    }
    report.legacyContainers = await LegacyContainers.removeAll()
    return report
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

  /// What the Mac needs before projects can start.
  public struct SetupStatus: Sendable, Equatable {
    public var virtualizationSupported = false
    public var imageAvailable = false
    public var imageVersion: String?
    public var ready: Bool { virtualizationSupported && imageAvailable }
    public init() {}
  }

  public func setupStatus() -> SetupStatus {
    var s = SetupStatus()
    s.virtualizationSupported = VZVirtualMachine.isSupported
    s.imageAvailable = image != nil
    s.imageVersion = image?.version
    return s
  }

  // MARK: - Helpers

  /// The project's name for the VM's host name (prompt of its terminal).
  private nonisolated func hostName(_ p: ProjectIdentity) -> String {
    let name = p.url.lastPathComponent.map { c -> Character in
      c.isASCII && (c.isLetter || c.isNumber || c == "-") ? c : "-"
    }
    let s = String(name).trimmingCharacters(in: CharacterSet(charactersIn: "-"))
    return s.isEmpty ? "project" : String(s.prefix(40))
  }

  private nonisolated func tsugumidLog(_ p: ProjectIdentity) -> String {
    let url = controlDir(p).appendingPathComponent("tsugumid.log")
    let text = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
    return String(text.suffix(4000))
  }

  private nonisolated func removeFolder(_ url: URL) throws {
    guard FileManager.default.fileExists(atPath: url.path) else { return }
    do { try FileManager.default.removeItem(at: url) } catch {
      throw LauncherError.deleteFailed(what: url.path, log: "\(error)")
    }
  }

  private nonisolated func writeEnvironmentInfo(_ p: ProjectIdentity) throws {
    let info: [String: Any] = ["project": p.url.path, "id": p.id]
    try JSONSerialization.data(withJSONObject: info, options: [.prettyPrinted, .sortedKeys])
      .write(to: envDir(p).appendingPathComponent("environment.json"), options: .atomic)
  }

  private nonisolated func readyEndpoint(_ p: ProjectIdentity, port: Int) throws -> Endpoint {
    let url = controlDir(p).appendingPathComponent("server.ready")
    let data = try Data(contentsOf: url)
    guard let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any],
          let token = obj["token"] as? String, let pid = obj["project_id"] as? String,
          let sid = obj["server_instance_id"] as? String else {
      throw LauncherError.notReady("server.ready is incomplete")
    }
    return Endpoint(url: URL(string: "ws://127.0.0.1:\(port)/rpc")!, token: token, projectId: pid, serverInstanceId: sid)
  }
}

/// Environments of earlier versions (one Apple container per project), only
/// for removing them on reset. Nothing else uses Apple container any more.
enum LegacyContainers {
  static let label = "tsugumi.project"

  static func removeAll() async -> [String] {
    let cli = ContainerCLI()
    guard FileManager.default.isExecutableFile(atPath: cli.executable.path), await cli.serviceRunning(),
          let o = try? await cli.run(["ls", "-a", "--format", "json"], timeout: 30), o.ok,
          let list = try? JSONSerialization.jsonObject(with: Data(o.stdout.utf8)) as? [[String: Any]] else { return [] }
    var removed: [String] = []
    for c in list {
      let config = c["configuration"] as? [String: Any]
      guard let id = config?["id"] as? String, (config?["labels"] as? [String: String])?[label] != nil else { continue }
      _ = try? await cli.run(["stop", "--time", "20", id], timeout: 60)
      if let r = try? await cli.run(["rm", "-f", id], timeout: 60), r.ok { removed.append(id) }
    }
    return removed
  }
}
