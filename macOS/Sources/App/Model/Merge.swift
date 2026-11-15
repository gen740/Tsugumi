import Foundation
import TsugumiRPC

// Wire adapters only; the host owns all merge decisions.
extension DefinitionDraft {
  var mergeJSON: JSONValue {
    var fields: [(String, JSONValue)] = [("card", card)]
    if let meta { fields.append(("meta", meta)) }
    if let link { fields.append(("link", link)) }
    return .object(fields)
  }
  init(mergeJSON value: JSONValue) {
    self.init(card: value["card"] ?? .object([]), meta: value["meta"], link: value["link"])
  }
}
extension ExternalProject {
  var mergeJSON: JSONValue {
    .object([("project", project), ("layout", layout),
             ("definitions", .object(definitions.sorted { $0.key < $1.key }.map { ($0.key, $0.value.mergeJSON) }))])
  }
  init(mergeJSON value: JSONValue) {
    self.init(project: value["project"] ?? .object([]), layout: value["layout"] ?? .object([]),
              definitions: Dictionary(uniqueKeysWithValues: (value["definitions"]?.object ?? []).map { ($0.0, DefinitionDraft(mergeJSON: $0.1)) }))
  }
}
extension ProjectSession.DocState {
  var mergeJSON: JSONValue {
    .object([("project", project), ("layout", layout),
      ("definitions", .object(definitions.sorted { $0.key < $1.key }.map { ($0.key, $0.value.mergeJSON) })),
      ("externals", .object(externals.sorted { $0.key < $1.key }.map { ($0.key, $0.value.mergeJSON) }))])
  }
  init(mergeJSON value: JSONValue) {
    self.init(project: value["project"] ?? .object([]), layout: value["layout"] ?? .object([]),
      definitions: Dictionary(uniqueKeysWithValues: (value["definitions"]?.object ?? []).map { ($0.0, DefinitionDraft(mergeJSON: $0.1)) }),
      externals: Dictionary(uniqueKeysWithValues: (value["externals"]?.object ?? []).map { ($0.0, ExternalProject(mergeJSON: $0.1)) }))
  }
}
