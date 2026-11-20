import Foundation
import TsugumiRPC

/// Settings adapter only: Card secrets live in hostd's secret store, beside the
/// API keys. The app sets, replaces and deletes them; hostd never sends a
/// value back, only the names that are set.
public enum CardSecrets {
  public static func names() async throws -> [String] {
    let reply: Tsugumi_Host_CardSecretListT = try await CoreHostClient.shared.hostCall(
      .cardsecretlistrequest, Tsugumi_Host_CardSecretListRequestT())
    return reply.names.compactMap { $0 }
  }
  public static func set(_ name: String, value: String) async throws -> [String] {
    let request = Tsugumi_Host_CardSecretSetRequestT()
    request.name = name
    request.value = value
    let reply: Tsugumi_Host_CardSecretListT = try await CoreHostClient.shared.hostCall(.cardsecretsetrequest, request)
    return reply.names.compactMap { $0 }
  }
  public static func remove(_ name: String) async throws -> [String] {
    let request = Tsugumi_Host_CardSecretSetRequestT()
    request.name = name
    request.remove = true
    let reply: Tsugumi_Host_CardSecretListT = try await CoreHostClient.shared.hostCall(.cardsecretsetrequest, request)
    return reply.names.compactMap { $0 }
  }
  /// The same rule as a Card's `secrets` entries: [a-z][a-z0-9_]{0,63}.
  public static func validName(_ name: String) -> Bool {
    name.range(of: "^[a-z][a-z0-9_]{0,63}$", options: .regularExpression) != nil
  }
}
