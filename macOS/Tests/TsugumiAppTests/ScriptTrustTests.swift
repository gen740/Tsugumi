import Foundation
import Testing
import TsugumiAgentClient

@Test func scriptTrustRequiresScriptMode() {
  #expect(ScriptTrust.folder(environment: ["TSUGUMI_UI_TRUSTED_FOLDER": "/trials"]) == nil)
  #expect(ScriptTrust.folder(environment: ["TSUGUMI_UI_SCRIPT": "/script"]) == nil)
  #expect(ScriptTrust.folder(environment: ["TSUGUMI_UI_SCRIPT": "/script", "TSUGUMI_UI_TRUSTED_FOLDER": "/trials"]) == "/trials")
}

@Test func scriptTrustUsesTheHostSettingsRevisionAndPreservesOtherRoots() async throws {
  var methods: [String] = []
  try await ScriptTrust.register("/trials") { method, fields in
    methods.append(method)
    #expect(fields["profile"] as? String == "desktop")
    if method == "SettingsGetRequest" {
      return ["revision": 7, "values": [["key": "agent.trustedFolders", "value_json": "[\"/existing\"]"]]]
    }
    #expect(fields["revision"] as? Int == 7)
    let changes = try #require(fields["changes"] as? [[String: String]])
    #expect(changes.count == 1)
    #expect(changes[0]["key"] == "agent.trustedFolders")
    let folders = try JSONSerialization.jsonObject(with: Data(changes[0]["value_json"]!.utf8)) as? [String]
    #expect(folders == ["/existing", "/trials"])
    return [:]
  }
  #expect(methods == ["SettingsGetRequest", "SettingsUpdateRequest"])
}
