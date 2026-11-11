import AppKit
import Markdown
import SwiftMath
import SwiftUI

/// Markdown with TeX math, drawn natively: the text with SwiftUI, the math
/// with CoreText (SwiftMath). `$…$` is inline math, `$$…$$` on lines of its
/// own is a displayed formula; TeX that cannot be typeset stays as written.
/// Everything is laid out at `fontSize`, so a canvas zoom draws it again at
/// the new size (never a scaled bitmap).
struct MarkdownView: View {
  let text: String
  var fontSize: CGFloat = 13
  var selectable = true
  /// Lines shown at most (nil: all), for one-line previews.
  var lineLimit: Int? = nil
  /// Code blocks with their language and a copy button (chat).
  var codeTools = false
  /// New blocks fade in (an answer being written).
  var fadeIn = false

  var body: some View {
    let doc = MarkdownDocument.parse(text)
    VStack(alignment: .leading, spacing: fontSize * 0.55) {
      ForEach(Array(doc.blocks.enumerated()), id: \.offset) { _, b in
        BlockView(block: b, fontSize: fontSize, lineLimit: lineLimit, codeTools: codeTools)
          .transition(.opacity.animation(.easeOut(duration: 0.25)))
      }
    }
    .animation(fadeIn ? .easeOut(duration: 0.25) : nil, value: doc.blocks.count)
    .modifier(SelectableText(on: selectable))
  }
}

private struct SelectableText: ViewModifier {
  let on: Bool
  func body(content: Content) -> some View {
    if on { content.textSelection(.enabled) } else { content.textSelection(.disabled) }
  }
}

private struct BlockView: View {
  @Environment(\.evidenceLinks) private var links
  let block: MarkdownDocument.Block
  let fontSize: CGFloat
  var lineLimit: Int?
  var codeTools = false

  var body: some View {
    switch block {
    case .paragraph(let inlines):
      InlineText.make(inlines, size: fontSize, links: links).lineLimit(lineLimit).fixedSize(horizontal: false, vertical: true)
    case .heading(let level, let inlines):
      let scale: CGFloat = level == 1 ? 1.5 : level == 2 ? 1.3 : level == 3 ? 1.15 : 1.0
      InlineText.make(inlines, size: fontSize * scale, bold: true, links: links)
        .padding(.top, fontSize * (level <= 2 ? 0.35 : 0.15))
        .fixedSize(horizontal: false, vertical: true)
    case .code(let code, let language) where codeTools:
      ChatCodeBlock(code: code.hasSuffix("\n") ? String(code.dropLast()) : code, language: language ?? "")
    case .code(let code, _):
      ScrollView(.horizontal, showsIndicators: false) {
        SwiftUI.Text(code.hasSuffix("\n") ? String(code.dropLast()) : code)
          .font(.system(size: fontSize * 0.9, design: .monospaced))
          .padding(fontSize * 0.55)
      }
      .background(Color.secondary.opacity(0.1), in: RoundedRectangle(cornerRadius: fontSize * 0.35))
    case .math(let tex):
      MathBlock(tex: tex, fontSize: fontSize * 1.15)
        .frame(maxWidth: .infinity)
    case .quote(let blocks):
      HStack(alignment: .top, spacing: fontSize * 0.6) {
        Rectangle().fill(Color.secondary.opacity(0.4)).frame(width: max(2, fontSize * 0.2))
        VStack(alignment: .leading, spacing: fontSize * 0.4) {
          ForEach(Array(blocks.enumerated()), id: \.offset) { _, b in BlockView(block: b, fontSize: fontSize, codeTools: codeTools) }
        }
        .foregroundStyle(.secondary)
      }
    case .list(let ordered, let start, let items):
      VStack(alignment: .leading, spacing: fontSize * 0.3) {
        ForEach(Array(items.enumerated()), id: \.offset) { i, item in
          HStack(alignment: .firstTextBaseline, spacing: fontSize * 0.45) {
            Group {
              if let done = item.check {
                Image(systemName: done ? "checkmark.square" : "square")
              } else {
                SwiftUI.Text(ordered ? "\(start + i)." : "•")
              }
            }
            .font(.system(size: fontSize).monospacedDigit())
            .foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: fontSize * 0.3) {
              ForEach(Array(item.blocks.enumerated()), id: \.offset) { _, b in BlockView(block: b, fontSize: fontSize, codeTools: codeTools) }
            }
          }
        }
      }
    case .table(let head, let rows):
      ScrollView(.horizontal, showsIndicators: false) {
        Grid(alignment: .leading, horizontalSpacing: fontSize, verticalSpacing: fontSize * 0.3) {
          GridRow { ForEach(Array(head.enumerated()), id: \.offset) { _, c in InlineText.make(c, size: fontSize, bold: true, links: links) } }
          Divider()
          ForEach(Array(rows.enumerated()), id: \.offset) { _, r in
            GridRow { ForEach(Array(r.enumerated()), id: \.offset) { _, c in InlineText.make(c, size: fontSize, links: links) } }
          }
        }
        .padding(.vertical, fontSize * 0.2)
      }
    case .rule:
      Divider()
    }
  }
}

/// A displayed formula; what cannot be typeset is shown as written.
private struct MathBlock: View {
  let tex: String
  let fontSize: CGFloat

  var body: some View {
    if let m = MathImages.image(tex, size: fontSize, display: true) {
      ScrollView(.horizontal, showsIndicators: false) {
        Image(nsImage: m.image).padding(.vertical, fontSize * 0.2)
      }
      .accessibilityLabel(tex)
    } else {
      SwiftUI.Text("$$" + tex + "$$").font(.system(size: fontSize * 0.85, design: .monospaced)).foregroundStyle(.secondary)
    }
  }
}

/// Inline content as one Text: runs of styled text, inline code, links and
/// inline math (an image on the text's baseline).
@MainActor
enum InlineText {
  static func make(_ inlines: [MarkdownDocument.Inline], size: CGFloat, bold: Bool = false, links: Set<String> = []) -> SwiftUI.Text {
    var out = SwiftUI.Text(verbatim: "")
    for i in inlines {
      out = SwiftUI.Text("\(out)\(piece(i, size: size, bold: bold, links: links))")
    }
    return out
  }

  private static func piece(_ i: MarkdownDocument.Inline, size: CGFloat, bold: Bool, links: Set<String>) -> SwiftUI.Text {
    switch i {
    case .text(let s, let style):
      var a = AttributedString(s)
      var font = Font.system(size: size, weight: bold || style.contains(.strong) ? .semibold : .regular)
      if style.contains(.emphasis) { font = font.italic() }
      a.font = font
      if style.contains(.strike) { a.strikethroughStyle = .single }
      return SwiftUI.Text(a)
    case .code(let s):
      var a = AttributedString(s)
      a.font = .system(size: size * 0.92, design: .monospaced)
      a.backgroundColor = Color.secondary.opacity(0.14)
      return SwiftUI.Text(a)
    case .link(let label, let url):
      var a = AttributedString(label)
      a.font = .system(size: size, weight: bold ? .semibold : .regular)
      if let u = URL(string: url), (["http", "https", "mailto"].contains(u.scheme?.lowercased() ?? "") || links.contains(url)) { a.link = u }
      else if EvidenceTarget.recognizes(url) { a.foregroundColor = Color.secondary }
      return SwiftUI.Text(a)
    case .math(let tex):
      if let m = MathImages.image(tex, size: size, display: false) {
        return SwiftUI.Text(Image(nsImage: m.image)).baselineOffset(-m.descent)
      }
      var a = AttributedString("$" + tex + "$")
      a.font = .system(size: size * 0.92, design: .monospaced)
      return SwiftUI.Text(a)
    case .lineBreak:
      return SwiftUI.Text(verbatim: "\n")
    }
  }
}

/// Typeset formulas, drawn when shown (resolution independent), kept per size.
@MainActor
enum MathImages {
  struct Typeset {
    let image: NSImage
    let descent: CGFloat
  }

  private static var cache: [String: Typeset?] = [:]

  /// Common commands the typesetter lacks, written with ones it has
  /// (\pmod, \bmod, \quad, \qquad).
  static func supported(_ tex: String) -> String {
    var t = tex
    let rules: [(String, String)] = [
      (#"\\pmod\s*\{([^{}]*)\}"#, #"\\ (\\mathrm{mod}\\ $1)"#),
      (#"\\pmod\s*([A-Za-z0-9])"#, #"\\ (\\mathrm{mod}\\ $1)"#),
      (#"\\bmod(?![A-Za-z])"#, #"\\ \\mathrm{mod}\\ "#),
      (#"\\qquad(?![A-Za-z])"#, #"\\ \\ \\ \\ \\ \\ \\ \\ "#),
      (#"\\quad(?![A-Za-z])"#, #"\\ \\ \\ \\ "#),
    ]
    for (p, r) in rules { t = t.replacingOccurrences(of: p, with: r, options: .regularExpression) }
    return t
  }

  static func image(_ tex: String, size: CGFloat, display: Bool) -> Typeset? {
    let k = "\(display ? "D" : "T")\(Int(size * 4))|\(tex)"
    if let hit = cache[k] { return hit }
    let label = MTMathUILabel()
    label.displayErrorInline = false
    label.labelMode = display ? .display : .text
    label.fontSize = size
    label.textColor = NSColor.labelColor
    label.latex = supported(tex)
    var made: Typeset?
    if label.error == nil {
      let fit = label.fittingSize
      label.frame = CGRect(origin: .zero, size: CGSize(width: ceil(fit.width), height: ceil(fit.height)))
      label.layout()
      if let line = label.displayList, fit.width > 0, fit.height > 0 {
        let img = NSImage(size: label.frame.size, flipped: false) { _ in
          guard let ctx = NSGraphicsContext.current?.cgContext else { return false }
          ctx.saveGState()
          line.draw(ctx)
          ctx.restoreGState()
          return true
        }
        made = Typeset(image: img, descent: line.descent)
      }
    }
    if cache.count > 2000 { cache.removeAll() }
    cache[k] = made
    return made
  }
}

/// A Markdown text as blocks and inlines (math taken out before parsing, so
/// TeX is never read as emphasis or escapes).
struct MarkdownDocument {
  struct Style: OptionSet, Hashable {
    let rawValue: Int
    static let emphasis = Style(rawValue: 1), strong = Style(rawValue: 2), strike = Style(rawValue: 4)
  }

  enum Inline: Equatable {
    case text(String, Style)
    case code(String)
    case link(String, String)
    case math(String)
    case lineBreak
  }

  struct ListItem: Equatable {
    var blocks: [Block]
    var check: Bool?
  }

  indirect enum Block: Equatable {
    case paragraph([Inline])
    case heading(Int, [Inline])
    case code(String, String?)
    case math(String)
    case quote([Block])
    case list(Bool, Int, [ListItem])
    case table([[Inline]], [[[Inline]]])
    case rule
  }

  var blocks: [Block]

  @MainActor private static var cache: [String: MarkdownDocument] = [:]

  @MainActor static func parse(_ text: String) -> MarkdownDocument {
    if let d = cache[text] { return d }
    let d = make(text)
    if cache.count > 500 { cache.removeAll() }
    cache[text] = d
    return d
  }

  static let open: Character = "\u{E000}", close: Character = "\u{E001}"

  static func make(_ text: String) -> MarkdownDocument {
    let (masked, math) = maskMath(text)
    let doc = Document(parsing: masked, options: [.disableSmartOpts])
    let w = Walker(math: math)
    return MarkdownDocument(blocks: doc.children.compactMap { w.block($0) })
  }

  /// Replaces `$$…$$` and `$…$` outside code with placeholders.
  static func maskMath(_ text: String) -> (String, [String]) {
    var out = ""
    var math: [String] = []
    let chars = Array(text)
    var i = 0
    var fence: String?
    var lineStart = true
    func placeholder(_ tex: String) -> String {
      math.append(tex)
      return "\(open)\(math.count - 1)\(close)"
    }
    while i < chars.count {
      let c = chars[i]
      if lineStart {
        // Fenced code blocks are left alone.
        var j = i
        while j < chars.count, chars[j] == " " { j += 1 }
        let rest = String(chars[j..<min(chars.count, j + 3)])
        if rest == "```" || rest == "~~~" {
          if fence == nil { fence = rest } else if fence == rest { fence = nil }
          while i < chars.count, chars[i] != "\n" { out.append(chars[i]); i += 1 }
          continue
        }
      }
      lineStart = c == "\n"
      if fence != nil { out.append(c); i += 1; continue }
      if c == "\\", i + 1 < chars.count, chars[i + 1] == "$" { out.append("\\$"); i += 2; continue }
      if c == "`" {
        // Inline code: copied up to the matching run of backticks.
        var n = 0
        while i + n < chars.count, chars[i + n] == "`" { n += 1 }
        let tick = String(repeating: "`", count: n)
        var j = i + n
        var found = -1
        while j + n <= chars.count {
          if String(chars[j..<j + n]) == tick { found = j; break }
          j += 1
        }
        let end = found >= 0 ? found + n : i + n
        out += String(chars[i..<end])
        i = end
        continue
      }
      if c == "$", i + 1 < chars.count, chars[i + 1] == "$" {
        if let e = find("$$", in: chars, from: i + 2) {
          let tex = String(chars[i + 2..<e]).trimmingCharacters(in: .whitespacesAndNewlines)
          out += "\n\n" + placeholder(tex) + "\n\n"
          i = e + 2
          continue
        }
      }
      if c == "$", i + 1 < chars.count, chars[i + 1] != " ", chars[i + 1] != "\n" {
        var j = i + 1
        while j < chars.count, chars[j] != "$", chars[j] != "\n" { j += 1 }
        if j < chars.count, chars[j] == "$", chars[j - 1] != " ", j > i + 1 {
          // "$5 and $6" is money, not math: a closing $ followed by a digit is not one.
          if !(j + 1 < chars.count && chars[j + 1].isNumber) {
            out += placeholder(String(chars[i + 1..<j]))
            i = j + 1
            continue
          }
        }
      }
      out.append(c)
      i += 1
    }
    return (out, math)
  }

  private static func find(_ s: String, in chars: [Character], from: Int) -> Int? {
    let p = Array(s)
    var i = from
    while i + p.count <= chars.count {
      if Array(chars[i..<i + p.count]) == p { return i }
      i += 1
    }
    return nil
  }

  private struct Walker {
    let math: [String]

    func block(_ m: Markup) -> Block? {
      switch m {
      case let p as Paragraph:
        let inl = inlines(p.children, [])
        if inl.count == 1, case .math(let tex) = inl[0], displayed(p) { return .math(tex) }
        return .paragraph(inl)
      case let h as Heading: return .heading(h.level, inlines(h.children, []))
      case let c as Markdown.CodeBlock: return .code(c.code, c.language)
      case let q as BlockQuote: return .quote(q.children.compactMap { block($0) })
      case let l as UnorderedList:
        return .list(false, 1, l.listItems.map { ListItem(blocks: $0.children.compactMap { block($0) }, check: $0.checkbox.map { $0 == .checked }) })
      case let l as OrderedList:
        let first: UInt = l.startIndex
        return .list(true, Int(first), l.listItems.map { ListItem(blocks: $0.children.compactMap { block($0) }, check: $0.checkbox.map { $0 == .checked }) })
      case let t as Markdown.Table:
        let head = t.head.cells.map { inlines($0.children, []) }
        var rows: [[[Inline]]] = []
        for r in t.body.rows { rows.append(r.cells.map { inlines($0.children, []) }) }
        return .table(Array(head), rows)
      case is ThematicBreak: return .rule
      case let h as HTMLBlock: return .code(h.rawHTML, "html")
      default:
        let inl = inlines(m.children, [])
        return inl.isEmpty ? nil : .paragraph(inl)
      }
    }

    /// A paragraph that is only a `$$…$$` placeholder.
    private func displayed(_ p: Paragraph) -> Bool {
      p.childCount == 1 && (p.child(at: 0) as? Markdown.Text).map { t in
        let s = t.string.trimmingCharacters(in: .whitespaces)
        return s.first == MarkdownDocument.open && s.last == MarkdownDocument.close
      } ?? false
    }

    func inlines(_ children: some Sequence<Markup>, _ style: Style) -> [Inline] {
      var out: [Inline] = []
      for c in children {
        switch c {
        case let t as Markdown.Text: out += split(t.string, style)
        case let e as Emphasis: out += inlines(e.children, style.union(.emphasis))
        case let s as Strong: out += inlines(s.children, style.union(.strong))
        case let s as Strikethrough: out += inlines(s.children, style.union(.strike))
        case let code as InlineCode: out.append(.code(code.code))
        case let l as Markdown.Link:
          let label = inlines(l.children, style).map { i -> String in
            if case .text(let s, _) = i { return s }
            if case .code(let s) = i { return s }
            return ""
          }.joined()
          out.append(.link(label, l.destination ?? ""))
        case is SoftBreak: out.append(.text(" ", style))
        case is LineBreak: out.append(.lineBreak)
        case let h as InlineHTML: out.append(.text(h.rawHTML, style))
        case let img as Markdown.Image: out.append(.text(img.plainText, style))
        default: out += inlines(c.children, style)
        }
      }
      return out
    }

    /// Puts the math placeholders of a text run back as math.
    private func split(_ s: String, _ style: Style) -> [Inline] {
      guard s.contains(MarkdownDocument.open) else { return [.text(s, style)] }
      var out: [Inline] = []
      var buf = ""
      var it = s.makeIterator()
      while let c = it.next() {
        if c == MarkdownDocument.open {
          var n = ""
          while let d = it.next(), d != MarkdownDocument.close { n.append(d) }
          if !buf.isEmpty { out.append(.text(buf, style)); buf = "" }
          if let i = Int(n), i < math.count { out.append(.math(math[i])) }
        } else {
          buf.append(c)
        }
      }
      if !buf.isEmpty { out.append(.text(buf, style)) }
      return out
    }
  }
}
