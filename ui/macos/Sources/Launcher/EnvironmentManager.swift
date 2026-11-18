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

  public init(support: URL? = nil, image: VMImage? = VMImage.find()) {
    let support = support ?? AppChannel.current.supportDirectory
    self.support = support
    client = CoreHostClient(support: support)
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
        else {
          let previous = pendingOutput[session] ?? [:]
          let bytes = (previous["data"] as? [UInt8] ?? []) + (value["data"] as? [UInt8] ?? [])
          pendingOutput[session] = ["data": bytes, "closed": (previous["closed"] as? Bool ?? false) || (value["closed"] as? Bool ?? false)]
        }
      }
    case "Disconnected":
      for (key, state) in states { if case .ready = state { set(key, .reconnecting) } }
      for pipe in pipes.values { pipe.stop() }; pipes.removeAll()
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
  public func stop(_ project: ProjectIdentity) async throws { _ = try await environment(project, .stop) }
  public func attachFolders(_ project: ProjectIdentity, _ paths: [String]) async throws -> (Endpoint, [(folder: String, resource: String)]) {
    await prepare()
    let reply = try await client.call("AttachFoldersRequest", ["project": project.url.path, "folders": paths])
    guard let value = reply["state"] as? [String: Any], case .ready(let endpoint) = adopt(value) else {
      throw LauncherError.notReady("the attachment did not return a ready environment")
    }
    let folders = (reply["folders"] as? [[String: String]] ?? []).compactMap { row -> (folder: String, resource: String)? in
      guard let folder = row["folder"], let resource = row["resource"] else { return nil }
      return (folder, resource)
    }
    return (endpoint, folders)
  }
  public func removeFolders(_ project: ProjectIdentity, _ paths: [String]) async throws {
    await prepare()
    _ = try await client.call("EnvironmentRequest", ["project": project.url.path, "action": Tsugumi_Host_EnvironmentAction.removefolders, "folders": paths])
  }
  private func runtime(_ action: Tsugumi_Host_RuntimeAction = .state, project: String? = nil, key: String? = nil, command: String? = nil) async throws -> [String: Any] {
    await prepare()
    var fields: [String: Any] = ["action": action]
    fields["project"] = project; fields["project_id"] = key; fields["command"] = command
    let reply = try await client.call("HostRuntimeRequest", fields)
    vm.adopt(reply)
    return reply
  }
  public func verifyStore() async throws -> String {
    try await runtime(.verifystore)["message"] as? String ?? ""
  }
  public func cleanupStore() async throws -> (bytes: UInt64, paths: Int) {
    let reply = try await runtime(.cleanupstore)
    return (reply["bytes"] as? UInt64 ?? 0, Int(reply["paths"] as? UInt32 ?? 0))
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
    func size(_ key: String) -> VMResources {
      let v = reply[key] as? [String: Int] ?? [:]
      return VMResources(cpus: v["cpus"] ?? 0, memoryGiB: v["memory_gib"] ?? 0, storeGiB: v["store_gib"] ?? 0)
    }
    return VMSize(next: size("vm_resources"), defaults: size("vm_default_resources"), limits: size("vm_limits"),
                  storeDiskGiB: Int(reply["store_disk_gib"] as? UInt32 ?? 0))
  }
  public func resetConfirmation(english: Bool) async throws -> String {
    let reply = try await runtime()
    return reply[english ? "reset_confirmation_en" : "reset_confirmation_ja"] as? String ?? ""
  }
  public struct ResetReport: Sendable { public var environments = 0; public var vmRemoved = false; public init() {} }
  public func resetAll() async throws -> ResetReport {
    let reply = try await runtime(.reset)
    var report = ResetReport(); report.environments = Int(reply["environments"] as? UInt32 ?? 0); report.vmRemoved = reply["vm_removed"] as? Bool ?? false; return report
  }
  public enum SetupStatus: Sendable, Equatable {
    case checked(virtualizationSupported: Bool, imageAvailable: Bool, imageVersion: String?)
    case failed(String)

    public var ready: Bool {
      if case .checked(let supported, let image, _) = self { return supported && image }
      return false
    }

    /// A failed request or malformed reply says nothing about the Mac or image.
    static func query(_ runtime: () async throws -> [String: Any]) async -> SetupStatus {
      do {
        let reply = try await runtime()
        guard let supported = reply["virtualization_supported"] as? Bool,
              let image = reply["image_available"] as? Bool else {
          return .failed(String(localized: "実行環境からの状態の応答が不正です。", bundle: .main))
        }
        return .checked(virtualizationSupported: supported, imageAvailable: image, imageVersion: reply["image_version"] as? String)
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
  func adopt(_ value: [String: Any]) {
    let state: VMState
    switch value["vm_state"] as? String {
    case "running": state = .running
    case "starting": state = .starting(VMStage(rawValue: value["stage"] as? String ?? "") ?? .boot)
    case "failed": state = .failed(value["message"] as? String ?? "")
    default: state = .stopped
    }
    let handlers = lock.withLock { current = state; return Array(observers.values) }
    for handler in handlers { handler(state) }
    for line in value["console"] as? [String] ?? [] { _ = console.feed(Data((line + "\n").utf8)) }
  }
}
