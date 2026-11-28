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
  /// The display name from the worker's own settings; empty when unset.
  public var workerLabel: String = ""
  /// The worker's HTTPS dashboard and the SHA-256 of its certificate, which
  /// the dashboard view pins. Empty before the worker is verified.
  public var dashboardURL: String = ""
  public var dashboardCertificateSHA256: String = ""
  /// The source revision the worker was built from; empty when it does not say.
  public var revision: String = ""
}

public struct ExecutionProbeMeasurement: Codable, Equatable, Sendable {
  public let probe: String
  public let score: Double?
  public let failure: String?
  public let measuredAt: Int64
}

public struct ExecutionServiceConnection: Codable, Identifiable, Equatable, Sendable {
  public let id: String
  public let name: String
  public let endpoint: String
  public let workerPublicKey: String
  public let probes: [ExecutionProbeMeasurement]
  public let capabilities: ExecutionServiceCapabilities
  /// hostd found the worker built from another revision (another worker contract): runs do not use it.
  public let outdated: Bool
  /// Why hostd cannot read what it stored about the worker; empty when it can.
  public var recordError: String = ""

  /// How a list shows the service: its name, and its id only when the two differ.
  public var label: String { Self.label(name: name, id: id) }
  public static func label(name: String, id: String) -> String {
    name.isEmpty || name == id ? id : "\(name) (\(id))"
  }
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
  /// The worker contract this Tsugumi speaks.
  public let workerContract: Int
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
    let reply: Tsugumi_Host_ExecutionServicesT = try await CoreHostClient.shared.hostCall(.executionserviceputrequest, request)
    return state(reply)
  }
  public static func remeasure(_ id: String?) async throws -> ExecutionServicesState {
    let request = Tsugumi_Host_ExecutionProbeRemeasureRequestT()
    request.id = id
    let reply: Tsugumi_Host_ExecutionServicesT = try await CoreHostClient.shared.hostCall(.executionproberemeasurerequest, request)
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
        workerPublicKey: service.workerPublicKey, probes: service.probes.compactMap { probe in
          probe.map { ExecutionProbeMeasurement(probe: $0.probe ?? "", score: $0.score, failure: $0.failure, measuredAt: $0.measuredAt) }
        },
        capabilities: ExecutionServiceCapabilities(serverId: c.serverId, nativeSystem: c.nativeSystem,
          systems: c.systems.compactMap { $0 }, isolation: c.isolation ?? "", resourceLimits: c.resourceLimits,
          maxConcurrentJobs: Int(c.maxConcurrentJobs), workerContract: Int(c.workerContract),
          workerLabel: c.workerLabel ?? "", dashboardURL: c.dashboardUrl ?? "",
          dashboardCertificateSHA256: c.dashboardCertificateSha256 ?? "",
          revision: c.revision ?? ""), outdated: service.outdated, recordError: service.recordError ?? "")
    }, hostPublicKey: value.hostPublicKey, workerContract: Int(value.workerContract))
  }
}
