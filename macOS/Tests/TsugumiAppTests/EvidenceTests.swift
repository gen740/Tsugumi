import Foundation
import Testing
@testable import TsugumiApp

@Test func evidenceAddressesAgreeAcrossMacAndWeb() throws {
  struct Case: Decodable {
    let href: String
    let kind: String?
    let card: [String]?
    let invocation: String?
    let offset: String?
    let artifact: String?
    let member: String?
  }
  let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../..")
  let cases = try JSONDecoder().decode([Case].self, from: Data(contentsOf: root.appendingPathComponent("tests/fixtures/evidence-links.json")))
  for c in cases {
    let target = EvidenceTarget.parse(c.href)
    #expect(target?.kind == c.kind, "\(c.href)")
    if c.kind != nil {
      #expect(target?.card == c.card)
      #expect(target?.invocation.map(String.init) == c.invocation)
      if c.kind == "log" { #expect(target.map { String($0.offset) } == c.offset) }
      #expect(target?.artifact == c.artifact)
      #expect(target?.member == c.member)
    }
  }
}

@MainActor @Test func evidenceLinksAreDiscoveredInNestedMarkdownWithoutReadingCode() {
  let document = MarkdownDocument.make("""
  > [Card](card:spectrum)

  - [Run](run:12345678-1234-1234-1234-123456789abc)

  `[ignored](card:code)`
  """)
  #expect(EvidenceMarkup.links(document.blocks) == ["card:spectrum", "run:12345678-1234-1234-1234-123456789abc"])
}
