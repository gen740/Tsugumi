import Foundation

/// What a connection offers for a model (agent/Shared/agent.json, from the
/// providers' documents). A model not listed offers nothing: nothing is sent
/// that the provider might refuse.
public struct ModelOptions: Equatable, Sendable {
  public var efforts: [String] = []
  /// The level the model uses when none is sent, when its documents say.
  public var defaultEffort: String?
  /// Fast mode's price as a multiple of the normal one; none when not offered.
  public var fastPrice: Double?

  public init(connection: String, model: String) {
    guard let c = Self.table.connections[connection] else { return }
    if let group = c.effort.first(where: { $0.models.contains(model) }) {
      efforts = group.levels
      defaultEffort = group.default
    }
    fastPrice = c.fast[model]
  }

  private struct Table: Decodable {
    struct Connection: Decodable {
      struct Group: Decodable {
        let `default`: String?
        let levels: [String]
        let models: [String]
      }
      let effort: [Group]
      let fast: [String: Double]
    }
    let connections: [String: Connection]
  }

  private struct Document: Decodable { let modelOptions: Table }

  private static let table = try! JSONDecoder().decode(Document.self, from: Data(Shared.agentJSON.utf8)).modelOptions
}

extension ProviderConfig {
  /// The connection's id in the table and the settings ("anthropic", "openai",
  /// "chatgpt"; "fixture" offers nothing).
  public var connection: String {
    switch auth {
    case .none: return "fixture"
    case .chatGPT: return "chatgpt"
    case .apiKey: return api == .openAIResponses ? "openai" : "anthropic"
    }
  }

  /// With what the chat's model menu chose: the level if the model takes it,
  /// and Fast mode for the model it was turned on for.
  public func choosing(effort: String, fastModel: String) -> ProviderConfig {
    let o = ModelOptions(connection: connection, model: model)
    var c = self
    c.usesChoices = true
    c.effort = o.efforts.contains(effort) ? effort : nil
    c.fast = o.fastPrice != nil && fastModel == model
    return c
  }

}
