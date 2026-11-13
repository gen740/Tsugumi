import AppKit
import CryptoKit
import SwiftUI
import TsugumiRPC
import WebKit

/// How a Card's figure follows its results (the Card setting `displayMode`).
enum FigureMode: String {
  /// The last figure, dimmed while the Card or one upstream of it runs.
  case update
  /// Each revision of a running Card replaces the last one in place.
  case live
}

/// The setting, and what the run is doing around the Card.
struct FigureState: Equatable {
  var mode: FigureMode = .update
  /// The Card runs (or is about to).
  var running = false
  /// A Card upstream of it runs: its figure is about to be replaced.
  var stale = false

  var holdsDisplay: Bool { mode == .update && (running || stale) }

  /// Whether a Card that reaches `card` through connections is busy: in the
  /// Card's own Workflow, then upstream of each Card in Card it is inside.
  static func upstreamBusy(_ card: CardKey, connections: ([String]) -> [JSONValue], busy: (CardKey) -> Bool) -> Bool {
    let p = path(card)
    guard let id = p.last else { return false }
    let scope = Array(p.dropLast())
    let conns = connections(scope)
    var seen: Set<String> = [], todo = [id]
    while let at = todo.popLast() {
      for c in conns {
        guard let from = c["from"]?["card"]?.string, c["to"]?["card"]?.string == at, seen.insert(from).inserted else { continue }
        if busy(key(scope + [from])) { return true }
        todo.append(from)
      }
    }
    return !scope.isEmpty && upstreamBusy(key(scope), connections: connections, busy: busy)
  }
}

/// The result a frame is to show: its content and the size it is drawn at.
/// Compared at every move of the canvas, so by the strings themselves (the
/// digest is taken once, when this changes).
struct FigureWanted: Equatable {
  let name: String
  let json: String
  var blobs: [String: Data] = [:]
  let width: Int
  let height: Int
}

/// What a frame shows of the picture it holds. A picture is at full strength
/// only as what it is: the wanted content, or (live) the previous revision of
/// the invocation that is still running. Every other one kept on screen is dimmed.
enum FigureLook: Equatable {
  case picture(dimmed: Bool, waiting: Bool)
  /// The wanted content could not be drawn.
  case failed
  /// Nothing to show yet.
  case waiting

  var isWaiting: Bool {
    switch self {
    case .picture(_, let waiting): waiting
    case .failed: false
    case .waiting: true
    }
  }

  /// `picture`: what the held picture is of, and whether it is an image (not a failure).
  static func of(wanted: FigureWanted, lineage: String,
                 picture: (wanted: FigureWanted, lineage: String, isImage: Bool)?, state: FigureState) -> FigureLook {
    guard let picture else { return .waiting }
    let same = picture.wanted.name == wanted.name && picture.wanted.json == wanted.json && picture.wanted.blobs == wanted.blobs
    guard picture.isImage else { return same ? .failed : .waiting }
    // The same content at another size (a resize, a zoom) is still this result's picture.
    if same { return .picture(dimmed: state.holdsDisplay, waiting: false) }
    let following = state.mode == .live && state.running && picture.lineage == lineage
    return .picture(dimmed: !following, waiting: !following)
  }
}

/// The picture one frame holds, and the work under way for the next one. The
/// last picture stays until another replaces it (FigureLook says how it may be shown).
@MainActor
@Observable
final class FigurePreview {
  typealias Make = @MainActor (FigureWanted, CGRect?) async -> ModuleThumbnails.Result?
  struct Picture {
    let wanted: FigureWanted
    let lineage: String
    let result: ModuleThumbnails.Result
    var isImage: Bool {
      if case .image = result { return true }
      return false
    }
  }

  private(set) var picture: Picture?
  @ObservationIgnored private var wanted: (FigureWanted, String, Bool)?
  @ObservationIgnored private var drawing: (id: UUID, lineage: String, task: Task<Void, Never>)?
  @ObservationIgnored private var next: (wanted: FigureWanted, lineage: String, place: CGRect?)?
  @ObservationIgnored private let make: Make

  init(make: @escaping Make = { w, place in
    await ModuleThumbnails.shared.result(.init(name: w.name, json: w.json, blobs: w.blobs, width: w.width, height: w.height), json: w.json, blobs: w.blobs, at: place)
  }) {
    self.make = make
  }

  /// `follow`: the Card runs and is followed live. A revision that arrives
  /// while the one before it (same run and invocation) is being drawn waits
  /// for it, and only the newest waits: the picture keeps advancing however
  /// fast revisions arrive, where restarting for each would never finish one.
  func want(_ w: FigureWanted, lineage: String, follow: Bool, at place: CGRect? = nil) {
    if let old = wanted, old.0 == w, old.1 == lineage, old.2 == follow { return }
    wanted = (w, lineage, follow)
    guard w.width >= 8, w.height >= 8 else { stop(); return }
    if let picture, picture.isImage, picture.wanted == w, picture.lineage == lineage {
      stop()
      return
    }
    if follow, let d = drawing, d.lineage == lineage {
      next = (w, lineage, place)
      return
    }
    stop()
    draw(w, lineage: lineage, at: place)
  }

  /// The frame left the screen: nothing more is drawn for it (the picture is asked for again when it returns).
  func close() {
    stop()
    wanted = nil
    picture = nil
  }

  private func stop() {
    drawing?.task.cancel()
    drawing = nil
    next = nil
  }

  private func draw(_ w: FigureWanted, lineage: String, at place: CGRect?) {
    let id = UUID()
    let task = Task { [weak self, make] in
      let result = await make(w, place)
      // A result that arrives after this one stopped being wanted is not shown.
      guard let self, self.drawing?.id == id else { return }
      self.drawing = nil
      let next = self.next
      self.next = nil
      // A revision that failed while a newer one waits is passed over: the last picture stays.
      if let result {
        if case .failed = result, next != nil {} else { self.picture = Picture(wanted: w, lineage: lineage, result: result) }
      }
      if let next { self.draw(next.wanted, lineage: next.lineage, at: next.place) }
    }
    drawing = (id, lineage, task)
  }
}

/// A Plotly figure or a Vega-Lite chart on the canvas: a picture of it. A web
/// view per figure costs a process each and lays the page out again at every
/// zoom; on the canvas the figure is still anyway (ModuleViewer is the one in
/// the expanded view, where it is interactive).
///
/// The picture is of this result and no other: while the picture of a new
/// result is being made, the last one stays, dimmed (FigureLook), never as the new one.
struct ModuleThumbnail: View {
  let name: String
  let json: String
  var blobs: [String: Data] = [:]
  /// The run and the invocation the result is of.
  let lineage: String
  /// The canvas's zoom: the picture is taken at the display's own size and scaled with it.
  var zoom: CGFloat = 1
  /// How the figure follows the Card's results.
  var figure = FigureState()

  private struct Request: Equatable {
    let wanted: FigureWanted
    let lineage: String
    let follow: Bool
  }
  @State private var preview = FigurePreview()
  /// Where its frame is on the canvas: the pictures of what is in view are made first.
  @Environment(\.canvasPlace) private var place

  var body: some View {
    GeometryReader { geo in
      let wanted = settled(FigureWanted(name: name, json: json, blobs: blobs,
                                        width: Int((geo.size.width / max(zoom, 0.01)).rounded()),
                                        height: Int((geo.size.height / max(zoom, 0.01)).rounded())))
      let follow = figure.mode == .live && figure.running
      let held = preview.picture
      let look = FigureLook.of(wanted: wanted, lineage: lineage,
                               picture: held.map { ($0.wanted, $0.lineage, $0.isImage) }, state: figure)
      ZStack(alignment: .bottomLeading) {
        switch (look, held?.result) {
        case (.picture(let dimmed, _), .image(let image)?):
          Image(nsImage: image).resizable().interpolation(.high)
            .opacity(dimmed ? 0.4 : 1)
        case (.failed, .failed(let message)?):
          Text(message).font(.caption).foregroundStyle(.red).padding(4).textSelection(.enabled)
        default: EmptyView()
        }
        if look.isWaiting {
          ProgressView().controlSize(.small).frame(maxWidth: .infinity, maxHeight: .infinity)
        }
      }
      .frame(width: geo.size.width, height: geo.size.height)
      .background(Color.white)
      .clipShape(RoundedRectangle(cornerRadius: 4))
      .task(id: Request(wanted: wanted, lineage: lineage, follow: follow)) {
        preview.want(wanted, lineage: lineage, follow: follow, at: place)
      }
      .onDisappear { preview.close() }
    }
    .accessibilityLabel(name == "plotly" ? String(localized: "Plotly のグラフ") : String(localized: "Vega-Lite のグラフ"))
  }

  /// The frame's size in the canvas's own units wobbles by a point with the
  /// zoom (its text does not scale evenly): the picture shown is kept while
  /// the size is within a few points of its own.
  private func settled(_ w: FigureWanted) -> FigureWanted {
    guard let s = preview.picture?.wanted, s.name == w.name, s.json == w.json, s.blobs == w.blobs,
          abs(s.width - w.width) <= 6, abs(s.height - w.height) <= 6 else { return w }
    return s
  }
}

extension EnvironmentValues {
  /// Where a display frame is on its canvas (the canvas's own units; nil elsewhere).
  @Entry var canvasPlace: CGRect?
}

/// Makes the pictures of module displays, one at a time, in one web view that
/// is kept only while there is work: afterwards only the pictures remain, in
/// memory and in the app's cache folder (a project opened again shows them at
/// once; they are of the results' content, whichever project it is in).
@MainActor
final class ModuleThumbnails {
  static let shared = ModuleThumbnails()

  /// The result (by the digest of its JSON), the kind of figure and the size in points.
  struct Key: Hashable {
    let name: String
    let digest: String
    let width: Int
    let height: Int

    init(name: String, json: String, blobs: [String: Data] = [:], width: Int, height: Int) {
      self.name = name
      var content = Data(json.utf8)
      for name in blobs.keys.sorted() {
        content.append(Data("\(name.utf8.count):\(name):\(blobs[name]!.count):".utf8))
        content.append(blobs[name]!)
      }
      digest = SHA256.hash(data: content).map { String(format: "%02x", $0) }.joined()
      self.width = width
      self.height = height
    }
  }

  enum Result {
    case image(NSImage)
    case failed(String)
  }

  /// Pixels per point of the pictures: sharp on a Retina display at the canvas's own size.
  private nonisolated static let scale: CGFloat = 2

  private final class Entry {
    let result: Result
    init(_ result: Result) { self.result = result }
  }
  private final class Boxed: NSObject {
    let key: Key
    init(_ key: Key) { self.key = key }
    override var hash: Int { key.hashValue }
    override func isEqual(_ object: Any?) -> Bool { (object as? Boxed)?.key == key }
  }
  private let cache: NSCache<Boxed, Entry> = {
    let c = NSCache<Boxed, Entry>()
    c.totalCostLimit = 192 << 20
    return c
  }()

  private struct Job {
    let json: String
    var blobs: [String: Data] = [:]
    var waiting: [UUID: (answer: CheckedContinuation<Result?, Never>, place: CGRect?)] = [:]
  }
  /// What the canvases show now (the canvas's own units), by canvas: CanvasPanHost keeps it.
  var viewports: [ObjectIdentifier: CGRect] = [:]
  /// Pictures asked for and not made yet. Those of frames in view are made
  /// first, and of those the one asked for last (what is on screen now, before
  /// what scrolled past).
  private var jobs: [Key: Job] = [:]
  private var order: [Key] = []
  private var working = false
  private var web: WKWebView?
  private var release: Task<Void, Never>?

  /// The picture of a result, made if there is none yet. Nil when the task
  /// asking was cancelled: a picture nobody waits for any more is not made.
  func result(_ key: Key, json: String, blobs: [String: Data] = [:], at place: CGRect? = nil) async -> Result? {
    if let hit = cache.object(forKey: Boxed(key)) { return hit.result }
    if let image = await Self.stored(key) {
      let result = Result.image(image)
      keep(result, for: key)
      return result
    }
    if Task.isCancelled { return nil }
    let id = UUID()
    return await withTaskCancellationHandler {
      await withCheckedContinuation { (c: CheckedContinuation<Result?, Never>) in
        if Task.isCancelled { return c.resume(returning: nil) }
        jobs[key, default: Job(json: json, blobs: blobs)].waiting[id] = (c, place)
        order.removeAll { $0 == key }
        order.append(key)
        Task { await work() }
      }
    } onCancel: {
      Task { @MainActor in self.forget(id, of: key) }
    }
  }

  private func forget(_ id: UUID, of key: Key) {
    guard let c = jobs[key]?.waiting.removeValue(forKey: id) else { return }
    c.answer.resume(returning: nil)
  }

  private func work() async {
    guard !working else { return }
    working = true
    release?.cancel()
    while let key = next() {
      // Not wanted any more (a live display moved on, the frame was closed): skipped.
      guard let job = jobs[key], !job.waiting.isEmpty else { jobs[key] = nil; continue }
      let result = await make(key, json: job.json, blobs: job.blobs)
      keep(result, for: key)
      if case .image(let image) = result { Self.store(image, for: key) }
      for c in (jobs.removeValue(forKey: key)?.waiting ?? [:]).values { c.answer.resume(returning: result) }
    }
    working = false
    // The web view's process goes once nothing has been asked for a while.
    release = Task {
      try? await Task.sleep(for: .seconds(20))
      if !Task.isCancelled, !working {
        web?.evaluateJavaScript("document.querySelector('iframe')?.remove()", completionHandler: nil)
        web = nil
      }
    }
  }

  /// The next picture to make: the last asked for of those in view, else the last asked for.
  private func next() -> Key? {
    let views = Array(viewports.values)
    let inView = order.lastIndex { key in
      jobs[key]?.waiting.values.contains { w in w.place.map { p in views.contains { $0.intersects(p) } } ?? false } ?? false
    }
    guard let i = inView ?? order.indices.last else { return nil }
    return order.remove(at: i)
  }

  private func keep(_ result: Result, for key: Key) {
    if case .image(let image) = result {
      cache.setObject(Entry(result), forKey: Boxed(key), cost: Int(image.size.width * image.size.height * Self.scale * Self.scale) * 4)
    } else {
      cache.setObject(Entry(result), forKey: Boxed(key), cost: 1024)
    }
  }

  // MARK: The pictures on disk

  /// Caches/<the app>/module-thumbnails-<build>: a build draws with its own
  /// viewer and libraries, so another build's pictures are not its own.
  private nonisolated static let folder: URL? = {
    guard let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first else { return nil }
    let app = caches.appendingPathComponent(Bundle.main.bundleIdentifier ?? "app.tsugumi.Tsugumi", isDirectory: true)
    let prefix = "module-thumbnails-"
    let build = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "0"
    let folder = app.appendingPathComponent(prefix + build, isDirectory: true)
    try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    // Once a launch, behind everything else: other builds' pictures go, and the oldest of this one's beyond the limit.
    Task.detached(priority: .background) {
      let files = FileManager.default
      for other in (try? files.contentsOfDirectory(at: app, includingPropertiesForKeys: nil)) ?? []
      where other.lastPathComponent.hasPrefix(prefix) && other != folder {
        try? files.removeItem(at: other)
      }
      let keys: Set<URLResourceKey> = [.fileSizeKey, .contentModificationDateKey]
      var kept = ((try? files.contentsOfDirectory(at: folder, includingPropertiesForKeys: Array(keys))) ?? []).compactMap { url -> (URL, Int, Date)? in
        guard let v = try? url.resourceValues(forKeys: keys) else { return nil }
        return (url, v.fileSize ?? 0, v.contentModificationDate ?? .distantPast)
      }
      kept.sort { $0.2 > $1.2 }
      var total = 0
      for (url, size, _) in kept {
        total += size
        if total > diskLimit { try? files.removeItem(at: url) }
      }
    }
    return folder
  }()

  private nonisolated static let diskLimit = 256 << 20

  private nonisolated static func file(_ key: Key) -> URL? {
    folder?.appendingPathComponent("\(SHA256.hash(data: Data(key.name.utf8)).map { String(format: "%02x", $0) }.joined())-\(key.digest)-\(key.width)x\(key.height).png")
  }

  /// The picture kept from an earlier time, if there is one (read off the main thread).
  private nonisolated static func stored(_ key: Key) async -> NSImage? {
    guard let url = file(key) else { return nil }
    let size = CGSize(width: CGFloat(key.width), height: CGFloat(key.height))
    return await Task.detached(priority: .userInitiated) { () -> NSImage? in
      guard let data = try? Data(contentsOf: url), let source = CGImageSourceCreateWithData(data as CFData, nil),
            let pixels = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return nil }
      // Used again: among the last to go when the folder is full.
      try? FileManager.default.setAttributes([.modificationDate: Date()], ofItemAtPath: url.path)
      return NSImage(cgImage: pixels, size: size)
    }.value
  }

  private nonisolated static func store(_ image: NSImage, for key: Key) {
    // At the pixels it was taken at, not at one a point.
    var whole = CGRect(x: 0, y: 0, width: CGFloat(key.width) * scale, height: CGFloat(key.height) * scale)
    guard let url = file(key), let pixels = image.cgImage(forProposedRect: &whole, context: nil, hints: nil) else { return }
    Task.detached(priority: .utility) {
      guard let png = NSBitmapImageRep(cgImage: pixels).representation(using: .png, properties: [:]) else { return }
      try? png.write(to: url, options: .atomic)
    }
  }

  private func make(_ key: Key, json: String, blobs: [String: Data]) async -> Result {
    guard let directory = ModuleViewerHost.directory else { return .failed("modules") }
    let size = CGSize(width: CGFloat(key.width), height: CGFloat(key.height))
    do {
      let web = try await viewer(directory)
      // The page is laid out at the display's own size and drawn at `scale` pixels a point.
      web.frame = CGRect(x: 0, y: 0, width: size.width * Self.scale, height: size.height * Self.scale)
      web.pageZoom = Self.scale
      let code = try await ModuleSources.source(key.name)
      let message: [String: Any] = ["name": key.name, "json": json, "blobs": blobs.mapValues { $0.base64EncodedString() }, "code": code, "interactive": false]
      // Drawn, and a moment later (what the libraries leave to a timer), before the picture is taken.
      let state = try await withTimeout(seconds: 30) {
        try await web.callAsyncJavaScript(
          """
          await tsugumiRender(message)
          await new Promise((r) => setTimeout(r, 30))
          return document.body.dataset.state === 'error' ? document.body.dataset.error : ''
          """, arguments: ["message": message], contentWorld: .page) as? String
      }
      if let state, !state.isEmpty { return .failed(state) }
      let config = WKSnapshotConfiguration()
      config.rect = web.bounds
      config.afterScreenUpdates = true
      let image = try await web.takeSnapshot(configuration: config)
      image.size = size
      return .image(image)
    } catch {
      // A page that hangs or whose process died is not used again.
      web = nil
      return .failed(error.localizedDescription)
    }
  }

  /// The web view with the viewer's page loaded: as ModuleViewerHost's, it
  /// reaches nothing but the viewer's own files (a Card's plot is untrusted).
  private func viewer(_ directory: URL) async throws -> WKWebView {
    if let web { return web }
    let config = WKWebViewConfiguration()
    config.websiteDataStore = .nonPersistent()
    let rules: WKContentRuleList? = await withCheckedContinuation { c in
      ModuleViewerHost.Coordinator.withBlockedNetwork { c.resume(returning: $0) }
    }
    guard let rules else { throw TimedOut() }
    config.userContentController.add(rules)
    let made = WKWebView(frame: CGRect(x: 0, y: 0, width: 640, height: 400), configuration: config)
    made.underPageBackgroundColor = .white
    ModuleNavigation.shared.prepare(made)
    made.loadHTMLString(try ModuleViewerHost.page(directory), baseURL: URL(string: "https://localhost/"))
    // Until the page's script has run (tsugumiRender is there).
    for _ in 0..<400 {
      if let ready = try? await made.callAsyncJavaScript("return typeof window.tsugumiRender === 'function'", contentWorld: .page) as? Bool,
         ready {
        web = made
        return made
      }
      try await Task.sleep(for: .milliseconds(30))
    }
    throw TimedOut()
  }

  /// Whether a continuation has been resumed (by the answer or by the timeout, whichever came first).
  @MainActor private final class Once { var done = false }

  private struct TimedOut: LocalizedError {
    var errorDescription: String? { String(localized: "グラフの描画が終わりませんでした") }
  }

  /// The page's answer or, when it has not come in time, an error: the call
  /// into the page cannot be cancelled, so it is left behind (with its web view).
  private func withTimeout<T>(seconds: Double, _ body: @escaping @MainActor () async throws -> T) async throws -> T {
    let once = Once()
    return try await withCheckedThrowingContinuation { (c: CheckedContinuation<T, Error>) in
      let timer = Task { @MainActor in
        try? await Task.sleep(for: .seconds(seconds))
        if !once.done, !Task.isCancelled {
          once.done = true
          c.resume(throwing: TimedOut())
        }
      }
      Task { @MainActor in
        let outcome: Swift.Result<T, Error>
        do { outcome = .success(try await body()) } catch { outcome = .failure(error) }
        timer.cancel()
        if !once.done {
          once.done = true
          c.resume(with: outcome)
        }
      }
    }
  }
}
