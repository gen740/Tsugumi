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
    let file: String?
    let lineStart: Int?
    let lineEnd: Int?
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
      #expect(target?.file == c.file)
      #expect(target?.lineStart == c.lineStart)
      #expect(target?.lineEnd == c.lineEnd)
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
  #expect(recordedDisplay(Tsugumi_Data_DisplayT()) == nil)
}

@Test func findingParagraphsKeepExactSourceLocations() throws {
  let values = ChatFinding.list("Read only: [source](card:outer/inner?file=src%2Fmain.py&lines=12-18) uses a clock.\n\nOther prose.")
  #expect(values.count == 1)
  #expect(values.first?.target.card == ["outer", "inner"])
  #expect(values.first?.target.file == "src/main.py")
  #expect(values.first?.target.lineStart == 12)
  #expect(values.first?.target.lineEnd == 18)
}

@Test func keptFindingsRemainCommentsWithoutBecomingChangeRequests() {
  let meta = JSONValue.object([("comments", .array([
    .object([("id", .string("finding")), ("text", .string("Read only: clock")), ("finding", .bool(true))]),
    .object([("id", .string("request")), ("text", .string("Use an explicit time input"))])]))])
  let comments = CardComment.list(in: meta)
  #expect(comments.count == 2)
  #expect(comments.filter { !$0.resolved && !$0.finding }.map(\.id) == ["request"])
}
