import Foundation
@_exported import TsugumiVMTypes
import TsugumiAgentClient

/// Trusted binary caches belong to the human's VM settings, never a project.
public struct BinaryCache: Codable, Identifiable, Equatable, Sendable {
  public var id = UUID()
  public var name = ""
  public var url = ""
  public var publicKey = ""
  public var username = ""
  public var password = ""
  public init() {}
}

public enum BinaryCaches {
  public static func load(support: URL) async throws -> [BinaryCache] {
    let reply = try await CoreHostClient(support: support).call("BinaryCachesGetRequest", [:])
    guard let json = reply["caches_json"] as? String else { throw LauncherError.notReady("Invalid binary cache reply") }
    return try JSONDecoder().decode([BinaryCache].self, from: Data(json.utf8))
  }
  public static func save(_ caches: [BinaryCache], support: URL) async throws {
    let json = String(decoding: try JSONEncoder().encode(caches), as: UTF8.self)
    _ = try await CoreHostClient(support: support).call("BinaryCachesPutRequest", ["caches_json": json])
  }
}
