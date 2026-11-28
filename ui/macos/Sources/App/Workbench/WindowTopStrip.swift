import AppKit
import SwiftUI

/// The strip at the top of a project window, the title bar's height, over
/// which the canvas and the chat column run up to the window's edge. Its empty
/// space moves the window as a title bar does; the capsules floating in it
/// (run control, chat header or toggle) keep the same distance from the
/// window's edges and from the chat column's edge in every layout. The
/// project's name stays in the window's toolbar, placed by AppKit beside the
/// traffic lights and the sidebar button.
enum TopStrip {
  /// From a capsule to the window's edge and to the chat column's edges.
  static let inset: CGFloat = 14
  /// Between two capsules side by side.
  static let gap: CGFloat = 8
  static let capsuleHeight: CGFloat = 34
  /// Above and below a capsule at least (its center is the strip's, which is
  /// the traffic lights' center).
  static let minVerticalMargin: CGFloat = 8
  /// The chat header never gets narrower than this, even in a squeezed column.
  static let minChatHeaderWidth: CGFloat = 104

  /// The strip's height for the window's top safe-area inset (the title bar
  /// and toolbar): tall enough for a capsule even without a toolbar.
  static func height(safeTop: CGFloat) -> CGFloat {
    max(safeTop, capsuleHeight + 2 * minVerticalMargin)
  }

  struct Frames: Equatable {
    var run: CGRect
    var chat: CGRect
  }

  /// Where the run capsule (`run`, its natural size) and the chat capsule go
  /// in the strip `bounds` (the width of the workbench and the chat column,
  /// laid out by `ChatSplit` with `chatWidth`). With the chat open, the chat
  /// header spans its column less the insets and the run capsule ends an inset
  /// before the column; with it closed, the chat toggle (`chat`, its natural
  /// size) sits at the window's edge with the run capsule beside it.
  static func frames(in bounds: CGRect, chatWidth: Double, chatOpen: Bool, run: CGSize, chat: CGSize) -> Frames {
    func y(_ h: CGFloat) -> CGFloat { bounds.midY - h / 2 }
    let chatFrame: CGRect
    let runMaxX: CGFloat
    if chatOpen {
      let column = ChatSplit.chatWidth(chatWidth, in: bounds.width)
      let columnX = bounds.maxX - column
      let w = max(minChatHeaderWidth, column - 2 * inset)
      chatFrame = CGRect(x: bounds.maxX - inset - w, y: y(capsuleHeight), width: w, height: capsuleHeight)
      runMaxX = min(columnX - inset, chatFrame.minX - gap)
    } else {
      chatFrame = CGRect(x: bounds.maxX - inset - chat.width, y: y(chat.height), width: chat.width, height: chat.height)
      runMaxX = chatFrame.minX - gap
    }
    return Frames(run: CGRect(x: runMaxX - run.width, y: y(run.height), width: run.width, height: run.height),
                  chat: chatFrame)
  }
}

/// Lays out the strip's two capsules (the run control, then the chat header
/// or toggle) by `TopStrip.frames`.
struct TopStripLayout: Layout {
  var chatWidth: Double
  var chatOpen: Bool

  func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
    proposal.replacingUnspecifiedDimensions()
  }

  func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
    guard subviews.count == 2 else { return }
    let f = TopStrip.frames(in: bounds, chatWidth: chatWidth, chatOpen: chatOpen,
                            run: subviews[0].sizeThatFits(.unspecified),
                            chat: chatOpen ? .zero : subviews[1].sizeThatFits(.unspecified))
    subviews[0].place(at: f.run.origin, proposal: ProposedViewSize(f.run.size))
    subviews[1].place(at: f.chat.origin, proposal: ProposedViewSize(f.chat.size))
  }
}

/// What a double click on a title bar does, as set in System Settings
/// (Desktop & Dock, "Double-click a window's title bar to").
enum TitleBarDoubleClick: Equatable {
  case zoom, minimize, none

  static func action(_ defaults: UserDefaults = .standard) -> Self {
    action(setting: defaults.string(forKey: "AppleActionOnDoubleClick"),
           miniaturize: defaults.bool(forKey: "AppleMiniaturizeOnDoubleClick"))
  }

  /// `setting` is the current key's value; `miniaturize` the older key's,
  /// read only when the current one is not set.
  static func action(setting: String?, miniaturize: Bool) -> Self {
    switch setting {
    case "Minimize": return .minimize
    case "None": return .none
    // "Fill" has no public counterpart: zooming fills the screen as near as AppKit offers.
    case "Maximize", "Fill": return .zoom
    default: return miniaturize ? .minimize : .zoom
    }
  }

  @MainActor func perform(on window: NSWindow) {
    switch self {
    case .zoom: window.zoom(nil)
    case .minimize: window.miniaturize(nil)
    case .none: break
    }
  }
}

/// Empty title-bar space laid over content that would otherwise take the
/// mouse (the canvas, the sidebar): dragging it moves the window, a double
/// click does the title bar's action. Views above it keep their clicks.
struct WindowDragArea: NSViewRepresentable {
  final class DragView: NSView {
    // The drag is done here, not by AppKit, so the double click reaches mouseDown.
    override var mouseDownCanMoveWindow: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func mouseDown(with event: NSEvent) {
      guard let window else { return }
      if event.clickCount == 2 {
        TitleBarDoubleClick.action().perform(on: window)
      } else {
        window.performDrag(with: event)
      }
    }
  }

  func makeNSView(context: Context) -> DragView { DragView() }
  func updateNSView(_ view: DragView, context: Context) {}
}

extension View {
  /// Lays `strip` over this view's top `height` points, from the window's top
  /// edge (under the title bar and toolbar). The rest of the view keeps its
  /// clicks.
  func windowTopStrip(height: CGFloat, @ViewBuilder _ strip: () -> some View) -> some View {
    overlay {
      VStack(spacing: 0) {
        strip().frame(height: height)
        Spacer(minLength: 0)
      }
      .ignoresSafeArea(.container, edges: .top)
    }
  }
}
