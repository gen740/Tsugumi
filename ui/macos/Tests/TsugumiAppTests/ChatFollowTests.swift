import CoreGraphics
import Testing
@testable import TsugumiApp

/// The chat stays at its newest message until the reader scrolls up, and comes
/// back when the reader returns to the end.
struct ChatFollowTests {
  /// What the scroll view shows now, and whether the chat should go to the end.
  private func step(_ f: inout ChatFollow, top: CGFloat, content: CGFloat, goesToEnd: Bool,
                    sourceLocation: SourceLocation = #_sourceLocation) {
    let goes = f.update(ChatFollow.View(top: top, bottom: top + 500, content: content))
    #expect(goes == goesToEnd, sourceLocation: sourceLocation)
  }

  @Test func contentGrowingGoesToTheEnd() {
    var f = ChatFollow()
    step(&f, top: 0, content: 100, goesToEnd: false)
    // A long conversation loaded at once: far from the end, and not the reader's doing.
    step(&f, top: 0, content: 9000, goesToEnd: true)
    step(&f, top: 8500, content: 9000, goesToEnd: false)
    // Markdown or math measured later.
    step(&f, top: 8500, content: 9400, goesToEnd: true)
    #expect(f.following)
  }

  @Test func scrollingUpLeavesTheEndUntilBack() {
    var f = ChatFollow()
    step(&f, top: 1500, content: 2000, goesToEnd: false)
    step(&f, top: 1000, content: 2000, goesToEnd: false)
    #expect(!f.following)
    // A new reply while reading moves nothing.
    step(&f, top: 1000, content: 2600, goesToEnd: false)
    // Down, short of the end, still reading; at the end, following again.
    step(&f, top: 1500, content: 2600, goesToEnd: false)
    #expect(!f.following)
    step(&f, top: 2100, content: 2600, goesToEnd: false)
    #expect(f.following)
    step(&f, top: 2100, content: 3000, goesToEnd: true)
  }

  @Test func aSmallMovementAtTheEndStaysAtTheEnd() {
    var f = ChatFollow()
    step(&f, top: 1500, content: 2000, goesToEnd: false)
    step(&f, top: 1500 - ChatFollow.endMargin, content: 2000, goesToEnd: true)
    step(&f, top: 1500, content: 2000, goesToEnd: false)
    step(&f, top: 1500 - ChatFollow.endMargin - 1, content: 2000, goesToEnd: false)
    #expect(!f.following)
  }

  @Test func theLazyStackMeasuringRowsIsNotTheReader() {
    var f = ChatFollow()
    step(&f, top: 8500, content: 9000, goesToEnd: false)
    // Rows measured shorter than estimated: the position moves up with the height.
    step(&f, top: 7000, content: 8000, goesToEnd: true)
    #expect(f.following)
  }

  @Test func theReaderScrollingIsNotPulledBack() {
    var f = ChatFollow()
    step(&f, top: 8500, content: 9000, goesToEnd: false)
    f.readerScrolling = true
    // Rows measured in the same step: the height changes as the reader goes up.
    step(&f, top: 7000, content: 8800, goesToEnd: false)
    #expect(!f.following)
    f.readerScrolling = false
    step(&f, top: 7000, content: 9500, goesToEnd: false)

    // A hand on the trackpad that has not moved yet: nothing moves under it,
    // and the end is kept once it lets go.
    var g = ChatFollow()
    step(&g, top: 8500, content: 9000, goesToEnd: false)
    g.readerScrolling = true
    step(&g, top: 8500, content: 9400, goesToEnd: false)
    #expect(g.following)
    g.readerScrolling = false
    step(&g, top: 8500, content: 9500, goesToEnd: true)
  }

  @Test func restartFollowsAgain() {
    var f = ChatFollow()
    step(&f, top: 1500, content: 2000, goesToEnd: false)
    step(&f, top: 1000, content: 2000, goesToEnd: false)
    #expect(!f.following)
    f.restart()
    #expect(f.following)
    step(&f, top: 0, content: 2000, goesToEnd: true)
  }
}
