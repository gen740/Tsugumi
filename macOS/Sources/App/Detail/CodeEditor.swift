import AppKit
import FlatBuffers
import SwiftUI
import TsugumiAgentClient
import TsugumiRPC

/// Token classes the editor colors.
enum SyntaxToken: CaseIterable {
  case plain, comment, string, number, keyword, function, type, constant, property
}

/// Regex based highlighting for the languages Cards use. It only colors text;
/// errors come from the formatter and the core diagnostics.
enum SyntaxHighlighter {
  struct Rules {
    let comment: [String]
    let string: [String]
    let keywords: [String]
  }

  static func rules(for language: String) -> Rules? {
    switch language.lowercased() {
    case "cs", "csharp":
      return Rules(comment: [#"//[^\n]*"#, #"/\*[\s\S]*?\*/"#],
                   string: [#"\$*\"\"\"[\s\S]*?\"\"\""#, #"(?:\$@|@\$|@)"(?:""|[^"])*""#, #"\$?"(?:\\.|[^"\\\n])*""#, #"'(?:\\.|[^'\\\n])'"#],
                   keywords: ["abstract", "as", "async", "await", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class",
                              "const", "continue", "decimal", "default", "delegate", "do", "double", "else", "enum", "event", "extern", "false", "finally",
                              "fixed", "float", "for", "foreach", "if", "in", "int", "interface", "internal", "is", "lock", "long", "namespace", "new", "null",
                              "object", "out", "override", "params", "partial", "private", "protected", "public", "readonly", "record", "ref", "required", "return",
                              "sbyte", "sealed", "short", "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true", "try", "typeof",
                              "uint", "ulong", "unchecked", "unsafe", "ushort", "using", "var", "virtual", "void", "volatile", "while", "yield"])
    case "cpp", "cc", "hpp", "h", "cxx":
      return Rules(comment: [#"//[^\n]*"#, #"/\*[\s\S]*?\*/"#], string: [#""(?:\\.|[^"\\\n])*""#, #"'(?:\\.|[^'\\\n])'"#],
                   keywords: ["auto", "bool", "break", "case", "class", "const", "constexpr", "continue", "double", "else", "enum",
                              "false", "float", "for", "if", "include", "int", "namespace", "nullptr", "return", "static", "std",
                              "struct", "switch", "template", "true", "uint64_t", "using", "void", "while"])
    case "py", "python":
      return Rules(comment: [#"#[^\n]*"#], string: [#"\"\"\"[\s\S]*?\"\"\""#, #""(?:\\.|[^"\\\n])*""#, #"'(?:\\.|[^'\\\n])*'"#],
                   keywords: ["and", "as", "class", "def", "elif", "else", "except", "False", "for", "from", "if", "import", "in",
                              "is", "lambda", "None", "not", "or", "pass", "raise", "return", "True", "try", "while", "with", "yield"])
    case "rb", "ruby":
      return Rules(comment: [#"#[^\n]*"#], string: [#""(?:\\.|[^"\\\n])*""#, #"'(?:\\.|[^'\\\n])*'"#],
                   keywords: ["alias", "and", "begin", "break", "case", "class", "def", "defined?", "do", "else", "elsif", "end", "ensure", "false", "for", "if", "in", "module", "next", "nil", "not", "or", "redo", "rescue", "retry", "return", "self", "super", "then", "true", "undef", "unless", "until", "when", "while", "yield"])
    case "nix":
      return Rules(comment: [#"#[^\n]*"#, #"/\*[\s\S]*?\*/"#], string: [#"''[\s\S]*?''"#, #""(?:\\.|[^"\\])*""#],
                   keywords: ["assert", "else", "if", "import", "in", "inherit", "let", "rec", "then", "with", "true", "false", "null"])
    case "fbs":
      return Rules(comment: [#"//[^\n]*"#], string: [#""(?:\\.|[^"\\\n])*""#],
                   keywords: ["attribute", "bool", "byte", "double", "enum", "file_identifier", "float", "include", "int", "long",
                              "namespace", "required", "root_type", "short", "string", "struct", "table", "ubyte", "uint", "ulong",
                              "union", "ushort"])
    case "json":
      return Rules(comment: [], string: [#""(?:\\.|[^"\\\n])*""#], keywords: ["true", "false", "null"])
    case "sh", "bash":
      return Rules(comment: [#"(?m)(?:^|\s)#[^\n]*"#], string: [#""(?:\\.|[^"\\])*""#, #"'[^']*'"#],
                   keywords: ["case", "do", "done", "echo", "elif", "else", "esac", "export", "fi", "for", "function", "if", "in",
                              "local", "then", "while"])
    default:
      return nil
    }
  }

  /// Applies colors to the whole text storage (texts here are Card sized):
  /// Tree-sitter for the languages it knows, the rules above otherwise.
  static func apply(_ storage: NSTextStorage, language: String, theme: EditorTheme, font: NSFont) {
    let full = NSRange(location: 0, length: storage.length)
    storage.beginEditing()
    storage.setAttributes([.font: font, .foregroundColor: theme.color(.plain)], range: full)
    if storage.length < 400_000, let spans = TreeSitterHighlighter.spans(storage.string, language: language) {
      for (range, token) in spans where range.upperBound <= storage.length {
        storage.addAttribute(.foregroundColor, value: theme.color(token), range: range)
      }
    } else if let r = rules(for: language), storage.length < 400_000 {
      let text = storage.string
      var claimed = IndexSet()
      func paint(_ pattern: String, _ token: SyntaxToken, group: Int = 0) {
        guard let re = try? NSRegularExpression(pattern: pattern) else { return }
        for m in re.matches(in: text, range: full) {
          let range = m.range(at: group)
          guard range.location != NSNotFound, !claimed.contains(integersIn: range.location..<(range.location + max(range.length, 1))) else { continue }
          storage.addAttribute(.foregroundColor, value: theme.color(token), range: range)
          if token == .comment || token == .string { claimed.insert(integersIn: range.location..<(range.location + range.length)) }
        }
      }
      for p in r.comment { paint(p, .comment) }
      for p in r.string { paint(p, .string) }
      paint(#"\b(?:0x[0-9a-fA-F]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\b"#, .number)
      if !r.keywords.isEmpty { paint("\\b(?:" + r.keywords.joined(separator: "|") + ")\\b", .keyword) }
      paint(#"\b([A-Za-z_][A-Za-z0-9_]*)\s*\("#, .function, group: 1)
    }
    storage.endEditing()
  }
}

/// One problem located in the edited text (byte offset into UTF-8).
struct EditorMarker: Equatable {
  var offset: UInt64
  var message: String
}

/// Code editor: monospaced, syntax colors, line numbers, indentation from the
/// settings, guides and markers for errors. Undo stays in the editor.
struct CodeEditor: NSViewRepresentable {
  @Binding var text: String
  var language = ""
  var editable = true
  var markers: [EditorMarker] = []
  /// Which document is shown (a file path). Selection and undo belong to a
  /// document: switching documents never carries them over.
  var documentID = ""

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
    // ⌘F finds in the file shown (the app's Find command opens this bar).
    tv.usesFindBar = true
    tv.isAutomaticQuoteSubstitutionEnabled = false
    tv.isAutomaticDashSubstitutionEnabled = false
    tv.isAutomaticTextReplacementEnabled = false
    tv.isAutomaticSpellingCorrectionEnabled = false
    tv.smartInsertDeleteEnabled = false
    tv.textContainerInset = NSSize(width: 4, height: 6)
    tv.drawsBackground = true
    tv.backgroundColor = .textBackgroundColor
    // The editor's area only: no insets for a title bar above it (the line
    // number column would otherwise reach under the title bar).
    scroll.automaticallyAdjustsContentInsets = false
    scroll.contentInsets = NSEdgeInsetsZero
    scroll.clipsToBounds = true
    scroll.hasVerticalRuler = true
    scroll.rulersVisible = true
    let ruler = LineNumberRuler(textView: tv)
    scroll.verticalRulerView = ruler
    tv.string = text
    tv.setSelectedRange(NSRange(location: 0, length: 0))
    context.coordinator.document = documentID
    configure(tv)
    return scroll
  }

  func updateNSView(_ scroll: NSScrollView, context: Context) {
    guard let tv = scroll.documentView as? GuideTextView else { return }
    let c = context.coordinator
    c.parent = self
    if c.document != documentID {
      // Another file: keep the old one's place, start the new one at its own.
      c.selections[c.document] = tv.selectedRanges.map(\.rangeValue)
      c.document = documentID
      tv.string = text
      tv.undoManager?.removeAllActions(withTarget: tv.textStorage as Any)
      tv.undoManager?.removeAllActions(withTarget: tv)
      Self.select(c.selections[documentID] ?? [NSRange(location: 0, length: 0)], in: tv)
      tv.scrollRangeToVisible(tv.selectedRange())
    } else if tv.string != text {
      let sel = tv.selectedRanges.map(\.rangeValue)
      tv.string = text
      Self.select(sel, in: tv)
    }
    configure(tv)
    (scroll.verticalRulerView as? LineNumberRuler)?.problemLines = markerLines(tv.string)
    scroll.verticalRulerView?.needsDisplay = true
  }

  /// Sets a selection that fits the current text (UTF-16 ranges). With no
  /// usable range left it places an empty cursor: NSTextView does not
  /// accept an empty list of ranges.
  static func select(_ ranges: [NSRange], in tv: NSTextView) {
    let s = tv.string as NSString
    let length = s.length
    var fitted: [NSValue] = []
    for r in ranges where r.location != NSNotFound {
      let loc = min(r.location, length)
      var range = NSRange(location: loc, length: min(r.length, length - loc))
      // Never split a character (surrogate pairs, combined sequences).
      if length > 0 { range = s.rangeOfComposedCharacterSequences(for: range) }
      fitted.append(NSValue(range: range))
    }
    if fitted.isEmpty {
      let loc = min(ranges.first.map { $0.location == NSNotFound ? 0 : $0.location } ?? 0, length)
      fitted = [NSValue(range: NSRange(location: loc, length: 0))]
    }
    tv.selectedRanges = fitted
  }

  private func configure(_ tv: GuideTextView) {
    let prefs = AppPreferences.shared
    let font = NSFont.monospacedSystemFont(ofSize: prefs.editorFontSize, weight: .regular)
    tv.isEditable = editable
    tv.indentWidth = prefs.indentWidth
    tv.showGuides = prefs.showIndentGuides
    tv.markerRanges = markers.map { characterRange(of: $0.offset, in: tv.string) }
    if let storage = tv.textStorage {
      SyntaxHighlighter.apply(storage, language: language, theme: prefs.editorTheme, font: font)
      for r in tv.markerRanges where r.location != NSNotFound {
        storage.addAttributes([.underlineStyle: NSUnderlineStyle.thick.rawValue | NSUnderlineStyle.patternDot.rawValue,
                               .underlineColor: NSColor.systemRed], range: r)
      }
    }
    tv.typingAttributes = [.font: font, .foregroundColor: prefs.editorTheme.color(.plain)]
    tv.needsDisplay = true
  }

  /// The line containing the byte offset, as a character range.
  private func characterRange(of byteOffset: UInt64, in s: String) -> NSRange {
    let utf8 = Array(s.utf8)
    let clamped = Int(min(byteOffset, UInt64(utf8.count)))
    let prefix = String(decoding: utf8[0..<clamped], as: UTF8.self)
    let loc = (prefix as NSString).length
    let line = (s as NSString).lineRange(for: NSRange(location: min(loc, (s as NSString).length), length: 0))
    return line
  }

  private func markerLines(_ s: String) -> [Int: String] {
    var out: [Int: String] = [:]
    for m in markers {
      let r = characterRange(of: m.offset, in: s)
      let line = (s as NSString).substring(to: r.location).components(separatedBy: "\n").count
      out[line] = m.message
    }
    return out
  }

  final class Coordinator: NSObject, NSTextViewDelegate {
    var parent: CodeEditor
    var document = ""
    /// Selection of each document seen in this editor.
    var selections: [String: [NSRange]] = [:]
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
        // Keep the indentation of the current line, one more after a block opener.
        let s = tv.string as NSString
        let line = s.lineRange(for: NSRange(location: tv.selectedRange().location, length: 0))
        let current = s.substring(with: line)
        let lead = current.prefix { $0 == " " || $0 == "\t" }
        let trimmed = current.trimmingCharacters(in: .whitespacesAndNewlines)
        let extra = trimmed.hasSuffix(":") || trimmed.hasSuffix("{") || trimmed.hasSuffix("[") || trimmed.hasSuffix("(") ? indent : ""
        tv.insertText("\n" + lead + extra, replacementRange: tv.selectedRange())
        return true
      }
      return false
    }
  }
}

/// Draws indentation guides and the background of lines with problems.
final class GuideTextView: NSTextView {
  var indentWidth = 4
  var showGuides = true
  var markerRanges: [NSRange] = []

  override func drawBackground(in rect: NSRect) {
    super.drawBackground(in: rect)
    guard let lm = layoutManager, let tc = textContainer, let font else { return }
    NSColor.systemRed.withAlphaComponent(0.1).setFill()
    for r in markerRanges where r.location != NSNotFound && r.upperBound <= (string as NSString).length {
      let glyphs = lm.glyphRange(forCharacterRange: r, actualCharacterRange: nil)
      var box = lm.boundingRect(forGlyphRange: glyphs, in: tc)
      box.origin.x = 0
      box.size.width = bounds.width
      box.origin.y += textContainerInset.height
      box.fill()
    }
    guard showGuides else { return }
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

/// Line numbers; lines with a problem get a red mark and the message as tooltip.
class LineNumberRuler: NSRulerView {
  weak var textView: NSTextView?
  var problemLines: [Int: String] = [:] {
    didSet { toolTip = problemLines.sorted { $0.key < $1.key }.map { "\($0.key): \($0.value)" }.joined(separator: "\n") }
  }

  init(textView: NSTextView) {
    self.textView = textView
    super.init(scrollView: textView.enclosingScrollView, orientation: .verticalRuler)
    ruleThickness = 38
    // Views draw outside their bounds unless told otherwise (macOS 14 and
    // later): the gutter would cover the editor and reach the title bar.
    clipsToBounds = true
    NotificationCenter.default.addObserver(forName: NSText.didChangeNotification, object: textView, queue: .main) { [weak self] _ in
      self?.needsDisplay = true
    }
    NotificationCenter.default.addObserver(forName: NSTextView.didChangeSelectionNotification, object: textView, queue: .main) { [weak self] _ in
      self?.needsDisplay = true
    }
    // The whole column on every scroll. When the text is only a little taller
    // than the view, AppKit asks for all of it at once, counts it as drawn and
    // asks for nothing more while scrolling, but drawing stops at the column's
    // height: the numbers would end at the last line shown before the scroll.
    if let clip = textView.enclosingScrollView?.contentView {
      clip.postsBoundsChangedNotifications = true
      NotificationCenter.default.addObserver(forName: NSView.boundsDidChangeNotification, object: clip, queue: nil) { [weak self] _ in
        self?.needsDisplay = true
      }
    }
  }

  required init(coder: NSCoder) { fatalError() }

  override func drawHashMarksAndLabels(in rect: NSRect) {
    guard let tv = textView, let lm = tv.layoutManager, let tc = tv.textContainer else { return }
    // Only inside the gutter itself (never beyond the scroll view).
    let area = bounds.intersection(visibleRect).intersection(rect)
    guard !area.isEmpty else { return }
    NSGraphicsContext.saveGraphicsState()
    defer { NSGraphicsContext.restoreGraphicsState() }
    NSBezierPath(rect: area).addClip()
    // A gutter of its own: calmer background and a line between numbers and code.
    (NSColor.textBackgroundColor.blended(withFraction: 0.06, of: .labelColor) ?? .windowBackgroundColor).setFill()
    area.fill()
    NSColor.separatorColor.setFill()
    NSRect(x: bounds.maxX - 1, y: area.minY, width: 1, height: area.height).fill()
    let cursorLine = (tv.string as NSString).substring(to: min(tv.selectedRange().location, (tv.string as NSString).length))
      .components(separatedBy: "\n").count
    let font = NSFont.monospacedDigitSystemFont(ofSize: max(9, (tv.font?.pointSize ?? 12) - 2), weight: .regular)
    let s = tv.string as NSString
    let visible = tv.visibleRect
    let glyphs = lm.glyphRange(forBoundingRect: visible, in: tc)
    let chars = lm.characterRange(forGlyphRange: glyphs, actualGlyphRange: nil)
    var line = s.substring(to: chars.location).components(separatedBy: "\n").count
    var index = chars.location
    let origin = convert(NSPoint.zero, from: tv)
    while index <= chars.upperBound && index <= s.length {
      let lineRange = s.lineRange(for: NSRange(location: index, length: 0))
      let g = lm.glyphRange(forCharacterRange: NSRange(location: lineRange.location, length: 0), actualCharacterRange: nil)
      let frag = lm.lineFragmentRect(forGlyphAt: min(g.location, max(0, lm.numberOfGlyphs - 1)), effectiveRange: nil)
      let y = frag.minY + tv.textContainerInset.height + origin.y
      let marked = problemLines[line] != nil
      let color: NSColor = marked ? .systemRed : (line == cursorLine ? .secondaryLabelColor : .tertiaryLabelColor)
      let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color]
      let label = "\(line)" as NSString
      let size = label.size(withAttributes: attrs)
      if marked {
        NSColor.systemRed.setFill()
        NSBezierPath(ovalIn: NSRect(x: 3, y: y + (frag.height - 6) / 2, width: 6, height: 6)).fill()
      }
      label.draw(at: NSPoint(x: ruleThickness - size.width - 5, y: y + (frag.height - size.height) / 2), withAttributes: attrs)
      if lineRange.upperBound == index || lineRange.upperBound >= s.length { break }
      index = lineRange.upperBound
      line += 1
    }
  }
}

/// A recorded input/output value as pretty JSON (decoded with its own BFBS).
func describeValue(_ v: Tsugumi_Data_NamedValueT, includeArtifact: Bool = true) -> String {
  var head = "\(v.name) : \(v.rootType)"
  if let n = v.sizeBytes { head += " · \(n) bytes" }
  if includeArtifact, let a = v.artifactId { head += String(localized: " · 成果物 ") + a }
  guard !v.inlineBytes.isEmpty else { return head + "\n" + String(localized: "（内容は記録されていません）") }
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
