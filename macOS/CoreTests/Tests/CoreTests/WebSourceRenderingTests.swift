import Foundation
import Testing
@testable import TsugumiAgentClient

struct WebSourceRenderingTests {
  @Test func sourcesAreSafeLinks() {
    let text = WebSearch.sourcesText([
      WebSource(url: "javascript:alert(1)", title: "x"),
      WebSource(url: "https://a.example/b)c", title: "a]b\nc"),
      WebSource(url: "https://a.example/b)c"),
    ])
    #expect(!text.contains("javascript:"), "\(text)")
    #expect(text.components(separatedBy: "https:").count == 2, "\(text)")
    #expect(text.contains("[a b c](https://a.example/b%29c)"), "\(text)")
    #expect(WebSearch.sourcesText([WebSource(url: "file:///etc/passwd")]).isEmpty)
  }
}
