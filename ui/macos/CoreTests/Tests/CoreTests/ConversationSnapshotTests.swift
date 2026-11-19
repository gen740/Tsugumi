import Foundation
import Testing
import TsugumiRPC
@testable import TsugumiAgentClient

struct ConversationSnapshotTests {
  @Test func nativeSnapshotUsesHostPresentationAndUsage() throws {
    let source = Tsugumi_Agent_ConversationT()
    source.id = "conversation"
    let row = Tsugumi_Agent_ChatRowT()
    row.id = "reply"
    row.kind = "text"
    row.text = "A reply with a citation."
    source.rows = [row]
    let counts = Tsugumi_Service_UsageT()
    counts.inputTokens = 3
    counts.outputTokens = 4
    source.usage = counts
    let conversation = try ServiceWire.conversation(source)
    #expect(conversation.id == source.id)
    #expect(conversation.rows.first?.text == row.text)
    #expect(conversation.usage.inputTokens == 3 && conversation.usage.outputTokens == 4)
  }
}
