import AppKit
import CryptoKit
import Foundation
import Observation
import TsugumiLauncher

/// What the newest GitHub release offers: its version, the app (Tsugumi.zip)
/// and the checksum file published next to it by the release workflow
/// (.github/workflows/release.yml).
struct ReleaseOffer: Equatable, Sendable {
  var version: String
  var zip: URL
  var checksum: URL
}

/// The parts of an update that need neither the network nor the running app
/// (UILogicTests).
enum UpdateCheck {
  static let assetName = "Tsugumi.zip"
  static var checksumName: String { assetName + ".sha256" }

  /// "v1.2.3" or "1.2.3" → [1, 2, 3]; nil for anything else.
  static func components(_ s: String) -> [Int]? {
    let t = s.hasPrefix("v") ? s.dropFirst() : Substring(s)
    let parts = t.split(separator: ".", omittingEmptySubsequences: false)
    let numbers = parts.compactMap { $0.allSatisfy(\.isNumber) ? Int($0) : nil }
    return parts.count == 3 && numbers.count == 3 ? numbers : nil
  }

  static func isNewer(_ candidate: String, than current: String) -> Bool {
    guard let a = components(candidate), let b = components(current) else { return false }
    return b.lexicographicallyPrecedes(a)
  }

  private struct Release: Decodable {
    struct Asset: Decodable {
      let name: String
      let browser_download_url: URL
    }
    let tag_name: String
    let draft: Bool?
    let prerelease: Bool?
    let assets: [Asset]
  }

  /// The update a `releases/latest` answer offers over `current`; nil when it
  /// is not newer, is a draft or pre-release, or lacks the app or its checksum.
  static func offer(_ json: Data, current: String) throws -> ReleaseOffer? {
    let r = try JSONDecoder().decode(Release.self, from: json)
    guard r.draft != true, r.prerelease != true, isNewer(r.tag_name, than: current),
          let zip = r.assets.first(where: { $0.name == assetName }),
          let sum = r.assets.first(where: { $0.name == checksumName }) else { return nil }
    let version = r.tag_name.hasPrefix("v") ? String(r.tag_name.dropFirst()) : r.tag_name
    return ReleaseOffer(version: version, zip: zip.browser_download_url, checksum: sum.browser_download_url)
  }

  /// The SHA-256 a `shasum -a 256` line gives for `name` (or a lone hash).
  static func checksum(in text: String, for name: String) -> String? {
    for line in text.split(whereSeparator: \.isNewline) {
      let fields = line.split(separator: " ", omittingEmptySubsequences: true)
      guard let hash = fields.first, hash.count == 64, hash.allSatisfy(\.isHexDigit) else { continue }
      let file = fields.dropFirst().first.map { $0.hasPrefix("*") ? String($0.dropFirst()) : String($0) }
      if file == nil || file == name { return hash.lowercased() }
    }
    return nil
  }

  static func sha256(of file: URL) throws -> String {
    let h = try FileHandle(forReadingFrom: file)
    defer { try? h.close() }
    var hasher = SHA256()
    while let chunk = try h.read(upToCount: 1 << 20), !chunk.isEmpty { hasher.update(data: chunk) }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
  }

  enum Problem: Error, CustomStringConvertible {
    case bundle(String)
    case checksum
    case http(Int)
    case tool(String, Int32)
    var description: String {
      switch self {
      case .bundle(let why): return String(localized: "ダウンロードしたアプリが正しくありません（\(why)）")
      case .checksum: return String(localized: "ダウンロードしたファイルのチェックサムが一致しません")
      case .http(let code): return String(localized: "GitHub から HTTP \(code) が返りました")
      case .tool(let name, let status): return "\(name) (exit \(status))"
      }
    }
  }

  /// A downloaded app must be the release of the same app, of the version offered.
  static func checkBundle(_ app: URL, version: String, bundleID: String) throws {
    let plist = app.appendingPathComponent("Contents/Info.plist")
    guard let info = NSDictionary(contentsOf: plist) as? [String: Any] else { throw Problem.bundle("Info.plist") }
    guard info["CFBundleIdentifier"] as? String == bundleID else { throw Problem.bundle("CFBundleIdentifier") }
    guard info["TsugumiChannel"] as? String == AppChannel.release.rawValue else { throw Problem.bundle("TsugumiChannel") }
    guard info["CFBundleShortVersionString"] as? String == version else { throw Problem.bundle("CFBundleShortVersionString") }
  }
}

/// Updates of a release build from the GitHub releases of the repository.
///
/// Once after launch and then every six hours (or when asked from the menu
/// or the settings) it reads the latest release; a newer one is downloaded
/// next to the app, its checksum and signature checked, and it takes the
/// app's place when the app quits: the VM has been powered off by then, so
/// no run is cut short by an update. "Restart to Update" quits and opens the
/// new version. A build from source (AppChannel.dev) never updates itself.
@MainActor @Observable
final class Updater {
  static let shared = Updater()
  nonisolated static let repository = "gen740/Tsugumi"
  nonisolated static var releasesPage: URL { URL(string: "https://github.com/\(repository)/releases")! }
  private static let automaticKey = "update.automatic"

  enum State: Equatable {
    case idle
    case checking
    case upToDate
    case downloading(String)
    /// Downloaded and checked: installed when the app quits.
    case ready(String)
    case failed(String)
  }

  private(set) var state: State = .idle
  private(set) var lastCheck: Date?
  let currentVersion: String
  let build: String
  /// Checks by themselves after launch and every six hours.
  var automatic: Bool {
    didSet {
      UserDefaults.standard.set(automatic, forKey: Self.automaticKey)
      schedule()
    }
  }

  private let app: URL
  private let bundleID: String
  private let channel: AppChannel
  private var staged: URL?
  private var relaunch = false
  private var loop: Task<Void, Never>?

  init(bundle: Bundle = .main, channel: AppChannel = .current) {
    app = bundle.bundleURL
    bundleID = bundle.bundleIdentifier ?? ""
    self.channel = channel
    currentVersion = bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0.0.0"
    build = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? ""
    automatic = UserDefaults.standard.object(forKey: Self.automaticKey) as? Bool ?? true
  }

  /// Why this copy of the app cannot update itself (nil: it can).
  var unavailable: String? {
    if channel == .dev || app.pathExtension != "app" {
      return String(localized: "ソースからビルドした開発版（Tsugumi Dev）は自動更新しません。")
    }
    if app.path.contains("/AppTranslocation/") {
      return String(localized: "macOS が隔離した場所から起動しているため更新できません。Tsugumi.app を「アプリケーション」フォルダへ移してから開き直してください。")
    }
    let folder = app.deletingLastPathComponent()
    if !FileManager.default.isWritableFile(atPath: folder.path) {
      return String(localized: "\(folder.path) に書き込めないため、このアプリを更新できません。")
    }
    return nil
  }

  /// From applicationDidFinishLaunching: the periodic checks of a release
  /// build, except in scripted runs (UI scripts, smoke tests).
  func start() {
    let env = ProcessInfo.processInfo.environment
    guard env["TSUGUMI_UI_SCRIPT"] == nil, env["TSUGUMI_SMOKE_DEMO"] == nil else { return }
    schedule()
  }

  private func schedule() {
    loop?.cancel()
    loop = nil
    guard automatic, unavailable == nil else { return }
    loop = Task { [weak self] in
      try? await Task.sleep(for: .seconds(20))
      while !Task.isCancelled {
        await self?.check()
        try? await Task.sleep(for: .seconds(6 * 3600))
      }
    }
  }

  var busy: Bool {
    switch state {
    case .checking, .downloading: return true
    default: return false
    }
  }

  /// Reads the latest release and, when it is newer, downloads it. `manual`:
  /// asked from the menu, so the outcome is said in a dialog.
  func check(manual: Bool = false) async {
    if let why = unavailable {
      if manual { Self.alert(String(localized: "このアプリは自動更新できません"), why) }
      return
    }
    if case .ready(let v) = state {
      if manual { askToRelaunch(v) }
      return
    }
    guard !busy else { return }
    state = .checking
    do {
      guard let offer = try await Self.latest(current: currentVersion) else {
        lastCheck = Date()
        state = .upToDate
        if manual { Self.alert(String(localized: "Tsugumi は最新です"), String(localized: "版 \(currentVersion) が最新の版です。")) }
        return
      }
      lastCheck = Date()
      state = .downloading(offer.version)
      staged = try await Self.stage(offer, beside: app, bundleID: bundleID)
      state = .ready(offer.version)
      if manual { askToRelaunch(offer.version) }
    } catch {
      state = .failed("\(error)")
      if manual { Self.alert(String(localized: "アップデートを確認できませんでした"), "\(error)") }
    }
  }

  /// Quits (asking about unsaved edits as usual) and opens the new version.
  func installAndRelaunch() {
    guard case .ready = state else { return }
    relaunch = true
    NSApp.terminate(nil)
  }

  /// From applicationWillTerminate: the downloaded version takes the app's
  /// place. Any failure keeps the current app as it is.
  func installIfReady() {
    guard let new = staged, FileManager.default.fileExists(atPath: new.path) else { return }
    do {
      _ = try FileManager.default.replaceItemAt(app, withItemAt: new, backupItemName: nil, options: [.usingNewMetadataOnly])
    } catch {
      NSLog("Tsugumi update not installed: \(error)")
      return
    }
    LSRegisterURL(app as CFURL, true)
    guard relaunch else { return }
    // Opened once this process has exited (the VM is off by then).
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/bin/sh")
    p.arguments = ["-c", "while /bin/kill -0 \"$1\" 2>/dev/null; do /bin/sleep 0.2; done; /usr/bin/open \"$2\"",
                   "sh", "\(getpid())", app.path]
    p.standardInput = FileHandle.nullDevice
    p.standardOutput = FileHandle.nullDevice
    p.standardError = FileHandle.nullDevice
    try? p.run()
  }

  private func askToRelaunch(_ version: String) {
    let a = NSAlert()
    a.messageText = String(localized: "Tsugumi \(version) の準備ができました")
    a.informativeText = String(localized: "再起動すると新しい版になります。あとにした場合は、次に終了したときに更新します。")
    a.addButton(withTitle: String(localized: "再起動して更新"))
    a.addButton(withTitle: String(localized: "あとで"))
    if a.runModal() == .alertFirstButtonReturn { installAndRelaunch() }
  }

  private static func alert(_ title: String, _ text: String) {
    let a = NSAlert()
    a.messageText = title
    a.informativeText = text
    a.runModal()
  }

  // MARK: - Network

  private nonisolated static func latest(current: String) async throws -> ReleaseOffer? {
    var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(repository)/releases/latest")!)
    req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
    req.setValue("Tsugumi/\(current)", forHTTPHeaderField: "User-Agent")
    req.cachePolicy = .reloadIgnoringLocalCacheData
    let (data, response) = try await URLSession.shared.data(for: req)
    let code = (response as? HTTPURLResponse)?.statusCode ?? 0
    if code == 404 { return nil }  // no release yet
    guard code == 200 else { throw UpdateCheck.Problem.http(code) }
    return try UpdateCheck.offer(data, current: current)
  }

  /// Downloads and checks the offered app in a folder on the app's volume
  /// (so it can take the app's place by a rename); returns the new app.
  private nonisolated static func stage(_ offer: ReleaseOffer, beside app: URL, bundleID: String) async throws -> URL {
    let fm = FileManager.default
    let dir = try fm.url(for: .itemReplacementDirectory, in: .userDomainMask, appropriateFor: app, create: true)
    do {
      let (sums, r1) = try await URLSession.shared.data(from: offer.checksum)
      try ok(r1)
      guard let expected = UpdateCheck.checksum(in: String(decoding: sums, as: UTF8.self), for: UpdateCheck.assetName) else {
        throw UpdateCheck.Problem.checksum
      }
      let (tmp, r2) = try await URLSession.shared.download(from: offer.zip)
      try ok(r2)
      let zip = dir.appendingPathComponent(UpdateCheck.assetName)
      try fm.moveItem(at: tmp, to: zip)
      guard try UpdateCheck.sha256(of: zip) == expected else { throw UpdateCheck.Problem.checksum }
      try run("/usr/bin/ditto", "-x", "-k", zip.path, dir.path)
      try? fm.removeItem(at: zip)
      let new = dir.appendingPathComponent("Tsugumi.app")
      try UpdateCheck.checkBundle(new, version: offer.version, bundleID: bundleID)
      try run("/usr/bin/codesign", "--verify", "--deep", "--strict", new.path)
      // Downloads of the app itself are not quarantined; made sure anyway.
      try? run("/usr/bin/xattr", "-dr", "com.apple.quarantine", new.path)
      return new
    } catch {
      try? fm.removeItem(at: dir)
      throw error
    }
  }

  private nonisolated static func ok(_ r: URLResponse) throws {
    let code = (r as? HTTPURLResponse)?.statusCode ?? 0
    guard code == 200 else { throw UpdateCheck.Problem.http(code) }
  }

  private nonisolated static func run(_ tool: String, _ args: String...) throws {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: tool)
    p.arguments = args
    p.standardOutput = FileHandle.nullDevice
    p.standardError = FileHandle.nullDevice
    try p.run()
    p.waitUntilExit()
    guard p.terminationStatus == 0 else { throw UpdateCheck.Problem.tool((tool as NSString).lastPathComponent, p.terminationStatus) }
  }
}
