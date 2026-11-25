import Darwin
import Foundation
@_exported import TsugumiVMTypes
@_exported import TsugumiAgentClient
import TsugumiRPC

/// Presentation and file-descriptor adapter for hostd's environment service.
public actor EnvironmentManager {
  public nonisolated let support: URL
  public nonisolated let vm: HostRuntimeSnapshot
  private let client: CoreHostClient
  private let runtimeArguments: [String]
  private var token: UUID?
  private var states: [String: EnvironmentState] = [:]
  private var observers: [String: [UUID: @Sendable (EnvironmentState) -> Void]] = [:]
  private var pipes: [UInt64: TerminalPipe] = [:]
  private var pendingOutput: [UInt64: [String: Any]] = [:]
  private var openingTerminals = 0

  public init(support: URL? = nil, image: VMImage? = VMImage.find(),
              client: CoreHostClient? = nil) {
    let support = support ?? AppChannel.current.supportDirectory
    self.support = support
    self.client = client ?? CoreHostClient(support: support)
    runtimeArguments = HostRuntimeConfiguration.arguments(support: support, image: image)
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
  private func adopt(_ value: [String: Any]) -> EnvironmentState {
    let state: EnvironmentState
    switch value["phase"] as? Tsugumi_Host_EnvironmentPhase ?? .stopped {
    case .ready:
      if let url = (value["endpoint"] as? String).flatMap(URL.init(string:)), let token = value["token"] as? String,
         let id = value["project_id"] as? String {
        state = .ready(Endpoint(url: url, token: token, projectId: id))
      } else { state = .failed(message: "Invalid environment endpoint", recovery: "Retry", log: "") }
    case .starting: state = .starting(value["message"] as? String ?? "")
    case .failed:
      if let diagnostic = value["startup_error"] as? [String: Any], let issue = HistoryVersionMismatch(diagnostic: diagnostic) {
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
    case "HostRuntime": if let value = event["value"] as? Tsugumi_Host_HostRuntimeT { vm.adopt(value) }
    case "HostTerminalData":
      if let value = event["value"] as? [String: Any], let session = value["session"] as? UInt64 {
        if let pipe = pipes[session] { pipe.receive(value) }
        else if openingTerminals > 0 {
          let previous = pendingOutput[session] ?? [:]
          let bytes = (previous["data"] as? [UInt8] ?? []) + (value["data"] as? [UInt8] ?? [])
          pendingOutput[session] = ["data": bytes, "closed": (previous["closed"] as? Bool ?? false) || (value["closed"] as? Bool ?? false)]
        }
      }
    case "Disconnected":
      for (key, state) in states { if case .ready = state { set(key, .reconnecting) } }
      for pipe in pipes.values { pipe.stop() }; pipes.removeAll()
      pendingOutput.removeAll()
    default: break
    }
  }
  private func environment(_ project: ProjectIdentity, _ action: Tsugumi_Host_EnvironmentAction) async throws -> EnvironmentState {
    await prepare()
    let reply = try await client.call("EnvironmentRequest", ["project": project.url.path, "action": action])
    return adopt(reply)
  }
  @discardableResult public func start(_ project: ProjectIdentity) async throws -> Endpoint {
    switch try await environment(project, .start) {
    case .ready(let endpoint): return endpoint
    case .incompatibleHistory(let issue): throw LauncherError.incompatibleHistory(issue)
    case .failed(let message, _, let log): throw LauncherError.startFailed(reason: message, log: log)
    default: throw LauncherError.notReady("the environment is not ready")
    }
  }
  public func moveStateAside(_ project: ProjectIdentity, path: String) async throws {
    await prepare()
    let reply = try await client.call("EnvironmentRequest", [
      "project": project.url.path, "action": Tsugumi_Host_EnvironmentAction.movestateaside, "stale_store": path,
    ])
    _ = adopt(reply)
  }
  public func stop(_ project: ProjectIdentity) async throws { _ = try await environment(project, .stop) }
  public func attachFolders(_ project: ProjectIdentity, _ paths: [String]) async throws -> (Endpoint, [(folder: String, resource: String)]) {
    await prepare()
    let request = Tsugumi_Host_AttachFoldersRequestT()
    request.project = project.url.path
    request.folders = paths.map(Optional.some)
    let reply: Tsugumi_Host_AttachedFoldersT = try await client.hostCall(.attachfoldersrequest, request)
    let state = reply.state
    guard let url = state.endpoint.flatMap(URL.init(string:)) else {
      throw LauncherError.notReady("Invalid environment endpoint")
    }
    let endpoint = Endpoint(url: url, token: state.token ?? "", projectId: state.projectId ?? "")
    set(state.key, .ready(endpoint))
    let folders = reply.folders.compactMap { $0.map { (folder: $0.folder, resource: $0.resource) } }
    return (endpoint, folders)
  }
  public func removeFolders(_ project: ProjectIdentity, _ paths: [String]) async throws {
    await prepare()
    _ = try await client.call("EnvironmentRequest", ["project": project.url.path, "action": Tsugumi_Host_EnvironmentAction.removefolders, "folders": paths])
  }
  private func runtime(_ action: Tsugumi_Host_RuntimeAction = .state, project: String? = nil, command: String? = nil) async throws -> Tsugumi_Host_HostRuntimeT {
    await prepare()
    let request = Tsugumi_Host_HostRuntimeRequestT()
    request.action = action
    request.project = project
    request.command = command
    let reply: Tsugumi_Host_HostRuntimeT = try await client.hostCall(.hostruntimerequest, request)
    vm.adopt(reply)
    return reply
  }
  public func verifyStore() async throws -> String {
    try await runtime(.verifystore).message ?? ""
  }
  public func cleanupStore() async throws {
    _ = try await runtime(.cleanupstore)
  }
  public func deleteData(_ project: ProjectIdentity) async throws {
    _ = try await runtime(.deleteenvironment, project: project.url.path)
  }
  public func runtimeCommand(_ command: String) async throws -> String {
    try await runtime(.command, command: command).message ?? ""
  }
  public func activityBeforeQuit() async throws -> Bool { try await client.activityBeforeQuit() }
  public func shutdownForQuit() async { await client.stopResident(waitForExit: false) }
  public func shutdownForUpdate() async { await client.stopResident() }
  /// The shared VM's size: what its next start uses, the defaults and limits
  /// of this Mac (hostd's), and the store disk file now (GiB).
  public struct VMSize: Sendable, Equatable {
    public var next: VMResources, defaults: VMResources, limits: VMResources
    public var storeDiskGiB: Int
  }
  public func vmSize() async throws -> VMSize {
    let reply = try await runtime()
    func size(_ value: Tsugumi_Host_VMResourcesT?) -> VMResources {
      VMResources(cpus: Int(value?.cpus ?? 0), memoryGiB: Int(value?.memoryGib ?? 0), storeGiB: Int(value?.storeGib ?? 0))
    }
    return VMSize(next: size(reply.vmResources), defaults: size(reply.vmDefaultResources),
                  limits: size(reply.vmLimits), storeDiskGiB: Int(reply.storeDiskGib))
  }
  public func resetConfirmation(english: Bool) async throws -> String {
    let reply = try await runtime()
    return (english ? reply.resetConfirmationEn : reply.resetConfirmationJa) ?? ""
  }
  public struct ResetReport: Sendable { public var environments = 0; public var vmRemoved = false; public init() {} }
  public func resetAll() async throws -> ResetReport {
    let reply = try await runtime(.reset)
    var report = ResetReport(); report.environments = Int(reply.environments); report.vmRemoved = reply.vmRemoved; return report
  }
  public enum SetupStatus: Sendable, Equatable {
    case checked(virtualizationSupported: Bool, imageAvailable: Bool, imageVersion: String?)
    case failed(String)
    /// hostd found host state written by another version. Not a setup problem:
    /// the project chooser shows the state and lets the person move it aside.
    case hostStateFormatMismatch

    public var ready: Bool {
      switch self {
      case .checked(let supported, let image, _): return supported && image
      case .hostStateFormatMismatch: return true
      case .failed: return false
      }
    }

    static func query(_ runtime: () async throws -> Tsugumi_Host_HostRuntimeT) async -> SetupStatus {
      do {
        let reply = try await runtime()
        return .checked(virtualizationSupported: reply.virtualizationSupported, imageAvailable: reply.imageAvailable, imageVersion: reply.imageVersion)
      } catch is HostStateFormatMismatch {
        return .hostStateFormatMismatch
      } catch {
        return .failed(String(describing: error))
      }
    }
  }
  public func setupStatus() async -> SetupStatus {
    await SetupStatus.query { try await self.runtime() }
  }
  public func openTerminal(_ project: ProjectIdentity, rows: Int, cols: Int) async throws -> (fd: Int32, session: UInt64) {
    return try await openTerminal(id: project.url.path, rows: rows, cols: cols)
  }
  public func openTerminal(id: String, rows: Int, cols: Int) async throws -> (fd: Int32, session: UInt64) {
    await prepare()
    let request = Tsugumi_Host_HostTerminalRequestT()
    request.action = .open_
    request.project = id
    request.rows = UInt32(truncatingIfNeeded: rows)
    request.cols = UInt32(truncatingIfNeeded: cols)
    openingTerminals += 1
    defer { openingTerminals -= 1 }
    let reply: Tsugumi_Host_HostTerminalT = try await client.hostCall(.hostterminalrequest, request)
    let session = reply.session
    let pipe = try TerminalPipe(client: client, session: session) { [weak self] in Task { await self?.removeTerminal(session) } }
    pipes[session] = pipe
    if let value = pendingOutput.removeValue(forKey: session) { pipe.receive(value) }
    return (pipe.local, session)
  }
  private func removeTerminal(_ session: UInt64) { pipes[session] = nil }
  public func resizeTerminal(session: UInt64, rows: Int, cols: Int) async throws {
    _ = try await client.call("HostTerminalRequest", ["action": Tsugumi_Host_TerminalAction.resize, "session": session, "rows": rows, "cols": cols])
  }
}

/// Runtime state and console presentation; it has no VM control methods.
public final class HostRuntimeSnapshot: @unchecked Sendable {
  public let directory: URL
  public let console = ConsoleLog()
  private let lock = NSLock()
  private var current: VMState = .stopped
  private var observers: [UUID: @Sendable (VMState) -> Void] = [:]
  public init(directory: URL) { self.directory = directory }
  public var state: VMState { lock.withLock { current } }
  public func observe(_ handler: @escaping @Sendable (VMState) -> Void) -> UUID {
    let token = UUID(); let state = lock.withLock { observers[token] = handler; return current }; handler(state); return token
  }
  public func stopObserving(_ token: UUID) { lock.withLock { observers[token] = nil } }
  func adopt(_ value: Tsugumi_Host_HostRuntimeT) {
    let state: VMState
    switch value.vmState {
    case "running": state = .running
    case "starting": state = .starting(VMStage(rawValue: value.stage ?? "") ?? .boot)
    case "failed": state = .failed(value.message ?? "")
    default: state = .stopped
    }
    let handlers = lock.withLock { current = state; return Array(observers.values) }
    for handler in handlers { handler(state) }
    for line in value.console.compactMap({ $0 }) { _ = console.feed(Data((line + "\n").utf8)) }
  }
}
