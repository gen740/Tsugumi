import AppKit
import SwiftUI
import TsugumiRPC

// MARK: - Draft

/// Provisional output of a Draft (kept as `_outputs.fbs` so it can be wired).
struct DraftPort: Identifiable, Equatable {
  let id = UUID()
  var name: String
  var type: String

  static let types = ["string", "double", "long", "ulong", "bool", "file", "directory"]

  /// Reads the outputs the Draft editor wrote earlier.
  static func parse(_ fbs: String) -> [DraftPort] {
    var valueTypes: [String: String] = [:]
    let tableRe = try! NSRegularExpression(pattern: #"table\s+(\w+)\s*\{\s*value\s*:\s*(\w+)\s*;\s*\}"#)
    let ns = fbs as NSString
    for m in tableRe.matches(in: fbs, range: NSRange(location: 0, length: ns.length)) {
      valueTypes[ns.substring(with: m.range(at: 1))] = ns.substring(with: m.range(at: 2))
    }
    guard let r = fbs.range(of: #"table\s+Outputs\s*\{([^}]*)\}"#, options: .regularExpression) else { return [] }
    let body = String(fbs[r]).drop { $0 != "{" }.dropFirst().prefix { $0 != "}" }
    let fieldRe = try! NSRegularExpression(pattern: #"(\w+)\s*:\s*(\w+)\s*(\(([^)]*)\))?\s*;"#)
    let b = String(body) as NSString
    return fieldRe.matches(in: String(body), range: NSRange(location: 0, length: b.length)).map { m in
      let name = b.substring(with: m.range(at: 1))
      let table = b.substring(with: m.range(at: 2))
      let attrs = m.range(at: 4).location == NSNotFound ? "" : b.substring(with: m.range(at: 4))
      let type = attrs.contains("\"file\"") ? "file" : attrs.contains("\"directory\"") ? "directory" : (valueTypes[table] ?? "string")
      return DraftPort(name: name, type: type)
    }
  }

  static func fbs(_ ports: [DraftPort], namespace: String) -> String {
    guard !ports.isEmpty else { return "" }
    var out = "// Provisional outputs of a Draft (edited in the Draft editor).\nnamespace \(namespace);\n"
    for p in ports {
      let t = p.type == "file" || p.type == "directory" ? "string" : p.type
      out += "table \(tableName(p.name)) { value:\(t); }\n"
    }
    out += "table Outputs {"
    out += ports.map { p in
      " \(p.name):\(tableName(p.name))" + (p.type == "file" || p.type == "directory" ? " (tsugumi: \"\(p.type)\")" : "") + ";"
    }.joined()
    out += " }\nroot_type Outputs;\nattribute \"tsugumi\";\n"
    return out
  }

  static func tableName(_ port: String) -> String {
    let parts = port.split { !$0.isLetter && !$0.isNumber }
    return parts.map { $0.prefix(1).uppercased() + $0.dropFirst() }.joined() + "Value"
  }
}

/// Name, processing description and provisional ports of a Draft Card.
struct DraftEditor: View {
  @Environment(\.dismiss) private var dismiss
  let session: ProjectSession
  let cardKey: CardKey
  @State private var model: CardEditorModel?
  @State private var outputs: [DraftPort] = []
  @State private var baseOutputs: [DraftPort] = []

  var body: some View {
    Group {
      if let m = model, m.exists, let defId = m.definitionId {
        form(m, defId)
      } else if model != nil {
        ContentUnavailableView("Draft が見つかりません", systemImage: "questionmark.square.dashed")
      } else {
        ProgressView()
      }
    }
    .task {
      guard model == nil else { return }
      let m = CardEditorModel(session: session, cardKey: cardKey)
      model = m
      if let id = m.definitionId, session.sourcePaths.contains("definitions/\(id)/_outputs.fbs") {
        outputs = DraftPort.parse(await m.text("definitions/\(id)/_outputs.fbs"))
      }
      baseOutputs = outputs
    }
    .navigationTitle("Draft · \(session.displayName(cardKey))")
  }

  private func form(_ m: CardEditorModel, _ defId: String) -> some View {
    VStack(spacing: 0) {
      Form {
        Section("Draft") {
          TextField("名前", text: Binding(get: { m.definition?["name"]?.string ?? "" },
                                         set: { m.definition?["name"] = .string($0) }))
          TextField("概要", text: meta(m, "description"), axis: .vertical).lineLimit(2...4)
          VStack(alignment: .leading) {
            Text("処理の説明")
            TextEditor(text: meta(m, "plan")).frame(minHeight: 100).border(.secondary.opacity(0.3))
          }
        }
        Section("仮の入力") {
          let inputs = m.definition?["inputs"]?.array ?? []
          ForEach(inputs.indices, id: \.self) { i in
            HStack {
              TextField("名前", text: Binding(get: { inputs[i]["name"]?.string ?? "" },
                                             set: { v in editInputs(m) { $0[i]["name"] = .string(v) } }))
              Toggle("省略可", isOn: Binding(get: { inputs[i]["optional"]?.bool ?? false },
                                          set: { v in editInputs(m) { $0[i]["optional"] = v ? .bool(true) : nil } }))
              Button { editInputs(m) { $0.remove(at: i) } } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless)
            }
          }
          Button("入力を追加") { editInputs(m) { $0.append(.object([("name", .string("in\($0.count + 1)"))])) } }
        }
        Section("仮の出力") {
          ForEach($outputs) { $p in
            HStack {
              TextField("名前", text: $p.name)
              Picker("", selection: $p.type) { ForEach(DraftPort.types, id: \.self) { Text($0).tag($0) } }.labelsHidden().frame(width: 120)
              Button { outputs.removeAll { $0.id == p.id } } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless)
            }
          }
          Button("出力を追加") { outputs.append(DraftPort(name: "out\(outputs.count + 1)", type: "string")) }
          Text("仮の入出力で先に配線できます。配線と内容を適用した後、Canvas の「Card を生成」で実装を依頼します。")
            .font(.caption).foregroundStyle(.secondary)
        }
      }
      .formStyle(.grouped)
      Divider()
      HStack {
        if let msg = m.message { Text(msg).font(.callout).foregroundStyle(.secondary).lineLimit(2) }
        Spacer()
        if m.unapplied || outputs != baseOutputs { Label("未適用の変更", systemImage: "pencil.circle.fill").foregroundStyle(.orange) }
        Button("再読込") { m.reload(); outputs = baseOutputs }
        Button("適用") { Task { await apply(m, defId) } }
          .disabled(!(m.unapplied || outputs != baseOutputs) || !session.isEditable)
        Button("閉じる") {
          Task {
            if m.unapplied || outputs != baseOutputs { guard await apply(m, defId) else { return } }
            dismiss()
          }
        }
        .keyboardShortcut(.cancelAction)
      }
      .padding(10)
    }
  }

  private func meta(_ m: CardEditorModel, _ k: String) -> Binding<String> {
    Binding(get: { m.meta?[k]?.string ?? "" }, set: { v in
      var meta = m.meta ?? .object([])
      meta[k] = .string(v)
      m.meta = meta
    })
  }

  private func editInputs(_ m: CardEditorModel, _ fn: (inout [JSONValue]) -> Void) {
    var inputs = m.definition?["inputs"]?.array ?? []
    fn(&inputs)
    m.definition?["inputs"] = inputs.isEmpty ? nil : .array(inputs)
  }

  @discardableResult
  private func apply(_ m: CardEditorModel, _ defId: String) async -> Bool {
    let p = "definitions/\(defId)/_outputs.fbs"
    if outputs != baseOutputs {
      let text = DraftPort.fbs(outputs.filter { !$0.name.isEmpty }, namespace: "draft_" + defId.replacingOccurrences(of: "-", with: "_"))
      if text.isEmpty { m.removeFile(p) } else if session.sourcePaths.contains(p) { m.texts[p] = text } else { m.createFile("_outputs.fbs", text: text) }
    }
    let ok = await m.apply()
    if ok { baseOutputs = outputs }
    return ok
  }
}

extension DraftPort {
  static func == (a: DraftPort, b: DraftPort) -> Bool { a.name == b.name && a.type == b.type }
}

// MARK: - Card database

/// Every definition: search, edit the shared definition, place on the canvas.
struct CardDatabaseView: View {
  @Environment(\.openWindow) private var openWindow
  let session: ProjectSession
  @State private var search = ""
  @State private var selected: String?
  @State private var cardText = ""
  @State private var metaText = ""
  @State private var baseCard = ""
  @State private var baseMeta = ""
  @State private var parseError: String?
  @State private var importing = false
  @State private var message: String?

  var body: some View {
    HSplitView {
      VStack(spacing: 0) {
        HStack {
          TextField("検索", text: $search).textFieldStyle(.roundedBorder)
          Menu {
            ForEach(DefinitionTemplate.allCases) { t in
              Button(t.label) { selected = session.createDefinition(name: "新しい定義", template: t) }
            }
            Divider()
            Button("他のプロジェクトからコピー…") { importing = true }
          } label: { Image(systemName: "plus") }
            .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            .disabled(!session.isEditable)
        }
        .padding(6)
        List(ids, id: \.self, selection: $selected) { id in
          let def = session.definitions[id]
          HStack {
            Image(systemName: definitionKind(def).symbol).foregroundStyle(.secondary).frame(width: 16)
            VStack(alignment: .leading) {
              Text(def?["name"]?.string ?? id)
              Text("\(id) · \(definitionKind(def).label) · \(session.usageCount(ofDefinition: id)) 枚").font(.caption).foregroundStyle(.secondary)
            }
          }
          .tag(id)
          .draggable(DefinitionDrag.prefix + id)
        }
      }
      .frame(minWidth: 240, idealWidth: 280)
      detail.frame(minWidth: 420)
    }
    .sheet(isPresented: $importing) { ImportDefinitionsSheet(session: session) }
    .onChange(of: selected) { load() }
    .navigationTitle("Card 定義")
  }

  private var ids: [String] {
    session.definitions.keys.sorted().filter { id in
      search.isEmpty || id.localizedCaseInsensitiveContains(search) ||
        (session.definitions[id]?["name"]?.string ?? "").localizedCaseInsensitiveContains(search) ||
        (session.definitions[id]?["description"]?.string ?? "").localizedCaseInsensitiveContains(search)
    }
  }

  @ViewBuilder private var detail: some View {
    if let id = selected, session.definitions[id] != nil {
      let unapplied = cardText != baseCard || metaText != baseMeta
      VStack(alignment: .leading, spacing: 8) {
        HStack {
          Text(session.definitions[id]?["name"]?.string ?? id).font(.title2.bold())
          Spacer()
          if unapplied { Label("未適用の変更", systemImage: "pencil.circle.fill").foregroundStyle(.orange) }
        }
        Text("この定義を使う \(session.usageCount(ofDefinition: id)) 枚の Card に変更が反映されます。")
          .font(.callout).foregroundStyle(.secondary)
        Text("_card.json（既定値・params・入力）").font(.headline)
        CodeEditor(text: $cardText, language: "json").frame(minHeight: 180)
        Text("_meta.json（説明・計画・追加コンテキスト、任意）").font(.headline)
        CodeEditor(text: $metaText, language: "json").frame(minHeight: 80, maxHeight: 140)
        if let parseError { Label(parseError, systemImage: "xmark.octagon").foregroundStyle(.red) }
        if let message { Text(message).font(.callout).foregroundStyle(.secondary) }
        HStack {
          Button("Canvas（最上位）に配置") { place(id) }.disabled(!session.isEditable)
          Button("ソースを開く…") {
            if let k = firstUse(id) { openWindow(id: "card", value: CardRef(path: session.identity.url.path, card: k, category: .source)) }
          }
          .disabled(firstUse(id) == nil)
          .help("この定義を使う Card の設定画面で開きます")
          Button("削除", role: .destructive) { session.edit { $0.removeDefinition(id) }; selected = nil }
            .disabled(session.usageCount(ofDefinition: id) > 0 || !session.isEditable)
            .help(session.usageCount(ofDefinition: id) > 0 ? "使用中の定義は削除できません" : "定義とソースを削除")
          Spacer()
          Button("再読込") { load() }
          Button("適用") { Task { await apply(id) } }.disabled(!unapplied || !session.isEditable)
        }
      }
      .padding(12)
    } else {
      ContentUnavailableView("定義を選んでください", systemImage: "square.stack", description: Text("ドラッグして Canvas に配置できます。"))
    }
  }

  private func firstUse(_ id: String) -> CardKey? {
    func find(_ scope: [String]) -> CardKey? {
      for c in session.cards(in: scope) {
        guard let cid = c["id"]?.string else { continue }
        if c["definition"]?.string == id { return key(scope + [cid]) }
        if c["canvas"] != nil, let k = find(scope + [cid]) { return k }
      }
      return nil
    }
    return find([])
  }

  private func load() {
    guard let id = selected else { return }
    cardText = session.definitions[id]?.serialized() ?? "{}"
    metaText = session.definitionMeta[id]?.serialized() ?? ""
    baseCard = cardText
    baseMeta = metaText
    parseError = nil
    message = nil
  }

  private func place(_ id: String) {
    let k = session.addCard(definition: id, in: [], at: CGPoint(x: 40, y: 40))
    message = "\(k) を最上位の Workflow に配置しました（未保存）。"
  }

  private func apply(_ id: String) async {
    do {
      let card = try JSONValue.parse(cardText)
      let meta = metaText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : try JSONValue.parse(metaText)
      parseError = nil
      session.edit { $0.setDefinition(id, card: card, meta: meta) }
      await session.save()
      message = session.saveError.map { "保存できませんでした: \($0)" } ?? "適用しました（revision \(session.rev)）。"
      load()
    } catch {
      parseError = "JSON として読めません: \(error)"
    }
  }
}

// MARK: - Workflow settings

/// Project settings shared in the project: general, database connections, build cache.
struct WorkflowSettingsView: View {
  let session: ProjectSession
  @State private var tab = Tab.general

  enum Tab: String, CaseIterable, Identifiable {
    case general, databases, cache
    var id: String { rawValue }
    var title: String {
      switch self {
      case .general: return String(localized: "一般")
      case .databases: return String(localized: "データベース接続先")
      case .cache: return String(localized: "ビルドキャッシュ")
      }
    }
  }

  var body: some View {
    VStack(spacing: 0) {
      Picker("", selection: $tab) { ForEach(Tab.allCases) { Text($0.title).tag($0) } }
        .pickerStyle(.segmented).labelsHidden().padding(12)
      Divider()
      switch tab {
      case .general: GeneralProjectSettings(session: session)
      case .databases: DatabaseSettings(session: session)
      case .cache: CacheSettings(session: session)
      }
    }
    .navigationTitle("Workflow 設定 · \(session.scopeName([]))")
  }
}

private struct GeneralProjectSettings: View {
  let session: ProjectSession
  @State private var name = ""
  @State private var pkgs = ""
  @State private var message: String?

  var body: some View {
    let changed = name != (session.project["name"]?.string ?? "") || pkgs != (session.project["pkgsPath"]?.string ?? "")
    VStack(alignment: .leading) {
      Form {
        TextField("プロジェクト名", text: $name)
        TextField("実行環境（Nixpkgs）", text: $pkgs, prompt: Text("tsugumi:default"))
        Text("「tsugumi:default」はアプリが固定した Nixpkgs です。変更すると Card の再ビルドが必要になります。")
          .font(.caption).foregroundStyle(.secondary)
        LabeledContent("場所", value: session.identity.url.path)
      }
      .formStyle(.grouped)
      HStack {
        if let message { Text(message).font(.callout).foregroundStyle(.secondary) }
        Spacer()
        Button("保存") {
          session.edit { s in
            s.project["name"] = .string(name)
            s.project["pkgsPath"] = .string(pkgs.isEmpty ? "tsugumi:default" : pkgs)
          }
          Task {
            await session.save()
            message = session.saveError ?? "保存しました。"
          }
        }
        .disabled(!changed || !session.isEditable)
        .keyboardShortcut(.defaultAction)
      }
      .padding(12)
    }
    .onAppear {
      name = session.project["name"]?.string ?? ""
      pkgs = session.project["pkgsPath"]?.string ?? ""
    }
  }
}

private struct DatabaseSettings: View {
  let session: ProjectSession
  struct Row: Identifiable, Hashable {
    let id = UUID()
    var name: String
    var url: String
  }
  @State private var rows: [Row] = []
  @State private var base: [Row] = []
  @State private var message: String?

  var body: some View {
    let changed = rows.map { [$0.name, $0.url] } != base.map { [$0.name, $0.url] }
    VStack(alignment: .leading, spacing: 8) {
      Text("記録 Card が外部 DB にログを保存するときの接続先です。core の履歴とは独立しています。")
        .font(.callout).foregroundStyle(.secondary).padding(.horizontal, 12).padding(.top, 8)
      Table($rows) {
        TableColumn("名前") { $r in TextField("", text: $r.name) }
        TableColumn("URL") { $r in TextField("", text: $r.url, prompt: Text("postgresql://…")) }
        TableColumn("") { $r in
          Button { rows.removeAll { $0.id == r.id } } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless)
        }
        .width(30)
      }
      HStack {
        Button("追加") { rows.append(Row(name: "db\(rows.count + 1)", url: "")) }
        if let message { Text(message).font(.callout).foregroundStyle(.secondary) }
        Spacer()
        Button("再読込") { load() }
        Button("保存") { save() }.disabled(!changed || !session.isEditable).keyboardShortcut(.defaultAction)
      }
      .padding(12)
    }
    .onAppear(perform: load)
  }

  private func load() {
    rows = (session.project["databaseConnections"]?.array ?? []).map {
      Row(name: $0["name"]?.string ?? "", url: $0["url"]?.string ?? "")
    }
    base = rows
    message = nil
  }

  private func save() {
    let list: [JSONValue] = rows.map { .object([("name", .string($0.name)), ("url", .string($0.url))]) }
    session.edit { $0.project["databaseConnections"] = .array(list) }
    Task {
      await session.save()
      message = session.saveError ?? "保存しました。"
      if session.saveError == nil { load() }
    }
  }
}

private struct CacheSettings: View {
  let session: ProjectSession
  @State private var result: Tsugumi_RPC_CacheClearResultT?
  @State private var working = false
  @State private var message: String?
  @State private var confirm = false

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("Card のビルド結果（Nix の評価・ビルドのキャッシュ）を削除します。文書・実行履歴・保持した成果物は削除しません。次回の実行で必要なものを再ビルドします。")
        .fixedSize(horizontal: false, vertical: true)
      Text("実行中・SDK 準備中は削除できません。").font(.callout).foregroundStyle(.secondary)
      if let r = result {
        GroupBox("結果") {
          VStack(alignment: .leading, spacing: 4) {
            LabeledContent("削除", value: "\(r.deleted.count) 件")
            LabeledContent("保持", value: "\(r.retained.count) 件")
            if let b = r.freedBytes { LabeledContent("解放", value: ByteCountFormatter.string(fromByteCount: Int64(b), countStyle: .file)) }
          }
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
      if let message { Text(message).foregroundStyle(.red).textSelection(.enabled) }
      Spacer()
      HStack {
        Spacer()
        if working { ProgressView().controlSize(.small) }
        Button("削除", role: .destructive) { confirm = true }
          .disabled(working || session.runActive || !session.connected)
      }
    }
    .padding(16)
    .confirmationDialog("ビルドキャッシュを削除しますか？", isPresented: $confirm) {
      Button("削除", role: .destructive) { clear() }
      Button("キャンセル", role: .cancel) {}
    }
  }

  private func clear() {
    working = true
    message = nil
    Task {
      do { result = try await session.clearBuildCache() } catch { message = "削除できませんでした: \(error)" }
      working = false
    }
  }
}
