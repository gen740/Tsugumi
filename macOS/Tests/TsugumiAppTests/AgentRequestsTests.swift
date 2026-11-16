import Testing
import TsugumiAgentClient

struct AgentRequestsTests {
  @Test func generationButtonsRequestSavedValidatedRunResults() {
    for kind in ["draftRequest", "commentRequest"] {
      let text = AgentRequests.make(kind, scope: ["outer"], cards: ["outer/work", "outer/render"])
      #expect(text.contains("the child workflow outer"))
      #expect(text.contains("- outer/work\n- outer/render"))
      #expect(text.contains("save") && text.contains("project_validate") && text.contains("run_record"))
      #expect(!text.contains("delegate") && !text.contains("{where}") && !text.contains("{list}"))
    }
    #expect(AgentRequests.make("draftRequest", scope: [], cards: ["work"]).contains("the root workflow"))
  }
}
