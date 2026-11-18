import Foundation
import Testing
import TsugumiAgentClient

struct ConversationSnapshotTests {
  @Test func publicConversationKeepsSearchSourcesAndToolImagesReadable() throws {
    let json = #"""
    {"id":"conversation","messages":[{"role":"assistant","content":[
      {"type":"web_search","id":"search-1","query":"public sources",
       "sources":[{"url":"https://example.com/source","title":"Source"}],"error":"partial results"},
      {"type":"text","text":"A reply with a citation."}],
      "providerContent":[{"type":"text","text":"A reply with a citation."}]},
      {"role":"user","content":[{"type":"tool_result","tool_use_id":"image-1",
       "content":"A displayed result","is_error":false,
       "images":[{"media_type":"image/png","data":"aW1hZ2U="}]}]}],
     "archive":[],"tools":[],"usage":{"inputTokens":3,"outputTokens":4}}
    """#
    let conversation = try JSONDecoder().decode(Conversation.self, from: Data(json.utf8))
    guard case .webSearch(let search) = conversation.messages[0].content[0] else {
      Issue.record("The public search block was not decoded")
      return
    }
    #expect(search.id == "search-1" && search.query == "public sources")
    #expect(search.sources == [WebSource(url: "https://example.com/source", title: "Source")])
    #expect(search.error == "partial results")
    guard case .toolResult(_, _, _, let images) = conversation.messages[1].content[0] else {
      Issue.record("The public tool result was not decoded")
      return
    }
    #expect(images.count == 1 && images[0].mediaType == "image/png" && images[0].data == "aW1hZ2U=")
    let encoded = try JSONEncoder().encode(conversation.messages[0].content[0])
    let fields = try #require(JSONSerialization.jsonObject(with: encoded) as? [String: Any])
    #expect(fields["type"] as? String == "web_search")
    #expect(fields["query"] as? String == "public sources")
    #expect(fields["search"] == nil)
  }
}
