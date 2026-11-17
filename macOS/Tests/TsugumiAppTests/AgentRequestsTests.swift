import Foundation
import Testing
@testable import TsugumiAgentClient

struct AgentRequestsTests {
  @Test func generationButtonsSubstituteScopeAndCardList() throws {
    let shared = try #require(JSONSerialization.jsonObject(with: Data(Shared.agentJSON.utf8)) as? [String: Any])
    let location = try #require(shared["draftRequest"] as? [String: String])
    for kind in ["draftRequest", "commentRequest"] {
      let request = try #require(shared[kind] as? [String: String])
      for scope in [[], ["outer"], ["outer", "inner"]] {
        let whereText = scope.isEmpty ? location["root"]! : location["child"]!.replacingOccurrences(of: "{scope}", with: scope.joined(separator: "/"))
        let expected = request["text"]!.replacingOccurrences(of: "{where}", with: whereText)
          .replacingOccurrences(of: "{list}", with: "- outer/work\n- outer/render")
        #expect(AgentRequests.make(kind, scope: scope, cards: ["outer/work", "outer/render"]) == expected)
      }
    }
  }
}
