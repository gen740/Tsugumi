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
  /// Boundary ports of a Card in Card renamed or removed here, per side
  /// ("inputs"/"outputs"): current name → name in the project, and removed
  /// project names. Applying updates the outer connections to match.
  var boundaryRenames: [String: [String: String]] = [:]
  var boundaryRemoved: [String: Set<String>] = [:]
  /// Category shown in the window (kept with the model, so edits and the
  /// place survive switching).
  var category: CardEditorCategory = .general
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

  /// Takes the project's current state as the new base. After an apply
  /// (`keepingSources`) the files stay open with the text just saved, which
  /// is now their base; otherwise they are read again.
  func reload(keepingSources: Bool = false) {
    let keep = keepingSources ? texts.filter { !removed.contains($0.key) } : [:]
    instance = session.instance(cardKey) ?? .object([])
    definition = definitionId.flatMap { session.definitions[$0] }
    meta = definitionId.flatMap { session.definitionMeta[$0] }
    baseInstance = instance
    baseDefinition = definition
    baseMeta = meta
    texts = [:]
    baseTexts = [:]
    baseHashes = [:]
    loads = [:]
    for (p, t) in keep {
      texts[p] = t
      baseTexts[p] = t
      loads[p] = .loaded
      if let h = session.sources[p]?.hash { baseHashes[p] = h }
    }
    generation += 1
    boundaryRenames = [:]
    boundaryRemoved = [:]
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

  /// Whether a source file's content is known. A file that could not be read
  /// is never shown or treated as an empty file.
  enum SourceLoad: Equatable {
    case loading
    case loaded
    case failed(String)
  }

  private(set) var loads: [String: SourceLoad] = [:]
  /// Bumped by every reload, so open editors read their file again if needed.
  private(set) var generation = 0
  /// Files open as tabs and the one shown (kept across categories and applies).
  private(set) var openFiles: [String] = []
  private(set) var shownFile: String?
  var collapsedFolders: Set<String> = []

  func openFile(_ p: String) {
    if !openFiles.contains(p) { openFiles.append(p) }
    shownFile = p
  }

  func closeFile(_ p: String) {
    guard let i = openFiles.firstIndex(of: p) else { return }
    openFiles.remove(at: i)
    if shownFile == p { shownFile = openFiles.isEmpty ? nil : openFiles[min(i, openFiles.count - 1)] }
  }

  /// Reads a source file once (later calls return the working copy).
  /// Throws when it cannot be read; nothing is recorded as its content then.
  func text(_ p: String) async throws -> String {
    if let t = texts[p] { return t }
    loads[p] = .loading
    let hash = session.sources[p]?.hash
    do {
      let t = try await session.sourceText(p)
      if texts[p] == nil {
        texts[p] = t
        baseTexts[p] = t
        if let hash { baseHashes[p] = hash }
      }
      loads[p] = .loaded
      return texts[p] ?? t
    } catch {
      loads[p] = .failed("\(error)")
      throw error
    }
  }

  /// The working copy of a file, only once it was read (or created here).
  func loadedText(_ p: String) -> String? { texts[p] }

  /// The file was changed here and not applied yet.
  func isEdited(_ p: String) -> Bool { texts[p] != nil && (created.contains(p) || texts[p] != baseTexts[p]) }

  /// `_outputs.fbs` of the definition.
  var outputsPath: String? { definitionId.map { "definitions/\($0)/_outputs.fbs" } }

  /// Output ports from the edited, unapplied `_outputs.fbs` (nil: not edited,
  /// the resolved ports apply).
  var editedOutputs: OutputCatalog.Result? {
    guard let p = outputsPath else { return nil }
    if removed.contains(p) { return .ports([]) }
    guard isEdited(p), let t = texts[p] else { return nil }
    return OutputCatalog.parse(t)
  }

  func createFile(_ rel: String, text: String = "") {
    guard let id = definitionId, !rel.isEmpty else { return }
    let p = "definitions/\(id)/\(rel)"
    removed.remove(p)
    created.insert(p)
    texts[p] = text
    loads[p] = .loaded
  }

  /// Renames by copying the content that was actually read; a file that
  /// cannot be read is left as it is.
  @discardableResult
  func rename(_ p: String, to rel: String) async -> Bool {
    let t: String
    do {
      t = try await text(p)
    } catch {
      message = String(localized: "読み込めないため名前を変更しませんでした: ") + "\(error)"
      return false
    }
    removeFile(p)
    createFile(rel, text: t)
    return true
  }

  func removeFile(_ p: String) {
    if created.contains(p) { created.remove(p) } else { removed.insert(p) }
    texts[p] = nil
    loads[p] = nil
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
    let renames = boundaryRenames, dropped = boundaryRemoved
    session.edit { s in
      // Settings only: the inner graph and the id are edited on the canvas.
      s.updateInstance(cardKey) { c in
        let keep = ["id", "canvas"]
        var out: [(String, JSONValue)] = (c.object ?? []).filter { keep.contains($0.0) }
        for (k, v) in inst.object ?? [] where !keep.contains(k) { out.append((k, v)) }
        c = .object(out)
      }
      // The outer wires follow renamed boundary ports; removed ones go.
      if !renames.isEmpty || !dropped.isEmpty, let outer = path(cardKey).last {
        s.updateGraph(Array(path(cardKey).dropLast())) { g in
          var conns = g["connections"]?.array ?? []
          for side in ["inputs", "outputs"] {
            let end = side == "inputs" ? "to" : "from"
            conns.removeAll { c in
              c[end]?["card"]?.string == outer && (dropped[side] ?? []).contains(c[end]?["port"]?.string ?? "")
            }
            for (current, original) in renames[side] ?? [:] where current != original {
              for i in conns.indices where conns[i][end]?["card"]?.string == outer && conns[i][end]?["port"]?.string == original {
                conns[i][end]?["port"] = .string(current)
              }
            }
          }
          g["connections"] = .array(conns)
        }
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
    reload(keepingSources: true)
    return true
  }
}

struct CardSettingsEditor: View {
  @Environment(\.dismiss) private var dismiss
  @Environment(\.openWindow) private var openWindow
  let session: ProjectSession
  let ref: CardRef
  @State private var model: CardEditorModel?
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
        let m = CardEditorModel(session: session, cardKey: ref.card)
        m.category = ref.category
        model = m
        UIScript.shared.editors[ref.card] = m
      }
    }
    // The Card's name is the title, the project the subtitle (DetailHost);
    // toolbar, sidebar, preview and form share the window background.
    .navigationTitle(session.displayName(ref.card))
    .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
    .containerBackground(Color(nsColor: .windowBackgroundColor), for: .window)
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
      SettingsSidebar(items: categories(m), selection: Binding(get: { m.category }, set: { m.category = $0 }),
                      title: \.title, symbol: \.symbol, width: 160)
      Divider()
      VStack(spacing: 0) {
        // The preview belongs to 一般; editors, ports and history use the whole height.
        if m.category == .general {
          CardPreview(session: session, model: m)
            .frame(minHeight: 120, idealHeight: 170, maxHeight: 220)
          Divider()
        }
        Group {
          switch m.category {
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
    // Resizable: wider windows give the editor and lists the room; smaller
    // ones scroll (each category scrolls on its own).
    .frame(minWidth: 620, maxWidth: .infinity, minHeight: 420, maxHeight: .infinity)
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
                  gridSize: AppPreferences.shared.gridSize, fitKey: model.cardKey)
      .onAppear(perform: refresh)
      .onChange(of: model.instance) { refresh() }
      .onChange(of: model.definition) { refresh() }
      .onChange(of: model.meta) { refresh() }
      .onChange(of: model.texts) { refresh() }
      .onChange(of: session.cardStates[model.cardKey]?.state) { refresh() }
      .onChange(of: session.resolution[model.cardKey]?.outputs) { refresh() }
      .overlay(alignment: .topLeading) { notice.padding(6) }
      .overlay(alignment: .topTrailing) {
        Text("プレビュー").font(.caption2).foregroundStyle(.secondary).padding(6)
      }
  }

  /// What the outputs shown come from, when it is not the resolved definition.
  @ViewBuilder private var notice: some View {
    switch model.editedOutputs {
    case .error(let e)?:
      Label("_outputs.fbs を読めません: \(e)（出力ポートは表示していません）", systemImage: "exclamationmark.triangle.fill")
        .font(.caption).foregroundStyle(.orange).lineLimit(2)
        .padding(.horizontal, 6).padding(.vertical, 3)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 5))
    case .ports?:
      Label("編集中の _outputs.fbs（未適用・型は適用後に解決）", systemImage: "pencil.circle")
        .font(.caption).foregroundStyle(.secondary)
        .padding(.horizontal, 6).padding(.vertical, 3)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 5))
    case nil:
      if session.saving {
        Label("解決中…", systemImage: "hourglass").font(.caption).foregroundStyle(.secondary)
          .padding(.horizontal, 6).padding(.vertical, 3)
          .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 5))
      }
    }
  }

  private func refresh() {
    let collapsed = items.first?.data.collapsed ?? session.collapsed(model.cardKey)
    var outputs: [PortData]?
    switch model.editedOutputs {
    case .ports(let p)?: outputs = p
    case .error?: outputs = []
    case nil: outputs = nil
    }
    let data = session.nodeData(model.cardKey, instance: model.instance, definition: model.definition, meta: model.meta,
                                collapsed: collapsed, outputs: outputs)
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
        LabeledContent("アイコン") {
          SymbolPicker(symbol: Binding(get: { model.instance["symbol"]?.string.flatMap { $0.isEmpty ? nil : $0 } },
                                       set: { model.instance["symbol"] = $0.map { .string($0) } }),
                       inherited: def?["symbol"]?.string ?? model.kind.symbol)
        }
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
              // An editable field that looks like one (border, focus ring).
              TextField("名前", text: Binding(get: { name }, set: { v in editInputs { $0[i]["name"] = .string(v) } }))
                .textFieldStyle(.roundedBorder).labelsHidden()
                .frame(maxWidth: 140)
                .help("入力ポートの名前")
              TextField("用途", text: note("inputs", name)).textFieldStyle(.roundedBorder).labelsHidden()
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
            Text(p.name).font(.body.monospaced()).frame(maxWidth: 140, alignment: .leading)
            TextField("用途", text: note("outputs", p.name)).textFieldStyle(.roundedBorder).labelsHidden()
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
          // Only File / Directory outputs can be kept as artifacts.
          let keepable = (r?.outputs ?? []).filter { $0.kind == .file || $0.kind == .directory }
          if keepable.isEmpty {
            LabeledContent("保持する出力") {
              Text(r == nil ? "出力の型がまだ解決されていません" : "File / Directory の出力がないため、保持できる出力はありません")
                .font(.callout).foregroundStyle(.secondary)
            }
          } else {
            Picker("保持する出力", selection: Binding(
              get: { model.instance["artifactOutput"]?.string ?? def?["artifactOutput"]?.string ?? "" },
              set: { model.instance["artifactOutput"] = $0.isEmpty ? nil : .string($0) })) {
              Text("選んでください").tag("")
              ForEach(keepable, id: \.name) { p in
                Text("\(p.name)（\(p.kind == .file ? "File" : "Directory")）").tag(p.name)
              }
            }
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

  /// Renames a boundary port; the outer connections follow on apply.
  private func rename(_ side: String, _ name: String, to new: String) {
    guard !new.isEmpty, new != name, var b = model.instance["boundary"], var s = b[side]?.object,
          !s.contains(where: { $0.0 == new }) else { return }
    if let i = s.firstIndex(where: { $0.0 == name }) { s[i].0 = new }
    b[side] = .object(s)
    model.instance["boundary"] = b
    var map = model.boundaryRenames[side] ?? [:]
    let original = map[name] ?? name
    map[name] = nil
    map[new] = original
    model.boundaryRenames[side] = map
  }

  /// Removes a boundary port; outer connections to it are removed on apply.
  private func remove(_ side: String, _ name: String) {
    guard var b = model.instance["boundary"], var s = b[side]?.object else { return }
    s.removeAll { $0.0 == name }
    b[side] = .object(s)
    model.instance["boundary"] = b
    let original = model.boundaryRenames[side]?[name] ?? name
    model.boundaryRenames[side]?[name] = nil
    model.boundaryRemoved[side, default: []].insert(original)
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

/// File tree, tabs and editor for a definition's sources, `_outputs.fbs` and
/// `default.nix`, with formatting, errors in place and the generated inputs.
/// The open files, the shown one and the expanded folders live on the model,
/// so switching categories or applying keeps them.
private struct SourceCategory: View {
  @Bindable var model: CardEditorModel
  let only: String?
  @State private var newName = ""
  @State private var renaming: String?
  @State private var showNew = false
  @State private var formatting = false
  @State private var formatMessage: String?
  @State private var formatMarkers: [EditorMarker] = []
  @State private var generated: [String: String] = [:]
  @State private var generatedError: String?

  static let generatedPrefix = "generated:"

  private var prefix: String { model.definitionId.map { "definitions/\($0)/" } ?? "" }

  var body: some View {
    let files = model.files.filter { only == nil || $0.hasSuffix("/" + only!) }
    HSplitView {
      if only == nil {
        VStack(spacing: 0) {
          ScrollView {
            VStack(alignment: .leading, spacing: 0) {
              FileTreeRows(nodes: FileNode.tree(files.map { String($0.dropFirst(prefix.count)) }, base: prefix), depth: 0,
                           model: model, open: open, contextMenu: menu)
              if model.kind == .code {
                Text("生成（読み取り専用）").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                  .padding(.leading, 10).padding(.top, 12).padding(.bottom, 2)
                ForEach(["_inputs.fbs", "_params.fbs"], id: \.self) { name in
                  FileRow(name: name, icon: "lock.doc", depth: 0, folder: nil,
                          selected: model.shownFile == Self.generatedPrefix + name) { open(Self.generatedPrefix + name) }
                }
              }
            }
            .padding(.vertical, 4)
          }
          Divider()
          HStack {
            Button { showNew = true; newName = "src/" } label: { Image(systemName: "plus") }.help("ファイルを作成")
            Button {
              if let s = model.shownFile, !s.hasPrefix(Self.generatedPrefix) { model.removeFile(s); model.closeFile(s) }
            } label: { Image(systemName: "minus") }
              .disabled(model.shownFile == nil || model.shownFile!.hasPrefix(Self.generatedPrefix)).help("表示中のファイルを削除")
            Spacer()
          }
          .buttonStyle(.borderless).padding(6)
        }
        .frame(minWidth: 170, idealWidth: 220, maxWidth: 360)
      }
      VStack(spacing: 0) {
        if only == nil && model.openFiles.count > 1 { tabs }
        editorPane(files: files)
      }
      .frame(minWidth: 320, maxWidth: .infinity, maxHeight: .infinity)
    }
    .onAppear {
      if only != nil {
        if let f = files.first { model.openFile(f) }
      } else if model.shownFile == nil || (model.shownFile?.hasSuffix("/default.nix") ?? false) {
        if let f = files.first(where: { $0.hasSuffix(".py") || $0.hasSuffix(".cc") }) ?? files.first { model.openFile(f) }
      }
    }
    .alert("ファイルを作成", isPresented: $showNew) {
      TextField("パス（例: src/util.py）", text: $newName)
      Button("作成") {
        model.createFile(newName)
        model.openFile(prefix + newName)
      }
      Button("キャンセル", role: .cancel) {}
    }
    .alert("名前を変更", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
      TextField("新しいパス", text: $newName)
      Button("変更") {
        if let r = renaming {
          let target = prefix + newName
          Task {
            if await model.rename(r, to: newName) {
              model.closeFile(r)
              model.openFile(target)
            }
          }
        }
        renaming = nil
      }
      Button("キャンセル", role: .cancel) { renaming = nil }
    }
  }

  private func open(_ path: String) { model.openFile(path) }

  @ViewBuilder private func menu(_ node: FileNode) -> some View {
    if node.children == nil {
      Button("開く") { open(node.path) }
      Button("名前を変更…") { renaming = node.path; newName = String(node.path.dropFirst(prefix.count)) }
      Button("削除", role: .destructive) {
        model.removeFile(node.path)
        model.closeFile(node.path)
      }
    }
  }

  /// Open files as tabs (a few at most; this is not an IDE).
  private var tabs: some View {
    ScrollView(.horizontal, showsIndicators: false) {
      HStack(spacing: 0) {
        ForEach(model.openFiles, id: \.self) { f in
          let shown = model.shownFile == f
          HStack(spacing: 5) {
            Image(systemName: f.hasPrefix(Self.generatedPrefix) ? "lock.doc" : FileNode.icon(f)).font(.caption)
            Text(displayName(f)).font(.caption).lineLimit(1)
            if model.isEdited(f) { Circle().fill(Color.orange).frame(width: 6, height: 6).help("未適用の変更") }
            Button { model.closeFile(f) } label: { Image(systemName: "xmark").font(.system(size: 8, weight: .bold)) }
              .buttonStyle(.borderless).foregroundStyle(.secondary).help("タブを閉じる（内容は保持されます）")
          }
          .padding(.horizontal, 10).padding(.vertical, 6)
          .background(shown ? Color(nsColor: .textBackgroundColor) : Color.clear)
          .overlay(alignment: .bottom) { if shown { Rectangle().fill(Color.accentColor).frame(height: 2) } }
          .contentShape(Rectangle())
          .onTapGesture { model.openFile(f) }
          .help(f)
        }
      }
    }
    .background(Color(nsColor: .windowBackgroundColor))
    .overlay(alignment: .bottom) { Divider() }
  }

  private func displayName(_ f: String) -> String {
    f.hasPrefix(Self.generatedPrefix) ? String(f.dropFirst(Self.generatedPrefix.count)) : (f as NSString).lastPathComponent
  }

  /// Header: the file's name and where it is (definition › folders).
  private func header(_ path: String, generatedFile: Bool) -> some View {
    let rel = generatedFile ? String(path.dropFirst(Self.generatedPrefix.count)) : String(path.dropFirst(prefix.count))
    let parts = rel.split(separator: "/").map(String.init)
    return HStack(spacing: 6) {
      Image(systemName: generatedFile ? "lock.doc" : FileNode.icon(path)).foregroundStyle(.secondary)
      Text(parts.last ?? rel).font(.callout.weight(.semibold).monospaced())
      HStack(spacing: 3) {
        Text(model.definitionId ?? "")
        ForEach(Array(parts.dropLast().enumerated()), id: \.offset) { _, p in
          Image(systemName: "chevron.right").font(.system(size: 8))
          Text(p)
        }
      }
      .font(.caption).foregroundStyle(.secondary).lineLimit(1)
      if generatedFile { Text("接続から生成（編集しません）").font(.caption2).foregroundStyle(.secondary) }
      if model.created.contains(path) { Text("新規").font(.caption2).foregroundStyle(.orange) }
      else if model.isEdited(path) { Text("未適用").font(.caption2).foregroundStyle(.orange) }
      Spacer()
      if let formatMessage { Text(formatMessage).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
      if !generatedFile {
        Button("整形") { format(path) }.disabled(formatting || model.loadedText(path) == nil).controlSize(.small)
      }
    }
    .padding(.horizontal, 10).padding(.vertical, 6)
    .background(Color(nsColor: .controlBackgroundColor))
    .overlay(alignment: .bottom) { Divider() }
  }

  @ViewBuilder private func editorPane(files: [String]) -> some View {
    VStack(spacing: 0) {
      if let selected = model.shownFile, selected.hasPrefix(Self.generatedPrefix) {
        let name = String(selected.dropFirst(Self.generatedPrefix.count))
        header(selected, generatedFile: true)
        if let e = generatedError {
          Text(e).font(.callout).foregroundStyle(.red).padding()
        }
        CodeEditor(text: .constant(generated[name] ?? ""), language: "fbs", editable: false, documentID: selected)
          .task(id: selected) { await loadGenerated() }
      } else if let selected = model.shownFile, model.files.contains(selected) {
        let markers = coreMarkers(for: selected) + formatMarkers
        let loaded = model.loadedText(selected) != nil
        header(selected, generatedFile: false)
        Group {
          if loaded {
            CodeEditor(text: Binding(get: { model.loadedText(selected) ?? "" },
                                     set: { model.texts[selected] = $0; formatMarkers = [] }),
                       language: (selected as NSString).pathExtension, markers: markers, documentID: selected)
          } else if case .failed(let e)? = model.loads[selected] {
            // Never an editable empty file: the content is unknown.
            ContentUnavailableView {
              Label("ファイルを読み込めませんでした", systemImage: "exclamationmark.triangle")
            } description: {
              Text("内容が不明なため、編集・名前変更・保存はできません。\n\(e)")
            } actions: {
              Button("再試行") { Task { try? await model.text(selected) } }
            }
          } else {
            ProgressView("読み込んでいます…").frame(maxWidth: .infinity, maxHeight: .infinity)
          }
        }
        .task(id: "\(selected)#\(model.generation)") {
          formatMarkers = []
          formatMessage = nil
          _ = try? await model.text(selected)
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
          model.openFile(prefix + "default.nix")
        }
      } else {
        ContentUnavailableView("ファイルを選んでください", systemImage: "doc.text")
      }
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
    guard let rpc = model.session.rpc, let source = model.loadedText(p) else { return }
    formatting = true
    Task {
      do {
        let r = try await rpc.format(path: p, text: source)
        let errors = r.diagnostics.compactMap { $0 }
        if errors.isEmpty {
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

/// Rows of a folder tree (VS Code style): indentation, a chevron and an
/// open/closed folder per folder, the shown file highlighted.
private struct FileTreeRows<Menu: View>: View {
  let nodes: [FileNode]
  let depth: Int
  @Bindable var model: CardEditorModel
  let open: (String) -> Void
  @ViewBuilder let contextMenu: (FileNode) -> Menu

  var body: some View {
    ForEach(nodes) { node in
      if let children = node.children {
        let expanded = !model.collapsedFolders.contains(node.path)
        FileRow(name: node.name, icon: expanded ? "folder.fill" : "folder", depth: depth, folder: expanded, selected: false) {
          if expanded { model.collapsedFolders.insert(node.path) } else { model.collapsedFolders.remove(node.path) }
        }
        if expanded {
          FileTreeRows(nodes: children, depth: depth + 1, model: model, open: open, contextMenu: contextMenu)
        }
      } else {
        FileRow(name: node.name, icon: FileNode.icon(node.name), depth: depth, folder: nil,
                selected: model.shownFile == node.path, edited: model.isEdited(node.path)) { open(node.path) }
          .contextMenu { contextMenu(node) }
          .help(node.path)
      }
    }
  }
}

private struct FileRow: View {
  let name: String
  let icon: String
  let depth: Int
  /// nil for a file; for a folder whether it is expanded.
  let folder: Bool?
  let selected: Bool
  var edited = false
  let action: () -> Void

  var body: some View {
    HStack(spacing: 4) {
      Group {
        if let folder { Image(systemName: folder ? "chevron.down" : "chevron.right").font(.system(size: 9, weight: .semibold)) }
        else { Color.clear }
      }
      .frame(width: 10)
      Image(systemName: icon).font(.callout).foregroundStyle(folder != nil ? Color.accentColor : .secondary).frame(width: 16)
      Text(name).lineLimit(1).truncationMode(.middle)
      Spacer(minLength: 0)
      if edited { Circle().fill(Color.orange).frame(width: 6, height: 6) }
    }
    .padding(.leading, 8 + CGFloat(depth) * 14).padding(.trailing, 8).padding(.vertical, 3)
    .background(RoundedRectangle(cornerRadius: 5).fill(selected ? Color.accentColor.opacity(0.25) : .clear).padding(.horizontal, 4))
    .contentShape(Rectangle())
    .onTapGesture(perform: action)
    .accessibilityAddTraits(selected ? [.isSelected, .isButton] : .isButton)
  }
}

struct FileNode: Identifiable, Hashable {
  var id: String { path }

  static func icon(_ name: String) -> String {
    switch (name as NSString).pathExtension {
    case "py": return "chevron.left.forwardslash.chevron.right"
    case "cc", "hh", "cpp", "hpp", "h": return "curlybraces"
    case "nix": return "snowflake"
    case "fbs": return "tablecells"
    case "json": return "doc.badge.gearshape"
    case "sh": return "terminal"
    default: return "doc.text"
    }
  }
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
