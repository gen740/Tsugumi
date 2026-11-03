import FlatBuffers
import Foundation

/// A model tool backed by one core RPC request.
public struct ToolSpec {
  public let name: String           // e.g. "project_save"
  public let requestName: String    // e.g. "Tsugumi.RPC.ProjectSaveRequest"
  public let payloadType: UInt8     // RequestPayload union value
  public let object: Int            // index in the schema
  public let description: String
  public let inputSchema: [String: Any]
  /// Mutating requests carry an op_id that the Agent assigns once per tool
  /// call; re-sending the same call reuses it and never runs twice.
  public let mutating: Bool
}

/// Tool definitions generated from the published RPC schema (tsugumi.bfbs).
public struct ToolCatalog {
  public static let defaultRequests = Shared.data.requests
  /// Filled by the Agent, never by the model.
  static let agentFields = Shared.data.agentFields

  public let schema: BFBS
  public let tools: [ToolSpec]

  public init(schema: BFBS, requests: [String] = ToolCatalog.defaultRequests) throws {
    self.schema = schema
    guard let union = schema.enums.first(where: { $0.name == "Tsugumi.RPC.RequestPayload" }) else {
      throw AgentError.schema("RequestPayload union not found")
    }
    var out: [ToolSpec] = []
    for r in requests {
      let full = "Tsugumi.RPC.\(r)"
      guard let (index, obj) = schema.object(named: full),
            let v = union.values.first(where: { $0.unionType?.index == Int32(index) }) else {
        throw AgentError.schema("request \(r) not found")
      }
      let gen = SchemaGenerator(schema: schema)
      let input = gen.object(index, depth: 0, skip: ToolCatalog.agentFields)
      let doc = obj.documentation.joined(separator: " ").trimmingCharacters(in: .whitespaces)
      out.append(ToolSpec(name: ToolCatalog.toolName(r), requestName: full, payloadType: UInt8(v.value), object: index,
                          description: "\(r.replacingOccurrences(of: "Request", with: "")): \(doc)",
                          inputSchema: input, mutating: obj.fields.contains { $0.name == "op_id" }))
    }
    tools = out
  }

  public func tool(named n: String) -> ToolSpec? { tools.first { $0.name == n } }

  /// "ProjectSaveRequest" -> "project_save"
  static func toolName(_ request: String) -> String {
    let base = request.replacingOccurrences(of: "Request", with: "")
    var out = ""
    for (i, c) in base.enumerated() {
      if c.isUppercase && i > 0 { out += "_" }
      out += c.lowercased()
    }
    return out
  }
}

/// agent/Shared/agent.json, which the web Agent reads too.
extension Shared {
  struct Data {
    struct Tool { let name: String, description: String, inputSchema: [String: Any] }
    struct Draft { let root: String, child: String, card: String, text: String }
    let requests: [String]
    let agentFields: Set<String>
    let referenceTool: Tool
    let draft: Draft
    let permissionDeclined: String
  }

  static let data: Data = {
    let o = try! JSONSerialization.jsonObject(with: Foundation.Data(agentJSON.utf8)) as! [String: Any]
    let t = o["referenceTool"] as! [String: Any], d = o["draftRequest"] as! [String: String]
    return Data(requests: o["requests"] as! [String], agentFields: Set(o["agentFields"] as! [String]),
                referenceTool: .init(name: t["name"] as! String, description: t["description"] as! String,
                                     inputSchema: t["inputSchema"] as! [String: Any]),
                draft: .init(root: d["root"]!, child: d["child"]!, card: d["card"]!, text: d["text"]!),
                permissionDeclined: o["permissionDeclined"] as! String)
  }()

  /// `{name}` in a text replaced by its value, in one pass (values are not read again).
  static func fill(_ text: String, _ values: [String: String]) -> String {
    let re = try! Regex(#"\{(\w+)\}"#)
    return text.replacing(re) { m in values[String(m.output[1].substring ?? "")] ?? String(m.output[0].substring ?? "") }
  }
}

/// JSON Schema (subset) from reflection objects.
struct SchemaGenerator {
  let schema: BFBS

  func object(_ index: Int, depth: Int, skip: Set<String> = []) -> [String: Any] {
    let obj = schema.objects[index]
    var props: [String: Any] = [:]
    var required: [String] = []
    guard depth < 8 else { return ["type": "object"] }
    for f in obj.fields where !skip.contains(f.name) {
      if f.type.base == .utype { continue }
      var p = type(f, depth: depth)
      if !f.documentation.isEmpty { p["description"] = f.documentation.joined(separator: " ") }
      if f.type.base == .union {
        let e = schema.enums[Int(f.type.index)]
        props["\(f.name)_type"] = ["type": "string", "enum": e.values.filter { $0.value != 0 }.map(\.name),
                                   "description": "variant of \(f.name)"]
      }
      props[f.name] = p
      if f.required { required.append(f.name) }
    }
    var out: [String: Any] = ["type": "object", "properties": props, "additionalProperties": false]
    if !required.isEmpty { out["required"] = required }
    return out
  }

  func type(_ f: BFBS.Field, depth: Int) -> [String: Any] {
    switch f.type.base {
    case .bool: return ["type": "boolean"]
    case .byte, .ubyte, .short, .ushort, .int, .uint:
      if f.type.index >= 0 {
        return ["type": "string", "enum": schema.enums[Int(f.type.index)].values.map(\.name)]
      }
      return ["type": "integer"]
    case .long, .ulong:
      return ["type": "string", "pattern": "^-?[0-9]+$", "description": "64 bit integer as a decimal string"]
    case .float, .double: return ["type": "number"]
    case .string: return ["type": "string"]
    case .obj: return object(Int(f.type.index), depth: depth + 1)
    case .union:
      let e = schema.enums[Int(f.type.index)]
      let variants = e.values.compactMap { v -> [String: Any]? in
        guard let t = v.unionType, t.index >= 0 else { return nil }
        return object(Int(t.index), depth: depth + 1)
      }
      return ["anyOf": variants]
    case .vector:
      switch f.type.element {
      case .ubyte, .byte: return ["type": "string", "description": "UTF-8 text (or {\"base64\": ...})"]
      case .string: return ["type": "array", "items": ["type": "string"]]
      case .obj: return ["type": "array", "items": object(Int(f.type.index), depth: depth + 1)]
      case .long, .ulong: return ["type": "array", "items": ["type": "string"]]
      case .double, .float: return ["type": "array", "items": ["type": "number"]]
      case .bool: return ["type": "array", "items": ["type": "boolean"]]
      default: return ["type": "array"]
      }
    default: return [:]
    }
  }
}
