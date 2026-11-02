import SwiftUI
import TsugumiRPC

/// Card settings and sources. Every edit goes to the draft (undoable) and is
/// committed with Save. "Inherit" removes the instance override; Off/0/""
/// stay explicit values.
struct InspectorView: View {
  @Bindable var session: ProjectSession
  @State private var tab = 0

  var body: some View {
    VStack(spacing: 0) {
      Picker("", selection: $tab) {
        Text("Card").tag(0)
        Text("Source").tag(1)
      }
      .pickerStyle(.segmented).labelsHidden().padding(6)
      if let k = session.selection.first, session.selection.count == 1, session.instance(k) != nil {
        if tab == 0 { CardSettings(session: session, cardKey: k) } else { SourceEditor(session: session, cardKey: k) }
      } else {
        ContentUnavailableView("Select a Card", systemImage: "square.on.square", description: Text("Click a Card on the canvas."))
      }
    }
  }
}

private enum Tri: String, CaseIterable, Identifiable {
  case inherit = "Inherit", on = "On", off = "Off"
  var id: String { rawValue }
}

struct CardSettings: View {
  @Bindable var session: ProjectSession
  let cardKey: CardKey

  var body: some View {
    let inst = session.instance(cardKey) ?? .object([])
    let defId = inst["definition"]?.string
    let def = defId.flatMap { session.definitions[$0] }
    Form {
      Section("Card") {
        LabeledContent("Path", value: cardKey)
        LabeledContent("Definition", value: defId ?? "(subgraph or draft)")
        TextField("Name", text: stringBinding("name", inst))
        TextField("Description", text: stringBinding("description", inst), axis: .vertical)
      }
      if let params = def?["params"]?.array, !params.isEmpty {
        Section("Parameters") {
          ForEach(params.indices, id: \.self) { i in
            ParamField(session: session, cardKey: cardKey, param: params[i])
          }
        }
      }
      Section("Execution") {
        numberField("maxRuns", inst, def, help: "Positive integer; limits invocations per run")
        timeoutField("timeoutSeconds", inst, def)
        timeoutField("buildTimeoutSeconds", inst, def)
        triPicker("recordIO", inst, def)
        triPicker("retainArtifact", inst, def)
        triPicker("filesystem", inst, def)
        triPicker("sandbox", inst, def)
        triPicker("canvasEnabled", inst, def)
      }
      let diags = session.resolution[cardKey]?.diagnostics ?? []
      if !diags.isEmpty {
        Section("Diagnostics") {
          ForEach(diags) { d in Label(d.message, systemImage: d.error ? "xmark.circle" : "exclamationmark.triangle") }
        }
      }
      if let r = session.resolution[cardKey] {
        Section("Ports") {
          ForEach(r.inputs, id: \.name) { p in LabeledContent("in \(p.name)", value: p.rootType ?? "unresolved") }
          ForEach(r.outputs, id: \.name) { p in LabeledContent("out \(p.name)", value: p.rootType ?? "unresolved") }
        }
      }
    }
    .formStyle(.grouped)
  }

  private func stringBinding(_ name: String, _ inst: JSONValue) -> Binding<String> {
    Binding(
      get: { inst[name]?.string ?? "" },
      set: { v in session.edit { $0.updateInstance(cardKey) { $0[name] = v.isEmpty ? nil : .string(v) } } })
  }

  private func triPicker(_ name: String, _ inst: JSONValue, _ def: JSONValue?) -> some View {
    let current: Tri = inst[name]?.bool.map { $0 ? .on : .off } ?? .inherit
    let inherited = def?[name]?.bool.map { $0 ? "on" : "off" } ?? "default"
    return Picker(name, selection: Binding(get: { current }, set: { v in
      session.edit { $0.updateInstance(cardKey) { c in
        c[name] = v == .inherit ? nil : .bool(v == .on)
      } }
    })) {
      Text("Inherit (\(inherited))").tag(Tri.inherit)
      Text("On").tag(Tri.on)
      Text("Off").tag(Tri.off)
    }
  }

  private func numberField(_ name: String, _ inst: JSONValue, _ def: JSONValue?, help: String) -> some View {
    let placeholder = def?[name]?.numberText.map { "Inherit (\($0))" } ?? "Inherit (1)"
    return TextField(name, text: Binding(
      get: { inst[name]?.numberText ?? "" },
      set: { v in
        session.edit { $0.updateInstance(cardKey) { c in
          let t = v.trimmingCharacters(in: .whitespaces)
          c[name] = t.isEmpty ? nil : (UInt64(t) != nil ? .number(t) : .string(t))  // core reports non-numbers
        } }
      }), prompt: Text(placeholder))
    .help(help)
  }

  private func timeoutField(_ name: String, _ inst: JSONValue, _ def: JSONValue?) -> some View {
    HStack {
      TextField(name, text: Binding(
        get: { inst[name]?.numberText ?? (inst[name] == .null ? "∞" : "") },
        set: { v in
          session.edit { $0.updateInstance(cardKey) { c in
            let t = v.trimmingCharacters(in: .whitespaces)
            if t.isEmpty { c[name] = nil } else if t == "∞" { c[name] = .null } else { c[name] = .number(t) }
          } }
        }), prompt: Text("Inherit"))
      Button("No limit") { session.edit { $0.updateInstance(cardKey) { $0[name] = .null } } }
        .help("Explicit null: no deadline even if the definition sets one")
    }
  }
}

struct ParamField: View {
  @Bindable var session: ProjectSession
  let cardKey: CardKey
  let param: JSONValue

  var body: some View {
    let name = param["name"]?.string ?? "?"
    let type = param["type"]?.string ?? "string"
    let override = session.instance(cardKey)?["params"]?[name]
    let value = override ?? param["data"]
    HStack {
      if type == "bool" {
        Toggle(name, isOn: Binding(get: { value?.bool ?? false }, set: { set(name, .bool($0)) }))
      } else {
        TextField(name, text: Binding(
          get: { display(value) },
          set: { v in set(name, parse(v, type)) }), prompt: Text(display(param["data"])))
      }
      Text(type).font(.caption2).foregroundStyle(.secondary)
      if override != nil {
        Button { set(name, nil) } label: { Image(systemName: "arrow.counterclockwise") }
          .buttonStyle(.borderless).help("Remove the override and inherit the default")
      }
    }
  }

  private func display(_ v: JSONValue?) -> String {
    switch v {
    case .string(let s)?: return s
    case .number(let n)?: return n
    case .bool(let b)?: return b ? "true" : "false"
    case .array?: return v!.serialized(pretty: false)
    default: return ""
    }
  }

  /// Integers keep their exact decimal spelling (64 bit safe); the core validates types and ranges.
  private func parse(_ s: String, _ type: String) -> JSONValue {
    switch type {
    case "int64", "uint64": return s.range(of: #"^-?[0-9]+$"#, options: .regularExpression) != nil ? .number(s) : .string(s)
    case "double": return Double(s) != nil ? .number(s) : .string(s)
    case "string": return .string(s)
    default: return (try? JSONValue.parse(s)) ?? .string(s)
    }
  }

  private func set(_ name: String, _ v: JSONValue?) {
    session.edit { $0.updateInstance(cardKey) { c in
      var params = c["params"] ?? .object([])
      params[name] = v
      c["params"] = (params.object?.isEmpty ?? true) ? nil : params
    } }
  }
}

struct SourceEditor: View {
  @Bindable var session: ProjectSession
  let cardKey: CardKey
  @State private var file: String?
  @State private var text = ""
  @State private var loadedHash: String?
  @State private var error: String?

  var body: some View {
    let def = session.instance(cardKey)?["definition"]?.string
    let prefix = def.map { "definitions/\($0)/" } ?? "-"
    let files = session.sources.keys.filter { $0.hasPrefix(prefix) }.sorted()
    VStack(alignment: .leading) {
      if files.isEmpty {
        Text("This Card has no source files.").foregroundStyle(.secondary).padding()
      } else {
        Picker("File", selection: $file) {
          ForEach(files, id: \.self) { Text(String($0.dropFirst(prefix.count))).tag(Optional($0)) }
        }
        .padding(.horizontal, 6)
        TextEditor(text: $text)
          .font(.system(size: 12, design: .monospaced))
          .onChange(of: text) { _, new in
            guard let f = file else { return }
            let data = Data(new.utf8)
            if session.sourceEdits[f] != data, loadedHash != nil {
              session.edit { $0.sourceEdits[f] = data }
            }
          }
        if let error { Text(error).font(.caption).foregroundStyle(.red).padding(.horizontal, 6) }
        if let f = file, session.sourceEdits[f] != nil {
          Text("Edited · saved with the project (⌘S)").font(.caption).foregroundStyle(.orange).padding(.horizontal, 6)
        }
      }
    }
    .task(id: file) { await load() }
    .onAppear { if file == nil { file = files.first { $0.hasSuffix(".py") || $0.hasSuffix(".cpp") } ?? files.first } }
  }

  private func load() async {
    guard let f = file, let rpc = session.rpc else { return }
    loadedHash = nil
    if let pending = session.sourceEdits[f] {
      text = String(decoding: pending, as: UTF8.self)
      loadedHash = session.sources[f]?.hash
      return
    }
    do {
      let data = try await rpc.readFile(path: f, atRev: session.rev)
      text = String(decoding: data, as: UTF8.self)
      loadedHash = session.sources[f]?.hash
      error = nil
    } catch {
      self.error = "\(error)"
    }
  }
}
