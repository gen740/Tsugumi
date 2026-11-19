import AppKit
import Foundation
import Testing

@testable import TsugumiApp

/// Exercise each language's grammar and bundled query with real source.
@Test(arguments: ["go", "Go"])
func treeSitterColorsGo(language: String) {
  expectTokens("// note\npackage main\n", language, ["comment:// note", "keyword:package"])
}

@Test(arguments: ["rs", "rust"])
func treeSitterColorsRust(language: String) {
  expectTokens(#"fn main() { let s = "hi"; }"#, language, ["keyword:fn", #"string:"hi""#])
}

@Test(arguments: ["jl", "julia"])
func treeSitterColorsJulia(language: String) {
  expectTokens("# note\nfunction f()\n    return 1.5\nend", language, ["comment:# note", "number:1.5"])
}

@Test(arguments: ["r", "R"])
func treeSitterColorsR(language: String) {
  expectTokens("# note\nif (TRUE) print(\"hi\")", language, ["comment:# note", #"string:"hi""#])
}

private func expectTokens(_ source: String, _ language: String, _ expected: Set<String>) {
  let tokens = (TreeSitterHighlighter.spans(source, language: language) ?? []).map { range, token in
    "\(token):" + (source as NSString).substring(with: range)
  }
  #expect(expected.isSubset(of: Set(tokens)))
}


@MainActor @Test(arguments: ["cs", "csharp"])
func syntaxHighlighterColorsCSharp(language: String) {
  let source = "// await\nawait Work();"
  let storage = NSTextStorage(string: source)
  let theme = EditorTheme.standard
  SyntaxHighlighter.apply(storage, language: language, theme: theme, font: .monospacedSystemFont(ofSize: 12, weight: .regular))
  let keyword = (source as NSString).range(of: "await", options: .backwards)
  #expect(storage.attribute(.foregroundColor, at: keyword.location, effectiveRange: nil) as? NSColor == theme.color(.keyword))
  #expect(storage.attribute(.foregroundColor, at: 3, effectiveRange: nil) as? NSColor == theme.color(.comment))
}
