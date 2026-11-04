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
  /// "New project" asked from the menu: Home opens its sheet.
  var requestNewProject = false
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

  /// One chat per project, shared by its panel and its detached window.
  func chat(for s: ProjectSession) -> ChatController {
    if let c = chats[s.identity.id] { return c }
    let c = ChatController(session: s)
    chats[s.identity.id] = c
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
    demosRoot = bundle.url(forResource: "demos", withExtension: nil) ?? AppModel.sourceTreeDemos()
    loadDemos()
    loadRecents()
  }

  /// When run from the source tree (swift run), demos come from the repository.
  private static func sourceTreeDemos() -> URL? {
    var dir = URL(fileURLWithPath: #filePath)
    for _ in 0..<4 { dir.deleteLastPathComponent() }
    let candidate = dir.appendingPathComponent("demos")
    return FileManager.default.fileExists(atPath: candidate.appendingPathComponent("index.json").path) ? candidate : nil
  }

  private func loadDemos() {
    guard let root = demosRoot, let data = try? Data(contentsOf: root.appendingPathComponent("index.json")),
          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let list = obj["demos"] as? [[String: Any]] else { return }
    demos = list.compactMap { d in
      guard let id = d["id"] as? String, let title = d["title"] as? String, let p = d["path"] as? String else { return nil }
      return DemoEntry(id: id, title: title, description: d["description"] as? String ?? "", url: root.appendingPathComponent(p),
                       tags: d["tags"] as? [String] ?? [])
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
  /// project's environment is given back (ProjectSession.close).
  private var holders: [String: Int] = [:]

  func hold(_ s: ProjectSession) { holders[s.identity.id, default: 0] += 1 }

  func release(_ s: ProjectSession) async {
    let id = s.identity.id
    holders[id, default: 1] -= 1
    guard holders[id] ?? 0 <= 0 else { return }
    holders[id] = nil
    chats[id]?.cancel()
    chats[id] = nil
    sessions[id] = nil
    await s.close()
  }

  func session(for url: URL) throws -> ProjectSession {
    let identity = try ProjectIdentity(url: url)
    if let s = sessions[identity.id] { return s }
    let s = ProjectSession(identity: identity, environments: environments)
    sessions[identity.id] = s
    let name = (try? JSONSerialization.jsonObject(with: Data(contentsOf: identity.url.appendingPathComponent("tsugumi.json"))) as? [String: Any])?["name"] as? String
    remember(identity.url, name: name ?? identity.url.lastPathComponent)
    s.start()
    return s
  }

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
    return dest
  }

  /// Creates an empty project; the initial documents are placed before the environment starts.
  func createProject(at url: URL, name: String) throws {
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    let project: JSONValueLiteral = """
    {
      "name": \(JSONValueLiteral.quote(name)),
      "pkgsPath": "tsugumi:default",
      "databaseConnections": [],
      "canvas": { "cards": [], "connections": [] }
    }

    """
    try Data(project.utf8).write(to: url.appendingPathComponent("tsugumi.json"))
    // The direction the new project's Cards are laid out in (the app's default).
    try Data("{\"flow\": \"\(AppPreferences.shared.flowDirection.rawValue)\"}\n".utf8).write(to: url.appendingPathComponent("layout.json"))
  }
}

typealias JSONValueLiteral = String
extension String {
  static func quote(_ s: String) -> String {
    let data = try! JSONSerialization.data(withJSONObject: [s], options: [.fragmentsAllowed])
    return String(String(decoding: data, as: UTF8.self).dropFirst().dropLast())
  }
}
