import Testing
@testable import TsugumiAgent

struct CompactionTests {
  @Test func recentToolCallsAndResultsStayTogether() {
    var messages = [ChatMessage(role: "user", content: [.text("task")])]
    for i in 1...3 {
      messages.append(ChatMessage(role: "assistant", content: [.toolUse(id: "t\(i)", name: "run", input: "{}")]))
      messages.append(ChatMessage(role: "user", content: [.toolResult(toolUseId: "t\(i)", content: "RESULT_\(i)", isError: false)]))
    }
    #expect(Agent.compactionBoundary(messages) == 3)
    messages.append(ChatMessage(role: "assistant", content: [.text("done")]))
    // Keeping exactly four would start with the second tool's result.
    #expect(Agent.compactionBoundary(messages) == 3)
  }
}
