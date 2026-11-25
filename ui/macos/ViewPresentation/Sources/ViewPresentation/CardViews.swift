import AppKit
import Observation
import SwiftUI

public enum CardViewMode: UInt32 {
  case panel, window, fullscreen
}

public struct CardViewOpening: Identifiable {
  public let id: UUID
  public let card: [String]
  public let name: String
  public let type: String
  public let mode: CardViewMode
  public let width: UInt32
  public let height: UInt32
  public let flags: UInt32

  public init(id: UUID, card: [String], name: String, type: String, mode: CardViewMode,
              width: UInt32, height: UInt32, flags: UInt32 = 0) {
    self.id = id
    self.card = card
    self.name = name
    self.type = type
    self.mode = mode
    self.width = width
    self.height = height
    self.flags = flags
  }
}

public enum CardViewInput: Sendable {
  case pointer(kind: UInt32, x: Float, y: Float, buttons: UInt32, wheelX: Float, wheelY: Float, modifiers: UInt32)
  case key(kind: UInt32, code: UInt32, modifiers: UInt32)
  case text(String)
  case gamepad(pad: UInt32, kind: UInt32, buttons: UInt32, lx: Float, ly: Float, rx: Float, ry: Float, lt: Float, rt: Float)
}

/// What a view host passes on to its view, in the order the core sent it.
enum CardViewMessage: Equatable {
  /// Opaque bytes the Card sent to this view.
  case data(Data)
}

/// Messages waiting for a view that is still busy with an earlier one.
struct CardViewOutbox {
  private var deliveries: [(CardViewMessage, () -> Void)] = []
  var pending: [CardViewMessage] { deliveries.map { $0.0 } }

  mutating func put(_ message: CardViewMessage, complete: @escaping () -> Void = {}) {
    deliveries.append((message, complete))
  }

  mutating func take() -> (CardViewMessage, () -> Void)? {
    deliveries.isEmpty ? nil : deliveries.removeFirst()
  }

  mutating func discard() {
    let pending = deliveries
    deliveries.removeAll()
    for (_, complete) in pending { complete() }
  }

}

public struct CardViewFilePage {
  let bytes: Data
  let eof: Bool
  let nextOffset: UInt64?

  public init(bytes: Data, eof: Bool, nextOffset: UInt64?) {
    self.bytes = bytes
    self.eof = eof
    self.nextOffset = nextOffset
  }
}

public struct CardViewFiles {
  static let limit = 32 * 1024 * 1024
  let page: @MainActor (_ session: UUID, _ offset: UInt64) async throws -> CardViewFilePage

  public init(page: @escaping @MainActor (UUID, UInt64) async throws -> CardViewFilePage) {
    self.page = page
  }

  @MainActor func read(session: UUID) async throws -> Data {
    var data = Data(), offset: UInt64 = 0
    while true {
      try Task.checkCancellation()
      let page = try await page(session, offset)
      guard data.count + page.bytes.count <= Self.limit else { throw CardViewFileError.tooLarge }
      data.append(page.bytes)
      if page.eof { return data }
      guard let next = page.nextOffset, next > offset else { throw CardViewFileError.malformed }
      offset = next
    }
  }

  public static let unavailable = CardViewFiles { _, _ in throw CardViewFileError.unavailable }
}

enum CardViewFileError: LocalizedError {
  case unavailable, tooLarge, malformed, notText

  var errorDescription: String? {
    switch self {
    case .unavailable: String(localized: "この接続では表示のファイルを読めません")
    case .tooLarge: String(localized: "表示のファイルが 32 MiB を超えています")
    case .malformed: String(localized: "表示のファイルを読めません")
    case .notText: String(localized: "HTML 表示のファイルが UTF-8 ではありません")
    }
  }
}

/// Holds the renderer and its queued messages for the lifetime of one open session.
@MainActor @Observable final class OpenCardView {
  let opening: CardViewOpening
  let requestClose: @MainActor () async throws -> Void
  var html: HTMLViewHost?
  var dawn: DawnViewHost?
  var error: String?
  @ObservationIgnored var recipients: Set<String> = []
  @ObservationIgnored private var outbox = CardViewOutbox()
  @ObservationIgnored private var load: Task<Void, Never>?
  @ObservationIgnored var window: NSWindow?
  @ObservationIgnored var windowDelegate: CardViewWindowDelegate?
  @ObservationIgnored private var hosting: NSView?
  private var closed = false

  var content: NSView {
    if let hosting { return hosting }
    let view = NSHostingView(rootView: CardViewContent(view: self))
    hosting = view
    return view
  }

  init(_ opening: CardViewOpening, files: CardViewFiles,
       testClock: DawnTestClock?,
       send: @escaping @MainActor (String, Data) async throws -> Void,
       close: @escaping @MainActor () async throws -> Void) {
    self.opening = opening
    requestClose = close
    load = Task { [weak self] in
      do {
        guard opening.type == "html" || opening.type == "dawn" else {
          self?.error = String(localized: "表示の種類「\(opening.type)」には対応していません")
          self?.outbox.discard()
          return
        }
        let file = try await files.read(session: opening.id)
        try Task.checkCancellation()
        guard let self else { return }
        if opening.type == "dawn" {
          dawn = DawnViewHost(wasm: file, flags: opening.flags, recipients: recipients,
                              testClock: testClock, sendToCard: send)
        } else {
          guard let source = String(data: file, encoding: .utf8) else { throw CardViewFileError.notText }
          let host = HTMLViewHost(sendToCard: send)
          while let (message, complete) = outbox.take() { host.send(message, complete: complete) }
          html = host
          try await host.mount(source)
        }
        while let (message, complete) = outbox.take() {
          receive(message, complete: complete)
        }
      } catch {
        guard let self, !Task.isCancelled else { return }
        self.error = error.localizedDescription
        html?.close()
        outbox.discard()
      }
    }
  }

  func receive(_ message: CardViewMessage, complete: @escaping () -> Void) {
    if closed || error != nil { complete() }
    else if let dawn { dawn.send(message, complete: complete) }
    else if let html { html.send(message, complete: complete) }
    else { outbox.put(message, complete: complete) }
  }

  func tearDown() {
    closed = true
    load?.cancel()
    load = nil
    html?.close()
    dawn?.close()
    outbox.discard()
    window?.delegate = nil
    window?.contentView = nil
    window = nil
    windowDelegate = nil
    hosting?.removeFromSuperview()
    hosting = nil
  }
}

@MainActor private struct CardViewContent: View {
  weak var view: OpenCardView?

  var body: some View {
    if let view {
      VStack(spacing: 0) {
        if let dawn = view.dawn {
          DawnViewRepresentable(host: dawn)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .overlay {
              if dawn.waiting {
                ZStack {
                  Color.black.opacity(0.4)
                  VStack(spacing: 12) {
                    Text("ビューの応答を待っています")
                    Button("停止") { dawn.stop() }
                  }
                  .padding(20)
                  .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
                }
              }
            }
        } else if let html = view.html {
          HTMLViewRepresentable(host: html).frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if view.error == nil {
          ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        if let error = view.error ?? view.html?.error ?? view.dawn?.error {
          Text(verbatim: error).foregroundStyle(.red).textSelection(.enabled).padding(8)
        }
      }
    }
  }
}

private struct HTMLViewRepresentable: NSViewRepresentable {
  let host: HTMLViewHost
  func makeNSView(context: Context) -> NSView { host.web }
  func updateNSView(_ view: NSView, context: Context) {}
}

@MainActor final class CardViewWindowDelegate: NSObject, NSWindowDelegate {
  let willClose: () -> Void
  init(willClose: @escaping () -> Void) { self.willClose = willClose }
  func windowWillClose(_ notification: Notification) { willClose() }
}

@MainActor final class DawnTestClock {
  private(set) var milliseconds: UInt64 = 0
  var seconds: Double { Double(milliseconds) / 1000 }

  func advance(by delta: UInt64) -> Bool {
    let next = milliseconds.addingReportingOverflow(delta)
    // Keep millisecond increments representable in Double seconds.
    guard !next.overflow, next.partialValue <= 1 << 52 else { return false }
    milliseconds = next.partialValue
    return true
  }
}

/// Routes each display to the renderer for that session. Route lifetime lives in the project.
@MainActor @Observable public final class CardViewPresentation {
  private var views: [UUID: OpenCardView] = [:]
  private var panelIDs: [UUID] = []
  private var pendingCloses = 0
  @ObservationIgnored private var packedMainWindow: NSWindow?
  private let packedTitle: String?
  private let testClock: DawnTestClock?

  public init(packedTitle: String? = nil, debugClock: Bool = false) {
    self.packedTitle = packedTitle
    self.testClock = debugClock ? DawnTestClock() : nil
  }

  public var hasPanels: Bool { !panelIDs.isEmpty }
  public var isEmpty: Bool { views.isEmpty }
  public var isIdle: Bool { isEmpty && pendingCloses == 0 }
  public var openings: [CardViewOpening] { views.values.map(\.opening) }

  public func open(_ opening: CardViewOpening, files: CardViewFiles,
                   send: @escaping @MainActor (String, Data) async throws -> Void,
                   close: @escaping @MainActor () async throws -> Void) {
    guard views[opening.id] == nil else { return }
    let view = OpenCardView(opening, files: files, testClock: testClock, send: send, close: close)
    views[opening.id] = view
    switch opening.mode {
    case .panel:
      panelIDs.append(opening.id)
      if let packedTitle, packedMainWindow == nil { openPackedMainWindow(title: packedTitle) }
    case .window, .fullscreen:
      showWindow(view, fullscreen: opening.mode == .fullscreen)
    }
  }

  public func setRecipient(_ card: String, registered: Bool, session: UUID) {
    guard let view = views[session] else { return }
    if registered { view.recipients.insert(card) }
    else { view.recipients.remove(card) }
    view.dawn?.setRecipient(card, registered: registered)
  }

  public func receive(_ session: UUID, bytes: Data, complete: @escaping () -> Void = {}) {
    guard let view = views[session] else { complete(); return }
    view.receive(.data(bytes), complete: complete)
  }

  public func sendInput(_ input: CardViewInput, session: UUID) async throws {
    guard let dawn = views[session]?.dawn else { throw DawnViewError.unavailable }
    try await dawn.sendInput(input)
  }

  public func audioRequested(session: UUID) -> Bool {
    views[session]?.dawn?.audioRequested == true
  }

  /// Advances Dawn presentation time and queues a frame for each open view.
  public func stepClock(milliseconds: UInt64) -> UInt64? {
    guard let testClock, testClock.advance(by: milliseconds) else { return nil }
    let at = testClock.seconds
    for view in views.values { view.dawn?.drawTestFrame(at: at) }
    return testClock.milliseconds
  }

  public func snapshot(session: UUID) async throws -> Data {
    guard let view = views[session] else { throw DawnViewError.unavailable }
    if let dawn = view.dawn { return try await dawn.snapshot() }
    if let html = view.html { return try await html.snapshot() }
    throw DawnViewError.unavailable
  }

  public func close(_ id: UUID) { close(id, windowIsClosing: false) }

  @discardableResult
  public func sessionClosed(_ id: UUID) -> CardViewOpening? {
    close(id, windowIsClosing: false, notifyCore: false)
  }

  @discardableResult
  private func close(_ id: UUID, windowIsClosing: Bool, notifyCore: Bool = true) -> CardViewOpening? {
    guard let view = views.removeValue(forKey: id) else { return nil }
    panelIDs.removeAll { $0 == id }
    let window = view.window
    view.tearDown()
    if !windowIsClosing { window?.close() }
    closeEmptyPackedWindow()
    guard notifyCore else { return view.opening }
    pendingCloses += 1
    Task { [request = view.requestClose] in
      defer { pendingCloses -= 1 }
      try? await request()
    }
    return view.opening
  }

  public func closeAll() {
    for id in Array(views.keys) { close(id) }
  }

  private func showWindow(_ view: OpenCardView, fullscreen: Bool = false) {
    let opening = view.opening
    let width = opening.width == 0 ? 900 : CGFloat(opening.width)
    let height = opening.height == 0 ? 600 : CGFloat(opening.height)
    let window = NSWindow(contentRect: CGRect(x: 0, y: 0, width: width, height: height),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
    window.isReleasedWhenClosed = false
    window.title = opening.name
    window.subtitle = opening.card.joined(separator: "/")
    window.collectionBehavior.insert(.fullScreenPrimary)
    view.content.removeFromSuperview()
    window.contentView = view.content
    window.setContentSize(NSSize(width: width, height: height))
    let delegate = CardViewWindowDelegate { [weak self] in
      self?.close(opening.id, windowIsClosing: true)
    }
    window.delegate = delegate
    view.windowDelegate = delegate
    view.window = window
    window.center()
    window.makeKeyAndOrderFront(nil)
    if fullscreen { window.toggleFullScreen(nil) }
  }

  @ObservationIgnored private var packedWindowDelegate: CardViewWindowDelegate?
  private func openPackedMainWindow(title: String) {
    let window = NSWindow(contentRect: CGRect(x: 0, y: 0, width: 900, height: 600),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
    window.isReleasedWhenClosed = false
    window.title = title
    window.contentView = NSHostingView(rootView: CardViewPanels(presentation: self) { EmptyView() })
    let delegate = CardViewWindowDelegate { [weak self] in
      guard let self else { return }
      packedMainWindow = nil
      packedWindowDelegate = nil
      for id in panelIDs { close(id) }
    }
    window.delegate = delegate
    packedWindowDelegate = delegate
    packedMainWindow = window
    window.center()
    window.makeKeyAndOrderFront(nil)
  }

  private func closeEmptyPackedWindow() {
    guard panelIDs.isEmpty, let window = packedMainWindow else { return }
    packedMainWindow = nil
    window.delegate = nil
    packedWindowDelegate = nil
    window.contentView = nil
    window.close()
  }

  fileprivate var panels: [OpenCardView] { panelIDs.compactMap { views[$0] } }

  fileprivate func detach(_ view: OpenCardView) {
    panelIDs.removeAll { $0 == view.opening.id }
    showWindow(view)
    closeEmptyPackedWindow()
  }
}

public struct CardViewPanels<Header: View>: View {
  let presentation: CardViewPresentation
  let showsLatest: Bool
  @ViewBuilder let header: () -> Header
  @State private var selected: UUID?

  public init(presentation: CardViewPresentation, showsLatest: Bool = false, @ViewBuilder header: @escaping () -> Header) {
    self.presentation = presentation
    self.showsLatest = showsLatest
    self.header = header
  }

  public var body: some View {
    let panels = presentation.panels
    let shown = panels.first { $0.opening.id == selected } ?? (showsLatest ? panels.last : panels.first)
    VStack(spacing: 0) {
      if let shown {
        HStack {
          header()
          Picker("表示の切り替え", selection: Binding(get: { shown.opening.id }, set: { selected = $0 })) {
            ForEach(panels, id: \.opening.id) { view in
              Text(verbatim: "\(view.opening.card.joined(separator: "/")) / \(view.opening.name)")
                .tag(view.opening.id)
            }
          }
          .labelsHidden()
          Spacer()
          Button { presentation.detach(shown) } label: { Image(systemName: "macwindow") }
            .help("別ウインドウで開く")
          Button { presentation.close(shown.opening.id) } label: { Image(systemName: "xmark") }
            .help("閉じる")
        }
        .padding(8)
        ZStack {
          ForEach(panels, id: \.opening.id) { view in
            PanelContent(view: view)
              .opacity(view === shown ? 1 : 0)
              .allowsHitTesting(view === shown)
              .accessibilityHidden(view !== shown)
          }
        }
      }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
  }
}

private struct PanelContent: NSViewRepresentable {
  let view: OpenCardView

  func makeNSView(context: Context) -> NSView {
    let container = NSView()
    let content = view.content
    content.frame = container.bounds
    content.autoresizingMask = [.width, .height]
    container.addSubview(content)
    return container
  }

  func updateNSView(_ container: NSView, context: Context) {}
}
