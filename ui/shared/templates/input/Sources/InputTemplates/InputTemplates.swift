import Foundation

public enum InputTemplates {
  public static func load() throws -> [String: [String: String]] {
    guard let url = Bundle.module.url(forResource: "inputs", withExtension: "json") else {
      throw CocoaError(.fileNoSuchFile)
    }
    return try JSONDecoder().decode([String: [String: String]].self, from: Data(contentsOf: url))
  }
}
