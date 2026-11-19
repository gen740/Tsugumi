import AppKit
import Foundation
import Testing
@testable import TsugumiAgentClient
@testable import TsugumiApp
import TsugumiLauncher
import TsugumiRPC
import TsugumiTestSupport

@MainActor struct ChatStateTests {
  private struct Transport: ToolTransport {
    func project() async throws -> AgentProject? { nil }
  }

  private func session(at directory: URL) throws -> ProjectSession {
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    return try offlineSession(directory, cards: [])
  }

  nonisolated private func row(_ text: String) -> Tsugumi_Agent_ChatRowT {
    let row = Tsugumi_Agent_ChatRowT()
    row.id = "message"; row.kind = "text"; row.role = "assistant"; row.text = text
    return row
  }

  nonisolated private func snapshot(_ id: String, _ text: String) -> Tsugumi_Agent_ConversationResultT {
    let result = Tsugumi_Agent_ConversationResultT()
    result.conversation.id = id
    result.conversation.rows = [row(text)]
    return result
  }

  @Test func theModelMenuFollowsTheChatsSelectedProvider() throws {
    let dir = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: dir) }
    var selected = ProviderConfig.chatGPT
    let chat = ChatController(session: try session(at: dir), connection: { selected }, loadOnInit: false)
    selected.model = "new-model"
    #expect(chat.displayedProvider.auth == .chatGPT)
    let menu = ModelMenu()
    menu.setContents(connections: [.init(id: "chatgpt", name: "ChatGPT", models: ["old-model", "new-model"])],
      provider: "chatgpt", model: chat.displayedProvider.model, names: [:])
    #expect(menu.menu.items.contains { $0.state == .on && $0.title == "new-model" })
  }

  @Test func compactionKeepsTheStreamedTranscript() async throws {
    let dir = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: dir) }
    let chat = ChatController(session: try session(at: dir), connection: { .fixture }, loadOnInit: false,
      makeAgent: { id, _, provider in
        Agent(id: id, transport: Transport(), provider: provider) { method, _ in
          guard method == "ConversationCreateRequest" else { throw RPCError.unexpectedReply(method) }
          return snapshot(id, "Cached reply")
        }
      })
    chat.select("chat")
    await chat.opening?.value
    #expect(chat.items.last?.text == "Cached reply")

    let streamed = snapshot("chat", "Streamed reply").conversation
    chat.handle(.presentation(try ServiceWire.conversation(streamed).rows), of: chat.answer)
    chat.handle(.compacting, of: chat.answer)
    #expect(chat.status != nil)
    chat.handle(.compacted, of: chat.answer)
    #expect(chat.status == nil && chat.items.last?.text == "Streamed reply")
  }

  @Test func failedAndCancelledSelectionsKeepTheVisibleConversation() async throws {
    let dir = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: dir) }
    let entered = AsyncStream<Void>.makeStream()
    let release = AsyncStream<Void>.makeStream()
    defer { entered.continuation.finish(); release.continuation.finish() }
    let chat = ChatController(session: try session(at: dir), connection: { .fixture }, loadOnInit: false,
      makeAgent: { id, _, provider in
        Agent(id: id, transport: Transport(), provider: provider) { method, _ in
          guard method == "ConversationCreateRequest" else { throw RPCError.unexpectedReply(method) }
          if id == "missing" { throw RPCError.unexpectedReply(id) }
          if id == "slow" {
            entered.continuation.yield(())
            for await _ in release.stream {}
          }
          return snapshot(id, id)
        }
      })
    chat.select("selected")
    await chat.opening?.value
    let visible = chat.items.map(\.text)
    try #require(visible == ["selected"])
    chat.select("missing")
    #expect(chat.items.map(\.text) == visible)
    await chat.opening?.value
    #expect(!chat.ready && !chat.loading && chat.status != nil)
    #expect(chat.items.map(\.text) == visible)

    chat.select("slow")
    let cancelled = chat.opening
    var receives = entered.stream.makeAsyncIterator()
    _ = await receives.next()
    chat.select("selected")
    await chat.opening?.value
    release.continuation.finish()
    await cancelled?.value
    #expect(chat.ready && chat.status == nil)
    #expect(chat.items.map(\.text) == visible)
  }

  @Test func aReplacementSessionRestoresTheSelectedChat() async throws {
    let dir = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: dir) }
    let initial = try session(at: dir)
    let replacement = try session(at: dir)
    func make(_ session: ProjectSession, _ previous: ChatController?) -> ChatController {
      ChatController(session: session, restoring: previous, connection: { .fixture }, loadOnInit: false,
        makeAgent: { id, _, provider in
          Agent(id: id, transport: Transport(), provider: provider) { method, _ in
            guard method == "ConversationCreateRequest" else { throw RPCError.unexpectedReply(method) }
            return snapshot(id, "Selected transcript")
          }
        })
    }

    let first = make(initial, nil)
    first.select("selected")
    await first.opening?.value
    try #require(first.items.map(\.text) == ["Selected transcript"])
    let reopened = make(replacement, first)
    #expect(reopened !== first && reopened.session === replacement)
    #expect(reopened.currentId == "selected" && reopened.items.map(\.text) == first.items.map(\.text))
    await reopened.opening?.value
    #expect(reopened.ready && reopened.currentId == "selected")
  }
}
