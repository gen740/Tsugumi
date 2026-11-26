import TsugumiRPC

enum RecordSetting: String, CaseIterable {
  case project, on, off
}

let cardSystems = ["aarch64-linux", "x86_64-linux", "aarch64-darwin", "x86_64-darwin"]

/// Values written when converting a Draft in the editor.
func implementedCard(_ kind: CardKind, symbol: String, systems: [String]) -> JSONValue {
  var definition: JSONValue = .object([("type", .string(kind.rawValue)), ("symbol", .string(symbol))])
  if [.code, .merge, .collect].contains(kind) {
    definition["maxRuns"] = .number("1")
    definition["recordIO"] = .string(RecordSetting.project.rawValue)
  }
  if kind == .merge {
    definition["inputs"] = .array(["a", "b"].map { .object([("name", .string($0))]) })
  }
  if kind == .code {
    definition["inputs"] = .array([])
    definition["reuseWorker"] = .bool(false)
    definition["timeoutSeconds"] = .number("0")
    definition["buildTimeoutSeconds"] = .number("0")
    definition["recordLogs"] = .string(RecordSetting.project.rawValue)
    definition["sandbox"] = .object([
      ("filesystem", .bool(true)), ("pid", .bool(true)), ("ipc", .bool(true)), ("identity", .bool(true)),
      ("limits", .object([("cpu", .number("0")), ("memoryMiB", .number("0"))])),
    ])
    definition["grants"] = .object([("network", .bool(false)), ("gui", .bool(false))])
    definition["systems"] = .array(systems.map(JSONValue.string))
  }
  return definition
}
