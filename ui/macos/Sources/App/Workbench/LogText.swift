import AppKit
import SwiftUI
import TsugumiRPC

/// What a log view draws, with how much of it is the same as the previous
/// revision, so the text view replaces only the end (new output, a progress
/// line redrawn) instead of laying out the whole log again.
struct LogText: Equatable {
  var text = ""
  /// UTF-16 length of the prefix the previous revision already had (-1: none).
  var kept = 0
  var revision = 0
}

/// One stream of a run's log (or of one Card invocation), read through
/// LogRange: its end first, then forward from where the last read stopped,
/// each time LogAvailable says there is more. Reads never overlap; a
/// notification during a read asks for one more read after it. Parsing is
/// done off the main thread, and only what is new.
///
/// The text goes straight to the text view (`onText`), not through SwiftUI:
/// a view that showed it would be evaluated again for every piece of output.
/// What SwiftUI observes changes rarely (empty or not, clipped, truncated).
@MainActor @Observable final class LogStreamReader {
  struct Source: Hashable {
    let runId: String
    let card: [String]?
    let invocation: UInt64?
    let stream: Tsugumi_Data_LogStream
  }

  /// How the bytes are shown.
  enum Form: Hashable, Sendable {
    /// As a terminal shows them (a carriage return redraws its line).
    case terminal
    /// Journal lines (JSON) of these phases, as "phase · event: message".
    case journal([String])
  }

  /// Bytes kept for drawing: the end of a longer log (export writes all of it).
  static let shownBytes: UInt32 = 1 << 20
  private static let chunk: UInt32 = 256 * 1024

  let form: Form
  private(set) var source: Source?
  /// Read at least once (or failed).
  private(set) var loaded = false
  /// There is text to show.
  private(set) var hasText = false
  /// Bytes before the shown part are not drawn.
  private(set) var clipped = false
  private(set) var truncatedAt: UInt64?
  private(set) var error: String?

  /// What the text view draws; it is told of each change through onText.
  @ObservationIgnored private(set) var shown = LogText()
  @ObservationIgnored var onText: ((LogText) -> Void)?
  /// The stream's size in bytes, as of the last read.
  @ObservationIgnored private(set) var size: UInt64 = 0

  @ObservationIgnored private var end: UInt64 = 0
  /// Shown text of the whole lines read so far, and the raw rest of the last line.
  @ObservationIgnored private var lines = ""
  @ObservationIgnored private var linesLength = 0
  @ObservationIgnored private var partial = ""
  @ObservationIgnored private var reading = false
  @ObservationIgnored private var again = false
  @ObservationIgnored private var generation = 0
  @ObservationIgnored private weak var followed: ProjectSession?

  init(form: Form = .terminal) { self.form = form }

  /// Shows another stream (or the same one again from its end).
  func show(_ next: Source?, session: ProjectSession) {
    follow(session)
    guard next != source else { update(session); return }
    source = next
    generation += 1
    end = 0
    size = 0
    lines = ""; linesLength = 0; partial = ""
    set(LogText(revision: shown.revision + 1))
    set(\.loaded, false); set(\.clipped, false); set(\.truncatedAt, nil); set(\.error, nil)
    reading = false; again = false
    update(session)
  }

  /// There may be more to read (LogAvailable, a state change).
  func update(_ session: ProjectSession) {
    guard source != nil else { return }
    if reading { again = true; return }
    reading = true
    let current = generation
    Task { @MainActor in
      repeat {
        again = false
        await read(session, generation: current)
      } while again && generation == current
      if generation == current { reading = false }
    }
  }

  /// Reads again whenever the session says new log bytes are available.
  private func follow(_ session: ProjectSession) {
    guard followed !== session else { return }
    followed = session
    observe(session)
  }

  private func observe(_ session: ProjectSession) {
    withObservationTracking { _ = session.logRevision } onChange: { [weak self, weak session] in
      Task { @MainActor in
        guard let self, let session, self.followed === session else { return }
        self.observe(session)
        self.update(session)
      }
    }
  }

  /// Observers hear of a property only when its value changes.
  private func set<T: Equatable>(_ key: ReferenceWritableKeyPath<LogStreamReader, T>, _ value: T) {
    if self[keyPath: key] != value { self[keyPath: key] = value }
  }

  private func set(_ text: LogText) {
    shown = text
    set(\.hasText, !text.text.isEmpty)
    onText?(text)
  }

  private func read(_ session: ProjectSession, generation current: Int) async {
    guard let source, let rpc = session.rpc else { return }
    do {
      while true {
        let first = !loaded
        let slice = try await rpc.logRange(runId: source.runId, card: source.card, invocation: source.invocation,
                                           stream: source.stream, offset: end, tail: first,
                                           limit: first ? Self.shownBytes : Self.chunk)
        guard generation == current else { return }
        if first { set(\.clipped, (slice.offset ?? 0) > 0) }
        if let t = slice.logTruncatedAtBytes { set(\.truncatedAt, t) }
        let next = slice.nextOffset ?? end
        size = slice.sizeBytes ?? next
        set(\.error, nil)
        set(\.loaded, true)
        guard next > end || first else { return }
        end = next
        await append(slice.text, generation: current)
        guard generation == current, end < size else { return }
      }
    } catch let f as RPCFailure where f.kind == .notFound {
      guard generation == current else { return }
      size = 0
      set(\.error, nil)
      set(\.loaded, true)
    } catch {
      guard generation == current else { return }
      set(\.error, "\(error)")
      set(\.loaded, true)
    }
  }

  private func append(_ text: String, generation current: Int) async {
    guard !text.isEmpty else { return }
    let form = self.form
    let raw = partial + text
    let parsed = await Task.detached(priority: .userInitiated) { LogStreamReader.parse(raw, form: form) }.value
    guard generation == current else { return }
    let previous = linesLength
    lines += parsed.lines
    linesLength += parsed.lines.utf16.count
    partial = parsed.rest
    var kept = previous
    // Keep about shownBytes drawn: cut at a line once well past it, so the
    // text view lays out the whole text again rarely.
    if linesLength > Int(Self.shownBytes) * 3 / 2 {
      let cut = lines.utf16.index(lines.utf16.startIndex, offsetBy: linesLength - Int(Self.shownBytes))
      let start = lines[cut...].firstIndex(of: "\n").map { lines.index(after: $0) } ?? cut
      lines = String(lines[start...])
      linesLength = lines.utf16.count
      set(\.clipped, true)
      kept = -1
    }
    set(LogText(text: lines + parsed.shownRest, kept: kept, revision: shown.revision + 1))
  }

  struct Parsed: Sendable, Equatable {
    var lines: String
    var rest: String
    var shownRest: String
  }

  /// Whole lines are shown for good; the last, unfinished one is shown as
  /// it stands and read again with what follows it.
  nonisolated static func parse(_ raw: String, form: Form) -> Parsed {
    let split = raw.lastIndex(of: "\n").map { raw.index(after: $0) } ?? raw.startIndex
    let whole = String(raw[..<split]), rest = String(raw[split...])
    switch form {
    case .terminal:
      return Parsed(lines: terminalText(whole), rest: rest, shownRest: terminalText(rest))
    case .journal(let phases):
      let text = whole.split(separator: "\n").compactMap { line -> String? in
        guard let v = try? JSONValue.parse(String(line)), let p = v["phase"]?.string, phases.contains(p) else { return nil }
        return "\(p) · \(v["event"]?.string ?? ""): \(v["message"]?.string ?? "")\n"
      }.joined()
      return Parsed(lines: text, rest: rest, shownRest: "")
    }
  }
}

/// Writes a whole stream to a file the reader picks, in pieces.
@MainActor func exportLog(_ session: ProjectSession, _ source: LogStreamReader.Source, size: UInt64) async throws {
  guard let rpc = session.rpc else { return }
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
                                         stream: source.stream, offset: offset, limit: 256 * 1024)
      guard let next = slice.nextOffset, next > offset else { break }
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

/// A log in an AppKit text view: lines are not wrapped (a window resize lays
/// nothing out again) and only what is visible is laid out. The reader hands
/// new text to the view directly; the end is replaced, and the view follows
/// the end while the reader of the log is there.
struct LogTextView: NSViewRepresentable {
  let reader: LogStreamReader
  var placeholder = ""

  @MainActor final class Coordinator {
    weak var scroll: NSScrollView?
    weak var reader: LogStreamReader?
    var revision = -1
    var placeholder = ""
    var showsPlaceholder = false
    var font: NSFont?

    func attach(_ next: LogStreamReader) {
      guard reader !== next else { return }
      reader?.onText = nil
      reader = next
      revision = -1
      next.onText = { [weak self] in self?.apply($0) }
    }

    func apply(_ log: LogText) {
      guard let scroll, let view = scroll.documentView as? NSTextView, let storage = view.textStorage else { return }
      let font = NSFont.monospacedSystemFont(ofSize: AppPreferences.shared.editorFontSize - 1, weight: .regular)
      let empty = log.text.isEmpty
      guard revision != log.revision || self.font != font || (empty && !showsPlaceholder) else { return }
      let attributes: [NSAttributedString.Key: Any] = [
        .font: empty ? NSFont.systemFont(ofSize: font.pointSize) : font,
        .foregroundColor: empty ? NSColor.secondaryLabelColor : NSColor.labelColor,
      ]
      let atEnd = revision < 0 || scroll.documentVisibleRect.maxY >= view.bounds.height - 8
      let incremental = !empty && !showsPlaceholder && log.kept >= 0 && log.revision == revision + 1
        && log.kept <= storage.length && self.font == font
      storage.beginEditing()
      if incremental {
        let utf16 = log.text.utf16
        let from = utf16.index(utf16.startIndex, offsetBy: log.kept)
        storage.replaceCharacters(in: NSRange(location: log.kept, length: storage.length - log.kept),
                                  with: NSAttributedString(string: String(log.text[from...]), attributes: attributes))
      } else {
        storage.setAttributedString(NSAttributedString(string: empty ? placeholder : log.text, attributes: attributes))
      }
      storage.endEditing()
      revision = log.revision
      self.font = font
      showsPlaceholder = empty
      if atEnd && !empty { view.scrollToEndOfDocument(nil) }
    }
  }

  func makeCoordinator() -> Coordinator { Coordinator() }

  func makeNSView(context: Context) -> NSScrollView {
    let scroll = NSTextView.scrollableTextView()
    scroll.hasHorizontalScroller = true
    scroll.autohidesScrollers = true
    scroll.drawsBackground = false
    let text = scroll.documentView as! NSTextView
    text.isEditable = false
    text.isSelectable = true
    text.isRichText = false
    text.drawsBackground = false
    text.textContainerInset = NSSize(width: 4, height: 4)
    text.isHorizontallyResizable = true
    text.textContainer?.widthTracksTextView = false
    text.textContainer?.containerSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
    text.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
    text.layoutManager?.allowsNonContiguousLayout = true
    text.layoutManager?.backgroundLayoutEnabled = false
    text.setAccessibilityIdentifier("log.text")
    context.coordinator.scroll = scroll
    return scroll
  }

  func updateNSView(_ scroll: NSScrollView, context: Context) {
    let state = context.coordinator
    state.attach(reader)
    if state.placeholder != placeholder {
      state.placeholder = placeholder
      state.showsPlaceholder = false  // drawn again with the new words
    }
    state.apply(reader.shown)
  }

  static func dismantleNSView(_ scroll: NSScrollView, coordinator: Coordinator) {
    coordinator.reader?.onText = nil
  }
}
