import FlatBuffers
import Foundation

/// Converts between JSON tool arguments/results and FlatBuffers tables using
/// the reflection schema. Unions use the flatc JSON convention
/// (`<field>_type` + `<field>`). 64 bit integers are decimal strings in JSON.
public struct FlatJSON {
  public let schema: BFBS

  public init(schema: BFBS) { self.schema = schema }

  // MARK: - JSON -> FlatBuffer

  /// Builds a table of `object` from `json`. Unknown keys and type errors are
  /// rejected so incomplete tool arguments never reach the core.
  public func build(_ object: Int, _ json: [String: Any], into b: inout FlatBufferBuilder, path: String = "") throws -> Offset {
    let obj = schema.objects[object]
    let known = Set(obj.fields.map(\.name))
    for k in json.keys where !known.contains(k) {
      throw AgentError.invalidArguments("\(path)\(k) is not a field of \(obj.name.split(separator: ".").last ?? "")")
    }
    // Children first: FlatBuffers needs nested objects before the table starts.
    var offsets: [UInt16: Offset] = [:]
    for f in obj.fields {
      guard let v = json[f.name], !(v is NSNull) else { continue }
      let where_ = "\(path)\(f.name)"
      switch f.type.base {
      case .string:
        guard let s = v as? String else { throw AgentError.invalidArguments("\(where_) must be a string") }
        offsets[f.offset] = b.create(string: s)
      case .obj:
        guard let o = v as? [String: Any] else { throw AgentError.invalidArguments("\(where_) must be an object") }
        offsets[f.offset] = try build(Int(f.type.index), o, into: &b, path: where_ + ".")
      case .union:
        guard let o = v as? [String: Any] else { throw AgentError.invalidArguments("\(where_) must be an object") }
        let variant = try unionVariant(f, json["\(f.name)_type"], where_)
        offsets[f.offset] = try build(Int(variant.unionType!.index), o, into: &b, path: where_ + ".")
      case .vector:
        offsets[f.offset] = try vector(f, v, &b, where_)
      default:
        continue
      }
    }
    let slots = (obj.fields.map { Int($0.id) }.max() ?? 0) + 1
    let start = b.startTable(with: slots)
    for f in obj.fields {
      let slot = VOffset(f.offset)
      if let off = offsets[f.offset] {
        b.add(offset: off, at: slot)
        continue
      }
      if f.type.base == .utype {
        let unionName = String(f.name.dropLast(5))  // "<name>_type"
        guard json[unionName] != nil, let uf = obj.fields.first(where: { $0.name == unionName }) else { continue }
        let variant = try unionVariant(uf, json[f.name], "\(path)\(f.name)")
        b.add(element: UInt8(variant.value), at: slot)
        continue
      }
      guard let v = json[f.name], !(v is NSNull) else { continue }
      try scalar(f, v, &b, slot, "\(path)\(f.name)")
    }
    return Offset(offset: b.endTable(at: start))
  }

  private func unionVariant(_ f: BFBS.Field, _ name: Any?, _ where_: String) throws -> BFBS.EnumValue {
    let e = schema.enums[Int(f.type.index)]
    guard let n = name as? String else {
      throw AgentError.invalidArguments("\(where_)_type is required (one of \(e.values.dropFirst().map(\.name).joined(separator: ", ")))")
    }
    guard let v = e.values.first(where: { $0.name == n && $0.value != 0 }) else {
      throw AgentError.invalidArguments("\(where_)_type \(n) is unknown")
    }
    return v
  }

  private func enumValue(_ f: BFBS.Field, _ v: Any, _ where_: String) throws -> Int64? {
    guard f.type.index >= 0, f.type.base != .union, f.type.base != .utype else { return nil }
    let e = schema.enums[Int(f.type.index)]
    if let s = v as? String {
      guard let ev = e.values.first(where: { $0.name == s }) else {
        throw AgentError.invalidArguments("\(where_) must be one of \(e.values.map(\.name).joined(separator: ", "))")
      }
      return ev.value
    }
    return nil
  }

  private func integer(_ v: Any, _ where_: String) throws -> (Int64?, UInt64?) {
    if let s = v as? String {
      // Digits with an optional minus only (as the web Agent: no "+", no spaces).
      let digits = s.hasPrefix("-") ? s.dropFirst() : Substring(s)
      guard !digits.isEmpty, digits.allSatisfy({ $0.isASCII && $0.isNumber }) else { throw AgentError.invalidArguments("\(where_) must be a decimal integer") }
      if let u = UInt64(s) { return (Int64(exactly: u), u) }
      if let i = Int64(s) { return (i, nil) }
      throw AgentError.invalidArguments("\(where_) must be a decimal integer")
    }
    if let n = v as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID() {
      let d = n.doubleValue
      guard d == d.rounded(), abs(d) <= 9_007_199_254_740_991 else {
        throw AgentError.invalidArguments("\(where_) must be an integer (use a decimal string for 64 bit values)")
      }
      return (n.int64Value, d >= 0 ? n.uint64Value : nil)
    }
    throw AgentError.invalidArguments("\(where_) must be an integer")
  }

  private func scalar(_ f: BFBS.Field, _ v: Any, _ b: inout FlatBufferBuilder, _ slot: VOffset, _ where_: String) throws {
    let enumRaw = try enumValue(f, v, where_)
    func int() throws -> Int64 {
      if let e = enumRaw { return e }
      guard let i = try integer(v, where_).0 else { throw AgentError.invalidArguments("\(where_) is out of range") }
      return i
    }
    func uint() throws -> UInt64 {
      if let e = enumRaw { return UInt64(e) }
      guard let u = try integer(v, where_).1 else { throw AgentError.invalidArguments("\(where_) must not be negative") }
      return u
    }
    func checked<T: FixedWidthInteger>(_ x: Int64, _: T.Type) throws -> T {
      guard let r = T(exactly: x) else { throw AgentError.invalidArguments("\(where_) is out of range for \(T.self)") }
      return r
    }
    func checkedU<T: FixedWidthInteger>(_ x: UInt64, _: T.Type) throws -> T {
      guard let r = T(exactly: x) else { throw AgentError.invalidArguments("\(where_) is out of range for \(T.self)") }
      return r
    }
    switch f.type.base {
    case .bool:
      guard let x = v as? Bool else { throw AgentError.invalidArguments("\(where_) must be true or false") }
      b.add(element: x, at: slot)
    case .byte: b.add(element: try checked(int(), Int8.self), at: slot)
    case .ubyte: b.add(element: try checkedU(uint(), UInt8.self), at: slot)
    case .short: b.add(element: try checked(int(), Int16.self), at: slot)
    case .ushort: b.add(element: try checkedU(uint(), UInt16.self), at: slot)
    case .int: b.add(element: try checked(int(), Int32.self), at: slot)
    case .uint: b.add(element: try checkedU(uint(), UInt32.self), at: slot)
    case .long: b.add(element: try int(), at: slot)
    case .ulong: b.add(element: try uint(), at: slot)
    case .float, .double:
      guard let n = v as? NSNumber, n.doubleValue.isFinite else { throw AgentError.invalidArguments("\(where_) must be a finite number") }
      if f.type.base == .float { b.add(element: n.floatValue, at: slot) } else { b.add(element: n.doubleValue, at: slot) }
    default:
      throw AgentError.invalidArguments("\(where_) has an unsupported type")
    }
  }

  private func vector(_ f: BFBS.Field, _ v: Any, _ b: inout FlatBufferBuilder, _ where_: String) throws -> Offset {
    switch f.type.element {
    case .ubyte, .byte:
      // Byte vectors carry text (UTF-8) or explicit {"base64": "..."}.
      if let s = v as? String { return b.createVector(Array(s.utf8)) }
      if let o = v as? [String: Any], let b64 = o["base64"] as? String, let d = Data(base64Encoded: b64) {
        return b.createVector([UInt8](d))
      }
      throw AgentError.invalidArguments("\(where_) must be text or {\"base64\": ...}")
    case .string:
      guard let a = v as? [String] else { throw AgentError.invalidArguments("\(where_) must be an array of strings") }
      return b.createVector(ofStrings: a)
    case .obj:
      guard let a = v as? [[String: Any]] else { throw AgentError.invalidArguments("\(where_) must be an array of objects") }
      var offs: [Offset] = []
      for (i, o) in a.enumerated() { offs.append(try build(Int(f.type.index), o, into: &b, path: "\(where_)[\(i)].")) }
      return b.createVector(ofOffsets: offs)
    default:
      guard let a = v as? [Any] else { throw AgentError.invalidArguments("\(where_) must be an array") }
      // Every element checked against its own type's range.
      func signed<T: FixedWidthInteger & SignedInteger>(_: T.Type) throws -> [T] {
        try a.enumerated().map { i, x in
          guard let v = try integer(x, "\(where_)[\(i)]").0, let r = T(exactly: v) else {
            throw AgentError.invalidArguments("\(where_)[\(i)] is out of range for \(T.self)")
          }
          return r
        }
      }
      func unsigned<T: FixedWidthInteger & UnsignedInteger>(_: T.Type) throws -> [T] {
        try a.enumerated().map { i, x in
          guard let v = try integer(x, "\(where_)[\(i)]").1, let r = T(exactly: v) else {
            throw AgentError.invalidArguments("\(where_)[\(i)] is out of range for \(T.self)")
          }
          return r
        }
      }
      switch f.type.element {
      case .double: return b.createVector(try a.map { try number($0, where_) })
      case .float: return b.createVector(try a.map { Float(try number($0, where_)) })
      case .short: return b.createVector(try signed(Int16.self))
      case .ushort: return b.createVector(try unsigned(UInt16.self))
      case .int: return b.createVector(try signed(Int32.self))
      case .uint: return b.createVector(try unsigned(UInt32.self))
      case .long: return b.createVector(try signed(Int64.self))
      case .ulong: return b.createVector(try unsigned(UInt64.self))
      case .bool: return b.createVector(try a.map { guard let x = $0 as? Bool else { throw AgentError.invalidArguments(where_) }; return x })
      default: throw AgentError.invalidArguments("\(where_) has an unsupported element type")
      }
    }
  }

  private func number(_ v: Any, _ where_: String) throws -> Double {
    guard let n = v as? NSNumber, n.doubleValue.isFinite else { throw AgentError.invalidArguments("\(where_) must be numbers") }
    return n.doubleValue
  }

  // MARK: - FlatBuffer -> JSON

  /// Renders a table as JSON for the model. Binary schemas are summarized, and
  /// byte vectors become text when they are valid UTF-8.
  public func json(_ object: Int, _ t: Table, depth: Int = 0) -> [String: Any] {
    var out: [String: Any] = [:]
    let obj = schema.objects[object]
    guard depth < 24 else { return ["truncated": true] }
    for f in obj.fields {
      let o = t.offset(Int32(f.offset))
      if o == 0 { continue }
      switch f.type.base {
      case .bool: out[f.name] = t.readBuffer(of: Bool.self, at: o)
      case .byte: out[f.name] = enumName(f, Int64(t.readBuffer(of: Int8.self, at: o)))
      case .ubyte: out[f.name] = enumName(f, Int64(t.readBuffer(of: UInt8.self, at: o)))
      case .short: out[f.name] = enumName(f, Int64(t.readBuffer(of: Int16.self, at: o)))
      case .ushort: out[f.name] = enumName(f, Int64(t.readBuffer(of: UInt16.self, at: o)))
      case .int: out[f.name] = Int64(t.readBuffer(of: Int32.self, at: o))
      case .uint: out[f.name] = UInt64(t.readBuffer(of: UInt32.self, at: o))
      case .long: out[f.name] = big(Int64(t.readBuffer(of: Int64.self, at: o)))
      case .ulong: out[f.name] = bigU(t.readBuffer(of: UInt64.self, at: o))
      case .float: out[f.name] = Double(t.readBuffer(of: Float.self, at: o))
      case .double: out[f.name] = t.readBuffer(of: Double.self, at: o)
      case .string: out[f.name] = t.string(at: o) ?? ""
      case .obj:
        out[f.name] = json(Int(f.type.index), Table(bb: t.bb, position: t.indirect(o + t.position)), depth: depth + 1)
      case .utype:
        let e = schema.enums[Int(f.type.index)]
        let raw = Int64(t.readBuffer(of: UInt8.self, at: o))
        out[f.name] = e.values.first { $0.value == raw }?.name ?? "\(raw)"
      case .union:
        let typeField = obj.fields.first { $0.name == "\(f.name)_type" }
        guard let tf = typeField else { continue }
        let raw = Int64(t.readBuffer(of: UInt8.self, at: t.offset(Int32(tf.offset))))
        guard let ev = schema.enums[Int(f.type.index)].values.first(where: { $0.value == raw }), let ut = ev.unionType else { continue }
        out[f.name] = json(Int(ut.index), Table(bb: t.bb, position: t.indirect(o + t.position)), depth: depth + 1)
      case .vector:
        out[f.name] = vectorJSON(f, t, o, depth)
      default:
        continue
      }
    }
    return out
  }

  private func vectorJSON(_ f: BFBS.Field, _ t: Table, _ o: Int32, _ depth: Int) -> Any {
    let n = Int(t.vector(count: o))
    let start = Int(t.vector(at: o))
    switch f.type.element {
    case .ubyte, .byte:
      if f.name.contains("bfbs") { return ["binary_schema_bytes": n] }
      let bytes = [UInt8](t.bb.readSlice(index: start, count: n) as [UInt8])
      if let s = String(bytes: bytes, encoding: .utf8) { return s }
      return ["base64": Data(bytes).base64EncodedString()]
    case .string:
      return (0..<n).compactMap { i -> String? in
        let pos = start + i * 4
        let s = pos + Int(t.bb.read(def: Int32.self, position: pos))
        let len = Int(t.bb.read(def: Int32.self, position: s))
        return t.bb.readString(at: s + 4, count: len)
      }
    case .obj:
      return (0..<n).map { i -> [String: Any] in
        let pos = start + i * 4
        let p = Int32(pos) + t.bb.read(def: Int32.self, position: pos)
        return json(Int(f.type.index), Table(bb: t.bb, position: p), depth: depth + 1)
      }
    case .double: return (0..<n).map { t.bb.read(def: Double.self, position: start + $0 * 8) }
    case .float: return (0..<n).map { Double(t.bb.read(def: Float.self, position: start + $0 * 4)) }
    case .short: return (0..<n).map { t.bb.read(def: Int16.self, position: start + $0 * 2) }
    case .ushort: return (0..<n).map { t.bb.read(def: UInt16.self, position: start + $0 * 2) }
    case .int: return (0..<n).map { t.bb.read(def: Int32.self, position: start + $0 * 4) }
    case .uint: return (0..<n).map { t.bb.read(def: UInt32.self, position: start + $0 * 4) }
    case .ulong: return (0..<n).map { bigU(t.bb.read(def: UInt64.self, position: start + $0 * 8)) }
    case .long: return (0..<n).map { big(t.bb.read(def: Int64.self, position: start + $0 * 8)) }
    case .bool: return (0..<n).map { t.bb.read(def: Bool.self, position: start + $0) }
    default: return ["elements": n]
    }
  }

  private func enumName(_ f: BFBS.Field, _ raw: Int64) -> Any {
    guard f.type.index >= 0 else { return raw }
    return schema.enums[Int(f.type.index)].values.first { $0.value == raw }?.name ?? raw
  }

  /// Beyond JSON's exact integers (±2^53 - 1) a 64 bit value is a decimal
  /// string; compared with both bounds (abs(Int64.min) does not exist).
  private func big(_ v: Int64) -> Any { v > 9_007_199_254_740_991 || v < -9_007_199_254_740_991 ? String(v) : v }
  private func bigU(_ v: UInt64) -> Any { v > 9_007_199_254_740_991 ? String(v) : v }
}
