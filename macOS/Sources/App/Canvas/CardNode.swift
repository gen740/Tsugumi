import AppKit
import SwiftUI
import TsugumiRPC

/// Size and port positions of a Card on the canvas (world units, relative
/// to the Card's top-left corner). Shared by drawing and hit-testing.
struct CardGeometry {
  static let width: CGFloat = 214
  static let header: CGFloat = 46
  static let summaryRow: CGFloat = 16
  static let portRow: CGFloat = 18
  static let paramRow: CGFloat = 26
  static let resultRow: CGFloat = 22

  let inputs: [String]
  let outputs: [String]
  let collapsed: Bool
  let summary: Bool
  let params: Int
  let result: Bool

  var portsTop: CGFloat { Self.header + (summary ? Self.summaryRow : 0) }

  var height: CGFloat {
    if collapsed { return Self.header }
    return portsTop + CGFloat(max(inputs.count, outputs.count, 1)) * Self.portRow + CGFloat(params) * Self.paramRow +
      (result ? Self.resultRow : 0) + 8
  }

  var size: CGSize { CGSize(width: Self.width, height: height) }

  func port(_ name: String, output: Bool) -> CGPoint {
    let list = output ? outputs : inputs
    let x = output ? Self.width : 0
    if collapsed { return CGPoint(x: x, y: Self.header / 2) }
    let i = CGFloat(list.firstIndex(of: name) ?? 0)
    return CGPoint(x: x, y: portsTop + i * Self.portRow + Self.portRow / 2)
  }
}

extension ProjectSession {
  /// Ports for drawing: resolved ports when available, else declared inputs.
  func ports(_ k: CardKey) -> (inputs: [String], outputs: [String]) {
    if let r = resolution[k] { return (r.inputs.map(\.name), r.outputs.map(\.name)) }
    let def = instance(k)?["definition"]?.string
    let ins = def.flatMap { definitions[$0]?["inputs"]?.array }?.compactMap { $0["name"]?.string } ?? []
    return (ins, [])
  }

  /// Params edited directly on input Cards.
  func inlineParams(_ k: CardKey) -> [JSONValue] {
    guard kind(k) == .input, let d = instance(k)?["definition"]?.string else { return [] }
    return (definitions[d]?["params"]?.array ?? []).filter {
      ["int64", "uint64", "double", "bool", "string"].contains($0["type"]?.string ?? "")
    }
  }

  func cardDescription(_ k: CardKey) -> String? {
    if let d = instance(k)?["description"]?.string, !d.isEmpty { return d }
    if let id = instance(k)?["definition"]?.string {
      if let d = definitions[id]?["description"]?.string, !d.isEmpty { return d }
      if let d = definitionMeta[id]?["description"]?.string, !d.isEmpty { return d }
    }
    return nil
  }

  /// One line describing the latest result of a Card.
  func resultSummary(_ k: CardKey) -> String? {
    if let s = cardStates[k], s.state == .failed { return s.failure ?? "失敗" }
    switch displays[k]?.value {
    case .text(let t)?: return t.split(separator: "\n").first.map(String.init) ?? ""
    case .scalar(let v, let l)?: return "\(l) = \(v.formatted(.number.precision(.significantDigits(6))))"
    case .lineChart(let s)?: return "折れ線 \(s.count) 系列 · \(s.map(\.x.count).max() ?? 0) 点"
    case nil: return nil
    }
  }

  func geometry(_ k: CardKey) -> CardGeometry {
    let p = ports(k)
    return CardGeometry(inputs: p.inputs, outputs: p.outputs, collapsed: collapsed(k), summary: cardDescription(k) != nil,
                        params: inlineParams(k).count, result: resultSummary(k) != nil)
  }
}

struct CardNode: View {
  let session: ProjectSession
  let cardKey: CardKey
  let selected: Bool
  let onPortDrag: (String, CGPoint) -> Void
  let onPortDrop: (String, CGPoint) -> Void

  var body: some View {
    let g = session.geometry(cardKey)
    let state = session.cardStates[cardKey]
    let diags = session.resolution[cardKey]?.diagnostics ?? []
    let errors = diags.filter(\.error)
    let kind = session.kind(cardKey)
    VStack(alignment: .leading, spacing: 0) {
      header(kind: kind, state: state, errors: errors, warnings: diags.count - errors.count)
        .frame(height: CardGeometry.header)
      if !g.collapsed {
        if let d = session.cardDescription(cardKey) {
          Text(d).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
            .padding(.horizontal, 10).frame(height: CardGeometry.summaryRow, alignment: .top)
        }
        ports(g)
        ForEach(Array(session.inlineParams(cardKey).enumerated()), id: \.offset) { _, p in
          InlineParamField(session: session, cardKey: cardKey, param: p)
            .frame(height: CardGeometry.paramRow)
            .padding(.horizontal, 10)
        }
        if let r = session.resultSummary(cardKey) {
          HStack(spacing: 4) {
            Image(systemName: state?.state == .failed ? "xmark.octagon" : "text.below.photo").font(.system(size: 9))
            Text(r).font(.system(size: 10, design: .monospaced)).lineLimit(1)
          }
          .foregroundStyle(state?.state == .failed ? .red : .secondary)
          .padding(.horizontal, 10)
          .frame(height: CardGeometry.resultRow)
        }
      }
    }
    .padding(.bottom, g.collapsed ? 0 : 8)
    .frame(width: CardGeometry.width, height: g.height, alignment: .topLeading)
    .background(RoundedRectangle(cornerRadius: 9).fill(Color(nsColor: .controlBackgroundColor)))
    .overlay(alignment: .leading) {
      // Collapsed Cards keep one connection dot per side.
      if g.collapsed {
        HStack {
          if !g.inputs.isEmpty { Circle().strokeBorder(Color.secondary).frame(width: 9, height: 9).offset(x: -4.5) }
          Spacer()
          if !g.outputs.isEmpty { Circle().fill(Color.accentColor).frame(width: 9, height: 9).offset(x: 4.5) }
        }
      }
    }
    .overlay(RoundedRectangle(cornerRadius: 9).strokeBorder(borderColor(state?.state, kind),
                                                            style: StrokeStyle(lineWidth: selected ? 2.5 : 1, dash: kind == .draft ? [5, 3] : [])))
    .shadow(color: .black.opacity(selected ? 0.25 : 0.1), radius: selected ? 5 : 2, y: 1)
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Card \(session.displayName(cardKey)), \(kind.label), \(state?.state.label ?? "未実行")")
  }

  private func header(kind: CardKind, state: CardRunState?, errors: [Diagnostic], warnings: Int) -> some View {
    HStack(spacing: 7) {
      Image(systemName: kind.symbol).font(.system(size: 13)).foregroundStyle(.secondary).frame(width: 16)
      VStack(alignment: .leading, spacing: 1) {
        Text(session.displayName(cardKey)).font(.system(size: 12, weight: .semibold)).lineLimit(1)
        HStack(spacing: 4) {
          if let s = state {
            Image(systemName: s.state.symbol).foregroundStyle(s.state.color)
            Text(s.state.label + (s.invocation > 1 ? " #\(s.invocation)" : ""))
          } else {
            Text(kind == .subgraph ? "子 Workflow · ダブルクリックで開く" : kind.label)
          }
        }
        .font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
      }
      Spacer(minLength: 0)
      if !errors.isEmpty {
        Image(systemName: "xmark.octagon.fill").foregroundStyle(.red).help(errors.map(\.message).joined(separator: "\n"))
      } else if warnings > 0 {
        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow)
          .help((session.resolution[cardKey]?.diagnostics ?? []).map(\.message).joined(separator: "\n"))
      }
      if session.collapsed(cardKey) {
        Image(systemName: "rectangle.compress.vertical").font(.caption2).foregroundStyle(.secondary).help("折りたたみ表示")
      }
    }
    .padding(.horizontal, 10)
  }

  private func ports(_ g: CardGeometry) -> some View {
    HStack(alignment: .top, spacing: 0) {
      VStack(alignment: .leading, spacing: 0) {
        ForEach(g.inputs, id: \.self) { p in
          HStack(spacing: 4) {
            Circle().strokeBorder(Color.secondary, lineWidth: 1.5).background(Circle().fill(Color(nsColor: .controlBackgroundColor)))
              .frame(width: 10, height: 10).offset(x: -5)
            Text(p).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
          }
          .frame(height: CardGeometry.portRow)
          .help(portHelp(p, output: false))
        }
      }
      Spacer(minLength: 4)
      VStack(alignment: .trailing, spacing: 0) {
        ForEach(g.outputs, id: \.self) { p in
          HStack(spacing: 4) {
            Text(p).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
            Circle().fill(Color.accentColor).frame(width: 10, height: 10).offset(x: 5)
              .contentShape(Circle().inset(by: -6))
              .gesture(DragGesture(coordinateSpace: .named("canvas"))
                .onChanged { onPortDrag(p, $0.location) }
                .onEnded { onPortDrop(p, $0.location) })
              .help("ドラッグして入力ポートへ接続")
          }
          .frame(height: CardGeometry.portRow)
          .help(portHelp(p, output: true))
        }
      }
    }
    .frame(minHeight: CardGeometry.portRow)
  }

  private func portHelp(_ name: String, output: Bool) -> String {
    let r = session.resolution[cardKey]
    let port = (output ? r?.outputs : r?.inputs)?.first { $0.name == name }
    var s = "\(output ? "出力" : "入力") \(name)"
    if let t = port?.rootType { s += " : \(t)" }
    if port?.optional == true { s += "（省略可）" }
    return s
  }

  private func borderColor(_ s: Tsugumi_Data_CardState?, _ kind: CardKind) -> Color {
    if selected { return .accentColor }
    if let s, s != .notStarted { return s.color.opacity(0.7) }
    return .secondary.opacity(kind == .draft ? 0.7 : 0.35)
  }
}

/// A param of an input Card edited in place; committed on Return or focus loss.
struct InlineParamField: View {
  let session: ProjectSession
  let cardKey: CardKey
  let param: JSONValue
  @State private var text = ""
  @FocusState private var focused: Bool

  var body: some View {
    let name = param["name"]?.string ?? "?"
    let type = param["type"]?.string ?? "string"
    let value = session.instance(cardKey)?["params"]?[name] ?? param["data"]
    HStack(spacing: 6) {
      Text(name).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
      if type == "bool" {
        Toggle("", isOn: Binding(get: { value?.bool ?? false }, set: { session.setParam(cardKey, name, .bool($0)) }))
          .toggleStyle(.switch).controlSize(.mini).labelsHidden()
          .disabled(!session.isEditable)
      } else {
        TextField("", text: $text)
          .textFieldStyle(.roundedBorder)
          .font(.system(size: 11, design: .monospaced))
          .controlSize(.small)
          .focused($focused)
          .onSubmit { commit(name, type) }
          .onChange(of: focused) { if !focused { commit(name, type) } }
          .disabled(!session.isEditable)
          .help(session.isEditable ? "\(type)・Return で反映" : "実行中は変更できません")
      }
    }
    .onAppear { text = display(value) }
    .onChange(of: value) { if !focused { text = display(value) } }
  }

  private func display(_ v: JSONValue?) -> String {
    switch v {
    case .string(let s)?: return s
    case .number(let n)?: return n
    default: return ""
    }
  }

  private func commit(_ name: String, _ type: String) {
    let current = display(session.instance(cardKey)?["params"]?[name] ?? param["data"])
    guard text != current else { return }
    session.setParam(cardKey, name, parseParam(text, type))
  }
}

/// Integers keep their exact decimal spelling (64 bit safe); the core
/// validates types and ranges and reports what it rejects.
func parseParam(_ s: String, _ type: String) -> JSONValue {
  let t = s.trimmingCharacters(in: .whitespaces)
  switch type {
  case "int64", "uint64": return t.range(of: #"^-?[0-9]+$"#, options: .regularExpression) != nil ? .number(t) : .string(t)
  case "double": return Double(t) != nil && !t.isEmpty ? .number(t) : .string(t)
  case "bool": return .bool(t == "true")
  case "string": return .string(s)
  default: return (try? JSONValue.parse(t)) ?? .string(s)
  }
}
