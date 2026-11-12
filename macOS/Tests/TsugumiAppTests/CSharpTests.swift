import AppKit
import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

@MainActor @Test func csharpCreationUsesTheSharedScaffold() async throws {
  let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("csharp-ui-\(UUID().uuidString)")
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  let document = #"{"name":"csharp","canvas":{"cards":[],"connections":[]}}"#
  try Data(document.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let session = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  session.project = try JSONValue.parse(document)
  let card = session.addCard(template: .csharp, in: [], at: .zero)
  #expect(session.cardDefinition(card)?["language"]?.string == "csharp")
  #expect(session.kind(card) == .code)
  let folder = try #require(session.definitionDir(card))
  for (name, text) in CSharpTemplate.files where name != "_card.json" {
    let actual = try await session.sourceText(folder + name)
    #expect(actual == text)
  }
  #expect(SyntaxHighlighter.rules(for: "cs")?.keywords.contains("await") == true)
  #expect(SyntaxHighlighter.rules(for: "csharp")?.keywords.contains("sealed") == true)
}
