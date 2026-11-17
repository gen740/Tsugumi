import FlatBuffers
import Foundation

/// Minimal reader for FlatBuffers binary schemas (reflection.fbs layout).
/// The app reads port types and recorded values from their schemas.
public struct BFBS {
  public enum BaseType: Int8 {
    case none = 0, utype, bool, byte, ubyte, short, ushort, int, uint, long, ulong, float, double
    case string, vector, obj, union, array, vector64
  }

  public struct TypeInfo {
    public let base: BaseType
    public let element: BaseType
    public let index: Int32
  }

  public struct Field {
    public let name: String
    public let type: TypeInfo
    public let id: UInt16
    public let offset: UInt16  // vtable slot
  }

  public struct Object {
    public let name: String
    public let fields: [Field]
    public let isStruct: Bool
  }

  public struct EnumValue {
    public let name: String
    public let value: Int64
    public let unionType: TypeInfo?
  }

  public struct Enum {
    public let name: String
    public let values: [EnumValue]
  }

  public let objects: [Object]
  public let enums: [Enum]

  public init(data: Data) throws {
    let bb = ByteBuffer(data: data)
    guard data.count > 8 else { throw AgentError.schema("schema too short") }
    let root = Int32(bb.read(def: UInt32.self, position: 0))
    let schema = Table(bb: bb, position: root)
    objects = BFBS.tables(schema, 0).map(BFBS.object)
    enums = BFBS.tables(schema, 1).map(BFBS.enumeration)

  }

  public func object(named name: String) -> (Int, Object)? {
    objects.firstIndex { $0.name == name }.map { ($0, objects[$0]) }
  }

  // MARK: - Parsing helpers

  private static func vo(_ id: Int32) -> Int32 { 4 + 2 * id }

  private static func tables(_ t: Table, _ id: Int32) -> [Table] {
    let o = t.offset(vo(id))
    guard o != 0 else { return [] }
    let n = t.vector(count: o)
    let start = t.vector(at: o)
    return (0..<n).map { i in
      let pos = start + i * 4
      return Table(bb: t.bb, position: pos + t.bb.read(def: Int32.self, position: Int(pos)))
    }
  }

  private static func string(_ t: Table, _ id: Int32) -> String {
    let o = t.offset(vo(id))
    return o == 0 ? "" : (t.string(at: o) ?? "")
  }

  private static func scalar<T>(_ t: Table, _ id: Int32, _ def: T) -> T {
    let o = t.offset(vo(id))
    return o == 0 ? def : t.readBuffer(of: T.self, at: o)
  }

  private static func child(_ t: Table, _ id: Int32) -> Table? {
    let o = t.offset(vo(id))
    return o == 0 ? nil : Table(bb: t.bb, position: t.indirect(o + t.position))
  }

  private static func type(_ t: Table) -> TypeInfo {
    TypeInfo(base: BaseType(rawValue: scalar(t, 0, Int8(0))) ?? .none,
             element: BaseType(rawValue: scalar(t, 1, Int8(0))) ?? .none,
             index: scalar(t, 2, Int32(-1)))
  }

  private static func object(_ t: Table) -> Object {
    let fields = tables(t, 1).map { f in
      Field(name: string(f, 0), type: child(f, 1).map(type) ?? TypeInfo(base: .none, element: .none, index: -1),
            id: scalar(f, 2, UInt16(0)), offset: scalar(f, 3, UInt16(0)))
    }.sorted { $0.id < $1.id }
    return Object(name: string(t, 0), fields: fields, isStruct: scalar(t, 2, false))
  }

  private static func enumeration(_ t: Table) -> Enum {
    let values = tables(t, 1).map { v in
      EnumValue(name: string(v, 0), value: scalar(v, 1, Int64(0)), unionType: child(v, 3).map(type))
    }
    return Enum(name: string(t, 0), values: values)
  }
}

public enum AgentError: Error, CustomStringConvertible {
  case schema(String)
  case invalidArguments(String)
  case provider(String)
  case cancelled

  public var description: String {
    switch self {
    case .schema(let s): return "schema: \(s)"
    case .invalidArguments(let s): return "invalid tool arguments: \(s)"
    case .provider(let s): return "model provider: \(s)"
    case .cancelled: return "cancelled"
    }
  }
}
