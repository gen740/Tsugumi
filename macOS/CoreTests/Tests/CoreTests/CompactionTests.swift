import Foundation
import Testing
@testable import TsugumiAgent

struct CompactionTests {
  @Test func recentToolCallsAndResultsStayTogether() throws {
    for c in try agentBehaviorCases("compaction") {
      let messages = try JSONDecoder().decode([ChatMessage].self, from: JSONSerialization.data(withJSONObject: c["messages"]!))
      let cut = Agent.compactionBoundary(messages)
      #expect(cut == c["cut"] as? Int, "\(c["name"]!)")
      #expect(Array(messages.prefix(cut)) + Array(messages.dropFirst(cut)) == messages)
      if cut < messages.count {
        #expect(!messages[cut].content.contains { if case .toolResult = $0 { return true }; return false })
      }
    }
  }
}
