import Foundation
import TsugumiRPC

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
  public let grade: String
  public let capabilities: ExecutionServiceCapabilities
}

public struct ExecutionServiceRegistration: Sendable {
  public var id = ""
  public var name = ""
  public var endpoint = ""
  public var workerPublicKey = ""
  public var grade = "1"
  public var verify = false
  public init() {}
}

public struct ExecutionServicesState: Codable, Sendable {
  public let services: [ExecutionServiceConnection]
  public let hostPublicKey: String
}

/// Settings adapter only: validation, probing, persistence and secrets live in hostd.
public enum ExecutionServices {
  public static func list() async throws -> ExecutionServicesState {
    let value: Tsugumi_Host_ExecutionServicesT = try await CoreHostClient.shared.hostCall(.executionservicesrequest, Tsugumi_Host_ExecutionServicesRequestT())
    return state(value)
  }
  public static func put(_ value: ExecutionServiceRegistration) async throws -> ExecutionServicesState {
    let request = Tsugumi_Host_ExecutionServicePutRequestT()
    request.id = value.id; request.name = value.name; request.endpoint = value.endpoint
    request.workerPublicKey = value.workerPublicKey; request.verify = value.verify
    let hex = value.grade.lowercased().hasPrefix("0x")
    request.grade = UInt64(hex ? String(value.grade.dropFirst(2)) : value.grade, radix: hex ? 16 : 10) ?? 0
    let reply: Tsugumi_Host_ExecutionServicesT = try await CoreHostClient.shared.hostCall(.executionserviceputrequest, request)
    return state(reply)
  }
  public static func remove(_ id: String) async throws -> ExecutionServicesState {
    let request = Tsugumi_Host_ExecutionServiceRemoveRequestT(); request.id = id
    let reply: Tsugumi_Host_ExecutionServicesT = try await CoreHostClient.shared.hostCall(.executionserviceremoverequest, request)
    return state(reply)
  }
  private static func state(_ value: Tsugumi_Host_ExecutionServicesT) -> ExecutionServicesState {
    ExecutionServicesState(services: value.services.compactMap { service in
      guard let service else { return nil }
      let c = service.capabilities
      return ExecutionServiceConnection(id: service.id, name: service.name, endpoint: service.endpoint,
        workerPublicKey: service.workerPublicKey, grade: String(service.grade),
        capabilities: ExecutionServiceCapabilities(serverId: c.serverId, nativeSystem: c.nativeSystem,
          systems: c.systems.compactMap { $0 }, isolation: c.isolation ?? "", resourceLimits: c.resourceLimits,
          maxConcurrentJobs: Int(c.maxConcurrentJobs), workerContract: Int(c.workerContract)))
    }, hostPublicKey: value.hostPublicKey)
  }
}
