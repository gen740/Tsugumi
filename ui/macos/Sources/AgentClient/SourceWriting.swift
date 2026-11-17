import Foundation

/// A file as it arrives in the arguments of a save (`project_save`) while the
/// model writes them: `path` is nil until its key has come, `text` is what has
/// come of `bytes` or the current patch replacement, and `done` says the change is complete.
public struct SourceWriting: Equatable, Sendable {
  public var path: String?
  public var text = ""
  public var done = false
  public var patch = false
  public var sequence = 0
  public var ready = false
}

/// Reads the JSON of tool arguments as it streams in, one piece at a time and
/// at any cut, for PutSource bytes or PatchSource replacement text. Nothing is parsed twice and nothing waits for the JSON to end.
public struct SourceWritingScanner {
  public private(set) var current: SourceWriting?

  private struct Object { var key: String?; var awaitingKey = true; var isFile = false; var patch = false }
  private enum Container { case object(Object), array(patches: Bool) }
  private enum StringRole { case key, path, bytes, patchText, other }

  private var sequence = 0
  private var stack: [Container] = []
  private var role: StringRole?
  private var token = String.UnicodeScalarView()
  private var escaped = false
  private var hex: [UInt8]?
  private var highSurrogate: UInt32?

  public init() {}

  public mutating func feed(_ delta: String) {
    for c in delta.unicodeScalars {
      if role != nil { string(c) } else { structure(c) }
    }
  }

  private mutating func structure(_ c: Unicode.Scalar) {
    switch c {
    case "{":
      let patch: Bool
      if case .array(patches: true)? = stack.last { patch = true } else { patch = false }
      if patch {
        sequence += 1
        current = SourceWriting(path: current?.path, patch: true, sequence: sequence)
      }
      stack.append(.object(Object(patch: patch)))
    case "[":
      let patches: Bool
      if case .object(let o)? = stack.last, o.key == "patches" { patches = true; startFile() }
      else { patches = false }
      stack.append(.array(patches: patches))
    case "}", "]":
      if case .object(let o)? = stack.popLast(), o.isFile { current?.done = true }
    case ",":
      withTop { $0.awaitingKey = true; $0.key = nil }
    case "\"":
      token = String.UnicodeScalarView()
      guard case .object(let o)? = stack.last else { role = .other; return }
      if o.awaitingKey {
        role = .key
      } else if o.key == "path" {
        role = .path
        startFile()
      } else if o.patch && o.key == "text" {
        role = .patchText
        current?.ready = true
      } else if o.key == "bytes" {
        role = .bytes
        startFile()
        current?.ready = true
      } else {
        role = .other
      }
    default: break
    }
  }

  private mutating func string(_ c: Unicode.Scalar) {
    if var digits = hex {
      digits.append(UInt8(truncatingIfNeeded: c.value))
      if digits.count < 4 { hex = digits; return }
      hex = nil
      unicode(UInt32(String(decoding: digits, as: UTF8.self), radix: 16) ?? 0xFFFD)
    } else if escaped {
      escaped = false
      switch c {
      case "n": append("\n")
      case "t": append("\t")
      case "r": append("\r")
      case "b": append("\u{8}")
      case "f": append("\u{c}")
      case "u": hex = []
      default: append(c)
      }
    } else if c == "\\" {
      escaped = true
    } else if c == "\"" {
      endString()
    } else {
      append(c)
    }
  }

  private mutating func unicode(_ code: UInt32) {
    if let high = highSurrogate {
      highSurrogate = nil
      if (0xDC00...0xDFFF).contains(code) {
        append(Unicode.Scalar(0x10000 + ((high - 0xD800) << 10) + (code - 0xDC00)) ?? "\u{FFFD}")
        return
      }
      append("\u{FFFD}")
    }
    if (0xD800...0xDBFF).contains(code) { highSurrogate = code; return }
    append(Unicode.Scalar(code) ?? "\u{FFFD}")
  }

  private mutating func append(_ c: Unicode.Scalar) {
    if role == .bytes || role == .patchText {
      current?.text.unicodeScalars.append(c)
    } else if role == .key || role == .path {
      token.append(c)
    }
  }

  private mutating func endString() {
    switch role {
    case .key:
      let key = String(token)
      withTop { $0.key = key; $0.awaitingKey = false }
    case .path:
      current?.path = String(token)
    default: break
    }
    role = nil
  }

  /// The object at the top is the file being written from its first `path` or `bytes`.
  private mutating func startFile() {
    guard case .object(let o)? = stack.last, !o.isFile else { return }
    sequence += 1
    current = SourceWriting(sequence: sequence)
    withTop { $0.isFile = true }
  }

  private mutating func withTop(_ change: (inout Object) -> Void) {
    guard case .object(var o)? = stack.last else { return }
    change(&o)
    stack[stack.count - 1] = .object(o)
  }
}
