import AppKit
import SwiftUI
import TsugumiRPC

/// The native text view receives window changes directly. SwiftUI observes
/// only the conditions it needs for controls and status.
@MainActor @Observable final class LogStreamReader {
  struct Source: Hashable {
    let runId: String
    let card: [String]?
    let invocation: UInt64?
    let stream: Tsugumi_Data_LogStream
    let live: Bool
    let recordingRevision: Int
  }

  private(set) var loaded = false
  private(set) var hasBytes = false
  private(set) var waiting = false
  private(set) var truncatedAt: UInt64?
  private(set) var error: String?

  @ObservationIgnored private(set) var window: LogWindow
  @ObservationIgnored private var source: Source?
  @ObservationIgnored private weak var session: ProjectSession?
  @ObservationIgnored private var unsubscribe: (() -> Void)?
  struct Completed {
    let kind: LogWindow.Kind
    let read: LogWindow.Read
    let result: Result<LogPage, Error>
  }

  @ObservationIgnored private var frames = LogReadFrames<Completed>()
  @ObservationIgnored private var demand: LogWindow.Kind?
  @ObservationIgnored private var changed = false
  @ObservationIgnored private var generation = 0
  @ObservationIgnored private var attached = false
  @ObservationIgnored var onText: (() -> Void)?
  @ObservationIgnored var onFrameNeeded: (() -> Void)?

  init(form: LogForm = .terminal) { window = LogWindow(form: form) }

  var size: UInt64 { window.rawSize }
  var needsFrame: Bool { frames.needsFrame }

  func show(_ next: Source?, form: LogForm = .terminal, session: ProjectSession) {
    if self.session !== session {
      unsubscribe?()
      unsubscribe = nil
      self.session = session
      subscribe()
    }
    guard next != source || form != window.form else { update(); return }
    source = next
    generation += 1
    frames = LogReadFrames()
    window = LogWindow(form: form)
    changed = false
    demand = next == nil ? nil : .tail
    error = nil
    publish()
    update()
  }

  func attach() {
    attached = true
    subscribe()
    update()
  }

  func detach() {
    attached = false
    unsubscribe?()
    unsubscribe = nil
    onText = nil
    onFrameNeeded = nil
  }

  private func subscribe() {
    guard attached, unsubscribe == nil else { return }
    unsubscribe = session?.subscribeLogs { [weak self] notice in self?.available(notice) }
  }

  private func available(_ notice: Tsugumi_RPC_LogAvailableT?) {
    guard let source else { return }
    if let notice {
      guard notice.live == source.live, notice.runId == source.runId, notice.card?.path == source.card,
            source.invocation == nil || notice.invocation == source.invocation,
            notice.endOffset == nil || notice.stream == source.stream else { return }
      if let end = notice.endOffset, notice.invocation == source.invocation {
        guard end > window.rawSize else { return }
        window.rawSize = end
        if !window.following {
          wake()
          return
        }
      }
    }
    update()
  }

  func update() {
    guard source != nil else { return }
    changed = true
    wake()
  }

  private func wake() {
    frames.wake()
    onFrameNeeded?()
  }

  func follow(_ following: Bool) {
    guard window.following != following else { return }
    window.follow(following)
    publish()
    if following && (window.end < window.size || window.hasUnread) { update() }
  }

  func keepVisible(_ rows: ClosedRange<LogRowID>?) { window.visible = rows }

  func page(_ kind: LogWindow.Kind) {
    guard demand != .tail, window.request(kind) != nil else { return }
    demand = kind
    wake()
  }

  func jump() {
    window.follow(true)
    publish()
    demand = .tail
    wake()
  }

  func frame() {
    if let completed = frames.takeResult() { apply(completed) }
    guard frames.begin() else { return }
    let kind = demand ?? (changed ? .status : nil)
    demand = nil
    changed = false
    guard let source, let rpc = session?.rpc, let kind, let read = window.request(kind) else {
      frames.finish()
      publish(draw: false)
      return
    }
    let form = window.form
    let current = generation
    Task { [weak self] in
      let completed: Result<LogPage, Error>
      do {
        let before = read.kind == .earlier ? read.offset : nil
        let result = try await rpc.logRange(runId: source.runId, card: source.card, invocation: source.invocation,
                                            stream: source.stream, offset: before == nil ? read.offset : 0,
                                            tail: read.kind == .tail, limit: read.limit, before: before,
                                            terminal: form == .terminal, terminalSince: read.since, live: source.live)
        let raw = result.text, offset = result.offset ?? 0, end = result.nextOffset ?? offset
        let size = result.sizeBytes ?? end, line = result.lineNumber, startsLine = result.startsLine
        let truncated = result.logTruncatedAtBytes, rawSize = result.rawSizeBytes, reset = result.reset
        completed = await .success(Task.detached(priority: .userInitiated) {
          LogPage(offset: offset, end: end, size: size, truncatedAt: truncated, line: line,
                  startsLine: startsLine, text: raw, form: form, rawSize: rawSize, reset: reset)
        }.value)
      } catch {
        completed = .failure(error)
      }
      guard let self, generation == current else { return }
      frames.finish(Completed(kind: kind, read: read, result: completed))
      onFrameNeeded?()
    }
  }

  func apply(_ completed: Completed) {
    switch completed.result {
    case .success(let slice):
      let read = completed.read, kind = completed.kind
      let paused = !window.following && (read.kind == .tail || read.kind == .append)
      let superseded = (kind == .earlier && (window.following || demand == .later || demand == .tail))
        || (kind == .later && (demand == .earlier || demand == .tail))
      let previous = window.blocks
      window.apply(read, slice, metadataOnly: paused || superseded)
      if error != nil { error = nil }
      if window.following && (window.end < window.size || window.hasUnread) { update() }
      if !superseded, (kind == .earlier || kind == .later), demand == nil, window.blocks == previous { page(kind) }
    case .failure(let failure):
      if let rpc = failure as? RPCFailure, rpc.kind == .notFound, !window.loaded {
        error = nil
      } else {
        error = "\(failure)"
      }
      window.loaded = true
    }
    publish()
  }

  private func publish(draw: Bool = true) {
    if loaded != window.loaded { loaded = window.loaded }
    if hasBytes != (window.rawSize > 0) { hasBytes = window.rawSize > 0 }
    if waiting != window.waiting { waiting = window.waiting }
    if truncatedAt != window.truncatedAt { truncatedAt = window.truncatedAt }
    if draw { onText?() }
  }
}
private enum LogExportError: LocalizedError {
  case disconnected, incomplete

  var errorDescription: String? {
    switch self {
    case .disconnected: String(localized: "実行環境に接続していません")
    case .incomplete: String(localized: "ログを読み込めませんでした")
    }
  }
}

/// Writes a whole stream to a file the reader picks, in pieces.
@MainActor func exportLog(_ session: ProjectSession, _ source: LogStreamReader.Source, size: UInt64) async throws {
  guard let rpc = session.rpc else { throw LogExportError.disconnected }
  let panel = NSSavePanel()
  panel.nameFieldStringValue = "\(source.runId)-\(source.stream).log"
  guard await panel.begin() == .OK, let url = panel.url else { return }
  let temporary = try FileManager.default.url(for: .itemReplacementDirectory, in: .userDomainMask,
                                               appropriateFor: url, create: true)
  defer { try? FileManager.default.removeItem(at: temporary) }
  let file = temporary.appendingPathComponent("log")
  try Data().write(to: file)
  let output = try FileHandle(forWritingTo: file)
  do {
    defer { try? output.close() }
    var offset: UInt64 = 0
    while offset < size {
      let slice = try await rpc.logRange(runId: source.runId, card: source.card, invocation: source.invocation,
                                         stream: source.stream, offset: offset, limit: LogWindow.readBytes, live: source.live)
      guard let next = slice.nextOffset, next > offset else { throw LogExportError.incomplete }
      try output.write(contentsOf: Data(slice.text.utf8.prefix(Int(min(next, size) - offset))))
      offset = next
    }
  }
  if FileManager.default.fileExists(atPath: url.path) {
    _ = try FileManager.default.replaceItemAt(url, withItemAt: file)
  } else {
    try FileManager.default.moveItem(at: file, to: url)
  }
}

/// Wraps the bounded log in a selectable, non-contiguously laid out text view.
struct LogTextView: View {
  let reader: LogStreamReader
  var placeholder = ""

  var body: some View {
    LogScrollView(reader: reader, placeholder: placeholder, fontSize: AppPreferences.shared.editorFontSize - 1)
      .overlay(alignment: .bottomTrailing) {
        if reader.waiting {
          Button("新しい出力 · 末尾へ") { reader.jump() }.controlSize(.small).padding(8)
        }
      }
  }
}

final class LogScroll: NSScrollView {
  var wheel: ((CGFloat) -> Void)?

  override func scrollWheel(with event: NSEvent) {
    super.scrollWheel(with: event)
    wheel?(-event.scrollingDeltaY)
  }

  static func make() -> LogScroll {
    let scroll = LogScroll()
    let text = NSTextView(usingTextLayoutManager: false)
    text.frame = scroll.contentView.bounds
    text.textContainer?.containerSize = NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude)
    text.textContainer?.widthTracksTextView = true
    text.textContainer?.lineBreakMode = .byCharWrapping
    text.layoutManager?.allowsNonContiguousLayout = true
    text.layoutManager?.backgroundLayoutEnabled = false
    text.isEditable = false
    text.isSelectable = true
    text.isRichText = false
    text.drawsBackground = false
    text.textContainerInset = NSSize(width: 4, height: 4)
    text.isHorizontallyResizable = false
    text.isVerticallyResizable = true
    text.autoresizingMask = [.width]
    text.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
    text.setAccessibilityIdentifier("log.text")
    scroll.documentView = text
    scroll.hasVerticalScroller = true
    scroll.hasHorizontalScroller = false
    scroll.autohidesScrollers = true
    scroll.drawsBackground = false
    scroll.contentView.postsBoundsChangedNotifications = true
    return scroll
  }

  static func attributes(font: NSFont) -> [NSAttributedString.Key: Any] {
    let indent = ceil(("999999999" as NSString).size(withAttributes: [.font: font]).width) + 12
    let paragraph = NSMutableParagraphStyle()
    paragraph.lineBreakMode = .byCharWrapping
    paragraph.headIndent = indent
    paragraph.tabStops = [NSTextTab(textAlignment: .left, location: indent)]
    return [.font: font, .foregroundColor: NSColor.labelColor, .paragraphStyle: paragraph]
  }
}

struct LogScrollView: NSViewRepresentable {
  let reader: LogStreamReader
  let placeholder: String
  let fontSize: CGFloat

  @MainActor final class Coordinator: NSObject {
    weak var scroll: LogScroll?
    weak var reader: LogStreamReader?
    var link: CADisplayLink?
    var placeholder = ""
    var fontSize: CGFloat = 0
    private var shown: [LogBlock] = []
    private var shownPlaceholder: String?
    private var shownFont: NSFont?
    private var following = false
    private var applying = false
    private var origin: CGFloat = 0

    func attach(_ next: LogStreamReader) {
      guard reader !== next else { return }
      reader?.detach()
      following = false
      reader = next
      next.onText = { [weak self] in self?.apply() }
      next.onFrameNeeded = { [weak self] in self?.updateLink() }
      next.attach()
      updateLink()
    }

    @objc func tick(_ link: CADisplayLink) {
      reader?.frame()
      updateLink()
    }

    private func updateLink() { link?.isPaused = reader?.needsFrame != true }

    @objc func scrolled(_ notification: Notification) {
      guard let scroll, !applying else { return }
      let y = scroll.contentView.bounds.minY
      let movement = y - origin
      origin = y
      if movement < 0 { reader?.follow(false) }
      nearEdge(movement)
    }

    func nearEdge(_ direction: CGFloat) {
      guard let scroll, let reader, let view = scroll.documentView, !applying else { return }
      let visible = scroll.documentVisibleRect
      if (!reader.window.following || direction < 0), let text = view as? NSTextView,
         let upper = anchor(at: visible.minY, in: text),
         let lower = anchor(at: visible.maxY, in: text) {
        reader.keepVisible(upper.row...max(upper.row, lower.row))
      }
      if direction < 0, visible.minY <= 12, reader.window.start > 0 {
        reader.follow(false)
        reader.page(.earlier)
      } else if direction > 0, visible.maxY >= view.bounds.maxY - 12 {
        if reader.window.end < reader.window.size { reader.page(.later) }
        else { reader.follow(true) }
      }
    }

    private func anchor(at y: CGFloat, in text: NSTextView) -> LogAnchor? {
      guard !shown.isEmpty, let layout = text.layoutManager, let container = text.textContainer,
            let storage = text.textStorage, storage.length > 0 else { return nil }
      let glyph = layout.glyphIndex(for: CGPoint(x: 0, y: max(0, y - text.textContainerOrigin.y)), in: container)
      return LogAnchor.at(min(storage.length - 1, layout.characterIndexForGlyph(at: glyph)), in: shown)
    }

    func apply() {
      guard let scroll, let reader, let text = scroll.documentView as? NSTextView,
            let storage = text.textStorage, let layout = text.layoutManager,
            let container = text.textContainer else { return }
      let blocks = reader.window.blocks
      let font = NSFont.monospacedSystemFont(ofSize: fontSize, weight: .regular)
      let empty = blocks.isEmpty
      let message = empty ? placeholder : nil
      let changed = shown != blocks || shownFont != font || shownPlaceholder != message
      let scrollToEnd = reader.window.following && (changed || !following) && !empty
      following = reader.window.following
      guard changed || scrollToEnd else { return }

      applying = true
      defer {
        origin = scroll.contentView.bounds.minY
        applying = false
      }
      if !changed {
        text.scrollRangeToVisible(NSRange(location: storage.length, length: 0))
        return
      }

      let edit = LogBlockEdit(from: shown, to: blocks)
      let inset = text.textContainerOrigin
      let bounds = scroll.documentVisibleRect
      var anchor: (position: LogAnchor, inset: CGFloat)?
      if !following, !shown.isEmpty, storage.length > 0 {
        let glyph = layout.glyphIndex(for: CGPoint(x: 0, y: max(0, bounds.minY - inset.y)), in: container)
        let line = layout.lineFragmentRect(forGlyphAt: glyph, effectiveRange: nil)
        if let position = LogAnchor.at(layout.characterIndexForGlyph(at: glyph), in: shown) {
          anchor = (position, bounds.minY - (line.minY + inset.y))
        }
      }

      let attributes = LogScroll.attributes(font: font)
      storage.beginEditing()
      if let message {
        storage.setAttributedString(NSAttributedString(string: message, attributes: [
          .font: NSFont.systemFont(ofSize: fontSize), .foregroundColor: NSColor.secondaryLabelColor,
        ]))
      } else if edit.shared == 0 || shownFont != font || shownPlaceholder != nil {
        storage.setAttributedString(NSAttributedString(string: blocks.map(\.text).joined(), attributes: attributes))
      } else {
        func replace(_ range: NSRange, _ text: String) {
          if range.length == 0 && text.isEmpty { return }
          storage.replaceCharacters(in: range, with: NSAttributedString(string: text, attributes: attributes))
        }
        let oldSuffix = edit.oldPrefix + edit.shared
        replace(NSRange(location: oldSuffix, length: edit.oldLength - oldSuffix),
                blocks[edit.retained.upperBound...].map(\.text).joined())
        replace(NSRange(location: 0, length: edit.oldPrefix),
                blocks[..<edit.retained.lowerBound].map(\.text).joined())
      }
      storage.endEditing()
      shown = blocks
      shownFont = font
      shownPlaceholder = message
      if scrollToEnd {
        text.scrollRangeToVisible(NSRange(location: storage.length, length: 0))
      } else if let anchor, let character = anchor.position.character(in: blocks) {
        layout.ensureLayout(forCharacterRange: NSRange(location: character, length: 1))
        let glyph = layout.glyphIndexForCharacter(at: character)
        let line = layout.lineFragmentRect(forGlyphAt: glyph, effectiveRange: nil)
        var rect = scroll.contentView.bounds
        rect.origin.y = line.minY + text.textContainerOrigin.y + anchor.inset
        scroll.contentView.scroll(to: scroll.contentView.constrainBoundsRect(rect).origin)
        scroll.reflectScrolledClipView(scroll.contentView)
      }
    }
  }

  func makeCoordinator() -> Coordinator { Coordinator() }

  func makeNSView(context: Context) -> LogScroll {
    let scroll = LogScroll.make()
    let coordinator = context.coordinator
    coordinator.scroll = scroll
    scroll.wheel = { [weak coordinator] in coordinator?.nearEdge($0) }
    NotificationCenter.default.addObserver(coordinator, selector: #selector(Coordinator.scrolled),
                                           name: NSView.boundsDidChangeNotification, object: scroll.contentView)
    let link = scroll.displayLink(target: coordinator, selector: #selector(Coordinator.tick))
    link.isPaused = true
    link.add(to: .main, forMode: .common)
    coordinator.link = link
    return scroll
  }

  func updateNSView(_ scroll: LogScroll, context: Context) {
    let coordinator = context.coordinator
    coordinator.fontSize = fontSize
    coordinator.placeholder = placeholder
    coordinator.attach(reader)
    coordinator.apply()
  }

  static func dismantleNSView(_ scroll: LogScroll, coordinator: Coordinator) {
    NotificationCenter.default.removeObserver(coordinator)
    coordinator.link?.invalidate()
    coordinator.reader?.detach()
  }
}
