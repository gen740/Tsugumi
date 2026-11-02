import Foundation

/// JSON value used for the UI's working copy of documents. Object key order
/// and the original number spelling are preserved so that 64 bit integers and
/// untouched values round-trip exactly.
public indirect enum JSONValue: Equatable, Sendable {
  case null
  case bool(Bool)
  case number(String)
  case string(String)
  case array([JSONValue])
  case object([(String, JSONValue)])

  public static func == (a: JSONValue, b: JSONValue) -> Bool {
    switch (a, b) {
    case (.null, .null): return true
    case (.bool(let x), .bool(let y)): return x == y
    case (.number(let x), .number(let y)): return x == y
    case (.string(let x), .string(let y)): return x == y
    case (.array(let x), .array(let y)): return x == y
    case (.object(let x), .object(let y)):
      return x.count == y.count && zip(x, y).allSatisfy { $0.0 == $1.0 && $0.1 == $1.1 }
    default: return false
    }
  }

  public subscript(key: String) -> JSONValue? {
    get {
      if case .object(let o) = self { return o.first { $0.0 == key }?.1 }
      return nil
    }
    set {
      guard case .object(var o) = self else { return }
      if let i = o.firstIndex(where: { $0.0 == key }) {
        if let v = newValue { o[i].1 = v } else { o.remove(at: i) }
      } else if let v = newValue {
        o.append((key, v))
      }
      self = .object(o)
    }
  }

  public var string: String? { if case .string(let s) = self { return s }; return nil }
  public var bool: Bool? { if case .bool(let b) = self { return b }; return nil }
  public var array: [JSONValue]? { if case .array(let a) = self { return a }; return nil }
  public var object: [(String, JSONValue)]? { if case .object(let o) = self { return o }; return nil }
  public var numberText: String? { if case .number(let n) = self { return n }; return nil }
  public var double: Double? { numberText.flatMap(Double.init) }
  public var uint64: UInt64? { numberText.flatMap { UInt64($0) } }

  public static func int(_ v: Int64) -> JSONValue { .number(String(v)) }
  public static func uint(_ v: UInt64) -> JSONValue { .number(String(v)) }
  public static func real(_ v: Double) -> JSONValue {
    var s = String(v)
    if !s.contains(".") && !s.contains("e") && !s.contains("n") { s += ".0" }
    return .number(s)
  }

  // MARK: - Parsing

  public struct ParseError: Error, CustomStringConvertible {
    public let message: String
    public let offset: Int
    public var description: String { "\(message) at byte \(offset)" }
  }

  public static func parse(_ text: String) throws -> JSONValue {
    var p = Parser(bytes: Array(text.utf8))
    p.skip()
    let v = try p.value(depth: 0)
    p.skip()
    guard p.i == p.bytes.count else { throw ParseError(message: "trailing characters", offset: p.i) }
    return v
  }

  struct Parser {
    let bytes: [UInt8]
    var i = 0

    mutating func skip() {
      while i < bytes.count, [0x20, 0x09, 0x0A, 0x0D].contains(bytes[i]) { i += 1 }
    }

    mutating func value(depth: Int) throws -> JSONValue {
      guard depth < 256 else { throw ParseError(message: "nesting too deep", offset: i) }
      guard i < bytes.count else { throw ParseError(message: "unexpected end", offset: i) }
      switch bytes[i] {
      case UInt8(ascii: "{"):
        i += 1
        var o: [(String, JSONValue)] = []
        skip()
        if i < bytes.count, bytes[i] == UInt8(ascii: "}") { i += 1; return .object(o) }
        while true {
          skip()
          guard i < bytes.count, bytes[i] == UInt8(ascii: "\"") else { throw ParseError(message: "expected key", offset: i) }
          let k = try string()
          if o.contains(where: { $0.0 == k }) { throw ParseError(message: "duplicate key \(k)", offset: i) }
          skip()
          guard i < bytes.count, bytes[i] == UInt8(ascii: ":") else { throw ParseError(message: "expected ':'", offset: i) }
          i += 1
          skip()
          o.append((k, try value(depth: depth + 1)))
          skip()
          guard i < bytes.count else { throw ParseError(message: "unterminated object", offset: i) }
          if bytes[i] == UInt8(ascii: ",") { i += 1; continue }
          if bytes[i] == UInt8(ascii: "}") { i += 1; return .object(o) }
          throw ParseError(message: "expected ',' or '}'", offset: i)
        }
      case UInt8(ascii: "["):
        i += 1
        var a: [JSONValue] = []
        skip()
        if i < bytes.count, bytes[i] == UInt8(ascii: "]") { i += 1; return .array(a) }
        while true {
          skip()
          a.append(try value(depth: depth + 1))
          skip()
          guard i < bytes.count else { throw ParseError(message: "unterminated array", offset: i) }
          if bytes[i] == UInt8(ascii: ",") { i += 1; continue }
          if bytes[i] == UInt8(ascii: "]") { i += 1; return .array(a) }
          throw ParseError(message: "expected ',' or ']'", offset: i)
        }
      case UInt8(ascii: "\""):
        return .string(try string())
      case UInt8(ascii: "t"): return try literal("true", .bool(true))
      case UInt8(ascii: "f"): return try literal("false", .bool(false))
      case UInt8(ascii: "n"): return try literal("null", .null)
      default:
        let start = i
        while i < bytes.count, "+-0123456789.eE".utf8.contains(bytes[i]) { i += 1 }
        let text = String(decoding: bytes[start..<i], as: UTF8.self)
        guard !text.isEmpty, text.range(of: #"^-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?$"#, options: .regularExpression) != nil
        else { throw ParseError(message: "invalid number", offset: start) }
        return .number(text)
      }
    }

    mutating func literal(_ s: String, _ v: JSONValue) throws -> JSONValue {
      let u = Array(s.utf8)
      guard i + u.count <= bytes.count, Array(bytes[i..<i + u.count]) == u else { throw ParseError(message: "invalid literal", offset: i) }
      i += u.count
      return v
    }

    mutating func string() throws -> String {
      i += 1
      var out = [UInt8]()
      while true {
        guard i < bytes.count else { throw ParseError(message: "unterminated string", offset: i) }
        let c = bytes[i]
        i += 1
        if c == UInt8(ascii: "\"") { break }
        if c < 0x20 { throw ParseError(message: "control character in string", offset: i) }
        if c != UInt8(ascii: "\\") { out.append(c); continue }
        guard i < bytes.count else { throw ParseError(message: "bad escape", offset: i) }
        let e = bytes[i]
        i += 1
        switch e {
        case UInt8(ascii: "n"): out.append(0x0A)
        case UInt8(ascii: "t"): out.append(0x09)
        case UInt8(ascii: "r"): out.append(0x0D)
        case UInt8(ascii: "b"): out.append(0x08)
        case UInt8(ascii: "f"): out.append(0x0C)
        case UInt8(ascii: "u"):
          var cp = try hex4()
          if (0xD800...0xDBFF).contains(cp) {
            guard i + 1 < bytes.count, bytes[i] == UInt8(ascii: "\\"), bytes[i + 1] == UInt8(ascii: "u") else {
              throw ParseError(message: "unpaired surrogate", offset: i)
            }
            i += 2
            let lo = try hex4()
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
          }
          guard let scalar = Unicode.Scalar(cp) else { throw ParseError(message: "invalid code point", offset: i) }
          out.append(contentsOf: Array(String(Character(scalar)).utf8))
        default: out.append(e)
        }
      }
      guard let s = String(bytes: out, encoding: .utf8) else { throw ParseError(message: "invalid UTF-8", offset: i) }
      return s
    }

    mutating func hex4() throws -> UInt32 {
      guard i + 4 <= bytes.count, let v = UInt32(String(decoding: bytes[i..<i + 4], as: UTF8.self), radix: 16) else {
        throw ParseError(message: "bad \\u escape", offset: i)
      }
      i += 4
      return v
    }
  }

  // MARK: - Serialization

  /// Pretty form with two space indentation (document files are edited by people too).
  public func serialized(pretty: Bool = true) -> String {
    var out = ""
    write(&out, indent: 0, pretty: pretty)
    if pretty { out += "\n" }
    return out
  }

  private func write(_ out: inout String, indent: Int, pretty: Bool) {
    let pad = { (n: Int) in pretty ? "\n" + String(repeating: "  ", count: n) : "" }
    switch self {
    case .null: out += "null"
    case .bool(let b): out += b ? "true" : "false"
    case .number(let n): out += n
    case .string(let s): out += JSONValue.quote(s)
    case .array(let a):
      if a.isEmpty { out += "[]"; return }
      out += "["
      for (k, v) in a.enumerated() {
        if k > 0 { out += "," }
        out += pad(indent + 1)
        v.write(&out, indent: indent + 1, pretty: pretty)
      }
      out += pad(indent) + "]"
    case .object(let o):
      if o.isEmpty { out += "{}"; return }
      out += "{"
      for (k, kv) in o.enumerated() {
        if k > 0 { out += "," }
        out += pad(indent + 1) + JSONValue.quote(kv.0) + (pretty ? ": " : ":")
        kv.1.write(&out, indent: indent + 1, pretty: pretty)
      }
      out += pad(indent) + "}"
    }
  }

  public static func quote(_ s: String) -> String {
    var out = "\""
    for u in s.unicodeScalars {
      switch u {
      case "\"": out += "\\\""
      case "\\": out += "\\\\"
      case "\n": out += "\\n"
      case "\r": out += "\\r"
      case "\t": out += "\\t"
      default:
        if u.value < 0x20 { out += String(format: "\\u%04x", u.value) } else { out.unicodeScalars.append(u) }
      }
    }
    return out + "\""
  }
}
