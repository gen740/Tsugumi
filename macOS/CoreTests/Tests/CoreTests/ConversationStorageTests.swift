import Foundation
import Testing
import TsugumiRPC
@testable import TsugumiAgent

struct ConversationStorageTests {
  @Test func onlyMissingFilesStartNewConversations() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let file = dir.appendingPathComponent("chat.json")
    #expect(try Conversation.load(file).messages.isEmpty)
    let broken = Data("{broken history".utf8)
    try broken.write(to: file)
    #expect(throws: (any Error).self) { try Conversation.load(file) }
    #expect(try Data(contentsOf: file) == broken)
    #expect(throws: (any Error).self) { try Conversation.load(dir) }
  }

  @Test(arguments: [false, true]) func saveFailurePreventsSending(approvedRetry: Bool) async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: dir) }
    let storage = dir.appendingPathComponent("chat.json")
    let schema = try BFBS(data: try #require(RPCSchema.bfbs()))
    let transport = ApprovalTransport(schema: schema, storage: storage)
    let agent = try Agent(catalog: try ToolCatalog(schema: schema), transport: transport,
                          provider: .localFixture(port: 1), storage: storage)
    if approvedRetry {
      await agent.setApprover { asked in
        try? FileManager.default.removeItem(at: storage)
        try? FileManager.default.createDirectory(at: storage, withIntermediateDirectories: true)
        return Set(asked)
      }
    } else {
      try FileManager.default.createDirectory(at: storage, withIntermediateDirectories: true)
    }
    await #expect(throws: ConversationStorageError.self) {
      try await agent.runTool(id: "tool", name: "project_save", input: "{\"base_rev\":\"0\",\"changes\":[]}") { _ in }
    }
    #expect(transport.calls == (approvedRetry ? 1 : 0))
  }
}
