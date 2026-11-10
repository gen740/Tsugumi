import CoreGraphics

/// Whether the chat (or a Card's log) keeps showing its newest message. Only the
/// reader leaves the end, by scrolling up: content growing never does, however
/// much it adds at once.
struct ChatFollow {
  /// This close to the end (points) counts as at the end.
  static let endMargin: CGFloat = 32

  /// What the scroll view shows of its content.
  struct View: Equatable {
    var top: CGFloat
    var bottom: CGFloat
    var content: CGFloat
    var gap: CGFloat { content - bottom }
  }

  private(set) var following = true
  /// The reader's hand is on it (the wheel, a trackpad or the scroll bar).
  var readerScrolling = false
  private var last: View?

  /// Opening a conversation or sending a message goes to the end.
  mutating func restart() {
    following = true
    last = nil
  }

  /// Takes what the scroll view shows now; true when it should go to the end.
  mutating func update(_ now: View) -> Bool {
    defer { last = now }
    if now.gap <= Self.endMargin {
      following = true
    } else if let last, now.top < last.top, readerScrolling || now.content == last.content {
      // (the rows of a lazy stack, measured as they appear, move the position
      // along with the content's height: on their own, that is not the reader)
      following = false
    }
    return following && !readerScrolling && now.gap > 0.5
  }
}
