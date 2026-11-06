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
  /// Run by the model provider (web search): it has no tool_use of ours.
  public var server = false
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
      let input = gen.object(index, depth: 0, skip: ToolCatalog.agentFields.union(Shared.data.notForModels[r] ?? []))
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
    struct Comment { let card: String, comment: String, text: String }
    struct Helper {
      let prompt: String, outside: String, invalid: String
      let each: String, eachReport: String, together: String, togetherReport: String
      let runRefused: String, reportsEach: String, reportsTogether: String
    }
    /// The guide (Agent.Role.guide): the tools it reads with, its own tool and the app's notes to it.
    struct Guide {
      let tools: [String]
      let showTool: Tool
      let texts: [String: String]
      func text(_ k: String) -> String { texts[k]! }
    }
    /// The provider's web search: how each API names it, and what the model and the user read.
    struct WebSearch {
      let name: String, anthropicType: String, openAIType: String, openAIInclude: String, note: String, sources: String
      let maxUses: Int
    }
    let requests: [String]
    let agentFields: Set<String>
    /// Fields of a request that the apps use and the model is not offered.
    let notForModels: [String: [String]]
    let referenceTool: Tool
    let delegateTool: Tool
    let helper: Helper
    let guide: Guide
    let draft: Draft
    let comment: Comment
    let webSearch: WebSearch
    let permissionDeclined: String
    let invalidArguments: String
  }

  static let data: Data = {
    let o = try! JSONSerialization.jsonObject(with: Foundation.Data(agentJSON.utf8)) as! [String: Any]
    let t = o["referenceTool"] as! [String: Any], d = o["draftRequest"] as! [String: String]
    let cm = o["commentRequest"] as! [String: String]
    let dt = o["delegateTool"] as! [String: Any], h = o["helper"] as! [String: String]
    let g = o["guide"] as! [String: Any], gt = g["showTool"] as! [String: Any]
    let w = o["webSearch"] as! [String: Any]
    return Data(requests: o["requests"] as! [String], agentFields: Set(o["agentFields"] as! [String]),
                notForModels: o["notForModels"] as! [String: [String]],
                referenceTool: .init(name: t["name"] as! String, description: t["description"] as! String,
                                     inputSchema: t["inputSchema"] as! [String: Any]),
                delegateTool: .init(name: dt["name"] as! String, description: dt["description"] as! String,
                                    inputSchema: dt["inputSchema"] as! [String: Any]),
                helper: .init(prompt: h["prompt"]!, outside: h["outside"]!, invalid: h["invalid"]!,
                              each: h["each"]!, eachReport: h["eachReport"]!, together: h["together"]!,
                              togetherReport: h["togetherReport"]!, runRefused: h["runRefused"]!,
                              reportsEach: h["reportsEach"]!, reportsTogether: h["reportsTogether"]!),
                guide: .init(tools: g["tools"] as! [String],
                             showTool: .init(name: gt["name"] as! String, description: gt["description"] as! String,
                                             inputSchema: gt["inputSchema"] as! [String: Any]),
                             texts: g.compactMapValues { $0 as? String }),
                draft: .init(root: d["root"]!, child: d["child"]!, card: d["card"]!, text: d["text"]!),
                comment: .init(card: cm["card"]!, comment: cm["comment"]!, text: cm["text"]!),
                webSearch: .init(name: w["name"] as! String, anthropicType: w["anthropicType"] as! String,
                                 openAIType: w["openAIType"] as! String, openAIInclude: w["openAIInclude"] as! String,
                                 note: w["note"] as! String, sources: w["sources"] as! String, maxUses: w["maxUses"] as! Int),
                permissionDeclined: o["permissionDeclined"] as! String,
                invalidArguments: o["invalidArguments"] as! String)
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
