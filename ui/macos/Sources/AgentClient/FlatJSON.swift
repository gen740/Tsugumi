import FlatBuffers
import Foundation

/// Renders recorded FlatBuffers values as JSON using their reflection schema.
/// Unions use the flatc JSON convention
/// (`<field>_type` + `<field>`). 64 bit integers are decimal strings in JSON.
public struct FlatJSON {
  public let schema: BFBS

  public init(schema: BFBS) { self.schema = schema }

  // MARK: - FlatBuffer -> JSON

  /// Renders a recorded table as JSON. Binary schemas are summarized, and
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
