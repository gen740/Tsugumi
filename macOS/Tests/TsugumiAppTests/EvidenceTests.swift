import Foundation
import Testing
import TsugumiRPC
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

@Test func evidenceDecodesTheCitedDisplayWithoutUsingTheLiveResult() throws {
  let text = Tsugumi_Data_DisplayTextT()
  text.text = "recorded photons: 157471"
  let recorded = Tsugumi_Data_DisplayT()
  recorded.runId = "ff54ac39-ffe0-4357-a04a-cd55ed374ba9"
  recorded.invocation = 1
  recorded.revision = 7
  recorded.value = Tsugumi_Data_DisplayValueUnion(text, type: .displaytext)
  let snapshot = try #require(recordedDisplay(recorded))
  #expect(snapshot.runId == recorded.runId)
  #expect(snapshot.invocation == 1)
  #expect(snapshot.revision == 7)
  guard case .text(let value) = snapshot.value else { Issue.record("Expected recorded text"); return }
  #expect(value == "recorded photons: 157471")
  if case .unavailable? = recordedDisplay(Tsugumi_Data_DisplayT())?.value {} else { Issue.record("Malformed display needs an unavailable state") }
}
