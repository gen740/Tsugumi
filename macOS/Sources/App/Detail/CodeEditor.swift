import AppKit
import FlatBuffers
import SwiftUI
import TsugumiAgent
import TsugumiRPC

/// Plain-text code editor: monospaced, Tab inserts the configured indent,
/// optional indent guides. Undo stays inside the editor until applied.
struct CodeEditor: NSViewRepresentable {
  @Binding var text: String
  var language = ""
  var editable = true

  func makeCoordinator() -> Coordinator { Coordinator(self) }

  func makeNSView(context: Context) -> NSScrollView {
    let scroll = NSTextView.scrollableTextView()
    let old = scroll.documentView as! NSTextView
    let tv = GuideTextView(frame: old.frame, textContainer: old.textContainer)
    tv.autoresizingMask = old.autoresizingMask
    tv.isVerticallyResizable = true
    tv.minSize = NSSize(width: 0, height: 0)
    tv.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
    scroll.documentView = tv
    tv.delegate = context.coordinator
    tv.isRichText = false
    tv.allowsUndo = true
    tv.isAutomaticQuoteSubstitutionEnabled = false
    tv.isAutomaticDashSubstitutionEnabled = false
    tv.isAutomaticTextReplacementEnabled = false
    tv.isAutomaticSpellingCorrectionEnabled = false
    tv.smartInsertDeleteEnabled = false
    tv.textContainerInset = NSSize(width: 4, height: 6)
    tv.drawsBackground = true
    tv.backgroundColor = .textBackgroundColor
    configure(tv)
    tv.string = text
    return scroll
  }

  func updateNSView(_ scroll: NSScrollView, context: Context) {
    guard let tv = scroll.documentView as? GuideTextView else { return }
    context.coordinator.parent = self
    configure(tv)
    if tv.string != text {
      let sel = tv.selectedRanges
      tv.string = text
      tv.selectedRanges = sel.filter { $0.rangeValue.upperBound <= (text as NSString).length }
    }
  }

  private func configure(_ tv: GuideTextView) {
    let prefs = AppPreferences.shared
    tv.font = .monospacedSystemFont(ofSize: prefs.editorFontSize, weight: .regular)
    tv.isEditable = editable
    tv.indentWidth = prefs.indentWidth
    tv.showGuides = prefs.showIndentGuides
    tv.needsDisplay = true
  }

  final class Coordinator: NSObject, NSTextViewDelegate {
    var parent: CodeEditor
    init(_ p: CodeEditor) { parent = p }

    func textDidChange(_ n: Notification) {
      guard let tv = n.object as? NSTextView else { return }
      parent.text = tv.string
    }

    func textView(_ tv: NSTextView, doCommandBy selector: Selector) -> Bool {
      let indent = String(repeating: " ", count: AppPreferences.shared.indentWidth)
      if selector == #selector(NSResponder.insertTab(_:)) {
        tv.insertText(indent, replacementRange: tv.selectedRange())
        return true
      }
      if selector == #selector(NSResponder.insertNewline(_:)) {
        // Keep the indentation of the current line.
        let s = tv.string as NSString
        let line = s.lineRange(for: NSRange(location: tv.selectedRange().location, length: 0))
        let current = s.substring(with: line)
        let lead = current.prefix { $0 == " " || $0 == "\t" }
        var extra = ""
        if current.trimmingCharacters(in: .whitespacesAndNewlines).hasSuffix(":") ||
           current.trimmingCharacters(in: .whitespacesAndNewlines).hasSuffix("{") { extra = indent }
        tv.insertText("\n" + lead + extra, replacementRange: tv.selectedRange())
        return true
      }
      return false
    }
  }
}

/// Draws vertical guides at each indent level of leading whitespace.
final class GuideTextView: NSTextView {
  var indentWidth = 4
  var showGuides = true

  override func drawBackground(in rect: NSRect) {
    super.drawBackground(in: rect)
    guard showGuides, let lm = layoutManager, let tc = textContainer, let font else { return }
    let space = (" " as NSString).size(withAttributes: [.font: font]).width
    let step = space * CGFloat(indentWidth)
    NSColor.separatorColor.withAlphaComponent(0.5).setFill()
    let glyphs = lm.glyphRange(forBoundingRect: rect, in: tc)
    let s = string as NSString
    lm.enumerateLineFragments(forGlyphRange: glyphs) { frag, _, _, range, _ in
      let chars = lm.characterRange(forGlyphRange: range, actualGlyphRange: nil)
      let line = s.substring(with: chars)
      let lead = line.prefix { $0 == " " }.count
      let levels = lead / max(1, self.indentWidth)
      guard levels > 0 else { return }
      for l in 0..<levels {
        let x = self.textContainerInset.width + tc.lineFragmentPadding + CGFloat(l) * step + 1
        NSRect(x: x, y: frag.minY + self.textContainerInset.height, width: 1, height: frag.height).fill()
      }
    }
  }
}

/// A recorded input/output value as pretty JSON (decoded with its own BFBS).
func describeValue(_ v: Tsugumi_Data_NamedValueT) -> String {
  var head = "\(v.name) : \(v.rootType)"
  if let n = v.sizeBytes { head += " · \(n) bytes" }
  if let a = v.artifactId { head += " · 成果物 \(a)" }
  guard !v.inlineBytes.isEmpty else { return head + "\n（内容は記録されていません）" }
  guard let schema = try? BFBS(data: Data(v.schemaBfbs)),
        let (index, _) = schema.object(named: v.rootType) ??
          schema.objects.enumerated().first(where: { $0.element.name.hasSuffix("." + v.rootType) }).map({ ($0.offset, $0.element) }) else {
    return head + "\n" + v.inlineBytes.prefix(256).map { String(format: "%02x", $0) }.joined(separator: " ")
  }
  let bb = ByteBuffer(bytes: v.inlineBytes)
  let root = Table(bb: bb, position: Int32(bb.read(def: UInt32.self, position: 0)))
  let obj = FlatJSON(schema: schema).json(index, root)
  let data = (try? JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted, .sortedKeys, .fragmentsAllowed])) ?? Data()
  return head + "\n" + String(decoding: data, as: UTF8.self)
}
