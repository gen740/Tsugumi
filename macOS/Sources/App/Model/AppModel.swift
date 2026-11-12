import AppKit
import Foundation
import Observation
import TsugumiLauncher

struct DemoEntry: Identifiable, Hashable {
  let id: String
  let title: String
  let description: String
  let url: URL
  var tags: [String] = []
  /// The field the guide lists it under (an implemented demo; nil: a Draft).
  var field: String?
}

struct RecentProject: Codable, Identifiable, Hashable {
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
  private(set) var sessions: [String: ProjectSession] = [:]
  var error: String?
  /// The main windows there are, each with its project or Home (MainWindowRouting.swift).
  var mainWindows: [Workspace] = []
  /// What the next new main windows do first.
  @ObservationIgnored var pendingWindowIntents: [MainWindowIntent] = []
  let demosRoot: URL?
  private(set) var setup: EnvironmentManager.SetupStatus?
  private(set) var checkingSetup = false
  @ObservationIgnored private var chats: [String: ChatController] = [:]
  /// Set when a process-level smoke check drives the app (no setup screen).
  var smokeMode = false

  /// True once the host check finished and something still needs doing.
  var setupNeeded: Bool { !smokeMode && (setup.map { !$0.ready } ?? false) }

  func refreshSetup() async {
    checkingSetup = true
    setup = await environments.setupStatus()
    checkingSetup = false
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

  init() {
    let bundle = Bundle.main
    // TSUGUMI_SUPPORT_DIR: another support folder (VM, store), for checks
    // that must not touch the user's own environment.
    let support = ProcessInfo.processInfo.environment["TSUGUMI_SUPPORT_DIR"].map { URL(fileURLWithPath: $0) }
    environments = EnvironmentManager(support: support, image: VMImage.find(bundle: bundle))
    // The demos in the app's language (demos/ja, demos/en: each a set of its own).
    demosRoot = (bundle.url(forResource: "demos", withExtension: nil) ?? AppModel.sourceTreeDemos())?
      .appendingPathComponent(AppPreferences.Language.running.rawValue, isDirectory: true)
    loadDemos()
    loadRecents()
  }

  /// When run from the source tree (swift run), demos come from the repository.
  private static func sourceTreeDemos() -> URL? {
    #if DEBUG
    var dir = URL(fileURLWithPath: #filePath)
    for _ in 0..<5 { dir.deleteLastPathComponent() }
    let candidate = dir.appendingPathComponent("demos")
    return FileManager.default.fileExists(atPath: candidate.appendingPathComponent("ja/index.json").path) ? candidate : nil
    #else
    return nil  // a release build has its demos in the bundle only
    #endif
  }

  private func loadDemos() {
    guard let root = demosRoot, let data = try? Data(contentsOf: root.appendingPathComponent("index.json")),
          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let list = obj["demos"] as? [[String: Any]] else { return }
    demos = list.compactMap { d in
      guard let id = d["id"] as? String, let title = d["title"] as? String, let p = d["path"] as? String else { return nil }
      return DemoEntry(id: id, title: title, description: d["description"] as? String ?? "", url: root.appendingPathComponent(p),
                       tags: d["tags"] as? [String] ?? [], field: (d["field"] as? String).flatMap { $0.isEmpty ? nil : $0 })
    }
  }

  private var recentsURL: URL {
    environments.support.appendingPathComponent("recent-projects.json")
  }

  private func loadRecents() {
    guard let data = try? Data(contentsOf: recentsURL),
          let list = try? JSONDecoder().decode([RecentProject].self, from: data) else { return }
    recents = list.filter { FileManager.default.fileExists(atPath: $0.path) }
  }

  /// Takes projects off the recent list (their folders stay).
  func forgetRecents(_ paths: Set<String>) {
    recents.removeAll { paths.contains($0.path) }
    try? JSONEncoder().encode(recents).write(to: recentsURL, options: .atomic)
  }

  private func remember(_ url: URL, name: String) {
    recents.removeAll { $0.path == url.path }
    recents.insert(RecentProject(path: url.path, name: name, openedAt: Date()), at: 0)
    recents = Array(recents.prefix(30))
    try? FileManager.default.createDirectory(at: recentsURL.deletingLastPathComponent(), withIntermediateDirectories: true)
    try? JSONEncoder().encode(recents).write(to: recentsURL, options: .atomic)
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
    guard !s.dirty else { return }
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
    let name = (try? JSONSerialization.jsonObject(with: Data(contentsOf: identity.url.appendingPathComponent("tsugumi.json"))) as? [String: Any])?["name"] as? String
    remember(identity.url, name: name ?? identity.url.lastPathComponent)
    s.start()
    return s
  }

  /// Projects made or copied here: they become git repositories when they
  /// first connect (a demo's history file becomes its versions).
  private var fresh: Set<String> = []

  /// Copies a bundled demo into a new, editable project (the original stays untouched).
  func copyDemo(_ demo: DemoEntry) throws -> URL {
    // TSUGUMI_PROJECTS_DIR: where copies go instead of ~/Documents/Tsugumi (checks).
    let docs = ProcessInfo.processInfo.environment["TSUGUMI_PROJECTS_DIR"].map { URL(fileURLWithPath: $0) }
      ?? FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Tsugumi")
    try FileManager.default.createDirectory(at: docs, withIntermediateDirectories: true)
    var dest = docs.appendingPathComponent(demo.id)
    var n = 2
    while FileManager.default.fileExists(atPath: dest.path) {
      dest = docs.appendingPathComponent("\(demo.id)-\(n)")
      n += 1
    }
    try FileManager.default.copyItem(at: demo.url, to: dest)
    fresh.insert(dest.resolvingSymlinksInPath().path)
    return dest
  }

  /// Creates an empty project; the initial documents are placed before the environment starts.
  func createProject(at url: URL, name: String) throws {
    try Self.writeNewProject(at: url, name: name, flow: AppPreferences.shared.flowDirection.rawValue)
    fresh.insert(url.resolvingSymlinksInPath().path)
  }

  /// A new project's documents in `url`: never over an existing project or
  /// other files (the save panel allows choosing a folder that exists).
  static func writeNewProject(at url: URL, name: String, flow: String) throws {
    let existing = (try? FileManager.default.contentsOfDirectory(atPath: url.path))?.filter { $0 != ".DS_Store" } ?? []
    if !existing.isEmpty { throw FolderNotEmpty(path: url.path) }
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    let project: JSONValueLiteral = """
    {
      "name": \(JSONValueLiteral.quote(name)),
      "pkgsPath": \(JSONValueLiteral.quote(NixpkgsPin.current ?? "tsugumi:default")),
      "canvas": { "cards": [], "connections": [] }
    }

    """
    try Data(project.utf8).write(to: url.appendingPathComponent("tsugumi.json"))
    // The direction the new project's Cards are laid out in (the app's default).
    try Data("{\"flow\": \"\(flow)\"}\n".utf8).write(to: url.appendingPathComponent("layout.json"))
  }
}

typealias JSONValueLiteral = String
extension String {
  static func quote(_ s: String) -> String {
    let data = try! JSONSerialization.data(withJSONObject: [s], options: [.fragmentsAllowed])
    return String(String(decoding: data, as: UTF8.self).dropFirst().dropLast())
  }
}

/// A new project goes into an empty folder only.
struct FolderNotEmpty: Error, CustomStringConvertible {
  let path: String
  var description: String { String(localized: "\(path) は空ではありません。空のフォルダか新しい名前を選んでください。") }
}
