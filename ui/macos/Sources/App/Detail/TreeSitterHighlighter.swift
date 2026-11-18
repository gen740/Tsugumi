import Foundation
import SwiftTreeSitter
import TreeSitterBash
import TreeSitterC
import TreeSitterCPP
import TreeSitterFBS
import TreeSitterGo
import TreeSitterJSON
import TreeSitterJulia
import TreeSitterNix
import TreeSitterPython
import TreeSitterR
import TreeSitterRust

/// Syntax colors from Tree-sitter grammars and their highlight queries
/// (Resources/TreeSitter). Part of the app only: Cards and SDKs never use it.
enum TreeSitterHighlighter {
  private struct Grammar {
    let language: Language
    let query: Query
  }

  /// Grammar name for a file extension or language name.
  static func grammarName(_ language: String) -> String? {
    switch language.lowercased() {
    case "cc", "hh", "cpp", "hpp", "cxx", "h": return "cpp"
    case "c": return "c"
    case "py", "python": return "python"
    case "nix": return "nix"
    case "fbs": return "fbs"
    case "json": return "json"
    case "sh", "bash": return "bash"
    case "go": return "go"
    case "rs", "rust": return "rust"
    case "jl", "julia": return "julia"
    case "r": return "r"
    default: return nil
    }
  }

  private static let lock = NSLock()
  nonisolated(unsafe) private static var cache: [String: Grammar?] = [:]

  private static func grammar(_ name: String) -> Grammar? {
    lock.lock()
    defer { lock.unlock() }
    if let g = cache[name] { return g }
    let pointer: OpaquePointer?
    switch name {
    case "cpp": pointer = tree_sitter_cpp()
    case "c": pointer = tree_sitter_c()
    case "python": pointer = tree_sitter_python()
    case "nix": pointer = tree_sitter_nix()
    case "fbs": pointer = tree_sitter_fbs()
    case "json": pointer = tree_sitter_json()
    case "bash": pointer = tree_sitter_bash()
    case "go": pointer = tree_sitter_go()
    case "rust": pointer = tree_sitter_rust()
    case "julia": pointer = tree_sitter_julia()
    case "r": pointer = tree_sitter_r()
    default: pointer = nil
    }
    var g: Grammar?
    if let pointer, let url = Bundle.module.url(forResource: name, withExtension: "scm", subdirectory: "TreeSitter"),
       let data = try? Data(contentsOf: url) {
      let language = Language(pointer)
      if let query = try? Query(language: language, data: data) { g = Grammar(language: language, query: query) }
    }
    cache[name] = g
    return g
  }

  /// Colored spans (UTF-16 ranges) of `text`, or nil when no grammar applies.
  static func spans(_ text: String, language: String) -> [(NSRange, SyntaxToken)]? {
    guard let name = grammarName(language), let g = grammar(name) else { return nil }
    let parser = Parser()
    guard (try? parser.setLanguage(g.language)) != nil, let tree = parser.parse(text) else { return nil }
    let cursor = g.query.execute(in: tree)
    // Less specific captures come first, so later (more specific) ones win.
    return cursor.resolve(with: .init(string: text)).highlights().compactMap { h in
      token(h.nameComponents).map { (h.range, $0) }
    }
  }

  /// Capture names of the highlight queries to the editor's token colors.
  static func token(_ name: [String]) -> SyntaxToken? {
    switch name.first {
    case "comment": return .comment
    case "string", "escape", "character": return .string
    case "number", "float": return .number
    case "boolean", "constant": return .constant
    case "keyword", "conditional", "repeat", "include", "exception", "storageclass", "label": return .keyword
    case "function", "method", "constructor": return .function
    case "type", "namespace", "module": return .type
    case "property", "field", "attribute", "tag": return .property
    case "variable" where name.count > 1 && name[1] == "builtin": return .keyword
    default: return nil
    }
  }
}
