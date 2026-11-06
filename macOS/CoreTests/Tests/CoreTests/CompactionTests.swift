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

  @Test func toolResultsAreCutToFitTheRoom() throws {
    for c in try agentBehaviorCases("fitToolResults") {
      let sizes = c["sizes"] as! [Int]
      let messages = sizes.enumerated().map { i, n in
        ChatMessage(role: "user", content: [.toolResult(toolUseId: "t\(i)", content: String(repeating: "r", count: n), isError: false)])
      }
      let fitted = Agent.fitToolResults(messages, room: c["room"] as! Int)
      let lengths = fitted.flatMap { $0.content.compactMap { b -> Int? in if case .toolResult(_, let s, _) = b { return s.count }; return nil } }
      #expect(lengths == c["expect"] as! [Int], "\(c["name"]!)")
      // Cut again with the same room: nothing changes.
      #expect(Agent.fitToolResults(fitted, room: c["room"] as! Int) == fitted, "\(c["name"]!)")
    }
  }
}
