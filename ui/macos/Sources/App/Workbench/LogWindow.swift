import Foundation
import TsugumiRPC

enum LogForm: Hashable, Sendable {
  case terminal
  case journal
}

struct LogRowID: Hashable, Comparable, Sendable {
  let line: UInt64
  var fragment: UInt64? = nil

  static func < (lhs: Self, rhs: Self) -> Bool {
    (lhs.line, lhs.fragment == nil ? 0 : 1, lhs.fragment ?? 0) <
      (rhs.line, rhs.fragment == nil ? 0 : 1, rhs.fragment ?? 0)
  }
}

struct LogBlock: Equatable, Sendable {
  struct Row: Equatable, Sendable {
    let id: LogRowID
    let offset: Int
  }

  var line: UInt64 { rows[0].id.line }
  var fragmentOffset: UInt64? { rows[0].id.fragment }
  let text: String
  let length: Int
  let rows: [Row]

  init(text: String, rows: [Row]) {
    self.text = text
    self.length = text.utf16.count
    self.rows = rows
  }

  init(line: UInt64, text: String, fragmentOffset: UInt64? = nil) {
    self.init(text: text, rows: [Row(id: LogRowID(line: line, fragment: fragmentOffset), offset: 0)])
  }

  func range(of row: Int) -> NSRange {
    let start = rows[row].offset
    let end = row + 1 == rows.count ? length : rows[row + 1].offset
    return NSRange(location: start, length: end - start)
  }
}

/// The physical line and its position survive a rebuilt page, even when a
/// CR changes the text or the viewport starts inside a wrapped visual line.
struct LogAnchor {
  let row: LogRowID
  let column: Int

  static func at(_ character: Int, in blocks: [LogBlock]) -> Self? {
    var offset = character
    for block in blocks {
      if offset < block.length {
        var low = 0, high = block.rows.count
        while low + 1 < high {
          let middle = (low + high) / 2
          if block.rows[middle].offset <= offset { low = middle }
          else { high = middle }
        }
        let row = block.rows[low]
        return Self(row: row.id, column: offset - row.offset)
      }
      offset -= block.length
    }
    return nil
  }

  func character(in blocks: [LogBlock]) -> Int? {
    var offset = 0
    for block in blocks {
      if block.rows.last!.id >= row,
         let index = block.rows.firstIndex(where: { $0.id >= row }) {
        let range = block.range(of: index)
        let inset = block.rows[index].id == row ? min(column, range.length - 1) : 0
        return offset + range.location + inset
      }
      offset += block.length
    }
    return offset > 0 ? offset - 1 : nil
  }
}

struct LogPage: Sendable {
  // An absolute grid keeps a fragment's identity across differently sized
  // reads and bounds the text that AppKit wraps as a single paragraph.
  private static let fragmentCells = 16 * 1024

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
  let rawSize: UInt64
  let reset: Bool
  let text: String
  let head: Fragment?
  let body: [LogBlock]
  let tail: Fragment?

  init(offset: UInt64, end: UInt64, size: UInt64, truncatedAt: UInt64? = nil,
       line: UInt64, startsLine: Bool, text: String, form: LogForm,
       rawSize: UInt64? = nil, reset: Bool = false) {
    self.offset = offset
    self.end = end
    self.size = size
    self.truncatedAt = truncatedAt
    self.line = line
    self.startsLine = startsLine
    self.rawSize = rawSize ?? size
    self.reset = reset
    self.text = text

    // Work in UTF-8. Swift treats CRLF as one Character, but LF alone
    // is the line delimiter in the stored stream.
    var bytes = text.utf8[...]
    var number = line
    var projectedOffset = offset
    if !startsLine, !bytes.isEmpty {
      let lf = bytes.firstIndex(of: 10)
      let leading = String(decoding: bytes.prefix(upTo: lf ?? bytes.endIndex), as: UTF8.self)
      head = Fragment(line: number, offset: offset, text: leading, starts: false, ends: lf != nil)
      if form == .terminal { projectedOffset += UInt64(4 * (leading.unicodeScalars.count + 1)) }
      bytes = bytes.suffix(from: lf.map { bytes.index(after: $0) } ?? bytes.endIndex)
      number += 1
    } else {
      head = nil
    }
    var blocks: [LogBlock] = []
    var rows: [LogBlock.Row] = []
    var bodyText = ""
    var position = 0
    func flushBody() {
      guard !rows.isEmpty else { return }
      blocks.append(LogBlock(text: bodyText, rows: rows))
      rows.removeAll()
      bodyText = ""
      position = 0
    }
    while let lf = bytes.firstIndex(of: 10) {
      let rawLine = String(decoding: bytes.prefix(upTo: lf), as: UTF8.self)
      let cells = form == .terminal ? rawLine.unicodeScalars.count : 0
      if cells >= Self.fragmentCells {
        flushBody()
        blocks += Self.terminalFragments(rawLine, offset: projectedOffset, number: number, startsLine: true)
      } else if let rendered = Self.render(rawLine, number: number, form: form) {
        rows.append(.init(id: LogRowID(line: number), offset: position))
        bodyText += rendered + "\n"
        position += rendered.utf16.count + 1
      }
      if form == .terminal { projectedOffset += UInt64(4 * (cells + 1)) }
      bytes = bytes.suffix(from: bytes.index(after: lf))
      number += 1
    }
    flushBody()
    body = blocks
    let tailLength = form == .terminal
      ? bytes.reduce(into: 0) { $0 += $1 & 0xC0 == 0x80 ? 0 : 4 }
      : bytes.count
    tail = bytes.isEmpty ? nil : Fragment(line: number, offset: end - UInt64(tailLength),
                                          text: String(decoding: bytes, as: UTF8.self), starts: true, ends: false)
  }

  static func longTerminalLine(_ text: String) -> Bool { text.unicodeScalars.count >= fragmentCells }

  static func terminalFragments(_ text: String, offset: UInt64, number: UInt64, startsLine: Bool) -> [LogBlock] {
    let cells = text.unicodeScalars
    let column = Int(offset / 4 % UInt64(fragmentCells))
    // Wait for a missing leading section so its wrapped height stays stable.
    let skip = startsLine || column == 0 ? 0 : fragmentCells - column
    guard var start = cells.index(cells.startIndex, offsetBy: skip, limitedBy: cells.endIndex) else { return [] }
    var scalarOffset = skip
    var blocks: [LogBlock] = []
    while start < cells.endIndex {
      let length = fragmentCells - ((column + scalarOffset) % fragmentCells)
      let end = cells.index(start, offsetBy: length, limitedBy: cells.endIndex) ?? cells.endIndex
      let fragment = String(cells[start..<end])
      blocks.append(LogBlock(line: number, text: "\(number)\t\(fragment)\n",
                             fragmentOffset: offset + UInt64(scalarOffset * 4)))
      start = end
      scalarOffset += length
    }
    return blocks
  }

  static func render(_ raw: String, number: UInt64, form: LogForm) -> String? {
    let text: String
    switch form {
    case .terminal:
      text = raw
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
    var since: UInt64? = nil
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
  var rawSize: UInt64 = 0
  private var displayedRawSize: UInt64 = 0
  var visible: ClosedRange<LogRowID>?
  var truncatedAt: UInt64?

  init(form: LogForm) { self.form = form }

  var start: UInt64 { pages.first?.offset ?? 0 }
  var end: UInt64 { pages.last?.end ?? 0 }
  var hasUnread: Bool { rawSize > displayedRawSize }
  var waiting: Bool { !following && rawSize > seenEnd }

  mutating func follow(_ value: Bool) {
    if following && !value { seenEnd = displayedRawSize }
    following = value
  }

  func request(_ kind: Kind) -> Read? {
    switch kind {
    case .tail: return Read(kind: .tail, offset: 0, limit: Self.readBytes)
    case .earlier: return start == 0 ? nil : Read(kind: .earlier, offset: start, limit: Self.historyBytes)
    case .later: return end >= size ? nil : Read(kind: .later, offset: end, limit: Self.historyBytes)
    case .append:
      if size > end && size - end > Self.readBytes { return request(.tail) }
      return Read(kind: .append, offset: end, limit: Self.readBytes,
                  since: form == .terminal ? displayedRawSize : nil)
    case .status:
      if !loaded { return request(.tail) }
      return following ? request(.append) : Read(kind: .status, offset: end, limit: 4)
    }
  }

  mutating func apply(_ read: Read, _ page: LogPage, metadataOnly: Bool = false) {
    loaded = true
    size = page.size
    rawSize = max(rawSize, page.rawSize)
    truncatedAt = page.truncatedAt ?? truncatedAt
    if metadataOnly || read.kind == .status { return }
    if read.kind == .tail || read.kind == .append { displayedRawSize = page.rawSize }
    let reset = read.kind == .tail || page.reset
    if reset { pages.removeAll() }
    if page.end > page.offset {
      if read.kind == .earlier {
        pages.insert(page, at: 0)
      } else if let last = pages.last, page.end - last.offset <= 16 * 1024 {
        // Amortize many small live writes with at most 16 KiB of reparsing.
        pages[pages.count - 1] = LogPage(offset: last.offset, end: page.end, size: size,
                                        line: last.line, startsLine: last.startsLine,
                                        text: last.text + page.text, form: form)
      } else {
        pages.append(page)
      }
      while end - start > Self.windowBytes {
        if read.kind == .earlier { pages.removeLast() } else { pages.removeFirst() }
      }
    }
    assemble(keeping: reset ? nil : visible)
  }

  private mutating func assemble(keeping visible: ClosedRange<LogRowID>?) {
    var pieces: [LogPage.Fragment] = []
    var next: [LogBlock] = []
    func finish() {
      defer { pieces.removeAll(keepingCapacity: true) }
      guard let first = pieces.first, let last = pieces.last,
            form == .terminal || (first.starts && last.ends) else { return }
      let text = pieces.map(\.text).joined()
      if form == .terminal, LogPage.longTerminalLine(text) {
        next += LogPage.terminalFragments(text, offset: first.offset, number: first.line, startsLine: first.starts)
      } else if first.starts, let rendered = LogPage.render(text, number: first.line, form: form) {
        next.append(LogBlock(line: first.line, text: last.ends ? rendered + "\n" : rendered))
      }
    }
    for page in pages {
      if let head = page.head {
        pieces.append(head)
        if !head.ends { continue }
      }
      finish()
      next += page.body
      if let tail = page.tail { pieces.append(tail) }
    }
    finish()
    if let visible {
      func intersects(_ block: LogBlock) -> Bool {
        block.rows[0].id <= visible.upperBound && block.rows.last!.id >= visible.lowerBound
      }
      let present = Set(next.filter(intersects).flatMap { $0.rows.lazy.map(\.id).filter(visible.contains) })
      for block in blocks.filter(intersects) {
        for (index, row) in block.rows.enumerated() where visible.contains(row.id) && !present.contains(row.id) {
          let text = (block.text as NSString).substring(with: block.range(of: index))
          next.append(LogBlock(line: row.id.line, text: text, fragmentOffset: row.id.fragment))
        }
      }
      next.sort { $0.rows[0].id < $1.rows[0].id }
    }
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
  }

}
