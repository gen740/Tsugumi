import AppKit
import Testing
@testable import TsugumiApp

private func logPage(_ text: String, at offset: UInt64 = 0, line: UInt64, starts: Bool = true,
                     size: UInt64? = nil, form: LogForm = .terminal,
                     rawSize: UInt64? = nil, reset: Bool = false) -> LogPage {
  let length = form == .terminal ? text.unicodeScalars.count * 4 : text.utf8.count
  let end = offset + UInt64(length)
  return LogPage(offset: offset, end: end, size: size ?? end, line: line, startsLine: starts,
                 text: text, form: form, rawSize: rawSize, reset: reset)
}

@Test func readingEarlierPreservesTheAnchorAndJoinsTerminalLines() throws {
  let prefix = "first\n完了 読み込み"
  let previous = logPage(prefix, line: 1)
  var window = LogWindow(form: .terminal)
  let tail = logPage("\nnext\n", at: previous.end, line: 2, starts: false)
  window.apply(try #require(window.request(.tail)), tail)
  window.follow(false)
  let old = window.blocks
  #expect(old.map(\.line) == [3])
  let anchor = try #require(LogAnchor.at(2, in: old))
  window.apply(try #require(window.request(.earlier)), previous)
  let displayed = window.blocks.map(\.text).joined()
  #expect(displayed == "1\tfirst\n2\t完了 読み込み\n3\tnext\n")
  let position = try #require(anchor.character(in: window.blocks))
  #expect((displayed as NSString).character(at: position) == 110)
  #expect(window.request(.earlier) == nil)
}

@Test func incompleteJournalRecordsAppearAtTheirAbsoluteLineAfterJoining() throws {
  let a = logPage("save · st", line: 7, form: .journal)
  let b = logPage("ored: ok\nexecute · done: finished\n",
                  at: a.end, line: 7, starts: false, form: .journal)
  var window = LogWindow(form: .journal)
  window.apply(try #require(window.request(.tail)), b)
  #expect(window.blocks.isEmpty)
  window.apply(try #require(window.request(.earlier)), a)
  #expect(window.blocks.map(\.text) == ["7\tsave · stored: ok\n"])
}

@Test func theLogWindowDropsAndReloadsBothEdges() throws {
  let line = String(repeating: "x", count: Int(LogWindow.readBytes) / 4 - 1) + "\n"
  let size = UInt64(LogWindow.readBytes)
  var window = LogWindow(form: .terminal)
  window.apply(try #require(window.request(.tail)), logPage(line, at: 2 * size, line: 3))
  window.follow(false)
  window.apply(try #require(window.request(.earlier)), logPage(line, at: size, line: 2, size: 3 * size))
  window.apply(try #require(window.request(.earlier)), logPage(line, line: 1, size: 3 * size))
  #expect(window.start == 0 && window.end == 2 * size)
  window.apply(try #require(window.request(.later)), logPage(line, at: 2 * size, line: 3))
  #expect(window.start == size && window.end == 3 * size)
  #expect(!window.waiting)

  let next = logPage("another line", at: window.end, line: 4)
  window.apply(try #require(window.request(.status)), next)
  #expect(window.end == 3 * size && window.waiting)
  window.follow(true)
  window.apply(try #require(window.request(.status)), next)
  #expect(window.end == next.end && !window.waiting)
}

@Test func aLongUnterminatedLineKeepsItsAnchorWhenPrepending() throws {
  let first = String(repeating: "a", count: 256 * 1024)
  let last = String(repeating: "b", count: 256 * 1024)
  var window = LogWindow(form: .terminal)
  let oldPage = logPage(last, at: UInt64(first.unicodeScalars.count * 4), line: 1, starts: false)
  window.apply(try #require(window.request(.tail)), oldPage)
  let anchor = try #require(LogAnchor.at(10, in: window.blocks))
  window.follow(false)
  window.apply(try #require(window.request(.earlier)), logPage(first, line: 1))
  let text = window.blocks.map(\.text).joined() as NSString
  let position = try #require(anchor.character(in: window.blocks))
  #expect(text.character(at: position) == 98)
  #expect(window.blocks.allSatisfy { $0.line == 1 })

  var overlap = LogWindow(form: .terminal)
  let clipped = logPage(String(last.dropFirst(3_000)), at: oldPage.offset + 3_000 * 4, line: 1, starts: false)
  overlap.apply(try #require(overlap.request(.tail)), clipped)
  let fragment = try #require(overlap.blocks.first)
  #expect(window.blocks.first(where: { $0.fragmentOffset == fragment.fragmentOffset }) == fragment)
}

@Test func keepingVisibleJournalRowsDoesNotKeepTheirRawPage() throws {
  let shown = "save · done: anchor\n"
  let filtered = "build · done: " + String(repeating: "a", count: (1 << 20) - 80) + "\n"
  let size = UInt64(4 << 20)
  var window = LogWindow(form: .journal)
  let tail = logPage(shown, at: size - UInt64(shown.utf8.count), line: 4, form: .journal)
  window.apply(try #require(window.request(.tail)), tail)
  window.follow(false)
  let row = LogRowID(line: 4)
  window.visible = row...row
  for line in stride(from: 3, through: 1, by: -1) {
    let page = logPage(filtered, at: window.start - UInt64(filtered.utf8.count),
                       line: UInt64(line), size: size, form: .journal)
    window.apply(try #require(window.request(.earlier)), page)
  }
  #expect(window.end < tail.offset)
  #expect(window.blocks.map(\.text) == ["4\tsave · done: anchor\n"])
}

@Test func appendingToARebuiltBlockKeepsTheLogicalRowAndColumn() throws {
  var window = LogWindow(form: .terminal)
  let initial = logPage("first\nsecond\nthird\n", line: 1, size: 128)
  window.apply(try #require(window.request(.tail)), initial)
  window.follow(false)
  let original = try #require(window.blocks.first).text as NSString
  let character = original.range(of: "cond").location
  let anchor = try #require(LogAnchor.at(character, in: window.blocks))

  window.apply(try #require(window.request(.later)), logPage("new\n", at: initial.end, line: 4))
  #expect(window.blocks.count == 1)
  #expect(anchor.character(in: window.blocks) == character)
  #expect(LogAnchor.at(character, in: window.blocks)?.row.line == 2)
}

@Test func aLiveRedrawReplacesTheUnfinishedLineWithoutMovingItsColumns() throws {
  var window = LogWindow(form: .terminal)
  let initial = logPage("first\nabcdef", line: 1, rawSize: 12)
  window.apply(try #require(window.request(.tail)), initial)
  let read = try #require(window.request(.status))
  #expect(read.since == 12 && read.offset == initial.end)
  let updated = logPage("first\n雪Ycdef", line: 1, rawSize: 17, reset: true)
  window.apply(read, updated)
  #expect(window.blocks.map(\.text).joined() == "1\tfirst\n2\t雪Ycdef")
  #expect(window.end == initial.end && window.rawSize == 17)
}

@Test func logNotificationsRequestOneReadPerFrameWithNoOverlap() {
  var frames = LogReadFrames<Int>()
  for _ in 0..<100 { frames.wake() }
  #expect(frames.needsFrame)
  let first = frames.begin()
  #expect(first)
  for _ in 0..<100 { frames.wake() }
  #expect(!frames.needsFrame && !frames.begin())
  frames.finish(1)
  #expect(frames.needsFrame)
  #expect(frames.takeResult() == 1)
  let second = frames.begin()
  #expect(second)
  frames.finish(2)
  #expect(frames.takeResult() == 2)
  #expect(!frames.begin() && !frames.needsFrame)
}

@MainActor @Test func longLogLinesWrapInTheNativeViewport() throws {
  let scroll = LogScroll.make()
  scroll.frame = NSRect(x: 0, y: 0, width: 240, height: 160)
  scroll.tile()
  let text = try #require(scroll.documentView as? NSTextView)
  let layout = try #require(text.layoutManager)
  let container = try #require(text.textContainer)
  let font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
  text.textStorage?.setAttributedString(NSAttributedString(
    string: "40\t" + String(repeating: "x", count: 240),
    attributes: LogScroll.attributes(font: font)))
  layout.ensureLayout(for: container)
  let first = layout.lineFragmentRect(forGlyphAt: 0, effectiveRange: nil)
  let last = layout.lineFragmentRect(forGlyphAt: layout.numberOfGlyphs - 1, effectiveRange: nil)
  #expect(last.minY > first.minY)
  #expect(!scroll.hasHorizontalScroller)
}

@MainActor @Test func jumpingToUnchangedJournalTextScrollsToTheEnd() throws {
  let scroll = LogScroll.make()
  scroll.frame = NSRect(x: 0, y: 0, width: 240, height: 130)
  scroll.tile()
  let text = try #require(scroll.documentView as? NSTextView)
  let reader = LogStreamReader(form: .journal)
  let coordinator = LogScrollView.Coordinator()
  coordinator.scroll = scroll
  coordinator.fontSize = 12
  coordinator.attach(reader)
  let records = (1...100).map { "save · done: record \($0)\n" }.joined()
  let base = logPage(records, line: 1, form: .journal)
  reader.apply(.init(kind: .tail, read: try #require(reader.window.request(.tail)), result: .success(base)))
  text.layoutManager?.ensureLayout(for: try #require(text.textContainer))
  let origin = scroll.contentView.bounds.minY
  #expect(origin > 0)

  reader.follow(false)
  scroll.contentView.scroll(to: .zero)
  let filtered = logPage("build · done: finished\n", at: base.end, line: 101, form: .journal)
  reader.apply(.init(kind: .status, read: try #require(reader.window.request(.status)), result: .success(filtered)))
  #expect(reader.waiting)
  let before = text.string
  reader.jump()
  #expect(!reader.waiting)
  #expect(text.string == before)
  #expect(scroll.contentView.bounds.minY == origin)
}
