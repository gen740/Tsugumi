import Foundation
import TsugumiRPC

struct CardParameter: Identifiable, Equatable {
  var id: String { name }
  let name: String
  let type: String
  let effectiveValue: JSONValue

  init?(_ parameter: Tsugumi_RPC_CardParameterT) {
    guard let value = try? JSONValue.parse(parameter.valueJson) else { return nil }
    name = parameter.name
    type = parameter.type
    effectiveValue = value
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

func parameterLiteral(_ value: JSONValue, _ type: String) -> String {
  if let values = value.array, type.hasPrefix("[") {
    let itemType = String(type.dropFirst().dropLast())
    return "[ " + values.map {
      let item = parameterLiteral($0, itemType)
      return item.hasPrefix("-") ? "(" + item + ")" : item
    }.joined(separator: " ") + " ]"
  }
  if let string = value.string {
    return "\"" + string.replacingOccurrences(of: "\\", with: "\\\\")
      .replacingOccurrences(of: "\"", with: "\\\"")
      .replacingOccurrences(of: "$" + "{", with: "\\$" + "{") + "\""
  }
  if let number = value.numberText, type == "float" {
    let exponent = number.firstIndex { $0 == "e" || $0 == "E" } ?? number.endIndex
    let mantissa = number[..<exponent]
    if !mantissa.contains(".") { return mantissa + ".0" + number[exponent...] }
  }
  return value.serialized(pretty: false)
}
