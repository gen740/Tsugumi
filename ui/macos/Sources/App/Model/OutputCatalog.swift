import Foundation

/// Output ports read from an `_outputs.fbs` text that is being edited, for
/// the Card preview. The core remains the authority (it resolves the types
/// on apply); this only reads the port catalog's shape: the root table's
/// fields, their message types and `required`.
enum OutputCatalog {
  enum Result: Equatable {
    case ports([PortData])
    case error(String)
  }

  static func parse(_ text: String) -> Result {
    // Comments out, strings kept intact enough for attribute values.
    var src = ""
    var i = text.startIndex
    while i < text.endIndex {
      let rest = text[i...]
      if rest.hasPrefix("//") {
        i = text[i...].firstIndex(of: "\n") ?? text.endIndex
        continue
      }
      if rest.hasPrefix("/*") {
        guard let end = text.range(of: "*/", range: text.index(i, offsetBy: 2)..<text.endIndex) else {
          return .error(String(localized: "コメントが閉じていません"))
        }
        i = end.upperBound
        continue
      }
      src.append(text[i])
      i = text.index(after: i)
    }
    guard let rootMatch = src.range(of: #"root_type\s+([A-Za-z_][A-Za-z0-9_.]*)\s*;"#, options: .regularExpression) else {
      return .error(String(localized: "root_type がありません"))
    }
    let rootDecl = String(src[rootMatch])
    let root = rootDecl.replacingOccurrences(of: #"root_type\s+"#, with: "", options: .regularExpression)
      .trimmingCharacters(in: CharacterSet(charactersIn: "; \n\t"))
    let short = root.split(separator: ".").last.map(String.init) ?? root
    guard let tableStart = src.range(of: #"table\s+\#(short)\s*\{"#, options: .regularExpression) else {
      return .error(String(localized: "root_type の table \(short) が見つかりません"))
    }
    guard let close = src[tableStart.upperBound...].firstIndex(of: "}") else {
      return .error(String(localized: "table \(short) が閉じていません"))
    }
    let body = src[tableStart.upperBound..<close]
    var ports: [PortData] = []
    for raw in body.split(separator: ";") {
      let field = raw.trimmingCharacters(in: .whitespacesAndNewlines)
      if field.isEmpty { continue }
      let parts = field.split(separator: ":", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
      guard parts.count == 2, parts[0].range(of: #"^[A-Za-z_][A-Za-z0-9_]*$"#, options: .regularExpression) != nil else {
        return .error(String(localized: "フィールドを読めません: \(field)"))
      }
      let typeAndAttrs = parts[1]
      let type = typeAndAttrs.split(separator: "(").first.map { $0.trimmingCharacters(in: .whitespaces) } ?? typeAndAttrs
      guard !type.isEmpty else { return .error(String(localized: "型がありません: \(parts[0])")) }
      let attrs = typeAndAttrs.contains("(") ? String(typeAndAttrs.drop { $0 != "(" }) : ""
      let required = attrs.range(of: #"\brequired\b"#, options: .regularExpression) != nil
      ports.append(PortData(name: parts[0], typeName: type, optional: !required))
    }
    return .ports(ports)
  }
}
