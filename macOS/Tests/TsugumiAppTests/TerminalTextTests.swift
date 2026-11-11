import Testing
@testable import TsugumiApp

/// A log's carriage returns write over their line, as in a terminal.
struct TerminalTextTests {
  @Test func progressShowsItsLatestState() {
    #expect(terminalText(" 10% 1.0M\r 55% 5.5M\r100%  10M\ndone\n") == "100%  10M\ndone\n")
  }

  @Test func shorterRedrawKeepsTheRestOfTheLine() {
    #expect(terminalText("abcdef\rXY") == "XYcdef")
  }

  @Test func carriageReturnAtTheEndKeepsTheLineUntilWrittenOver() {
    #expect(terminalText("50%\r") == "50%")
    #expect(terminalText("50%\r" + "60%\r") == "60%")
  }

  @Test func crlfEndsALine() {
    #expect(terminalText("a\r\nb\r\n") == "a\nb\n")
  }

  @Test func textWithoutCarriageReturnsIsUnchanged() {
    let s = "日本語 · é\nline\n"
    #expect(terminalText(s) == s)
  }
}
