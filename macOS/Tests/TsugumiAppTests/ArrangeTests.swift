import Foundation
import Testing
@testable import TsugumiApp

/// The 整列 button gives what the Web UI gives: the same input and result as
/// tests/fixtures/arrange-contract.json, which web/tests/arrange.ts checks there
/// (`cd web && npm test -- --update-arrange` after a change on purpose).
@MainActor
struct ArrangeTests {
  nonisolated static let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent()

  static func fixture(_ name: String) throws -> [[String: Any]] {
    let data = try Data(contentsOf: root.appendingPathComponent("tests/fixtures/\(name)"))
    return try #require(try JSONSerialization.jsonObject(with: data) as? [[String: Any]])
  }

  static func flow(_ name: String?) -> FlowDirection { name == "leftToRight" ? .leftToRight : .topToBottom }
  static func number(_ v: Any?) -> Double { (v as? NSNumber)?.doubleValue ?? 0 }

  static func kind(_ name: String) -> CardKind {
    switch name {
    case "trigger": return .trigger
    case "input": return .input
    case "fork": return .fork
    case "branch": return .branch
    case "merge": return .merge
    case "logger": return .logger
    case "draft": return .draft
    case "subgraph": return .subgraph
    default: return .code
    }
  }

  static func wires(_ list: Any?) -> [ArrangeWire] {
    (list as? [[String: Any]] ?? []).map {
      ArrangeWire(from: $0["from"] as? String ?? "", fromPort: $0["fromPort"] as? String ?? "", to: $0["to"] as? String ?? "", toPort: $0["toPort"] as? String ?? "")
    }
  }

  static func boxes(_ list: Any?) -> [ArrangeBox] {
    (list as? [[String: Any]] ?? []).map { ArrangeBox(id: $0["id"] as? String ?? "", width: number($0["width"]), height: number($0["height"])) }
  }

  static func ends(_ list: Any?) -> [ArrangeBarEnd] {
    (list as? [[String: Any]] ?? []).map { ArrangeBarEnd(card: $0["card"] as? String ?? "", port: $0["port"] as? String ?? "") }
  }

  /// The numbers of the contract's `input`.
  static func input(_ j: [String: Any]) -> ArrangeInput {
    func ports(_ list: Any?) -> [ArrangePort] {
      (list as? [[String: Any]] ?? []).map { ArrangePort(name: $0["name"] as? String ?? "", x: number($0["x"]), y: number($0["y"])) }
    }
    let cards = (j["cards"] as? [[String: Any]] ?? []).map {
      ArrangeCard(id: $0["id"] as? String ?? "", width: number($0["width"]), height: number($0["height"]), inputs: ports($0["inputs"]), outputs: ports($0["outputs"]))
    }
    let boundary = j["boundary"] as? [String: Any]
    return ArrangeInput(flow: flow(j["flow"] as? String), grid: number(j["grid"]), snap: j["snap"] as? Bool ?? false, cards: cards, wires: wires(j["wires"]), frames: boxes(j["frames"]), notes: boxes(j["notes"]),
                        boundaryInputs: ends(boundary?["inputs"]), boundaryOutputs: ends(boundary?["outputs"]))
  }

  /// What `CardGeometry` gives for a Card of arrange-demos.json / arrange-cases.json.
  static func card(_ j: [String: Any], flow: FlowDirection, grid: CGFloat?) -> ArrangeCard {
    let id = j["id"] as? String ?? ""
    func names(_ key: String) -> [String] { j[key] as? [String] ?? [] }
    let params = (0..<Int(number(j["params"]))).map { ParamData(name: "p\($0)", type: "int64", value: "0") }
    let data = CardNodeData(title: id, kind: kind(j["kind"] as? String ?? ""), summary: j["summary"] as? Bool == true ? "s" : nil,
                            inputs: names("inputs").map { PortData(name: $0) }, outputs: names("outputs").map { PortData(name: $0) }, params: params,
                            collapsed: j["collapsed"] as? Bool ?? false, hasDisplay: j["display"] as? Bool ?? false, portGrid: grid)
    let g = CardGeometry(data, flow: flow)
    func ports(_ list: [String], output: Bool) -> [ArrangePort] {
      list.map { let p = g.port($0, output: output); return ArrangePort(name: $0, x: Double(p.x), y: Double(p.y)) }
    }
    return ArrangeCard(id: id, width: Double(g.size.width), height: Double(g.size.height), inputs: ports(names("inputs"), output: false), outputs: ports(names("outputs"), output: true))
  }

  static func result(_ j: [String: Any]) -> ArrangeResult {
    func list(_ key: String) -> [ArrangeAt] {
      (j[key] as? [[String: Any]] ?? []).map { ArrangeAt(id: $0["id"] as? String ?? "", x: number($0["x"]), y: number($0["y"])) }
    }
    return ArrangeResult(cards: list("cards"), frames: list("frames"), notes: list("notes"))
  }

  @Test func theLayoutIsTheOneTheWebUIGives() throws {
    for e in try Self.fixture("arrange-contract.json") {
      let name = e["name"] as? String ?? ""
      let j = try #require(e["input"] as? [String: Any])
      let input = Self.input(j)
      let got = arrangeCards(input)
      #expect(got == Self.result(try #require(e["expect"] as? [String: Any])), "\(name)")
      #expect(got == arrangeCards(input), "\(name): the same input, the same result")
    }
  }

  @Test func theGeometryIsTheOneTheWebUIGives() throws {
    let contract = try Self.fixture("arrange-contract.json")
    let cases = try Self.fixture("arrange-demos.json") + Self.fixture("arrange-cases.json")
    for e in contract {
      let parts = (e["name"] as? String ?? "").split(separator: " ")
      let c = try #require(cases.first { $0["name"] as? String == String(parts[0]) })
      let flow = Self.flow(String(parts[1]))
      let grid: CGFloat? = parts[2] == "off" ? nil : 20
      let want = Self.input(try #require(e["input"] as? [String: Any]))
      let cards = (c["cards"] as? [[String: Any]] ?? []).map { Self.card($0, flow: flow, grid: grid) }
      #expect(cards.count == want.cards.count)
      for (a, b) in zip(cards, want.cards) {
        let same = a.id == b.id && a.width == b.width && a.height == b.height &&
          a.inputs.map(\.name) == b.inputs.map(\.name) && a.outputs.map(\.name) == b.outputs.map(\.name) &&
          zip(a.inputs + a.outputs, b.inputs + b.outputs).allSatisfy { $0.x == $1.x && $0.y == $1.y }
        #expect(same, "\(e["name"] ?? ""): \(a.id) (npm test -- --update-arrange after changing CardGeometry)")
      }
    }
  }

  @Test func nothingOverlaps() throws {
    for e in try Self.fixture("arrange-contract.json") {
      let name = e["name"] as? String ?? ""
      let input = Self.input(try #require(e["input"] as? [String: Any]))
      let r = arrangeCards(input)
      var rects: [(String, CGRect)] = []
      for a in r.cards {
        if input.snap {
          #expect(snapToGrid(a.x, input.grid) == a.x && snapToGrid(a.y, input.grid) == a.y, "\(name): the grid leaves \(a.id) where it is")
        }
        let c = try #require(input.cards.first { $0.id == a.id })
        rects.append(("card \(a.id)", CGRect(x: a.x, y: a.y, width: c.width, height: c.height)))
      }
      for a in r.frames {
        let f = try #require(input.frames.first { $0.id == a.id })
        rects.append(("frame \(a.id)", CGRect(x: a.x, y: a.y, width: f.width, height: f.height)))
      }
      for a in r.notes {
        let n = try #require(input.notes.first { $0.id == a.id })
        rects.append(("note \(a.id)", CGRect(x: a.x, y: a.y, width: n.width, height: n.height)))
      }
      for i in rects.indices {
        for j in rects.indices where j > i {
          #expect(!rects[i].1.intersects(rects[j].1), "\(name): \(rects[i].0) and \(rects[j].0) overlap")
        }
      }
    }
  }
}
