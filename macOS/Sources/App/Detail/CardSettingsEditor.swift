import AppKit
import SwiftUI
import TsugumiRPC

/// Local working copy of one Card's settings, definition, metadata and
/// sources. Nothing reaches the project until "適用".
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
  private var baseInstance: JSONValue = .object([])
  private var baseDefinition: JSONValue?
  private var baseMeta: JSONValue?
  private var baseTexts: [String: String] = [:]

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
    removed = []
    created = []
    message = nil
  }

  func text(_ p: String) async -> String {
    if let t = texts[p] { return t }
    let t = (try? await session.sourceText(p)) ?? ""
    if texts[p] == nil {
      texts[p] = t
      baseTexts[p] = t
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
    guard let id = definitionId else { return }
    let t = await text(p)
    removeFile(p)
    createFile(rel, text: t)
    _ = id
  }

  func removeFile(_ p: String) {
    if created.contains(p) { created.remove(p) } else { removed.insert(p) }
    texts[p] = nil
  }

  /// Copies the working copy into the project draft as one undoable edit, then saves.
  func apply() async -> Bool {
    guard session.isEditable else {
      message = "実行中は Card を変更できません。実行が終わってから適用してください。"
      return false
    }
    applying = true
    defer { applying = false }
    let inst = instance, def = definition, m = meta, defId = definitionId
    let changedTexts = texts.filter { baseTexts[$0.key] != $0.value || created.contains($0.key) }
    let gone = removed
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
      for (p, t) in changedTexts { s.putSource(p, t) }
      for p in gone { s.removeSource(p) }
    }
    await session.save()
    if let e = session.saveError {
      message = "変更は Workflow に反映しましたが保存できませんでした: \(e)"
    } else {
      message = "適用しました（revision \(session.rev)）。"
    }
    reload()
    return session.saveError == nil
  }
}

struct CardSettingsEditor: View {
  @Environment(\.dismiss) private var dismiss
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
    case .subgraph: return [.general, .executions]
    default: return [.general, .ports, .executions, .artifacts, .context]
    }
  }

  private func content(_ m: CardEditorModel) -> some View {
    NavigationSplitView {
      List(categories(m), selection: Binding(get: { category }, set: { if let c = $0 { category = c } })) { c in
        Label(c.title, systemImage: c.symbol).tag(c)
      }
      .navigationSplitViewColumnWidth(170)
    } detail: {
      VStack(spacing: 0) {
        HStack {
          Text(category.title).font(.title3.bold())
          Spacer()
          if m.unapplied {
            Label("未適用の変更", systemImage: "pencil.circle.fill").foregroundStyle(.orange)
          }
          if let s = session.cardStates[ref.card] {
            Label(s.state.label, systemImage: s.state.symbol).foregroundStyle(s.state.color)
          }
        }
        .padding(10)
        Divider()
        Group {
          switch category {
          case .general: GeneralCategory(model: m)
          case .nix: SourceCategory(model: m, only: "default.nix")
          case .source: SourceCategory(model: m, only: nil)
          case .ports: PortsCategory(model: m)
          case .executions: CardExecutionView(session: session, cardKey: ref.card)
          case .artifacts: CardArtifactsView(session: session, cardKey: ref.card)
          case .context: ContextCategory(model: m)
          }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
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
    HStack {
      if let msg = m.message {
        Text(msg).font(.callout).foregroundStyle(session.saveError == nil ? Color.secondary : Color.red).lineLimit(2).textSelection(.enabled)
      } else if !session.isEditable {
        Label("実行中は変更を適用できません", systemImage: "lock.fill").foregroundStyle(.secondary)
      } else if m.definitionId != nil, session.usageCount(ofDefinition: m.definitionId!) > 1 {
        Label("定義の変更は、この定義を使う \(session.usageCount(ofDefinition: m.definitionId!)) 枚の Card すべてに反映されます",
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
    .padding(10)
  }
}

// MARK: - Categories

private enum Tri: String, CaseIterable, Identifiable {
  case inherit, on, off
  var id: String { rawValue }
}

/// Name, description, params, execution conditions and display settings.
private struct GeneralCategory: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    let def = model.definition
    Form {
      Section("Card") {
        LabeledContent("パス", value: model.cardKey)
        LabeledContent("種類", value: model.kind.label)
        LabeledContent("定義", value: model.definitionId ?? "（なし）")
        TextField("名前", text: string("name"), prompt: Text(def?["name"]?.string ?? ""))
        TextField("概要", text: string("description"), prompt: Text(def?["description"]?.string ?? ""), axis: .vertical)
          .lineLimit(2...5)
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
    return Picker(label, selection: Binding(get: { current }, set: { v in
      model.instance[name] = v == .inherit ? nil : .bool(v == .on)
    })) {
      Text("定義に従う（\(inherited ? "オン" : "オフ")）").tag(Tri.inherit)
      Text("オン").tag(Tri.on)
      Text("オフ").tag(Tri.off)
    }
  }

  private func numberField(_ name: String, label: String, def: JSONValue?, fallback: String) -> some View {
    TextField(label, text: Binding(
      get: { model.instance[name]?.numberText ?? model.instance[name]?.string ?? "" },
      set: { v in
        let t = v.trimmingCharacters(in: .whitespaces)
        model.instance[name] = t.isEmpty ? nil : (UInt64(t) != nil ? .number(t) : .string(t))  // core reports non-numbers
      }), prompt: Text("定義に従う（\(def?[name]?.numberText ?? fallback)）"))
  }

  private func timeoutField(_ name: String, label: String, def: JSONValue?) -> some View {
    let inherited = def?[name].map { $0 == .null ? "無期限" : ($0.numberText ?? "") } ?? "無期限"
    return HStack {
      TextField(label, text: Binding(
        get: { model.instance[name] == .null ? "" : (model.instance[name]?.numberText ?? "") },
        set: { v in
          let t = v.trimmingCharacters(in: .whitespaces)
          model.instance[name] = t.isEmpty ? nil : .number(t)
        }), prompt: Text(model.instance[name] == .null ? "無期限（明示）" : "定義に従う（\(inherited)）"))
      Button("無期限") { model.instance[name] = .null }
        .help("明示的な null: 定義に期限があっても期限なし")
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

/// Ports, recording and artifact retention.
private struct PortsCategory: View {
  @Bindable var model: CardEditorModel
  @State private var outputsText: String?

  var body: some View {
    let r = model.session.resolution[model.cardKey]
    let def = model.definition
    let outputsFile = model.definitionId.map { "definitions/\($0)/_outputs.fbs" }
    Form {
      Section("入力ポート") {
        if model.definition != nil, [.code, .draft, .merge].contains(model.kind) {
          let inputs = model.definition?["inputs"]?.array ?? []
          ForEach(inputs.indices, id: \.self) { i in
            HStack {
              TextField("名前", text: Binding(
                get: { inputs[i]["name"]?.string ?? "" },
                set: { v in edit { $0[i]["name"] = .string(v) } }))
              Toggle("省略可", isOn: Binding(
                get: { inputs[i]["optional"]?.bool ?? false },
                set: { v in edit { $0[i]["optional"] = v ? .bool(true) : nil } }))
              Text(r?.inputs.first { $0.name == inputs[i]["name"]?.string }?.rootType ?? "未解決").font(.caption).foregroundStyle(.secondary)
              Button { edit { $0.remove(at: i) } } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless)
            }
          }
          Button("入力を追加") { edit { $0.append(.object([("name", .string("in\($0.count + 1)"))])) } }
          Text("入力の型は接続元から決まります。定義の変更は同じ定義の Card すべてに反映されます。").font(.caption).foregroundStyle(.secondary)
        } else {
          ForEach(r?.inputs ?? [], id: \.name) { p in LabeledContent(p.name, value: p.rootType ?? "未解決") }
          if (r?.inputs ?? []).isEmpty { Text("入力はありません").foregroundStyle(.secondary) }
        }
      }
      Section("出力ポート") {
        ForEach(r?.outputs ?? [], id: \.name) { p in
          LabeledContent(p.name, value: (p.rootType ?? "未解決") + (p.optional ? "（省略可）" : "") + " · \(String(describing: p.kind))")
        }
        if (r?.outputs ?? []).isEmpty { Text("出力はありません").foregroundStyle(.secondary) }
        if let outputsFile, model.kind == .code || model.kind == .input || model.kind == .draft {
          DisclosureGroup("_outputs.fbs を編集（出力ポートと型の正本）") {
            CodeEditor(text: Binding(get: { outputsText ?? "" }, set: { v in
              outputsText = v
              if model.files.contains(outputsFile) { model.texts[outputsFile] = v } else { model.createFile("_outputs.fbs", text: v) }
            }), language: "fbs")
              .frame(height: 160)
              .task {
                if outputsText == nil {
                  outputsText = model.files.contains(outputsFile) ? await model.text(outputsFile) : ""
                }
              }
          }
        }
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

  private func edit(_ fn: (inout [JSONValue]) -> Void) {
    var inputs = model.definition?["inputs"]?.array ?? []
    fn(&inputs)
    model.definition?["inputs"] = inputs.isEmpty ? nil : .array(inputs)
  }

  private func tri(_ name: String, label: String, def: JSONValue?) -> some View {
    let current: Tri = model.instance[name]?.bool.map { $0 ? .on : .off } ?? .inherit
    return Picker(label, selection: Binding(get: { current }, set: { v in
      model.instance[name] = v == .inherit ? nil : .bool(v == .on)
    })) {
      Text("定義に従う（\(def?[name]?.bool == true ? "オン" : "オフ")）").tag(Tri.inherit)
      Text("オン").tag(Tri.on)
      Text("オフ").tag(Tri.off)
    }
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
    .padding()
  }
}

/// File tree and editor for a definition's sources.
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

  var body: some View {
    let files = model.files.filter { only == nil || $0.hasSuffix("/" + only!) }
    let prefix = model.definitionId.map { "definitions/\($0)/" } ?? ""
    HSplitView {
      if only == nil {
        VStack(spacing: 0) {
          List(selection: $selected) {
            OutlineGroup(FileNode.tree(files.map { String($0.dropFirst(prefix.count)) }, base: prefix), children: \.children) { node in
              Label(node.name, systemImage: node.children == nil ? "doc.text" : "folder")
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
          }
          Divider()
          HStack {
            Button { showNew = true; newName = "src/" } label: { Image(systemName: "plus") }.help("ファイルを作成")
            Button {
              if let s = selected { model.removeFile(s); selected = nil }
            } label: { Image(systemName: "minus") }
              .disabled(selected == nil).help("選択したファイルを削除")
            Spacer()
          }
          .buttonStyle(.borderless).padding(6)
        }
        .frame(minWidth: 180, idealWidth: 220, maxWidth: 320)
      }
      VStack(spacing: 0) {
        if let selected {
          HStack {
            Text(String(selected.dropFirst(prefix.count))).font(.caption.monospaced())
            if model.created.contains(selected) { Text("新規").font(.caption2).foregroundStyle(.orange) }
            Spacer()
            if let formatMessage { Text(formatMessage).font(.caption).foregroundStyle(.secondary) }
            Button("整形") { format(selected) }.disabled(formatting).controlSize(.small)
          }
          .padding(6)
          CodeEditor(text: Binding(get: { text }, set: { text = $0; model.texts[selected] = $0 }),
                     language: (selected as NSString).pathExtension)
            .task(id: selected) { text = await model.text(selected) }
        } else if only != nil && files.isEmpty {
          ContentUnavailableView("default.nix がありません", systemImage: "snowflake",
                                 description: Text("処理 Card の実行環境は default.nix で定義します。"))
          Button("default.nix を作成") {
            model.createFile("default.nix", text: "{ pkgs, inputs, params }:\n{\n  package = inputs.tsugumi.pythonCard { src = ./src; };\n}\n")
            selected = prefix + "default.nix"
          }
        } else {
          ContentUnavailableView("ファイルを選んでください", systemImage: "doc.text")
        }
      }
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

  private func format(_ p: String) {
    guard let rpc = model.session.rpc else { return }
    formatting = true
    Task {
      do {
        let r = try await rpc.format(path: p, text: text)
        text = r.text
        model.texts[p] = r.text
        formatMessage = "整形しました"
      } catch let f as RPCFailure where f.kind == .unsupported {
        formatMessage = "この形式の整形には対応していません"
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
