import Foundation
import TsugumiRPC

struct CardParameter: Identifiable, Equatable {
  var id: String { name }
  let name: String
  let section: Tsugumi_RPC_ParamSection
  let type: String
  let defaultValue: JSONValue
  let override: JSONValue?

  var effectiveValue: JSONValue { override ?? defaultValue }

  init?(_ parameter: Tsugumi_RPC_CardParameterT) {
    guard let value = try? JSONValue.parse(parameter.defaultJson) else { return nil }
    name = parameter.name
    section = parameter.section
    type = parameter.type
    defaultValue = value
    override = parameter.valueJson.flatMap { try? JSONValue.parse($0) }
  }
}

/// Preserve JSON number spelling so editing an int64 never rounds it.
func parseParam(_ text: String, _ type: String) -> JSONValue {
  if type == "string" { return .string(text) }
  return (try? JSONValue.parse(text)) ?? .string(text)
}

func parameterText(_ value: JSONValue) -> String {
  value.string ?? value.serialized(pretty: false)
}
