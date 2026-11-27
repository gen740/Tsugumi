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

public struct CachePushTarget: Equatable, Sendable {
  public enum Kind: UInt8, CaseIterable, Sendable {
    case own, attic, cachix, nixCopy
  }

  public var kind: Kind = .own
  public var endpoint = ""
  public var cacheName = ""
  public private(set) var hasCredentials = false

  public init() {}

  private init(_ reply: Tsugumi_Host_CachePushTargetT) {
    kind = Kind(rawValue: reply.kind.rawValue) ?? .own
    endpoint = reply.endpoint ?? ""
    cacheName = reply.cacheName ?? ""
    hasCredentials = reply.hasCredentials
  }

  public static func load(support: URL) async throws -> Self {
    let reply: Tsugumi_Host_CachePushTargetT = try await CoreHostClient(support: support)
      .hostCall(.cachepushtargetgetrequest, Tsugumi_Host_CachePushTargetGetRequestT())
    return Self(reply)
  }

  public func save(
    support: URL, user: String = "", secret: String = "", clearCredentials: Bool = false
  ) async throws -> Self {
    let request = Tsugumi_Host_CachePushTargetPutRequestT()
    request.kind = Tsugumi_Host_CachePushKind(rawValue: kind.rawValue)!
    request.endpoint = endpoint
    request.cacheName = cacheName
    request.credentialUser = user
    request.credentialSecret = secret
    request.clearCredentials = clearCredentials
    let reply: Tsugumi_Host_CachePushTargetT = try await CoreHostClient(support: support)
      .hostCall(.cachepushtargetputrequest, request)
    return Self(reply)
  }
}
