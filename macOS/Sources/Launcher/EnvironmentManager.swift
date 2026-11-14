import Darwin
import Foundation
@_exported import TsugumiAgentClient
import TsugumiRPC

public enum EnvironmentStage: String, Sendable, CaseIterable {
  case vmImage = "installing the VM image"
  case vmDisk = "creating the Nix store disk"
  case vmBoot = "booting the VM"
  case vmStore = "mounting the Nix store disk"
  case vmNix = "starting the Nix daemon"
  case vmNetwork = "configuring the network"
  case vmSystem = "building Tsugumi in the VM"
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
    case .system: self = .vmSystem
    case .supervisor, .ready: self = .vmSupervisor
    }
  }
}

/// Presentation and file-descriptor adapter for agentd's environment service.
public actor EnvironmentManager {
  public nonisolated let support: URL
  public nonisolated let vm: HostRuntimeSnapshot
  public nonisolated let resources: VMResources
  private let client: CoreAgentClient
  private let runtimeArguments: [String]
  private var token: UUID?
  private var states: [String: EnvironmentState] = [:]
  private var observers: [String: [UUID: @Sendable (EnvironmentState) -> Void]] = [:]
  private var pipes: [UInt64: TerminalPipe] = [:]
  private var pendingOutput: [UInt64: [String: Any]] = [:]
  public private(set) var extraFolders: [String: [String]] = [:]
  public private(set) var allowedOrigins: [String] = []

  public init(support: URL? = nil, image: VMImage? = VMImage.find(), resources: VMResources = .default) {
    let support = support ?? AppChannel.current.supportDirectory
    self.support = support; self.resources = resources
    client = CoreAgentClient(support: support)
    runtimeArguments = HostRuntimeConfiguration.arguments(support: support, image: image, resources: resources)
    vm = HostRuntimeSnapshot(directory: support.appendingPathComponent("vm"))
  }
  public nonisolated func controlDir(_ project: ProjectIdentity) -> URL {
    support.appendingPathComponent("environments/\(project.id)/control")
  }
  private func prepare() async {
    guard token == nil else { return }
    await client.configureRuntime(runtimeArguments)
    token = await client.observeHost { [weak self] event in Task { await self?.event(event) } }
  }
  private func set(_ key: String, _ state: EnvironmentState) {
    states[key] = state
    for handler in observers[key]?.values ?? Dictionary<UUID, @Sendable (EnvironmentState) -> Void>().values { handler(state) }
  }
  public func observe(_ project: ProjectIdentity, _ handler: @escaping @Sendable (EnvironmentState) -> Void) async -> UUID {
    await prepare()
    let id = UUID(); observers[project.id, default: [:]][id] = handler
    handler(states[project.id] ?? .stopped)
    Task { _ = try? await environment(project, .state) }
    return id
  }
  public func stopObserving(_ project: ProjectIdentity, _ token: UUID) { observers[project.id]?[token] = nil }
  public func state(_ project: ProjectIdentity) -> EnvironmentState { states[project.id] ?? .stopped }
  private func adopt(_ value: [String: Any]) -> EnvironmentState {
    let state: EnvironmentState
    switch value["phase"] as? Tsugumi_Host_EnvironmentPhase ?? .stopped {
    case .ready:
      if let url = (value["endpoint"] as? String).flatMap(URL.init(string:)), let token = value["token"] as? String,
         let id = value["project_id"] as? String, let instance = value["server_instance_id"] as? String {
        state = .ready(Endpoint(url: url, token: token, projectId: id, serverInstanceId: instance))
      } else { state = .failed(message: "Invalid environment endpoint", recovery: "Retry", log: "") }
    case .starting: state = .starting(value["message"] as? String ?? "")
    case .failed:
      if let raw = value["startup_error_json"] as? String, let diagnostic = (try? JSONSerialization.jsonObject(with: Data(raw.utf8))) as? [String: Any], let issue = HistoryVersionMismatch(diagnostic: diagnostic) {
        state = .incompatibleHistory(issue)
      } else { state = .failed(message: value["message"] as? String ?? "", recovery: value["recovery"] as? String ?? "", log: value["log"] as? String ?? "") }
    case .stopped: state = .stopped
    }
    if let key = value["key"] as? String { set(key, state) }
    return state
  }
  private func event(_ event: [String: Any]) {
    switch event["type"] as? String {
    case "EnvironmentState": if let value = event["value"] as? [String: Any] { _ = adopt(value) }
    case "EnvironmentProgress": if let key = event["key"] as? String { set(key, event["preparing"] as? Bool == true ? .preparing(event["stage"] as? String ?? "") : .starting(event["stage"] as? String ?? "")) }
    case "HostRuntime": if let value = event["value"] as? [String: Any] { vm.adopt(value) }
    case "HostTerminalData":
      if let value = event["value"] as? [String: Any], let session = value["session"] as? UInt64 {
        if let pipe = pipes[session] { pipe.receive(value) }
        else if pendingOutput.count < 16 {
          let previous = pendingOutput[session] ?? [:]
          let bytes = (previous["data"] as? [UInt8] ?? []) + (value["data"] as? [UInt8] ?? [])
          if bytes.count <= 65536 { pendingOutput[session] = ["data": bytes, "closed": (previous["closed"] as? Bool ?? false) || (value["closed"] as? Bool ?? false)] }
        }
      }
    case "Disconnected":
      for (key, state) in states { if case .ready = state { set(key, .reconnecting) } }
      for pipe in pipes.values { pipe.stop() }; pipes.removeAll()
    default: break
    }
  }
  private func environment(_ project: ProjectIdentity, _ action: Tsugumi_Host_EnvironmentAction, archive: Bool = false) async throws -> EnvironmentState {
    await prepare()
    let reply = try await client.call("EnvironmentRequest", ["project": project.url.path, "action": action,
      "origins": allowedOrigins, "folders": extraFolders[project.id] ?? [], "archive_incompatible_history": archive])
    return adopt(reply)
  }
  @discardableResult public func start(_ project: ProjectIdentity, archiveIncompatibleHistory: Bool = false) async throws -> Endpoint {
    switch try await environment(project, .start, archive: archiveIncompatibleHistory) {
    case .ready(let endpoint): return endpoint
    case .incompatibleHistory(let issue): throw LauncherError.incompatibleHistory(issue)
    case .failed(let message, _, let log): throw LauncherError.startFailed(reason: message, log: log)
    default: throw LauncherError.notReady("the environment is not ready")
    }
  }
  public func stop(_ project: ProjectIdentity) async throws { _ = try await environment(project, .stop) }
  public func setExtraFolders(_ project: ProjectIdentity, _ paths: [String]) async throws {
    extraFolders[project.id] = paths
    _ = try await environment(project, .folders)
  }
  public func setAllowedOrigins(_ origins: [String]) { allowedOrigins = origins }
  private func runtime(_ action: Tsugumi_Host_RuntimeAction = .state, project: String? = nil, key: String? = nil, command: String? = nil) async throws -> [String: Any] {
    await prepare()
    var fields: [String: Any] = ["action": action]
    fields["project"] = project; fields["project_id"] = key; fields["command"] = command
    let reply = try await client.call("HostRuntimeRequest", fields)
    vm.adopt(reply)
    return reply
  }
  public func runtimeCommand(_ command: String) async throws -> String { try await runtime(.command, command: command)["message"] as? String ?? "" }
  public func cleanupStore() async throws -> (bytes: UInt64, paths: Int) {
    let reply = try await runtime(.cleanupstore)
    return (reply["bytes"] as? UInt64 ?? 0, Int(reply["paths"] as? UInt32 ?? 0))
  }
  public struct VMHistory: Sendable, Identifiable, Hashable { public let id: String; public let bytes: UInt64; public let lastStart: Date?; public let running: Bool }
  public func vmHistories() async throws -> [VMHistory] {
    let reply = try await runtime(.histories)
    return (reply["histories"] as? [[String: Any]] ?? []).compactMap { row in
      guard let id = row["id"] as? String else { return nil }
      let ms = row["last_start_ms"] as? Int64 ?? 0
      return VMHistory(id: id, bytes: row["bytes"] as? UInt64 ?? 0, lastStart: ms > 0 ? Date(timeIntervalSince1970: Double(ms) / 1000) : nil, running: row["running"] as? Bool ?? false)
    }
  }
  public func deleteVMData(id: String) async throws { _ = try await runtime(.deletehistory, key: id) }
  public func deleteData(_ project: ProjectIdentity) async throws { _ = try await runtime(.deleteenvironment, project: project.url.path) }
  public func shutdown() async { _ = try? await runtime(.shutdown) }
  public struct ResetReport: Sendable { public var environments = 0; public var vmRemoved = false; public init() {} }
  public func resetAll() async throws -> ResetReport {
    let reply = try await runtime(.reset)
    var report = ResetReport(); report.environments = Int(reply["environments"] as? UInt32 ?? 0); report.vmRemoved = reply["vm_removed"] as? Bool ?? false; return report
  }
  public struct SetupStatus: Sendable, Equatable {
    public var virtualizationSupported = false; public var imageAvailable = false; public var imageVersion: String?
    public var ready: Bool { virtualizationSupported && imageAvailable }; public init() {}
  }
  public func setupStatus() async -> SetupStatus {
    var status = SetupStatus()
    if let reply = try? await runtime() { status.virtualizationSupported = reply["virtualization_supported"] as? Bool ?? false; status.imageAvailable = reply["image_available"] as? Bool ?? false; status.imageVersion = reply["image_version"] as? String }
    return status
  }
  public func openTerminal(_ project: ProjectIdentity, rows: Int, cols: Int) async throws -> (fd: Int32, session: UInt64) {
    return try await openTerminal(id: project.url.path, rows: rows, cols: cols)
  }
  public func openTerminal(id: String, rows: Int, cols: Int) async throws -> (fd: Int32, session: UInt64) {
    await prepare()
    let reply = try await client.call("HostTerminalRequest", ["action": Tsugumi_Host_TerminalAction.open_, "project": id, "rows": rows, "cols": cols])
    guard let session = reply["session"] as? UInt64 else { throw LauncherError.notReady("invalid terminal session") }
    let pipe = try TerminalPipe(client: client, session: session) { [weak self] in Task { await self?.removeTerminal(session) } }
    pipes[session] = pipe
    if let value = pendingOutput.removeValue(forKey: session) { pipe.receive(value) }
    return (pipe.local, session)
  }
  private func removeTerminal(_ session: UInt64) { pipes[session] = nil }
  public func resizeTerminal(session: UInt64, rows: Int, cols: Int) async {
    _ = try? await client.call("HostTerminalRequest", ["action": Tsugumi_Host_TerminalAction.resize, "session": session, "rows": rows, "cols": cols])
  }
  public func grantImport(_ p: ProjectIdentity, source: URL) async throws -> String {
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

}

/// Runtime state and console presentation; it has no VM control methods.
public final class HostRuntimeSnapshot: @unchecked Sendable {
  public let directory: URL
  public let console = ConsoleLog()
  private let lock = NSLock()
  private var current: SharedVM.State = .stopped
  private var observers: [UUID: @Sendable (SharedVM.State) -> Void] = [:]
  public init(directory: URL) { self.directory = directory }
  public var state: SharedVM.State { lock.withLock { current } }
  public func observe(_ handler: @escaping @Sendable (SharedVM.State) -> Void) -> UUID {
    let token = UUID(); let state = lock.withLock { observers[token] = handler; return current }; handler(state); return token
  }
  public func stopObserving(_ token: UUID) { lock.withLock { observers[token] = nil } }
  func adopt(_ value: [String: Any]) {
    let state: SharedVM.State
    switch value["vm_state"] as? String {
    case "running": state = .running
    case "starting": state = .starting(SharedVM.Stage(rawValue: value["stage"] as? String ?? "") ?? .boot)
    case "failed": state = .failed(value["message"] as? String ?? "")
    default: state = .stopped
    }
    let handlers = lock.withLock { current = state; return Array(observers.values) }
    for handler in handlers { handler(state) }
    for line in value["console"] as? [String] ?? [] { _ = console.feed(Data((line + "\n").utf8)) }
  }
}
