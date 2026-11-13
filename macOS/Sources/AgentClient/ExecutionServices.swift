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
  public let workerPublicKey: String
  public let capabilities: ExecutionServiceCapabilities
}

public struct ExecutionServiceRegistration: Sendable {
  public var id = ""
  public var name = ""
  public var endpoint = ""
  public var workerPublicKey = ""
  public var verify = false
  public init() {}
}

public struct ExecutionServicesState: Codable, Sendable {
  public let services: [ExecutionServiceConnection]
  public let hostPublicKey: String
}

/// Settings adapter only: validation, probing, persistence and secrets live in agentd.
public enum ExecutionServices {
  public static func list() async throws -> ExecutionServicesState {
    try decode(await CoreAgentClient.shared.call("ExecutionServicesRequest"))
  }
  public static func put(_ value: ExecutionServiceRegistration) async throws -> ExecutionServicesState {
    let fields: [String: Any] = ["id": value.id, "name": value.name, "endpoint": value.endpoint, "workerPublicKey": value.workerPublicKey, "verify": value.verify]
    return try decode(await CoreAgentClient.shared.call("ExecutionServicePutRequest", fields))
  }
  public static func remove(_ id: String) async throws -> ExecutionServicesState {
    try decode(await CoreAgentClient.shared.call("ExecutionServiceRemoveRequest", ["id": id]))
  }
  private static func decode(_ reply: [String: Any]) throws -> ExecutionServicesState {
    try JSONDecoder().decode(ExecutionServicesState.self, from: JSONSerialization.data(withJSONObject: reply))
  }
}
