import Foundation
import TsugumiRPC

/// Settings adapter only: Card secrets live in hostd's secret store, beside the
/// API keys, bound to one project id. The app sets, replaces and deletes a
/// project's values; hostd never sends a value back, only the names that are set.
public enum CardSecrets {
  public static func names(project: String) async throws -> [String] {
    let request = Tsugumi_Host_CardSecretListRequestT()
    request.projectId = project
    let reply: Tsugumi_Host_CardSecretListT = try await CoreHostClient.shared.hostCall(.cardsecretlistrequest, request)
    return reply.names.compactMap { $0 }
  }
  public static func set(project: String, _ name: String, value: String) async throws -> [String] {
    let request = Tsugumi_Host_CardSecretSetRequestT()
    request.projectId = project
    request.name = name
    request.value = value
    let reply: Tsugumi_Host_CardSecretListT = try await CoreHostClient.shared.hostCall(.cardsecretsetrequest, request)
    return reply.names.compactMap { $0 }
  }
  public static func remove(project: String, _ name: String) async throws -> [String] {
    let request = Tsugumi_Host_CardSecretSetRequestT()
    request.projectId = project
    request.name = name
    request.remove = true
    let reply: Tsugumi_Host_CardSecretListT = try await CoreHostClient.shared.hostCall(.cardsecretsetrequest, request)
    return reply.names.compactMap { $0 }
  }
}

/// The document rule for Card ids, port names, and secret names.
public func validName(_ name: String) -> Bool {
  name.wholeMatch(of: #/^[a-z][a-z0-9_]{0,63}$/#) != nil
}
