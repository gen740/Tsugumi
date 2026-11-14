import AppKit
import Foundation
import Observation
import TsugumiLauncher
import TsugumiAgentClient

struct DemoEntry: Identifiable, Hashable {
  let id: String
  let title: String
  let description: String
  let url: URL
  var tags: [String] = []
  /// The field the guide lists it under (an implemented demo; nil: a Draft).
  var field: String?
}

struct RecentProject: Decodable, Identifiable, Hashable {
  var id: String { path }
  let path: String
  let name: String
  var openedAt: Date
}

/// Application wide state: environments, the Demos catalog, recent projects
/// and one ProjectSession per project shared by all of its windows.
@MainActor
@Observable
final class AppModel {
  static let shared = AppModel()

  let environments: EnvironmentManager
  private(set) var demos: [DemoEntry] = []
  private(set) var recents: [RecentProject] = []
  private(set) var homeLoaded = false
  private(set) var demosLoaded = false
  private(set) var recentsError: String?
  private(set) var demosError: String?
  @ObservationIgnored private let requestHome: ((String, [String: Any]) async throws -> [String: Any])?
  @ObservationIgnored private var homeRefresh: Task<Void, Never>?
  private(set) var sessions: [String: ProjectSession] = [:]
  var error: String?
  /// The main windows there are, each with its project or Home (MainWindowRouting.swift).
  var mainWindows: [Workspace] = []
  /// What the next new main windows do first.
  @ObservationIgnored var pendingWindowIntents: [MainWindowIntent] = []
  private(set) var nixpkgsPin: String?
  private(set) var setup: EnvironmentManager.SetupStatus?
  private(set) var checkingSetup = false
  @ObservationIgnored private var chats: [String: ChatController] = [:]
  /// Set when a process-level smoke check drives the app (no setup screen).
  var smokeMode = false

  /// True once the host check finished and something still needs doing.
  var setupNeeded: Bool { !smokeMode && (setup.map { !$0.ready } ?? false) }

  func refreshSetup() async {
    guard !checkingSetup else { return }
    checkingSetup = true
    defer { checkingSetup = false }
    setup = await environments.setupStatus()
  }

  /// First-start preparation of the shared VM happens when the first
  /// project opens (its loading card shows the steps), never here.
  let preparing: LoadingContent? = nil

  @ObservationIgnored private var launchProjectTaken = false

  /// A project folder given as a launch argument, handed out once.
  func takeLaunchProject() -> URL? {
    guard !launchProjectTaken else { return nil }
    launchProjectTaken = true
    for a in CommandLine.arguments.dropFirst() where !a.hasPrefix("-") {
      let url = URL(fileURLWithPath: a)
      if FileManager.default.fileExists(atPath: url.appendingPathComponent("tsugumi.json").path) { return url }
    }
    return nil
  }

  /// Chats already created (opening Settings never creates new ones).
  var openChats: [ChatController] { Array(chats.values) }

  /// One chat per project, shared by its panel and its detached window, on the
  /// session open now: a view leaving a closed session (drawn once more) never
  /// keeps the project's chat on it, where its tools have no connection.
  func chat(for s: ProjectSession) -> ChatController {
    let id = s.identity.id
    let current = sessions[id] ?? s
    if let c = chats[id], c.session === current { return c }
    chats[id]?.detach()
    let c = ChatController(session: current)
    chats[id] = c
    return c
  }

  /// The open session for a window value, opening the project if needed.
  func session(ref: ProjectRef) -> ProjectSession? { try? session(for: ref.url) }

  init(refreshOnInit: Bool = true, requestHome: ((String, [String: Any]) async throws -> [String: Any])? = nil) {
    self.requestHome = requestHome
    let bundle = Bundle.main
    // TSUGUMI_SUPPORT_DIR: another support folder (VM, store), for checks
    // that must not touch the user's own environment.
    let support = ProcessInfo.processInfo.environment["TSUGUMI_SUPPORT_DIR"].map { URL(fileURLWithPath: $0) }
    environments = EnvironmentManager(support: support, image: VMImage.find(bundle: bundle))
    if refreshOnInit { initialRefresh = Task { await refreshHome() } }
  }

  /// The refresh started by `init`. A caller that sets the core's state directory
  /// itself waits for it, so it cannot start the shared core there later.
  private(set) var initialRefresh: Task<Void, Never>?

  private func hostRequest(_ method: String, _ fields: [String: Any] = [:]) async throws -> [String: Any] {
    do {
      if let requestHome { return try await requestHome(method, fields) }
      return try await CoreAgentClient.shared.call(method, fields)
    }
    catch let failure as CoreAgentFailure { throw HostProjectError(code: failure.code) }
  }

  /// Home's first appearance joins the initial read instead of requesting it twice.
  func showHome() async {
    if let initialRefresh {
      await initialRefresh.value
      self.initialRefresh = nil
    } else { await refreshHome() }
  }

  /// Independent host reads update their own parts as soon as each arrives.
  func refreshHome() async {
    if let homeRefresh { await homeRefresh.value; return }
    let task = Task {
      async let recent: Void = refreshRecents()
      async let catalog: Void = refreshDemos()
      async let defaults: Void = refreshDefaults()
      _ = await (recent, catalog, defaults)
    }
    homeRefresh = task
    await task.value
    homeRefresh = nil
  }

  private func refreshRecents() async {
    recentsError = nil
    do { try await loadRecents(); homeLoaded = true }
    catch { recentsError = "\(error)" }
  }

  private func refreshDefaults() async {
    do { nixpkgsPin = try await hostRequest("HostProjectDefaultsRequest")["nixpkgsPin"] as? String }
    catch { self.error = "\(error)" }
  }

  private func refreshDemos() async {
    demosError = nil
    do {
      let catalog = try await hostRequest("HostDemoListRequest", ["language": AppPreferences.Language.running.rawValue])
      demos = (catalog["value"] as? [[String: Any]] ?? []).compactMap { d in
        guard let id = d["id"] as? String, let title = d["title"] as? String, let path = d["path"] as? String else { return nil }
        return DemoEntry(id: id, title: title, description: d["description"] as? String ?? "", url: URL(fileURLWithPath: path),
                         tags: d["tags"] as? [String] ?? [], field: d["field"] as? String)
      }
      demosLoaded = true
    } catch { demosError = "\(error)" }
  }

  static func decodeRecents(_ value: Any) throws -> [RecentProject] {
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .millisecondsSince1970
    return try decoder.decode([RecentProject].self, from: JSONSerialization.data(withJSONObject: value))
  }

  private func loadRecents() async throws {
    let result = try await hostRequest("HostRecentListRequest")
    recents = try Self.decodeRecents(result["value"] ?? [])
  }

  /// Takes projects off the recent list (their folders stay).
  func forgetRecents(_ paths: Set<String>) {
    Task {
      do {
        _ = try await hostRequest("HostRecentForgetRequest", ["paths": Array(paths)])
        try await loadRecents()
      } catch { self.error = "\(error)" }
    }
  }

  private func remember(_ url: URL) {
    Task {
      do {
        _ = try await hostRequest("HostRecentRememberRequest", ["path": url.path])
        try await loadRecents()
      } catch { self.error = "\(error)" }
    }
  }

  /// Main windows showing each project: when the last one leaves it, the
  /// project's UI subscriptions are released; core keeps running.
  private var holders: [String: Int] = [:]

  func hold(_ s: ProjectSession) { holders[s.identity.id, default: 0] += 1 }

  func release(_ s: ProjectSession) async {
    let id = s.identity.id
    holders[id, default: 1] -= 1
    guard holders[id] ?? 0 <= 0 else { return }
    holders[id] = nil
    // Edits that could not be saved stay open: opening it again finds them, quitting asks.
    guard !s.hasUnsavedEdits else { return }
    if let c = chats[id] { GuideController.shared.chatClosed(c) }
    chats[id]?.detach()
    chats[id] = nil
    sessions[id] = nil
    await s.close()
  }

  func session(for url: URL) throws -> ProjectSession {
    let identity = try ProjectIdentity(url: url)
    if let s = sessions[identity.id] { return s }
    let s = ProjectSession(identity: identity, environments: environments)
    s.makeRepositoryOnConnect = fresh.remove(identity.url.resolvingSymlinksInPath().path) != nil
    sessions[identity.id] = s
    remember(identity.url)
    s.start()
    return s
  }

  /// Projects made or copied here: they become git repositories when they
  /// first connect (a demo's history file becomes its versions).
  private var fresh: Set<String> = []

  /// The destination preference is UI state; the core chooses and copies the folder.
  func copyDemo(_ demo: DemoEntry) async throws -> URL {
    let docs = ProcessInfo.processInfo.environment["TSUGUMI_PROJECTS_DIR"].map { URL(fileURLWithPath: $0) }
      ?? FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Tsugumi")
    let result = try await hostRequest("HostDemoCopyRequest", ["id": demo.id, "parent": docs.path,
                                                               "language": AppPreferences.Language.running.rawValue])
    return try createdProject(result)
  }

  func createProject(at url: URL) async throws {
    let result = try await hostRequest("HostProjectCreateRequest", ["parent": url.deletingLastPathComponent().path,
      "name": url.lastPathComponent, "flow": AppPreferences.shared.flowDirection.rawValue])
    _ = try createdProject(result)
  }

  func duplicateProject(from source: URL, to destination: URL) async throws -> URL {
    let result = try await hostRequest("HostProjectDuplicateRequest", ["path": source.path,
      "parent": destination.deletingLastPathComponent().path, "name": destination.lastPathComponent])
    return try createdProject(result)
  }

  private func createdProject(_ result: [String: Any]) throws -> URL {
    guard let path = result["path"] as? String else { throw HostProjectError(code: "protocol") }
    fresh.insert(path)
    return URL(fileURLWithPath: path)
  }
}

/// Presentation only: the core chooses the failure code.
struct HostProjectError: Error, CustomStringConvertible {
  let code: String
  var description: String {
    switch code {
    case "invalid_project_name": return String(localized: "フォルダ名に使えない文字が含まれているか、名前が空です。")
    case "project_defaults_unavailable": return String(localized: "このビルドには既定の Nixpkgs が設定されていません。")
    case "invalid_flow": return String(localized: "フローの方向が正しくありません。")
    case "folder_not_empty": return String(localized: "作成先は空ではありません。空のフォルダか新しい名前を選んでください。")
    case "project_too_large": return String(localized: "プロジェクトが大きすぎます（64 MB まで）。")
    case "invalid_project": return String(localized: "有効なプロジェクトのフォルダを指定してください。複製先は元のフォルダの外にしてください。")
    case "demo_not_found": return String(localized: "デモが見つかりません。")
    case "invalid_language": return String(localized: "デモの言語が正しくありません。")
    case "permission_denied": return String(localized: "この操作にはユーザーの権限が必要です。")
    default: return String(localized: "プロジェクトの操作に失敗しました。")
    }
  }
}
