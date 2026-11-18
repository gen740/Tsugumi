import Foundation
@_exported import TsugumiVMTypes
import TsugumiAgentClient
import TsugumiRPC

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
    let reply: Tsugumi_Host_BinaryCachesT = try await CoreHostClient(support: support).hostCall(.binarycachesgetrequest, Tsugumi_Host_BinaryCachesGetRequestT())
    return try JSONDecoder().decode([BinaryCache].self, from: Data(reply.cachesJson.utf8))
  }
  public static func save(_ caches: [BinaryCache], support: URL) async throws {
    let request = Tsugumi_Host_BinaryCachesPutRequestT()
    request.cachesJson = String(decoding: try JSONEncoder().encode(caches), as: UTF8.self)
    let _: Tsugumi_Host_BinaryCachesT = try await CoreHostClient(support: support).hostCall(.binarycachesputrequest, request)
  }
}
