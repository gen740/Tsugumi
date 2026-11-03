import AppKit
import SwiftUI
import TsugumiAgent
import TsugumiRPC

/// Local working copy of one Card's settings (its `_card.json`), metadata
/// and sources. Nothing reaches the project until "適用". Every Card has its
/// own definition folder, so what is edited here is this Card only (inside a
/// Card in Card: the Card of the external, this project's copy).
///
/// The copy follows what changes elsewhere while it is open (the Agent,
/// the canvas): settings and files not touched here take the new content;
/// those changed on both sides are listed as conflicts, and applying keeps
/// what was edited here.
@MainActor
@Observable
final class CardEditorModel {
  let session: ProjectSession
  let cardKey: CardKey
  var definition: JSONValue?
  var meta: JSONValue?
  var texts: [String: String] = [:]     // loaded or edited source text
  var removed: Set<String> = []
  var created: Set<String> = []
  var message: String?
  var applying = false
  /// Settings and files changed both here and elsewhere since the copy was taken.
  var conflicts: [String] = []
  /// Category shown in the window (kept with the model, so edits and the
  /// place survive switching).
  var category: CardEditorCategory = .general
  private var baseDefinition: JSONValue?
  private var baseMeta: JSONValue?
  private var baseTexts: [String: String] = [:]
  private var baseHashes: [String: String] = [:]   // source hash when each file was read

  init(session: ProjectSession, cardKey: CardKey) {
    self.session = session
    self.cardKey = cardKey
    reload()
  }

  /// The Card's definition folder name (its id).
  var definitionId: String? { path(cardKey).last }
  var kind: CardKind { session.kind(cardKey) }
  var exists: Bool { session.instance(cardKey) != nil }
  private var prefix: String { session.definitionDir(cardKey) ?? "" }

  var unapplied: Bool {
    definition != baseDefinition || meta != baseMeta || !removed.isEmpty || !created.isEmpty ||
      texts.contains { baseTexts[$0.key] != $0.value }
  }

  /// Source files of the definition (documents excluded), with local changes.
  var files: [String] {
    guard !prefix.isEmpty else { return [] }
    let docs: Set<String> = [prefix + "_card.json", prefix + "_meta.json", prefix + "_link.json"]
    return Set(session.sourcePaths.filter { $0.hasPrefix(prefix) && !docs.contains($0) }).union(created).subtracting(removed).sorted()
  }

  /// Changes whenever what this copy follows changes (the settings, the
  /// files' hashes).
  var followToken: String {
    let files = session.sourcePaths.filter { $0.hasPrefix(prefix) }.map { "\($0)=\(session.sources[$0]?.hash ?? "-")" }
    return [session.cardDefinition(cardKey)?.serialized() ?? "", session.cardMeta(cardKey)?.serialized() ?? ""]
      .joined(separator: "\u{1}") + files.joined(separator: ";")
  }

  /// Takes the project's current state as the new base. After an apply
  /// (`keepingSources`) the files stay open with the text just saved, which
  /// is now their base; otherwise they are read again.
  func reload(keepingSources: Bool = false) {
    let keep = keepingSources ? texts.filter { !removed.contains($0.key) } : [:]
    definition = session.cardDefinition(cardKey)
    meta = session.cardMeta(cardKey)
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
    removed = []
    created = []
    message = nil
    conflicts = []
  }

  /// Takes in what changed elsewhere since the copy was taken.
  func follow() {
    let defNow = session.cardDefinition(cardKey), metaNow = session.cardMeta(cardKey)
    func clashes(_ base: JSONValue?, _ ours: JSONValue?, _ theirs: JSONValue?, _ file: String) -> [String] {
      guard ours != base, theirs != base, ours != theirs else { return [] }
      let keys = Set((base?.object ?? []).map(\.0) + (ours?.object ?? []).map(\.0) + (theirs?.object ?? []).map(\.0))
      return keys.sorted().filter { k in
        base?[k] != ours?[k] && base?[k] != theirs?[k] && ours?[k] != theirs?[k]
      }.map { "\(file): \($0)" }
    }
    var found: [String] = []
    if defNow != baseDefinition {
      found += clashes(baseDefinition, definition, defNow, "_card.json")
      definition = merge3(base: baseDefinition, ours: definition, theirs: defNow)
      baseDefinition = defNow
    }
    if metaNow != baseMeta {
      found += clashes(baseMeta, meta, metaNow, "_meta.json")
      meta = merge3(base: baseMeta, ours: meta, theirs: metaNow)
      baseMeta = metaNow
    }
    for p in Array(texts.keys) where loads[p] == .loaded && !created.contains(p) {
      let h = session.sources[p]?.hash
      guard let base = baseHashes[p], h != base else { continue }
      if isEdited(p) {
        found.append(String(p.dropFirst(prefix.count)))
      } else {
        // Not edited here: read the new content.
        texts[p] = nil
        baseTexts[p] = nil
        baseHashes[p] = nil
        loads[p] = nil
        generation += 1
      }
    }
    if !found.isEmpty { conflicts = Array(Set(conflicts + found)).sorted() }
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
  var outputsPath: String? { prefix.isEmpty ? nil : prefix + "_outputs.fbs" }

  /// Output ports from the edited, unapplied `_outputs.fbs` (nil: not edited,
  /// the resolved ports apply).
  var editedOutputs: OutputCatalog.Result? {
    guard let p = outputsPath else { return nil }
    if removed.contains(p) { return .ports([]) }
    guard isEdited(p), let t = texts[p] else { return nil }
    return OutputCatalog.parse(t)
  }

  func createFile(_ rel: String, text: String = "") {
    guard !prefix.isEmpty, !rel.isEmpty else { return }
    let p = prefix + rel
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

  /// Copies the working copy into the project as one undoable edit (saved at once).
  func apply() async -> Bool {
    guard session.isEditable else {
      message = String(localized: "実行中は Card を変更できません。実行が終わってから適用してください。")
      return false
    }
    follow()
    applying = true
    defer { applying = false }
    let def = definition, m = meta, k = cardKey
    let changedTexts = texts.filter { baseTexts[$0.key] != $0.value || created.contains($0.key) }
    let gone = removed
    let bases = baseHashes
    let clashing = Set(conflicts)
    session.edit { s in
      if let def, var d = s.cardDefinitionDraft(k), d.link == nil, def != d.card || m != d.meta {
        d.card = def
        d.meta = m
        s.setCardDefinition(k, d)
      }
      for (p, t) in changedTexts {
        s.putSource(p, t)
        // Saved against the version this editor read (a file changed on both
        // sides replaces the other version: the user chose so by applying).
        if let h = bases[p], !clashing.contains(String(p.dropFirst(prefix.count))) { s.sourceBases[p] = h }
      }
      for p in gone {
        s.removeSource(p)
        if let h = bases[p] { s.sourceBases[p] = h }
      }
    }
    await session.save()
    if let e = session.saveError {
      message = String(localized: "保存できませんでした: ") + e
      return false
    }
    message = String(localized: "適用しました。")
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
    // Renamed here (or elsewhere): the window follows the Card.
    .onReceive(NotificationCenter.default.publisher(for: .cardRenamed)) { n in
      guard let from = n.userInfo?["from"] as? String, let to = n.userInfo?["to"] as? String,
            n.userInfo?["project"] as? String == session.identity.url.path, from == model?.cardKey else { return }
      let m = CardEditorModel(session: session, cardKey: to)
      m.category = model?.category ?? .general
      model = m
      UIScript.shared.editors[to] = m
    }
    // The Card's name is the title, the project the subtitle (DetailHost);
    // toolbar, sidebar, preview and form share the window background.
    .navigationTitle(session.displayName(model?.cardKey ?? ref.card))
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
        if !m.conflicts.isEmpty {
          HStack(alignment: .top) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            Text("別の場所でも変更されました: \(m.conflicts.joined(separator: ", "))。適用するとここでの編集が残ります。")
              .font(.callout).fixedSize(horizontal: false, vertical: true)
            Spacer()
            Button("最新を読み込む") { confirmReload = true }
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
    // What changes elsewhere while the window is open is taken in at once.
    .onChange(of: m.followToken) { m.follow() }
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
    let data = session.nodeData(model.cardKey, definition: model.definition, meta: model.meta,
                                collapsed: collapsed, outputs: outputs)
    if items.isEmpty {
      items = [CanvasItem(id: model.cardKey, data: data, origin: .zero)]
    } else {
      items[0].data = data
    }
  }
}

// MARK: - Categories

/// Name (the Card's id and folder), description, params, when it runs, and
/// the worker's permissions.
private struct GeneralCategory: View {
  @Bindable var model: CardEditorModel
  @State private var name = ""
  @State private var renaming = false
  @State private var renameError: String?

  var body: some View {
    Form {
      Section {
        HStack {
          TextField("名前", text: $name)
            .onSubmit(rename)
          if name != model.session.displayName(model.cardKey) {
            Button("名前を変更") { rename() }.disabled(renaming || !model.session.isEditable)
          }
          if renaming { ProgressView().controlSize(.small) }
        }
        .help("名前は Card の ID で、定義のフォルダ名（definitions/<名前>/）です。変えると接続・配置・実行履歴も追従します（すぐに保存します）。")
        if let renameError { Text(renameError).font(.caption).foregroundStyle(.red) }
        LabeledContent("種類", value: model.kind.label)
        LabeledContent("アイコン") {
          SymbolPicker(symbol: Binding(get: { model.definition?["symbol"]?.string.flatMap { $0.isEmpty ? nil : $0 } },
                                       set: { model.definition?["symbol"] = $0.map { .string($0) } }),
                       inherited: model.kind.symbol)
        }
        TextField("概要", text: string("description"), axis: .vertical)
          .lineLimit(2...5)
      }
      if let params = model.definition?["params"]?.array, !params.isEmpty {
        Section("パラメータ") {
          ForEach(params.indices, id: \.self) { i in ParamRow(model: model, index: i) }
        }
      }
      if model.kind != .subgraph && model.kind != .draft {
        Section("実行") {
          if model.kind == .code {
            Picker("trigger", selection: string("trigger", fallback: "allInputs")) {
              Text("すべての入力が揃ったとき").tag("allInputs")
              Text("入力が届くたび").tag("eachInput")
            }
            .help("allInputs: 必須の入力がすべて届いたら 1 回動きます。eachInput: 値が 1 つ届くたびに動きます。")
          }
          numberField("maxRuns", label: "最大実行回数", fallback: "1")
          timeoutField("timeoutSeconds", label: "実行の期限（秒）")
          if model.kind == .logger {
            toggle("markdown", label: "Markdown として表示", fallback: false)
              .help("オン: 受け取った文字列（文字列のフィールドが 1 つのメッセージ）を Markdown と数式（$…$、$$…$$）で表示します。ほかのメッセージは JSON をコードとして表示します。")
          }
          if model.kind == .code {
            timeoutField("buildTimeoutSeconds", label: "ビルドの期限（秒）")
            toggle("reuseWorker", label: "worker の状態を残す", fallback: true)
              .help("オン: 読み込んだ worker を次の呼び出しや次の実行でも使い、状態（static 変数など）を残します。オフ: 呼び出しごとに新しい worker です。")
            toggle("filesystem", label: "作業・出力領域を使う", fallback: false)
            toggle("sandbox", label: "ファイルアクセスを制限", fallback: true)
          }
        }
        if model.kind == .code {
          Section {
            toggle("network", label: "ネットワークへのアクセスを許可する", fallback: false)
              .help("オフ（デフォルト）: preRun・run・postRun のどれからも外へ通信できません。")
            HostFoldersEditor(model: model)
          } header: {
            Label("Experimental", systemImage: "flask")
          }
        }
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
    .onAppear { name = model.session.displayName(model.cardKey) }
    .onChange(of: model.cardKey) { name = model.session.displayName(model.cardKey) }
  }

  private func rename() {
    let new = name.trimmingCharacters(in: .whitespaces)
    guard new != model.session.displayName(model.cardKey) else { return }
    renaming = true
    renameError = nil
    Task {
      do {
        if model.unapplied { guard await model.apply() else { renaming = false; return } }
        try await model.session.renameCard(model.cardKey, to: new)
        // The editor follows the Card under its new name.
        NotificationCenter.default.post(name: .cardRenamed, object: nil,
                                        userInfo: ["from": model.cardKey, "to": key(Array(path(model.cardKey).dropLast()) + [new]),
                                                   "project": model.session.identity.url.path])
      } catch {
        renameError = "\(error)"
        name = model.session.displayName(model.cardKey)
      }
      renaming = false
    }
  }

  private func string(_ name: String, fallback: String? = nil) -> Binding<String> {
    Binding(get: { model.definition?[name]?.string ?? fallback ?? "" },
            set: { v in model.definition?[name] = v.isEmpty || v == fallback ? nil : .string(v) })
  }

  private func toggle(_ name: String, label: LocalizedStringKey, fallback: Bool) -> some View {
    Toggle(label, isOn: Binding(get: { model.definition?[name]?.bool ?? fallback },
                                set: { v in model.definition?[name] = v == fallback ? nil : .bool(v) }))
  }

  private func numberField(_ name: String, label: LocalizedStringKey, fallback: String) -> some View {
    TextField(label, text: Binding(
      get: { model.definition?[name]?.numberText ?? model.definition?[name]?.string ?? "" },
      set: { v in
        let t = v.trimmingCharacters(in: .whitespaces)
        model.definition?[name] = t.isEmpty ? nil : (UInt64(t) != nil ? .number(t) : .string(t))  // core reports non-numbers
      }), prompt: Text(fallback))
  }

  private func timeoutField(_ name: String, label: LocalizedStringKey) -> some View {
    TextField(label, text: Binding(
      get: { model.definition?[name] == .null ? "" : (model.definition?[name]?.numberText ?? "") },
      set: { v in
        let t = v.trimmingCharacters(in: .whitespaces)
        model.definition?[name] = t.isEmpty ? nil : .number(t)
      }), prompt: Text("無期限"))
  }
}

/// Folders of the Mac this Card's worker may read (Experimental): each at
/// /host/<name> in the worker, read-only unless allowed to write. Only the
/// folder's path is kept in the project.
private struct HostFoldersEditor: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    let folders = model.definition?["hostFolders"]?.array ?? []
    VStack(alignment: .leading, spacing: 6) {
      HStack {
        Text("ホストのフォルダへのアクセスを許可する")
        Spacer()
        Button("フォルダを追加…") { add() }
      }
      ForEach(folders.indices, id: \.self) { i in
        let f = folders[i]
        let path = f["path"]?.string ?? ""
        var isDir: ObjCBool = false
        let present = FileManager.default.fileExists(atPath: path, isDirectory: &isDir) && isDir.boolValue
        HStack(spacing: 6) {
          Image(systemName: present ? "folder" : "folder.badge.questionmark").foregroundStyle(present ? Color.secondary : Color.orange)
          VStack(alignment: .leading, spacing: 1) {
            Text(path).lineLimit(1).truncationMode(.middle).font(.callout)
            Text(present ? "/host/\(f["name"]?.string ?? "")" : String(localized: "この Mac にはありません（実行すると失敗します）"))
              .font(.caption).foregroundStyle(present ? Color.secondary : Color.orange)
          }
          Spacer()
          Toggle("書き込み", isOn: Binding(get: { f["write"]?.bool ?? false }, set: { v in
            edit { $0[i]["write"] = v ? .bool(true) : nil }
          }))
          .toggleStyle(.checkbox)
          .help("オフ（デフォルト）: 読み取り専用")
          Button { edit { $0.remove(at: i) } } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless)
        }
      }
      Text("選んだフォルダだけを、この Card の worker から /host/<名前> として読めます。プロジェクトに保存するのはフォルダの場所だけです。追加・変更はプロジェクトを開き直すと反映されます。")
        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
    }
  }

  private func edit(_ fn: (inout [JSONValue]) -> Void) {
    var list = model.definition?["hostFolders"]?.array ?? []
    fn(&list)
    model.definition?["hostFolders"] = list.isEmpty ? nil : .array(list)
  }

  private func add() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.allowsMultipleSelection = false
    guard panel.runModal() == .OK, let url = panel.url else { return }
    let taken = Set((model.definition?["hostFolders"]?.array ?? []).compactMap { $0["name"]?.string })
    let base = String(url.lastPathComponent.map { $0.isASCII && ($0.isLetter || $0.isNumber || "-_.".contains($0)) ? $0 : "-" })
    var name = base.isEmpty || base == "." || base == ".." ? "folder" : base
    var n = 2
    while taken.contains(name) { name = "\(base)-\(n)"; n += 1 }
    edit { $0.append(.object([("path", .string(url.path)), ("name", .string(name))])) }
  }
}

private struct ParamRow: View {
  @Bindable var model: CardEditorModel
  let index: Int

  var body: some View {
    let param = model.definition?["params"]?.array?[index] ?? .object([])
    let name = param["name"]?.string ?? "?"
    let type = param["type"]?.string ?? "string"
    let value = param["data"]
    HStack {
      if type == "bool" {
        Toggle(name, isOn: Binding(get: { value?.bool ?? false }, set: { set(.bool($0)) }))
      } else {
        TextField(name, text: Binding(get: { display(value) }, set: { set(parseParam($0, type)) }))
      }
      Text(type + (param["nix"]?.bool == true ? " · nix" : "")).font(.caption2).foregroundStyle(.secondary)
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

  private func set(_ v: JSONValue?) {
    guard var params = model.definition?["params"]?.array, index < params.count else { return }
    params[index]["data"] = v
    model.definition?["params"] = .array(params)
  }
}

/// Ports, their purposes, recording and artifact retention.
private struct PortsCategory: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    let r = model.session.resolution[model.cardKey]
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
              TextField("用途", text: note("inputs", name), prompt: Text("用途（何を受け取るか）"))
                .textFieldStyle(.roundedBorder).labelsHidden()
                .help("このポートの用途（_meta.json の説明）。Agent とほかの人への説明です。")
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
        Text("入力の型は接続元の出力から決まります（_inputs.fbs は生成され、手で書きません）。")
          .font(.caption).foregroundStyle(.secondary)
      }
      OutputPortsSection(model: model)
      if model.kind != .subgraph && model.kind != .draft {
        Section("記録と成果物") {
          toggle("recordIO", label: "入出力を記録")
          toggle("retainArtifact", label: "成果物を保持")
          // Only File / Directory outputs can be kept as artifacts.
          let keepable = (r?.outputs ?? []).filter { $0.kind == .file || $0.kind == .directory }
          if keepable.isEmpty {
            LabeledContent("保持する出力") {
              Text(r == nil ? "出力の型がまだ解決されていません" : "File / Directory の出力がないため、保持できる出力はありません")
                .font(.callout).foregroundStyle(.secondary)
            }
          } else {
            Picker("保持する出力", selection: Binding(
              get: { model.definition?["artifactOutput"]?.string ?? "" },
              set: { model.definition?["artifactOutput"] = $0.isEmpty ? nil : .string($0) })) {
              Text("選んでください").tag("")
              ForEach(keepable, id: \.name) { p in
                Text("\(p.name)（\(p.kind == .file ? "File" : "Directory")）").tag(p.name)
              }
            }
          }
        }
      }
    }
    .formStyle(.grouped)
  }

  /// Port purpose kept in _meta.json (also after a Draft is implemented).
  private func note(_ list: String, _ name: String) -> Binding<String> {
    portNote(model, list, name)
  }

  private func editInputs(_ fn: (inout [JSONValue]) -> Void) {
    var inputs = model.definition?["inputs"]?.array ?? []
    fn(&inputs)
    model.definition?["inputs"] = inputs.isEmpty ? nil : .array(inputs)
  }

  private func toggle(_ name: String, label: LocalizedStringKey) -> some View {
    Toggle(label, isOn: Binding(get: { model.definition?[name]?.bool ?? false },
                                set: { model.definition?[name] = $0 ? .bool(true) : nil }))
  }
}

/// A port's purpose in _meta.json.
@MainActor
func portNote(_ model: CardEditorModel, _ list: String, _ name: String) -> Binding<String> {
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

/// Output ports as `_outputs.fbs` defines them (its source is the truth).
/// Each port's type is chosen from the tables of that schema, as the core
/// compiles it (bfbs; the app never parses fbs), and written back to it.
private struct OutputPortsSection: View {
  @Bindable var model: CardEditorModel
  @State private var types: [String] = []    // tables of the schema (fully qualified), the catalog excluded
  @State private var error: String?

  var body: some View {
    let r = model.session.resolution[model.cardKey]
    Section {
      ForEach(ports(r), id: \.name) { p in
        HStack {
          Text(p.name).font(.body.monospaced()).frame(maxWidth: 120, alignment: .leading)
          TextField("用途", text: portNote(model, "outputs", p.name), prompt: Text("用途（何を出すか）"))
            .textFieldStyle(.roundedBorder).labelsHidden()
          if types.isEmpty || model.outputsPath == nil {
            Text((p.rootType ?? String(localized: "未解決")) + (p.optional ? String(localized: "（省略可）") : ""))
              .font(.caption).foregroundStyle(.secondary)
          } else {
            Picker("型", selection: Binding(get: { p.rootType ?? "" }, set: { set(p.name, type: $0, kind: p.kind) })) {
              if p.rootType == nil { Text("未解決").tag("") }
              ForEach(types, id: \.self) { Text(short($0)).tag($0).help($0) }
            }
            .labelsHidden().frame(maxWidth: 150)
            .help("_outputs.fbs で定義した型から選びます（_outputs.fbs に書き戻します）")
            Picker("種類", selection: Binding(get: { p.kind }, set: { set(p.name, type: p.rootType ?? "", kind: $0) })) {
              Text("値").tag(Tsugumi_Data_PortKind.flatbuffer)
              Text("File").tag(Tsugumi_Data_PortKind.file)
              Text("Directory").tag(Tsugumi_Data_PortKind.directory)
            }
            .labelsHidden().frame(maxWidth: 100)
            .help("File / Directory は、型に文字列の value（パス）がある場合に選べます")
          }
        }
      }
      if ports(r).isEmpty { Text("出力はありません").foregroundStyle(.secondary) }
      if let error { Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
    } header: { Text("出力ポート") } footer: {
      Text("出力ポートと型の正本は _outputs.fbs です（型はここで選べます。型そのものはソースで定義します）。").font(.caption).foregroundStyle(.secondary)
    }
    .task(id: "\(model.outputsPath ?? "")#\(model.generation)#\(model.loadedText(model.outputsPath ?? "")?.hashValue ?? 0)") { await loadTypes() }
  }

  private func ports(_ r: ResolvedCard?) -> [PortInfo] { r?.outputs ?? [] }

  private func short(_ full: String) -> String { full.split(separator: ".").last.map(String.init) ?? full }

  private func loadTypes() async {
    guard let p = model.outputsPath, let rpc = model.session.rpc, model.session.sources[p] != nil || model.loadedText(p) != nil,
          let text = try? await model.text(p) else { types = []; return }
    do {
      let res = try await rpc.outputTypes(text: text)
      let schema = try BFBS(data: Data(res.bfbs))
      types = schema.objects.filter { !$0.isStruct && $0.name != res.rootType }.map(\.name).sorted()
      error = res.error
    } catch {
      types = []
    }
  }

  /// Rewrites the port in the working copy of _outputs.fbs (applied with the rest).
  private func set(_ port: String, type: String, kind: Tsugumi_Data_PortKind) {
    guard !type.isEmpty, let p = model.outputsPath, let rpc = model.session.rpc else { return }
    Task {
      do {
        let text = try await model.text(p)
        let res = try await rpc.outputTypes(text: text, port: port, typeName: type, kind: kind)
        if let changed = res.text {
          model.texts[p] = changed
          error = nil
        } else {
          error = res.error ?? String(localized: "変更できませんでした")
        }
      } catch {
        self.error = "\(error)"
      }
    }
  }
}

/// Inputs and outputs of a Card in Card: its external project's boundary
/// (each outer port maps to a port of a Card inside).
private struct BoundaryCategory: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    let b = model.session.boundary(model.cardKey)
    Form {
      side(b?["inputs"]?.object ?? [], title: "入力ポート", output: false)
      side(b?["outputs"]?.object ?? [], title: "出力ポート", output: true)
      Section {
        LabeledContent("external", value: model.session.cardExternal(model.cardKey) ?? "")
        Text("Card in Card は externals にあるこのプロジェクト用のコピーです。ポートの対応はその tsugumi.json の boundary にあります。中に入ると Card と接続を編集できます。")
          .font(.caption).foregroundStyle(.secondary)
      }
    }
    .formStyle(.grouped)
  }

  private func side(_ entries: [(String, JSONValue)], title: LocalizedStringKey, output: Bool) -> some View {
    Section(title) {
      if entries.isEmpty { Text("ありません").foregroundStyle(.secondary) }
      ForEach(entries.indices, id: \.self) { i in
        let (name, ep) = entries[i]
        HStack {
          Text(name).font(.body.monospaced()).frame(maxWidth: 140, alignment: .leading)
          Image(systemName: output ? "arrow.left" : "arrow.right").foregroundStyle(.secondary)
          Text("\(ep["card"]?.string ?? "?").\(ep["port"]?.string ?? "?")").font(.callout.monospaced()).foregroundStyle(.secondary)
        }
      }
    }
  }
}

/// Free text describing intent and notes, kept in `_meta.json` (`context`).
private struct ContextCategory: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("処理の意図や補足を文章で残します（_meta.json）。チャットで Card を参照したときにも使われます。")
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

  private var prefix: String { model.session.definitionDir(model.cardKey) ?? "" }

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
                Text("生成").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                  .padding(.leading, 10).padding(.top, 12).padding(.bottom, 2)
                ForEach(["_inputs.fbs", "_params.fbs"], id: \.self) { name in
                  FileRow(name: name, icon: "lock.doc", depth: 0, folder: nil,
                          selected: model.shownFile == Self.generatedPrefix + name, readOnly: true) { open(Self.generatedPrefix + name) }
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
      if generatedFile { ReadOnlyMark(why: String(localized: "接続から生成されるファイルです（手で編集しません）")) }
      if model.created.contains(path) { Text("新規").font(.caption2).foregroundStyle(.orange) }
      else if model.isEdited(path) { Text("未適用").font(.caption2).foregroundStyle(.orange) }
      Spacer()
      if let formatMessage { Text(formatMessage).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
      if !generatedFile {
        Button("整形") { format(path) }.disabled(formatting || model.loadedText(path) == nil).controlSize(.small)
          .shortcut(.formatSource)
          .help(tip("ソースコードを整形", .formatSource))
      }
    }
    .padding(.horizontal, 10).padding(.vertical, 6)
    // The window's own background: the header continues the title bar above it.
    .background(Color(nsColor: .windowBackgroundColor))
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
  var readOnly = false
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
      if readOnly { ReadOnlyMark(why: String(localized: "接続から生成されるファイルです（手で編集しません）")) }
      if edited { Circle().fill(Color.orange).frame(width: 6, height: 6) }
    }
    .padding(.leading, 8 + CGFloat(depth) * 14).padding(.trailing, 8).padding(.vertical, 3)
    .background(RoundedRectangle(cornerRadius: 5).fill(selected ? Color.accentColor.opacity(0.25) : .clear).padding(.horizontal, 4))
    .contentShape(Rectangle())
    .onTapGesture(perform: action)
    .accessibilityAddTraits(selected ? [.isSelected, .isButton] : .isButton)
  }
}

/// A short "RO" mark for files that are not edited here; hover tells why.
struct ReadOnlyMark: View {
  let why: String
  var body: some View {
    Text("RO")
      .font(.system(size: 9, weight: .semibold, design: .monospaced))
      .padding(.horizontal, 4).padding(.vertical, 1)
      .foregroundStyle(.secondary)
      .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(.secondary.opacity(0.5)))
      .help(why)
      .accessibilityLabel(Text("読み取り専用"))
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
