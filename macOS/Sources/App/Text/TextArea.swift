import AppKit
import SwiftUI

/// The look shared by every text input: a rounded field on the text
/// background with a thin border (accent while editing).
struct FieldBox: ViewModifier {
  var focused = false

  func body(content: Content) -> some View {
    content
      .background(RoundedRectangle(cornerRadius: 7).fill(Color(nsColor: .textBackgroundColor)))
      .overlay(RoundedRectangle(cornerRadius: 7)
        .strokeBorder(focused ? Color.accentColor.opacity(0.8) : Color.secondary.opacity(0.3), lineWidth: focused ? 1.5 : 1))
  }
}

/// A one-line text field in the shared look, with a light placeholder.
struct BoxedTextField: View {
  let placeholder: String
  @Binding var text: String
  var monospaced = false
  var onSubmit: () -> Void = {}
  @FocusState private var focused: Bool

  var body: some View {
    TextField("", text: $text, prompt: Text(placeholder).foregroundStyle(.tertiary))
      .textFieldStyle(.plain)
      .font(monospaced ? .body.monospaced() : .body)
      .padding(.horizontal, 8).padding(.vertical, 5)
      .modifier(FieldBox(focused: focused))
      .focused($focused)
      .onSubmit(onSubmit)
  }
}

/// Multi-line text in the shared look: inner margins so no text touches the
/// border, and a scroll bar only once the text is longer than the field.
/// While empty it shows `placeholder` in light text (for example a template).
struct TextArea: View {
  @Binding var text: String
  var placeholder = ""
  var minHeight: CGFloat = 60
  var monospaced = false
  @State private var focused = false

  var body: some View {
    ScrollingText(text: $text, monospaced: monospaced, focused: $focused)
      .frame(minHeight: minHeight)
      .overlay(alignment: .topLeading) {
        if text.isEmpty && !placeholder.isEmpty {
          Text(placeholder)
            .font(monospaced ? .body.monospaced() : .body)
            .foregroundStyle(.tertiary)
            .padding(.horizontal, ScrollingText.inset.width + 5).padding(.vertical, ScrollingText.inset.height)
            .allowsHitTesting(false)
        }
      }
      .modifier(FieldBox(focused: focused))
  }
}

/// NSTextView in a scroll view whose scroller appears only when needed
/// (overlay scrollers, hidden while the text fits).
private struct ScrollingText: NSViewRepresentable {
  static let inset = CGSize(width: 6, height: 7)
  @Binding var text: String
  var monospaced: Bool
  @Binding var focused: Bool

  func makeCoordinator() -> Coordinator { Coordinator(self) }

  func makeNSView(context: Context) -> NSScrollView {
    let scroll = NSTextView.scrollableTextView()
    scroll.drawsBackground = false
    scroll.borderType = .noBorder
    scroll.hasVerticalScroller = true
    scroll.hasHorizontalScroller = false
    scroll.autohidesScrollers = true
    scroll.scrollerStyle = .overlay
    guard let tv = scroll.documentView as? NSTextView else { return scroll }
    tv.delegate = context.coordinator
    tv.drawsBackground = false
    tv.isRichText = false
    tv.allowsUndo = true
    tv.isAutomaticQuoteSubstitutionEnabled = false
    tv.isAutomaticDashSubstitutionEnabled = false
    tv.textContainerInset = Self.inset
    tv.font = monospaced ? .monospacedSystemFont(ofSize: NSFont.systemFontSize, weight: .regular) : .systemFont(ofSize: NSFont.systemFontSize)
    tv.textColor = .labelColor
    tv.string = text
    return scroll
  }

  func updateNSView(_ scroll: NSScrollView, context: Context) {
    context.coordinator.parent = self
    guard let tv = scroll.documentView as? NSTextView, tv.string != text, !context.coordinator.editing else { return }
    tv.string = text
  }

  final class Coordinator: NSObject, NSTextViewDelegate {
    var parent: ScrollingText
    var editing = false
    init(_ p: ScrollingText) { parent = p }

    func textDidBeginEditing(_ notification: Notification) {
      editing = true
      parent.focused = true
    }

    func textDidEndEditing(_ notification: Notification) {
      editing = false
      parent.focused = false
    }

    func textDidChange(_ notification: Notification) {
      guard let tv = notification.object as? NSTextView else { return }
      parent.text = tv.string
    }
  }
}
