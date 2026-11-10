import Foundation
import Testing
@testable import TsugumiApp

/// Help > Learn about Tsugumi opens on a page that exists, and its list has the story
/// pages: the site's generator decides which they are.
@Test func theExplainerFindsItsPages() throws {
  #expect(Explainer.file(for: Explainer.start) != nil)
  let pages = Explainer.pages
  #expect(pages.contains("pages/problem.html"))
  #expect(pages.contains("pages/philosophy.html"))
  // A plain document of the site is not a story page, and is not listed.
  #expect(!pages.contains("pages/security.html"))
}

/// The scheme serves what is under the root, and nothing else.
@Test func theSchemeServesOnlyWhatIsBundled() {
  let url = { (s: String) in URL(string: "\(Explainer.scheme)://app/\(s)")! }
  #expect(Explainer.file(for: url("site/site.css")) != nil)
  #expect(Explainer.file(for: url("site/js/reader.js")) != nil)
  #expect(Explainer.file(for: url("no/such/file.html")) == nil)
  #expect(Explainer.file(for: url("pages")) == nil, "a directory is not a file")
  #expect(Explainer.file(for: url("../README.md")) == nil, "out of the root")
  #expect(Explainer.file(for: url("pages/../../README.md")) == nil, "out of the root")
  #expect(Explainer.file(for: URL(string: "https://example.com/site/site.css")!) == nil, "another scheme")
}
