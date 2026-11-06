import FlatBuffers
import Foundation
import Testing
@testable import TsugumiAgent
import TsugumiRPC

@Suite(.serialized) struct ToolImageTests {
  private static let png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"

  /// The same cases as the web Agent (tests/fixtures/agent-behavior.json).
  @Test func imagesLeaveTheTextOfAResult() throws {
    for c in try agentBehaviorCases("toolImages") {
      let result = try #require(c["result"] as? String)
      let (text, images) = ToolImage.split(result)
      if let expected = c["text"] as? [String: Any] {
        let got = try JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]
        #expect(got.map { NSDictionary(dictionary: $0) } == NSDictionary(dictionary: expected), "\(c["name"]!)")
      } else {
        #expect(text == result, "\(c["name"]!)")
      }
      let wanted = try JSONDecoder().decode([ToolImage].self, from: JSONSerialization.data(withJSONObject: c["images"]!))
      #expect(images == wanted, "\(c["name"]!)")
    }
  }

  @Test func anImageTooLargeIsNotSent() throws {
    var bytes = Data(base64Encoded: Self.png)!
    bytes.insert(contentsOf: Data(count: 8 << 20), at: 33)
    let result = #"{"data":{"base64":"\#(bytes.base64EncodedString())"}}"#
    let (text, images) = ToolImage.split(result)
    #expect(images.isEmpty && text.contains("too large to show") && text.count < 100)
  }

  /// A long image no longer fills the text, and the block still carries it.
  @Test func anImageIsNotClippedWithTheText() {
    var bytes = Data(base64Encoded: Self.png)!
    bytes.insert(contentsOf: Data(count: 200_000), at: 33)
    let b64 = bytes.base64EncodedString()
    guard case .toolResult(_, let content, _, let images) = Agent.resultBlock(id: "t", result: #"{"data":{"base64":"\#(b64)"}}"#, isError: false) else {
      Issue.record("not a tool result"); return
    }
    #expect(images.first?.data == b64 && !content.contains("cut:"))
  }

  @Test func bothProvidersGetTheImage() throws {
    let block = ContentBlock.toolResult(toolUseId: "t", content: "text", isError: false, images: [ToolImage(mediaType: "image/png", data: "AAAA")])
    let parts = try #require(block.wire["content"] as? [[String: Any]])
    #expect(parts[0]["text"] as? String == "text")
    #expect((parts[1]["source"] as? [String: Any])?["data"] as? String == "AAAA")
    let plain = ContentBlock.toolResult(toolUseId: "t", content: "text", isError: false)
    #expect(plain.wire["content"] as? String == "text")
    let input = MessagesProvider.responsesInput([ChatMessage(role: "user", content: [block])], namespace: nil)
    let output = try #require(input[0]["output"] as? [[String: Any]])
    #expect(output[0]["text"] as? String == "text")
    #expect(output[1]["image_url"] as? String == "data:image/png;base64,AAAA")
  }

  @Test func theConversationKeepsItsImages() throws {
    let message = ChatMessage(role: "user", content: [.toolResult(toolUseId: "t", content: "x", isError: false, images: [ToolImage(mediaType: "image/png", data: "AAAA")])])
    let again = try JSONDecoder().decode(ChatMessage.self, from: JSONEncoder().encode(message))
    #expect(again == message)
  }

  private func looksAtPicture(_ ask: String) async throws -> (String, Conversation) {
    var h = AgentHarness()
    // Before the start: tsugumid reads the project when it opens it.
    let file = h.dir.appendingPathComponent("project/definitions/src/picture.png")
    try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
    try Data(base64Encoded: Self.png)!.write(to: file)
    try h.start()
    defer { h.stop() }
    let client = RPCClient(endpoint: h.endpoint!)
    _ = try await client.connect(token: h.token, clientId: UUID(), expectedProjectId: nil)
    let agent = try makeAgent(h, client)
    let log = EventLog()
    await agent.send(ask, context: []) { log.add($0) }
    await waitFinished(log)
    let conversation = await agent.conversation
    await client.close()
    return (log.text, conversation)
  }

  @Test func theModelSeesAPictureItReads() async throws {
    let (text, conversation) = try await looksAtPicture("look at the picture definitions/src/picture.png")
    #expect(text.contains(#"Saw images: ['image/png']. Data in the text: False."#), "\(text)")
    #expect(conversation.hasToolImages)
  }

  /// A provider that refuses an image would refuse every later request of the
  /// conversation too: the images go, and the model is told.
  @Test func aRefusedImageDoesNotStopTheConversation() async throws {
    let (text, conversation) = try await looksAtPicture("look at the picture definitions/src/picture.png, which is refused")
    #expect(text.contains("Saw images: []. Data in the text: False. Told it was refused: True."), "\(text)")
    #expect(!conversation.hasToolImages)
  }
}
