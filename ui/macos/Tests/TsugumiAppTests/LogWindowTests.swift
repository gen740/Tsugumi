import AppKit
import Testing
@testable import TsugumiApp

private func logPage(_ text: String, at offset: UInt64 = 0, line: UInt64, starts: Bool = true,
                     size: UInt64? = nil, form: LogForm = .terminal) -> LogPage {
  let end = offset + UInt64(text.utf8.count)
  return LogPage(offset: offset, end: end, size: size ?? end, line: line, startsLine: starts, raw: text, form: form)
}

@Test func readingEarlierPreservesTheAnchorAndJoinsTerminalLines() throws {
  let prefix = "first\r\n日本 読み込み"
  let previous = logPage(prefix, line: 1)
  var window = LogWindow(form: .terminal)
  let tail = logPage("\r完了\nnext\n", at: previous.end, line: 2, starts: false)
  window.apply(try #require(window.request(.tail)), tail)
  window.follow(false)
  let old = window.blocks
  let oldOffset = try #require(old.first).length + 2  // The first letter of "next".
  window.apply(try #require(window.request(.earlier)), previous)
  let edit = LogBlockEdit(from: old, to: window.blocks)
  let displayed = window.blocks.map(\.text).joined()
  #expect(displayed == "1\tfirst\n2\t完了 読み込み\n3\tnext\n")
  #expect((displayed as NSString).character(at: edit.character(oldOffset)) == 110)
  #expect(window.request(.earlier) == nil)
}

@Test func incompleteJournalRecordsAppearAtTheirAbsoluteLineAfterJoining() throws {
  let a = logPage(#"{"phase":"save","event":"st"#, line: 7, form: .journal)
  let b = logPage(#"ored","message":"ok"}"# + "\n" + #"{"phase":"execute"}"# + "\n",
                  at: a.end, line: 7, starts: false, form: .journal)
  var window = LogWindow(form: .journal)
  window.apply(try #require(window.request(.tail)), b)
  #expect(window.blocks.isEmpty)
  window.apply(try #require(window.request(.earlier)), a)
  #expect(window.blocks.map(\.text) == ["7\tsave · stored: ok\n"])
}

@Test func theLogWindowDropsAndReloadsBothEdges() throws {
  let line = String(repeating: "x", count: Int(LogWindow.readBytes) - 1) + "\n"
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
  let oldPage = logPage(last, at: UInt64(first.utf8.count), line: 1, starts: false)
  window.apply(try #require(window.request(.tail)), oldPage)
  let old = window.blocks
  window.follow(false)
  window.apply(try #require(window.request(.earlier)), logPage(first, line: 1))
  let edit = LogBlockEdit(from: old, to: window.blocks)
  let text = window.blocks.map(\.text).joined() as NSString
  #expect(text.character(at: edit.character(10)) == 98)
  #expect(window.blocks.allSatisfy { $0.line == 1 })
}

@Test func logNotificationsRequestOneReadPerFrameWithNoOverlap() {
  var frames = LogReadFrames<Int>()
  for _ in 0..<100 { frames.wake() }
  #expect(frames.needsFrame)
  #expect(frames.begin())
  for _ in 0..<100 { frames.wake() }
  #expect(!frames.needsFrame && !frames.begin())
  frames.finish(1)
  #expect(frames.needsFrame)
  #expect(frames.takeResult() == 1)
  #expect(frames.begin())
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
