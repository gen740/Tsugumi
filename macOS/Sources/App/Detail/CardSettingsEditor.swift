import AppKit
import SwiftUI
import TsugumiRPC

/// Local working copy of one Card's settings, definition, metadata and
/// sources. Nothing reaches the project until "適用".
///
/// The copy remembers what it was based on (the confirmed instance,
/// definition and source hashes). If the project changes underneath it, the
/// next apply is held back as a conflict instead of silently replacing the
/// newer content, and sources are saved against the hashes actually read.
@MainActor
@Observable
final class CardEditorModel {
  let session: ProjectSession
  let cardKey: CardKey
  var instance: JSONValue = .object([])
  var definition: JSONValue?
  var meta: JSONValue?
  var texts: [String: String] = [:]     // loaded or edited source text
  var removed: Set<String> = []
  var created: Set<String> = []
  var message: String?
  var applying = false
  /// The project changed since this copy was taken (see `apply`).
  var conflict: String?
  private var baseInstance: JSONValue = .object([])
  private var baseDefinition: JSONValue?
  private var baseMeta: JSONValue?
  private var baseTexts: [String: String] = [:]
  private var baseHashes: [String: String] = [:]   // source hash when each file was read
  private var confirmedAtLoad: (instance: JSONValue?, definition: JSONValue?, meta: JSONValue?) = (nil, nil, nil)

  init(session: ProjectSession, cardKey: CardKey) {
    self.session = session
    self.cardKey = cardKey
    reload()
  }

  var definitionId: String? { instance["definition"]?.string }
  var kind: CardKind { session.kind(cardKey) }
  var exists: Bool { session.instance(cardKey) != nil }

  var unapplied: Bool {
    instance != baseInstance || definition != baseDefinition || meta != baseMeta || !removed.isEmpty || !created.isEmpty ||
      texts.contains { baseTexts[$0.key] != $0.value }
  }

  /// Source files of the definition (documents excluded), with local changes.
  var files: [String] {
    guard let id = definitionId else { return [] }
    let prefix = "definitions/\(id)/"
    let docs: Set<String> = [prefix + "_card.json", prefix + "_meta.json"]
    return Set(session.sourcePaths.filter { $0.hasPrefix(prefix) && !docs.contains($0) }).union(created).subtracting(removed).sorted()
  }

  func reload() {
    instance = session.instance(cardKey) ?? .object([])
    definition = definitionId.flatMap { session.definitions[$0] }
    meta = definitionId.flatMap { session.definitionMeta[$0] }
    baseInstance = instance
    baseDefinition = definition
    baseMeta = meta
    texts = [:]
    baseTexts = [:]
    baseHashes = [:]
    removed = []
    created = []
    message = nil
    conflict = nil
    confirmedAtLoad = currentConfirmed()
  }

  private func currentConfirmed() -> (instance: JSONValue?, definition: JSONValue?, meta: JSONValue?) {
    let def = definitionId
    return (session.confirmedInstance(cardKey), def.flatMap { session.confirmedDefinitions[$0] },
            def.flatMap { session.confirmedDefinitionMeta[$0] })
  }

  func text(_ p: String) async -> String {
    if let t = texts[p] { return t }
    let hash = session.sources[p]?.hash
    let t = (try? await session.sourceText(p)) ?? ""
    if texts[p] == nil {
      texts[p] = t
      baseTexts[p] = t
      if let hash { baseHashes[p] = hash }
    }
    return texts[p] ?? t
  }

  func createFile(_ rel: String, text: String = "") {
    guard let id = definitionId, !rel.isEmpty else { return }
    let p = "definitions/\(id)/\(rel)"
    removed.remove(p)
    created.insert(p)
    texts[p] = text
  }

  func rename(_ p: String, to rel: String) async {
    let t = await text(p)
    removeFile(p)
    createFile(rel, text: t)
  }

  func removeFile(_ p: String) {
    if created.contains(p) { created.remove(p) } else { removed.insert(p) }
    texts[p] = nil
  }

  /// Copies the working copy into the project draft as one undoable edit, then
  /// saves. With `force` the user chose to replace newer content explicitly.
  func apply(force: Bool = false) async -> Bool {
    guard session.isEditable else {
      message = String(localized: "実行中は Card を変更できません。実行が終わってから適用してください。")
      return false
    }
    let now = currentConfirmed()
    if !force, now.instance != confirmedAtLoad.instance || now.definition != confirmedAtLoad.definition || now.meta != confirmedAtLoad.meta {
      conflict = String(localized: "この Card は編集を始めた後に別の場所で更新されました。編集内容は保持しています。最新を読み込むか、自分の編集で置き換えるかを選んでください。")
      return false
    }
    conflict = nil
    applying = true
    defer { applying = false }
    let inst = instance, def = definition, m = meta, defId = definitionId
    let changedTexts = texts.filter { baseTexts[$0.key] != $0.value || created.contains($0.key) }
    let gone = removed
    let bases = baseHashes
    session.edit { s in
      // Settings only: the inner graph and the id are edited on the canvas.
      s.updateInstance(cardKey) { c in
        let keep = ["id", "canvas"]
        var out: [(String, JSONValue)] = (c.object ?? []).filter { keep.contains($0.0) }
        for (k, v) in inst.object ?? [] where !keep.contains(k) { out.append((k, v)) }
        c = .object(out)
      }
      if let defId, let def, def != s.definitions[defId] || m != s.definitionMeta[defId] {
        s.setDefinition(defId, card: def, meta: m)
      }
      for (p, t) in changedTexts {
        s.putSource(p, t)
        // Saved against the version this editor read, not the newest one.
        if let h = bases[p], !force { s.sourceBases[p] = h }
      }
      for p in gone {
        s.removeSource(p)
        if let h = bases[p], !force { s.sourceBases[p] = h }
      }
    }
    await session.save()
    if let e = session.saveError {
      message = String(localized: "Workflow には反映しましたが保存できませんでした: ") + e
      return false
    }
    message = String(localized: "適用しました（revision \(session.rev)）。")
    reload()
    return true
  }
}

struct CardSettingsEditor: View {
  @Environment(\.dismiss) private var dismiss
  @Environment(\.openWindow) private var openWindow
  let session: ProjectSession
  let ref: CardRef
  @State private var model: CardEditorModel?
  @State private var category: CardEditorCategory = .general
  @State private var confirmReload = false

  var body: some View {
    Group {
      if let model, model.exists {
        content(model)
      } else if model != nil {
        ContentUnavailableView("Card が見つかりません", systemImage: "questionmark.square.dashed",
                               description: Text("\(ref.card) は削除されたか、まだ保存されていません。"))
      } else {
        ProgressView()
      }
    }
    .task {
      if model == nil {
        model = CardEditorModel(session: session, cardKey: ref.card)
        category = ref.category
      }
    }
    .navigationTitle("Card 設定 · \(session.displayName(ref.card))")
  }

  private func categories(_ m: CardEditorModel) -> [CardEditorCategory] {
    switch m.kind {
    case .code: return CardEditorCategory.allCases
    case .input: return [.general, .source, .ports, .executions, .artifacts, .context]
    case .draft: return [.general, .ports, .context]
    case .subgraph: return [.general, .ports, .executions]
    default: return [.general, .ports, .executions, .artifacts, .context]
    }
  }

  private func content(_ m: CardEditorModel) -> some View {
    HStack(spacing: 0) {
      List(categories(m), selection: Binding(get: { Optional(category) }, set: { if let c = $0 { category = c } })) { c in
        Label(c.title, systemImage: c.symbol).tag(Optional(c))
      }
      .listStyle(.sidebar)
      .frame(width: 160)
      Divider()
      VStack(spacing: 0) {
        CardPreview(session: session, model: m)
          .frame(height: 150)
        Divider()
        Group {
          switch category {
          case .general: GeneralCategory(model: m)
          case .nix: SourceCategory(model: m, only: "default.nix")
          case .source: SourceCategory(model: m, only: nil)
          case .ports: m.kind == .subgraph ? AnyView(BoundaryCategory(model: m)) : AnyView(PortsCategory(model: m))
          case .executions: CardExecutionView(session: session, cardKey: ref.card)
          case .artifacts: CardArtifactsView(session: session, cardKey: ref.card)
          case .context: ContextCategory(model: m)
          }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        if let c = m.conflict {
          HStack(alignment: .top) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            Text(c).font(.callout).fixedSize(horizontal: false, vertical: true)
            Spacer()
            Button("最新を読み込む") { confirmReload = true }
            Button("自分の編集で置き換える") { Task { _ = await m.apply(force: true) } }
          }
          .padding(8)
          .background(Color.orange.opacity(0.1))
        }
        Divider()
        footer(m)
      }
    }
    .confirmationDialog("未適用の変更を破棄して読み込み直しますか？", isPresented: $confirmReload) {
      Button("破棄して再読込", role: .destructive) { m.reload() }
      Button("キャンセル", role: .cancel) {}
    }
  }

  private func footer(_ m: CardEditorModel) -> some View {
    HStack(spacing: 8) {
      if m.unapplied {
        Label("未適用の変更", systemImage: "pencil.circle.fill").foregroundStyle(.orange).labelStyle(.titleAndIcon)
      }
      if let msg = m.message {
        Text(msg).font(.callout).foregroundStyle(session.saveError == nil ? Color.secondary : Color.red).lineLimit(2).textSelection(.enabled)
      } else if !session.isEditable {
        Label("実行中は変更を適用できません", systemImage: "lock.fill").foregroundStyle(.secondary)
      } else if let id = m.definitionId, session.usageCount(ofDefinition: id) > 1 {
        Label("定義の変更は、この定義を使う \(session.usageCount(ofDefinition: id)) 枚の Card すべてに反映されます",
              systemImage: "square.on.square").font(.callout).foregroundStyle(.secondary)
      }
      Spacer()
      if m.applying { ProgressView().controlSize(.small) }
      Button("再読込") { if m.unapplied { confirmReload = true } else { m.reload() } }
      Button("適用") { Task { _ = await m.apply() } }
        .disabled(!m.unapplied || m.applying || !session.isEditable)
        .keyboardShortcut("s", modifiers: .command)
      Button("閉じる") {
        Task {
          if m.unapplied {
            guard await m.apply() else { return }
          }
          dismiss()
        }
      }
      .keyboardShortcut(.cancelAction)
      .help("未適用の変更は適用してから閉じます")
    }
    .padding(.horizontal, 10).padding(.vertical, 7)
  }
}

/// The Card as the canvas draws it, from the working copy (unapplied changes
/// included). It can be selected, dragged, collapsed and zoomed; nothing here
/// changes the project.
struct CardPreview: View {
  let session: ProjectSession
  let model: CardEditorModel
  @State private var items: [CanvasItem] = []

  var body: some View {
    CanvasSurface(items: $items, wires: [], flow: session.flow, grid: AppPreferences.shared.showGrid,
                  gridSize: AppPreferences.shared.gridSize)
      .onAppear(perform: refresh)
      .onChange(of: model.instance) { refresh() }
      .onChange(of: model.definition) { refresh() }
      .onChange(of: model.meta) { refresh() }
      .onChange(of: session.cardStates[model.cardKey]?.state) { refresh() }
      .overlay(alignment: .topTrailing) {
        Text("プレビュー").font(.caption2).foregroundStyle(.secondary).padding(6)
      }
  }

  private func refresh() {
    let collapsed = items.first?.data.collapsed ?? session.collapsed(model.cardKey)
    let data = session.nodeData(model.cardKey, instance: model.instance, definition: model.definition, meta: model.meta,
                                collapsed: collapsed)
    if items.isEmpty {
      items = [CanvasItem(id: model.cardKey, data: data, origin: .zero)]
    } else {
      items[0].data = data
    }
  }
}

// MARK: - Categories

private enum Tri: String, CaseIterable, Identifiable {
  case inherit, on, off
  var id: String { rawValue }
}

/// Shows where an effective value comes from.
private struct SourceBadge: View {
  let overridden: Bool
  var body: some View {
    Text(overridden ? "上書き" : "定義の既定値")
      .font(.caption2)
      .padding(.horizontal, 5).padding(.vertical, 1)
      .background((overridden ? Color.accentColor : Color.secondary).opacity(0.15), in: Capsule())
      .foregroundStyle(overridden ? Color.accentColor : .secondary)
  }
}

/// Name, description, params, execution conditions and display settings.
private struct GeneralCategory: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    let def = model.definition
    Form {
      Section {
        LabeledContent("パス", value: model.cardKey)
        LabeledContent("種類", value: model.kind.label)
        if let id = model.definitionId { LabeledContent("定義", value: id) }
        TextField("名前", text: string("name"), prompt: Text(def?["name"]?.string ?? ""))
        TextField("記号（SF Symbol 名）", text: string("symbol"), prompt: Text(def?["symbol"]?.string ?? model.kind.symbol))
        TextField("概要", text: string("description"), prompt: Text(def?["description"]?.string ?? ""), axis: .vertical)
          .lineLimit(2...5)
      } footer: {
        Text("空欄の項目は定義の既定値を使います。「オフ」「0」は明示的な値で、定義より優先されます。「無期限」は明示的な null です。")
          .font(.caption).foregroundStyle(.secondary)
      }
      if let params = def?["params"]?.array, !params.isEmpty {
        Section("パラメータ") {
          ForEach(params.indices, id: \.self) { i in ParamRow(model: model, param: params[i]) }
        }
      }
      if model.kind != .subgraph && model.kind != .draft {
        Section("実行条件") {
          if model.definition != nil, model.kind == .code {
            Picker("入力の揃え方（定義）", selection: defString("inputMode", fallback: "all")) {
              Text("すべて揃ってから（all）").tag("all")
              Text("最初に届いたもの（any）").tag("any")
            }
          }
          numberField("maxRuns", label: "最大実行回数", def: def, fallback: "1")
          timeoutField("timeoutSeconds", label: "実行の期限（秒）", def: def)
          if model.kind == .code { timeoutField("buildTimeoutSeconds", label: "ビルドの期限（秒）", def: def) }
          tri("filesystem", label: "作業・出力領域を使う", def: def, fallback: false)
          tri("sandbox", label: "ファイルアクセスを制限", def: def, fallback: true)
        }
      }
      Section("表示") {
        tri("canvasEnabled", label: "出力表示を Canvas に出す", def: def, fallback: false)
        Toggle("Canvas で折りたたむ（実行には影響しません）", isOn: Binding(
          get: { model.session.collapsed(model.cardKey) },
          set: { model.session.setCollapsed([model.cardKey], $0) }))
      }
      let diags = model.session.resolution[model.cardKey]?.diagnostics ?? []
      if !diags.isEmpty {
        Section("診断") {
          ForEach(diags) { d in
            Label(d.message, systemImage: d.error ? "xmark.circle" : "exclamationmark.triangle")
              .foregroundStyle(d.error ? .red : .primary)
          }
        }
      }
    }
    .formStyle(.grouped)
  }

  private func string(_ name: String) -> Binding<String> {
    Binding(get: { model.instance[name]?.string ?? "" },
            set: { model.instance[name] = $0.isEmpty ? nil : .string($0) })
  }

  private func defString(_ name: String, fallback: String) -> Binding<String> {
    Binding(get: { model.definition?[name]?.string ?? fallback },
            set: { v in model.definition?[name] = v == fallback ? nil : .string(v) })
  }

  private func tri(_ name: String, label: String, def: JSONValue?, fallback: Bool) -> some View {
    let current: Tri = model.instance[name]?.bool.map { $0 ? .on : .off } ?? .inherit
    let inherited = def?[name]?.bool ?? fallback
    return HStack {
      Picker(label, selection: Binding(get: { current }, set: { v in
        model.instance[name] = v == .inherit ? nil : .bool(v == .on)
      })) {
        Text("定義に従う（\(inherited ? "オン" : "オフ")）").tag(Tri.inherit)
        Text("オン").tag(Tri.on)
        Text("オフ").tag(Tri.off)
      }
      SourceBadge(overridden: current != .inherit)
    }
  }

  private func numberField(_ name: String, label: String, def: JSONValue?, fallback: String) -> some View {
    HStack {
      TextField(label, text: Binding(
        get: { model.instance[name]?.numberText ?? model.instance[name]?.string ?? "" },
        set: { v in
          let t = v.trimmingCharacters(in: .whitespaces)
          model.instance[name] = t.isEmpty ? nil : (UInt64(t) != nil ? .number(t) : .string(t))  // core reports non-numbers
        }), prompt: Text("定義に従う（\(def?[name]?.numberText ?? fallback)）"))
      SourceBadge(overridden: model.instance[name] != nil)
    }
  }

  private func timeoutField(_ name: String, label: String, def: JSONValue?) -> some View {
    let inherited = def?[name].map { $0 == .null ? String(localized: "無期限") : ($0.numberText ?? "") } ?? String(localized: "無期限")
    return HStack {
      TextField(label, text: Binding(
        get: { model.instance[name] == .null ? "" : (model.instance[name]?.numberText ?? "") },
        set: { v in
          let t = v.trimmingCharacters(in: .whitespaces)
          model.instance[name] = t.isEmpty ? nil : .number(t)
        }), prompt: Text(model.instance[name] == .null ? String(localized: "無期限（明示）") : String(localized: "定義に従う（\(inherited)）")))
      Button("無期限") { model.instance[name] = .null }
        .help("明示的な null: 定義に期限があっても期限なし")
      SourceBadge(overridden: model.instance[name] != nil)
      if model.instance[name] != nil {
        Button { model.instance[name] = nil } label: { Image(systemName: "arrow.counterclockwise") }
          .buttonStyle(.borderless).help("上書きを消して定義に従う")
      }
    }
  }
}

private struct ParamRow: View {
  @Bindable var model: CardEditorModel
  let param: JSONValue

  var body: some View {
    let name = param["name"]?.string ?? "?"
    let type = param["type"]?.string ?? "string"
    let override = model.instance["params"]?[name]
    let value = override ?? param["data"]
    HStack {
      if type == "bool" {
        Toggle(name, isOn: Binding(get: { value?.bool ?? false }, set: { set(name, .bool($0)) }))
      } else {
        TextField(name, text: Binding(get: { display(value) }, set: { set(name, parseParam($0, type)) }),
                  prompt: Text(display(param["data"])))
      }
      Text(type + (param["nix"]?.bool == true ? " · nix" : "")).font(.caption2).foregroundStyle(.secondary)
      SourceBadge(overridden: override != nil)
      if override != nil {
        Button { set(name, nil) } label: { Image(systemName: "arrow.counterclockwise") }
          .buttonStyle(.borderless).help("上書きを消して既定値に戻す")
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

  private func set(_ name: String, _ v: JSONValue?) {
    var params = model.instance["params"] ?? .object([])
    params[name] = v
    model.instance["params"] = (params.object?.isEmpty ?? true) ? nil : params
  }
}

/// Ports, their purposes, recording and artifact retention.
private struct PortsCategory: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    let r = model.session.resolution[model.cardKey]
    let def = model.definition
    Form {
      Section {
        if model.definition != nil, [.code, .draft, .merge].contains(model.kind) {
          let inputs = model.definition?["inputs"]?.array ?? []
          ForEach(inputs.indices, id: \.self) { i in
            let name = inputs[i]["name"]?.string ?? ""
            HStack {
              TextField("名前", text: Binding(get: { name }, set: { v in editInputs { $0[i]["name"] = .string(v) } }))
                .frame(maxWidth: 140)
              TextField("用途", text: note("inputs", name))
              Toggle("省略可", isOn: Binding(get: { inputs[i]["optional"]?.bool ?? false },
                                          set: { v in editInputs { $0[i]["optional"] = v ? .bool(true) : nil } }))
              Text(r?.inputs.first { $0.name == name }?.rootType ?? "未解決").font(.caption).foregroundStyle(.secondary)
              Button { editInputs { $0.remove(at: i) } } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless)
            }
          }
          Button("入力を追加") { editInputs { $0.append(.object([("name", .string("in\($0.count + 1)"))])) } }
        } else {
          ForEach(r?.inputs ?? [], id: \.name) { p in LabeledContent(p.name, value: p.rootType ?? String(localized: "未解決")) }
          if (r?.inputs ?? []).isEmpty { Text("入力はありません").foregroundStyle(.secondary) }
        }
      } header: { Text("入力ポート") } footer: {
        Text("入力の型は接続元の出力から決まります（_inputs.fbs は生成され、手で書きません）。定義の変更は同じ定義の Card すべてに反映されます。")
          .font(.caption).foregroundStyle(.secondary)
      }
      Section {
        ForEach(r?.outputs ?? [], id: \.name) { p in
          HStack {
            Text(p.name).frame(maxWidth: 140, alignment: .leading)
            TextField("用途", text: note("outputs", p.name))
            Text((p.rootType ?? String(localized: "未解決")) + (p.optional ? String(localized: "（省略可）") : ""))
              .font(.caption).foregroundStyle(.secondary)
          }
        }
        if (r?.outputs ?? []).isEmpty { Text("出力はありません").foregroundStyle(.secondary) }
      } header: { Text("出力ポート") } footer: {
        Text("出力ポートと型の正本は _outputs.fbs です（ソースで編集）。").font(.caption).foregroundStyle(.secondary)
      }
      if model.kind != .subgraph && model.kind != .draft {
        Section("記録と成果物") {
          tri("recordIO", label: "入出力を記録", def: def)
          tri("retainArtifact", label: "成果物を保持", def: def)
          Picker("保持する出力", selection: Binding(
            get: { model.instance["artifactOutput"]?.string ?? def?["artifactOutput"]?.string ?? "" },
            set: { model.instance["artifactOutput"] = $0.isEmpty ? nil : .string($0) })) {
            Text("なし").tag("")
            ForEach(r?.outputs ?? [], id: \.name) { Text($0.name).tag($0.name) }
          }
          Picker("ログを保存する DB 接続", selection: Binding(
            get: { model.instance["loggerDatabaseConnection"]?.string ?? "" },
            set: { model.instance["loggerDatabaseConnection"] = $0.isEmpty ? nil : .string($0) })) {
            Text("なし（core の履歴のみ）").tag("")
            ForEach((model.session.project["databaseConnections"]?.array ?? []).compactMap { $0["name"]?.string }, id: \.self) {
              Text($0).tag($0)
            }
          }
        }
      }
    }
    .formStyle(.grouped)
  }

  /// Port purpose kept in _meta.json (also after a Draft is implemented).
  private func note(_ list: String, _ name: String) -> Binding<String> {
    Binding(get: {
      model.meta?[list]?.array?.first { $0["name"]?.string == name }?["description"]?.string ?? ""
    }, set: { v in
      var m = model.meta ?? .object([])
      var items = m[list]?.array ?? []
      if let i = items.firstIndex(where: { $0["name"]?.string == name }) {
        items[i]["description"] = v.isEmpty ? nil : .string(v)
      } else if !v.isEmpty {
        items.append(.object([("name", .string(name)), ("description", .string(v))]))
      }
      m[list] = items.isEmpty ? nil : .array(items)
      model.meta = (m.object?.isEmpty ?? true) ? nil : m
    })
  }

  private func editInputs(_ fn: (inout [JSONValue]) -> Void) {
    var inputs = model.definition?["inputs"]?.array ?? []
    fn(&inputs)
    model.definition?["inputs"] = inputs.isEmpty ? nil : .array(inputs)
  }

  private func tri(_ name: String, label: String, def: JSONValue?) -> some View {
    let current: Tri = model.instance[name]?.bool.map { $0 ? .on : .off } ?? .inherit
    return HStack {
      Picker(label, selection: Binding(get: { current }, set: { v in
        model.instance[name] = v == .inherit ? nil : .bool(v == .on)
      })) {
        Text("定義に従う（\(def?[name]?.bool == true ? "オン" : "オフ")）").tag(Tri.inherit)
        Text("オン").tag(Tri.on)
        Text("オフ").tag(Tri.off)
      }
      SourceBadge(overridden: current != .inherit)
    }
  }
}

/// Inputs and outputs of a Card in Card: each outer port maps to a port of a
/// Card inside (`boundary.inputs` / `boundary.outputs`).
private struct BoundaryCategory: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    let inner = model.session.cards(in: path(model.cardKey)).compactMap { $0["id"]?.string }
    Form {
      side("inputs", title: "入力ポート", inner: inner, output: false)
      side("outputs", title: "出力ポート", inner: inner, output: true)
      Text("外側からは通常の Card と同じように接続・実行できます。開くと内部を編集できます。見た目だけのグループとは違い、実行の単位です。")
        .font(.caption).foregroundStyle(.secondary)
    }
    .formStyle(.grouped)
  }

  private func side(_ key: String, title: LocalizedStringKey, inner: [String], output: Bool) -> some View {
    let entries = model.instance["boundary"]?[key]?.object ?? []
    return Section(title) {
      ForEach(entries.indices, id: \.self) { i in
        let (name, ep) = entries[i]
        HStack {
          TextField("名前", text: Binding(get: { name }, set: { v in rename(key, name, to: v) })).frame(maxWidth: 140)
          Image(systemName: output ? "arrow.left" : "arrow.right").foregroundStyle(.secondary)
          Picker("", selection: Binding(get: { ep["card"]?.string ?? "" }, set: { v in setEndpoint(key, name, card: v, port: nil) })) {
            ForEach(inner, id: \.self) { Text(model.session.displayName(key2(path(model.cardKey) + [$0]))).tag($0) }
          }
          .labelsHidden().frame(maxWidth: 160)
          let ports = (ep["card"]?.string).map { model.session.ports(key2(path(model.cardKey) + [$0])) }
          Picker("", selection: Binding(get: { ep["port"]?.string ?? "" }, set: { v in setEndpoint(key, name, card: nil, port: v) })) {
            ForEach((output ? ports?.outputs : ports?.inputs) ?? [], id: \.self) { Text($0).tag($0) }
          }
          .labelsHidden().frame(maxWidth: 120)
          Button { remove(key, name) } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless)
        }
      }
      Button(output ? "出力を追加" : "入力を追加") {
        var boundary = model.instance["boundary"] ?? .object([])
        var side = boundary[key] ?? .object([])
        var n = (side.object?.count ?? 0) + 1
        while side[(output ? "out" : "in") + "\(n)"] != nil { n += 1 }
        side[(output ? "out" : "in") + "\(n)"] = .object([("card", .string(inner.first ?? "")), ("port", .string(""))])
        boundary[key] = side
        model.instance["boundary"] = boundary
      }
      .disabled(inner.isEmpty)
    }
  }

  private func key2(_ p: [String]) -> CardKey { key(p) }

  private func setEndpoint(_ side: String, _ name: String, card: String?, port: String?) {
    var b = model.instance["boundary"] ?? .object([])
    var s = b[side] ?? .object([])
    var ep = s[name] ?? .object([])
    if let card { ep["card"] = .string(card); ep["port"] = .string("") }
    if let port { ep["port"] = .string(port) }
    s[name] = ep
    b[side] = s
    model.instance["boundary"] = b
  }

  private func rename(_ side: String, _ name: String, to new: String) {
    guard !new.isEmpty, new != name, var b = model.instance["boundary"], var s = b[side]?.object else { return }
    if let i = s.firstIndex(where: { $0.0 == name }) { s[i].0 = new }
    b[side] = .object(s)
    model.instance["boundary"] = b
  }

  private func remove(_ side: String, _ name: String) {
    guard var b = model.instance["boundary"], var s = b[side]?.object else { return }
    s.removeAll { $0.0 == name }
    b[side] = .object(s)
    model.instance["boundary"] = b
  }
}

/// Free text describing intent and notes, kept in `_meta.json` (`context`).
private struct ContextCategory: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("処理の意図や補足を文章で残します。チャットで Card を参照したときにも使われます。定義を使うすべての Card で共有されます。")
        .font(.callout).foregroundStyle(.secondary)
      if model.definitionId == nil {
        Text("定義を持たない Card には追加コンテキストを置けません。").foregroundStyle(.secondary)
      } else {
        TextEditor(text: Binding(
          get: { model.meta?["context"]?.string ?? "" },
          set: { v in
            var m = model.meta ?? .object([])
            m["context"] = v.isEmpty ? nil : .string(v)
            model.meta = (m.object?.isEmpty ?? true) ? nil : m
          }))
          .font(.body)
          .border(.secondary.opacity(0.3))
      }
    }
    .padding(12)
  }
}

/// File tree and editor for a definition's sources, `_outputs.fbs` and
/// `default.nix`, with formatting, errors in place and the generated inputs.
private struct SourceCategory: View {
  @Bindable var model: CardEditorModel
  let only: String?
  @State private var selected: String?
  @State private var text = ""
  @State private var newName = ""
  @State private var renaming: String?
  @State private var showNew = false
  @State private var formatting = false
  @State private var formatMessage: String?
  @State private var formatMarkers: [EditorMarker] = []
  @State private var generated: [String: String] = [:]
  @State private var generatedError: String?

  private static let generatedPrefix = "generated:"

  var body: some View {
    let files = model.files.filter { only == nil || $0.hasSuffix("/" + only!) }
    let prefix = model.definitionId.map { "definitions/\($0)/" } ?? ""
    HSplitView {
      if only == nil {
        VStack(spacing: 0) {
          List(selection: $selected) {
            OutlineGroup(FileNode.tree(files.map { String($0.dropFirst(prefix.count)) }, base: prefix), children: \.children) { node in
              Label(node.name, systemImage: node.children == nil ? icon(node.name) : "folder")
                .tag(node.children == nil ? Optional(node.path) : nil)
                .contextMenu {
                  if node.children == nil {
                    Button("名前を変更…") { renaming = node.path; newName = String(node.path.dropFirst(prefix.count)) }
                    Button("削除", role: .destructive) {
                      model.removeFile(node.path)
                      if selected == node.path { selected = nil }
                    }
                  }
                }
            }
            if model.kind == .code {
              Section("生成（読み取り専用）") {
                Label("_inputs.fbs", systemImage: "lock.doc").tag(Optional(Self.generatedPrefix + "_inputs.fbs"))
                Label("_params.fbs", systemImage: "lock.doc").tag(Optional(Self.generatedPrefix + "_params.fbs"))
              }
            }
          }
          Divider()
          HStack {
            Button { showNew = true; newName = "src/" } label: { Image(systemName: "plus") }.help("ファイルを作成")
            Button {
              if let s = selected, !s.hasPrefix(Self.generatedPrefix) { model.removeFile(s); selected = nil }
            } label: { Image(systemName: "minus") }
              .disabled(selected == nil || selected!.hasPrefix(Self.generatedPrefix)).help("選択したファイルを削除")
            Spacer()
          }
          .buttonStyle(.borderless).padding(6)
        }
        .frame(minWidth: 170, idealWidth: 210, maxWidth: 320)
      }
      editorPane(files: files, prefix: prefix)
    }
    .onAppear { if selected == nil { selected = files.first { only != nil || $0.hasSuffix(".py") || $0.hasSuffix(".cpp") } ?? files.first } }
    .alert("ファイルを作成", isPresented: $showNew) {
      TextField("パス（例: src/util.py）", text: $newName)
      Button("作成") {
        model.createFile(newName)
        selected = prefix + newName
      }
      Button("キャンセル", role: .cancel) {}
    }
    .alert("名前を変更", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
      TextField("新しいパス", text: $newName)
      Button("変更") {
        if let r = renaming {
          Task {
            await model.rename(r, to: newName)
            selected = prefix + newName
          }
        }
        renaming = nil
      }
      Button("キャンセル", role: .cancel) { renaming = nil }
    }
  }

  @ViewBuilder private func editorPane(files: [String], prefix: String) -> some View {
    VStack(spacing: 0) {
      if let selected, selected.hasPrefix(Self.generatedPrefix) {
        let name = String(selected.dropFirst(Self.generatedPrefix.count))
        HStack {
          Text(name).font(.caption.monospaced())
          Text("接続から生成（編集しません）").font(.caption2).foregroundStyle(.secondary)
          Spacer()
        }
        .padding(6)
        if let e = generatedError {
          Text(e).font(.callout).foregroundStyle(.red).padding()
        }
        CodeEditor(text: .constant(generated[name] ?? ""), language: "fbs", editable: false)
          .task(id: selected) { await loadGenerated() }
      } else if let selected {
        let markers = coreMarkers(for: selected) + formatMarkers
        HStack(spacing: 8) {
          Text(String(selected.dropFirst(prefix.count))).font(.caption.monospaced())
          if model.created.contains(selected) { Text("新規").font(.caption2).foregroundStyle(.orange) }
          Spacer()
          if let formatMessage { Text(formatMessage).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
          Button("整形") { format(selected) }.disabled(formatting).controlSize(.small)
        }
        .padding(6)
        CodeEditor(text: Binding(get: { text }, set: { text = $0; model.texts[selected] = $0; formatMarkers = [] }),
                   language: (selected as NSString).pathExtension, markers: markers)
          .task(id: selected) {
            text = await model.text(selected)
            formatMarkers = []
            formatMessage = nil
          }
        if !markers.isEmpty {
          ScrollView {
            VStack(alignment: .leading, spacing: 2) {
              ForEach(Array(markers.enumerated()), id: \.offset) { _, m in
                Label(m.message, systemImage: "xmark.octagon").font(.caption).foregroundStyle(.red).textSelection(.enabled)
              }
            }
            .padding(6)
          }
          .frame(maxHeight: 80)
        }
      } else if only != nil && files.isEmpty {
        ContentUnavailableView("default.nix がありません", systemImage: "snowflake",
                               description: Text("処理 Card の実行環境は default.nix で定義します。"))
        Button("default.nix を作成") {
          model.createFile("default.nix", text: "{ pkgs, inputs, params }:\n{\n  package = inputs.tsugumi.pythonCard { src = ./src; };\n}\n")
          self.selected = prefix + "default.nix"
        }
      } else {
        ContentUnavailableView("ファイルを選んでください", systemImage: "doc.text")
      }
    }
    .frame(minWidth: 320)
  }

  private func icon(_ name: String) -> String {
    switch (name as NSString).pathExtension {
    case "py": return "chevron.left.forwardslash.chevron.right"
    case "cpp", "hpp", "h": return "curlybraces"
    case "nix": return "snowflake"
    case "fbs": return "tablecells"
    default: return "doc.text"
    }
  }

  /// Diagnostics of the core that point into this file (shown in place).
  private func coreMarkers(for file: String) -> [EditorMarker] {
    let diags = model.session.resolution[model.cardKey]?.diagnostics ?? []
    return diags.filter { $0.path == file }.map { EditorMarker(offset: $0.offset ?? 0, message: $0.message) }
  }

  private func loadGenerated() async {
    guard let rpc = model.session.rpc else { return }
    do {
      let files = try await rpc.generatedContracts(card: path(model.cardKey), atRev: model.session.rev)
      for f in files {
        let name = (f.path as NSString).lastPathComponent
        guard name.hasSuffix(".fbs") else { continue }
        generated[name] = String(decoding: try await rpc.readFile(path: f.path, atRev: model.session.rev), as: UTF8.self)
      }
      generatedError = nil
    } catch {
      generatedError = String(localized: "生成できませんでした（保存されていない変更や未解決の型があると生成できません）: ") + "\(error)"
    }
  }

  private func format(_ p: String) {
    guard let rpc = model.session.rpc else { return }
    formatting = true
    Task {
      do {
        let r = try await rpc.format(path: p, text: text)
        let errors = r.diagnostics.compactMap { $0 }
        if errors.isEmpty {
          text = r.text
          model.texts[p] = r.text
          formatMessage = String(localized: "整形しました")
          formatMarkers = []
        } else {
          formatMarkers = errors.map { EditorMarker(offset: $0.offset ?? 0, message: $0.message) }
          formatMessage = String(localized: "構文エラーがあります（テキストは変更していません）")
        }
      } catch let f as RPCFailure where f.kind == .unsupported {
        formatMessage = f.diagnostic ?? String(localized: "この形式の整形には対応していません")
      } catch {
        formatMessage = "\(error)"
      }
      formatting = false
    }
  }
}

struct FileNode: Identifiable, Hashable {
  var id: String { path }
  let name: String
  let path: String
  var children: [FileNode]?

  static func tree(_ rel: [String], base: String) -> [FileNode] {
    var dirs: [String: [String]] = [:]
    var leaves: [String] = []
    for r in rel {
      if let i = r.firstIndex(of: "/") {
        dirs[String(r[..<i]), default: []].append(String(r[r.index(after: i)...]))
      } else {
        leaves.append(r)
      }
    }
    let folders = dirs.keys.sorted().map { d in
      FileNode(name: d, path: base + d + "/", children: tree(dirs[d]!, base: base + d + "/"))
    }
    return folders + leaves.sorted().map { FileNode(name: $0, path: base + $0, children: nil) }
  }
}
