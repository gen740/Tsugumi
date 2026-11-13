import Foundation

public struct ExecutionServiceCapabilities: Codable, Equatable, Sendable {
  public let serverId: String
  public let nativeSystem: String
  public let systems: [String]
  public let isolation: String
  public let resourceLimits: Bool
  public let maxConcurrentJobs: Int
  public let workerContract: Int
}

public struct ExecutionServiceConnection: Codable, Identifiable, Equatable, Sendable {
  public let id: String
  public let name: String
  public let endpoint: String
  public let capabilities: ExecutionServiceCapabilities
}

public struct ExecutionServiceRegistration: Sendable {
  public var id = ""
  public var name = ""
  public var endpoint = ""
  public var token: String?
  public init() {}
}

/// Settings adapter only: validation, probing, persistence and secrets live in agentd.
public enum ExecutionServices {
  public static func list() async throws -> [ExecutionServiceConnection] {
    try decode(await CoreAgentClient.shared.call("ExecutionServicesRequest"))
  }
  public static func put(_ value: ExecutionServiceRegistration) async throws -> [ExecutionServiceConnection] {
    var fields: [String: Any] = ["id": value.id, "name": value.name, "endpoint": value.endpoint]
    fields["token"] = value.token
    return try decode(await CoreAgentClient.shared.call("ExecutionServicePutRequest", fields))
  }
  public static func remove(_ id: String) async throws -> [ExecutionServiceConnection] {
    try decode(await CoreAgentClient.shared.call("ExecutionServiceRemoveRequest", ["id": id]))
  }
  private static func decode(_ reply: [String: Any]) throws -> [ExecutionServiceConnection] {
    try JSONDecoder().decode([ExecutionServiceConnection].self, from: JSONSerialization.data(withJSONObject: reply["services"] ?? []))
  }
}
