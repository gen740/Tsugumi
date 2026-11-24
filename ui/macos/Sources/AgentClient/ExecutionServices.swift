import Foundation
import TsugumiRPC

public enum WorkerBits {
  public static let range: ClosedRange<UInt8> = 0...63

  public static func valid(_ bits: [UInt8]) -> Bool {
    Set(bits).count == bits.count && bits.allSatisfy(range.contains)
  }

  public static func toggling(_ bit: UInt8, in bits: [UInt8]) -> [UInt8] {
    bits.contains(bit) ? bits.filter { $0 != bit } : (bits + [bit]).sorted()
  }
}

enum InvalidWorkerBits: LocalizedError {
  case invalid
  var errorDescription: String? { String(localized: "ワーカーのビットは 0〜63 の範囲で重複なしに指定してください。") }
}

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
  public let workerCapabilities: [UInt8]
  public let capabilities: ExecutionServiceCapabilities
}

public struct ExecutionServiceRegistration: Sendable {
  public var id = ""
  public var name = ""
  public var endpoint = ""
  public var workerPublicKey = ""
  public var workerCapabilities: [UInt8] = []
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
    guard WorkerBits.valid(value.workerCapabilities) else { throw InvalidWorkerBits.invalid }
    request.workerCapabilities = value.workerCapabilities
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
        workerPublicKey: service.workerPublicKey, workerCapabilities: service.workerCapabilities,
        capabilities: ExecutionServiceCapabilities(serverId: c.serverId, nativeSystem: c.nativeSystem,
          systems: c.systems.compactMap { $0 }, isolation: c.isolation ?? "", resourceLimits: c.resourceLimits,
          maxConcurrentJobs: Int(c.maxConcurrentJobs), workerContract: Int(c.workerContract)))
    }, hostPublicKey: value.hostPublicKey)
  }
}
