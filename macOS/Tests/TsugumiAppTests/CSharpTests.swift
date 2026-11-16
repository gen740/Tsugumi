import AppKit
import Foundation
import Testing
import TsugumiTestSupport
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

@MainActor @Test func csharpIsOfferedAndHasSyntaxHighlighting() {
  #expect(BuiltinCardItems.written.contains(.csharp))
  #expect(SyntaxHighlighter.rules(for: "cs")?.keywords.contains("await") == true)
  #expect(SyntaxHighlighter.rules(for: "csharp")?.keywords.contains("sealed") == true)
}
