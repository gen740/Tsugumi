import Foundation

/// Test-launch input, consumed before a scripted project asks for trust.
/// Like the script setting command, registration uses the host settings RPC.
public enum ScriptTrust {
  public static func folder(environment: [String: String]) -> String? {
    guard let script = environment["TSUGUMI_UI_SCRIPT"], !script.isEmpty,
      let folder = environment["TSUGUMI_UI_TRUSTED_FOLDER"], !folder.isEmpty else { return nil }
    return folder
  }

  public static func register(_ folder: String,
                             call: (String, [String: Any]) async throws -> [String: Any]) async throws {
    let settings = try await call("SettingsGetRequest", ["profile": "desktop"])
    let values = settings["values"] as? [[String: String]] ?? []
    let raw = values.first { $0["key"] == "agent.trustedFolders" }?["value_json"] ?? "[]"
    var folders = (try JSONSerialization.jsonObject(with: Data(raw.utf8))) as? [String] ?? []
    folders.append(folder)
    let value = String(decoding: try JSONSerialization.data(withJSONObject: folders), as: UTF8.self)
    _ = try await call("SettingsUpdateRequest", ["profile": "desktop", "revision": settings["revision"] ?? "",
      "changes": [["key": "agent.trustedFolders", "value_json": value]]])
  }
}
