import Foundation
import TsugumiRPC

enum LogForm: Hashable, Sendable {
  case terminal
  case journal
}

struct LogBlock: Equatable, Sendable {
  let line: UInt64
  let fragmentOffset: UInt64?
  let text: String
  let length: Int

  init(line: UInt64, text: String, fragmentOffset: UInt64? = nil) {
    self.line = line
    self.fragmentOffset = fragmentOffset
    self.text = text
    self.length = text.utf16.count
  }
}

struct LogPage: Sendable {
  struct Fragment: Sendable {
    let line: UInt64
    let offset: UInt64
    let text: String
    let starts: Bool
    let ends: Bool
  }

  let offset: UInt64
  let end: UInt64
  let size: UInt64
  let truncatedAt: UInt64?
  let line: UInt64
  let startsLine: Bool
  let raw: String
  let head: Fragment?
  let body: LogBlock?
  let tail: Fragment?

  init(offset: UInt64, end: UInt64, size: UInt64, truncatedAt: UInt64? = nil,
       line: UInt64, startsLine: Bool, raw: String, form: LogForm) {
    self.offset = offset
    self.end = end
    self.size = size
    self.truncatedAt = truncatedAt
    self.line = line
    self.startsLine = startsLine
    self.raw = raw

    // Work in UTF-8. Swift treats CRLF as one Character, but LF alone
    // is the line delimiter in the stored stream.
    var bytes = raw.utf8[...]
    var number = line
    if !startsLine, !bytes.isEmpty {
      let lf = bytes.firstIndex(of: 10)
      head = Fragment(line: number, offset: offset, text: String(decoding: bytes.prefix(upTo: lf ?? bytes.endIndex), as: UTF8.self),
                      starts: false, ends: lf != nil)
      bytes = bytes.suffix(from: lf.map { bytes.index(after: $0) } ?? bytes.endIndex)
      number += 1
    } else {
      head = nil
    }
    var first: UInt64?
    var text = ""
    while let lf = bytes.firstIndex(of: 10) {
      let rawLine = String(decoding: bytes.prefix(upTo: lf), as: UTF8.self)
      if let rendered = Self.render(rawLine, number: number, form: form) {
        if first == nil { first = number }
        text += rendered
        text += "\n"
      }
      bytes = bytes.suffix(from: bytes.index(after: lf))
      number += 1
    }
    body = first.map { LogBlock(line: $0, text: text) }
    tail = bytes.isEmpty ? nil : Fragment(line: number, offset: end - UInt64(bytes.count),
                                          text: String(decoding: bytes, as: UTF8.self), starts: true, ends: false)
  }

  static func render(_ raw: String, number: UInt64, form: LogForm) -> String? {
    let text: String
    switch form {
    case .terminal:
      text = terminalText(raw)
    case .journal:
      guard let record = try? JSONValue.parse(raw), let phase = record["phase"]?.string,
            phase == "validate" || phase == "save" else { return nil }
      text = "\(phase) · \(record["event"]?.string ?? ""): \(record["message"]?.string ?? "")"
    }
    return "\(number)\t\(text)"
  }
}

struct LogWindow {
  enum Kind { case tail, earlier, later, append, status }
  struct Read {
    let kind: Kind
    let offset: UInt64
    let limit: UInt32
  }

  static let readBytes: UInt32 = 1 << 20
  private static let windowBytes = 2 * UInt64(readBytes)
  private static let historyBytes = readBytes / 4

  let form: LogForm
  private var pages: [LogPage] = []
  private(set) var blocks: [LogBlock] = []
  private(set) var following = true
  private var seenEnd: UInt64 = 0
  var loaded = false
  var size: UInt64 = 0
  var truncatedAt: UInt64?

  var start: UInt64 { pages.first?.offset ?? 0 }
  var end: UInt64 { pages.last?.end ?? 0 }
  var waiting: Bool { !following && size > seenEnd }

  mutating func follow(_ value: Bool) {
    if following && !value { seenEnd = end }
    following = value
  }

  func request(_ kind: Kind) -> Read? {
    switch kind {
    case .tail: return Read(kind: .tail, offset: 0, limit: Self.readBytes)
    case .earlier: return start == 0 ? nil : Read(kind: .earlier, offset: start, limit: Self.historyBytes)
    case .later: return end >= size ? nil : Read(kind: .later, offset: end, limit: Self.historyBytes)
    case .append:
      if size > end && size - end > Self.readBytes { return request(.tail) }
      return Read(kind: .append, offset: end, limit: Self.readBytes)
    case .status:
      if !loaded { return request(.tail) }
      return following ? request(.append) : Read(kind: .status, offset: end, limit: 4)
    }
  }

  mutating func apply(_ read: Read, _ page: LogPage, metadataOnly: Bool = false) {
    loaded = true
    size = max(size, page.size)
    truncatedAt = page.truncatedAt ?? truncatedAt
    if metadataOnly || read.kind == .status { return }
    if read.kind == .tail { pages.removeAll() }
    if page.end > page.offset {
      if read.kind == .earlier {
        pages.insert(page, at: 0)
      } else if let last = pages.last, page.end - last.offset <= 16 * 1024 {
        // Amortize many small live writes with at most 16 KiB of reparsing.
        pages[pages.count - 1] = LogPage(offset: last.offset, end: page.end, size: size,
                                        line: last.line, startsLine: last.startsLine,
                                        raw: last.raw + page.raw, form: form)
      } else {
        pages.append(page)
      }
      while end - start > Self.windowBytes {
        if read.kind == .earlier { pages.removeLast() } else { pages.removeFirst() }
      }
    }
    assemble()
  }

  private mutating func assemble() {
    var pieces: [LogPage.Fragment] = []
    var next: [LogBlock] = []
    func finish() {
      defer { pieces.removeAll(keepingCapacity: true) }
      guard let first = pieces.first, let last = pieces.last,
            form == .terminal || (first.starts && last.ends) else { return }
      let raw = pieces.map(\.text).joined()
      // A single very long plain line may span the entire window. Keep
      // page-relative anchors for its visible pieces so it can still be
      // browsed in both directions. A redraw needs the whole resident line.
      if form == .terminal, raw.utf8.count >= 64 * 1024, !raw.utf8.contains(13) {
        for piece in pieces where !piece.text.isEmpty {
          next.append(LogBlock(line: piece.line, text: "\(piece.line)\t\(piece.text)\n", fragmentOffset: piece.offset))
        }
      } else if let text = LogPage.render(raw, number: first.line, form: form) {
        next.append(LogBlock(line: first.line, text: last.ends ? text + "\n" : text))
      }
    }
    for page in pages {
      if let head = page.head {
        pieces.append(head)
        if !head.ends { continue }
      }
      finish()
      if let body = page.body { next.append(body) }
      if let tail = page.tail { pieces.append(tail) }
    }
    finish()
    blocks = next
  }
}

/// One request in flight. Completed reads and pending work are consumed
/// on display frames, including notifications received during I/O.
struct LogReadFrames<Value> {
  private var pending = false
  private var busy = false
  private var result: Value?

  var needsFrame: Bool { !busy && (pending || result != nil) }

  mutating func wake() { pending = true }

  mutating func takeResult() -> Value? {
    defer { result = nil }
    return result
  }

  mutating func begin() -> Bool {
    guard pending && !busy else { return false }
    pending = false
    busy = true
    return true
  }

  mutating func finish(_ result: Value? = nil) {
    self.result = result
    busy = false
  }
}

/// Retain the common text-storage range so TextKit keeps the existing glyphs.
struct LogBlockEdit {
  let oldPrefix: Int
  let newPrefix: Int
  let shared: Int
  let oldLength: Int
  let newLength: Int
  let retained: Range<Int>

  init(from old: [LogBlock], to new: [LogBlock]) {
    var o = 0, n = 0, before = 0, inserted = 0, length = 0
    while o < old.count && n < new.count && old[o] != new[n] {
      if (old[o].line, old[o].fragmentOffset ?? 0) <= (new[n].line, new[n].fragmentOffset ?? 0) {
        before += old[o].length
        o += 1
      } else {
        inserted += new[n].length
        n += 1
      }
    }
    let firstShared = n
    while o < old.count && n < new.count && old[o] == new[n] {
      length += old[o].length
      o += 1
      n += 1
    }
    retained = firstShared..<n
    oldPrefix = before
    newPrefix = inserted
    shared = length
    oldLength = old.reduce(0) { $0 + $1.length }
    newLength = new.reduce(0) { $0 + $1.length }
  }

  func character(_ previous: Int) -> Int {
    // A change to an unfinished line can replace the anchor itself.
    min(max(0, previous - oldPrefix + newPrefix), max(0, newLength - 1))
  }
}
