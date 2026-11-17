import Foundation
import Testing
@testable import TsugumiApp

struct ToolRowTests {
  @Test @MainActor func searchResultsKeepMarkupLiteral() {
    let text = "[**Injected title**](javascript:alert(1)) <b>HTML</b>\nhttps://example.com/source"
    let linked = ToolRow.linked(text)
    #expect(String(linked.characters) == text)
    let links = linked.runs.compactMap { run -> (String, URL)? in
      guard let url = run.link else { return nil }
      return (String(linked[run.range].characters), url)
    }
    #expect(links.count == 1)
    #expect(links.first?.0 == "https://example.com/source")
    #expect(links.first?.1 == URL(string: "https://example.com/source"))
    #expect(linked.runs.allSatisfy { $0.inlinePresentationIntent == nil && $0.presentationIntent == nil })
  }

  @Test @MainActor func searchResultsLinkOnlyHTTPAndHTTPS() {
    let text = "http://example.com/plain\nhttps://example.com/secure\nfile:///etc/passwd\nmailto:person@example.com\njavascript:alert(1)"
    let linked = ToolRow.linked(text)
    #expect(String(linked.characters) == text)
    #expect(linked.runs.compactMap(\.link) == [URL(string: "http://example.com/plain")!, URL(string: "https://example.com/secure")!])
  }
}
