import AppKit
import SwiftUI
import TsugumiAgentClient
import TsugumiRPC

/// Local working copy of one Card's settings (its `_card.json`), metadata
/// and sources. Nothing reaches the project until "Apply". Every Card has its
/// own definition folder, so what is edited here is this Card only (inside a
/// Card in Card: the Card of the external, this project's copy).
///
/// The copy follows what changes elsewhere while it is open (the Agent,
/// the canvas): settings and files not touched here take the new content;
/// those changed on both sides require an explicit overwrite or reload choice.
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
  var renames: [String: String] = [:]
  private var renameDestinations: [String: String] = [:]
  var message: String?
  var applying = false
  let revisionEdit = RevisionEdit()
  /// Settings and files changed both here and elsewhere since the copy was taken.
  var conflicts: [String] = []
  /// Category shown in the window (kept with the model, so edits and the
  /// place survive switching).
  var category: CardEditorCategory = .general
  private var baseDefinition: JSONValue?
  private var baseMeta: JSONValue?
  private var baseTexts: [String: String] = [:]
  private var presetFiles: [String: String] = [:]
  private var baseHashes: [String: String] = [:]   // source hash when each file was read

  init(session: ProjectSession, cardKey: CardKey) {
    self.session = session
    self.cardKey = cardKey
    reload()
  }

  /// The Card's definition folder name (its id).
  var definitionId: String? { path(cardKey).last }
  var kind: CardKind {
    session.kind(cardKey) == .subgraph ? .subgraph : definitionKind(definition)
  }
  var exists: Bool { session.instance(cardKey) != nil }
  private var prefix: String { session.definitionDir(cardKey) ?? "" }

  var unapplied: Bool {
    definition != baseDefinition || meta != baseMeta || !removed.isEmpty || !created.isEmpty ||
      texts.contains { baseTexts[$0.key] != $0.value }
  }

  var rebindings: [String: String] = [:]
  struct DraftEnd: Identifiable {
    var id: String
    var label: String
    var side: String
    var index: Int?
    var boundary: String?
  }
  /// Incident wires and boundary ends. No port is selected implicitly.
  var draftEnds: [DraftEnd] {
    guard session.kind(cardKey) == .draft, kind != .draft, let id = path(cardKey).last else { return [] }
    let scope = Array(path(cardKey).dropLast())
    var ends: [DraftEnd] = []
    for (index, c) in session.connections(in: scope).enumerated() {
      for side in ["from", "to"] where c[side]?["card"]?.string == id {
        let label = "\(c["from"]?["card"]?.string ?? "").\(c["from"]?["port"]?.string ?? "") → \(c["to"]?["card"]?.string ?? "").\(c["to"]?["port"]?.string ?? "")"
        ends.append(DraftEnd(id: "\(index):\(side):\(c.serialized(pretty: false))", label: label, side: side, index: index))
      }
    }
    let boundary = scope.isEmpty ? nil : session.boundary(key(scope))
    for side in ["inputs", "outputs"] {
      for (name, ep) in boundary?[side]?.object ?? [] where ep["card"]?.string == id {
        ends.append(DraftEnd(id: "\(side):\(name):\(ep.serialized(pretty: false))", label: "\(side).\(name) → \(id)", side: side, boundary: name))
      }
    }
    return ends
  }

  var implementation: String {
    let kind = definitionKind(definition)
    if kind == .input, presetFiles[prefix + "params.nix"] == Self.inputParameters(boolean: true) ||
      session.resolution[cardKey]?.parameters.contains(where: { $0.name == "value" && $0.type == "bool" }) == true {
      return DefinitionTemplate.booleanInput.rawValue
    }
    return kind.rawValue
  }

  func implementAs(_ value: String) {
    if value == "input" || value == DefinitionTemplate.booleanInput.rawValue {
      let boolean = value == DefinitionTemplate.booleanInput.rawValue
      let type = boolean ? "bool" : "double"
      definition = .object([("type", .string("input"))])
      preset("_outputs.fbs", "namespace card;\ntable Value { value:\(type); }\ntable Outputs { value:Value (required); }\nroot_type Outputs;\n")
      preset("params.nix", Self.inputParameters(boolean: boolean))
    } else {
      definition = .object([("type", .string(value))])
      for (file, text) in presetFiles where created.contains(file) && texts[file] == text { removeFile(file) }
      presetFiles = [:]
    }
    if value == "merge" { definition?["inputs"] = .array([.object([("name", .string("a"))]), .object([("name", .string("b"))])]) }
  }

  private static func inputParameters(boolean: Bool) -> String {
    "{ value = \(boolean ? "false" : "0.0"); }\n"
  }

  private func preset(_ relative: String, _ text: String) {
    let file = prefix + relative
    guard !prefix.isEmpty, session.sources[file] == nil, !removed.contains(file) else { return }
    let untouched = created.contains(file) && presetFiles[file] != nil && texts[file] == presetFiles[file]
    guard untouched || (!created.contains(file) && texts[file] == nil) else { return }
    createFile(relative, text: text)
    presetFiles[file] = text
  }

  /// Source files of the definition (documents excluded), with local changes.
  var files: [String] {
    guard !prefix.isEmpty else { return [] }
    let docs: Set<String> = [prefix + "_card.json", prefix + "README.md", prefix + "_comments.json"]
    return Set(session.sourcePaths.filter { $0.hasPrefix(prefix) && !docs.contains($0) }).union(created).subtracting(removed).sorted()
  }

  /// Changes whenever what this copy follows changes (the settings, the
  /// files' hashes).
  var followToken: String {
    let files = session.sourcePaths.filter { $0.hasPrefix(prefix) }.map { "\($0)=\(session.sources[$0]?.hash ?? "-")" }
    return [String(session.rev), session.cardDefinition(cardKey)?.serialized() ?? "", session.cardMeta(cardKey)?.serialized() ?? ""]
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
    presetFiles = [:]
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
    renames = [:]
    renameDestinations = [:]
    rebindings = [:]
    message = nil
    conflicts = []
    revisionEdit.reload(session.rev)
  }

  /// Untouched editors follow confirmed state; working copies are never merged locally.
  func follow(whileApplying: Bool = false) async {
    if (!applying || whileApplying) && !unapplied { reload() }
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

  func openSource(_ source: String, line: Int) {
    guard let prefix = session.definitionDir(cardKey), line > 0, !source.hasPrefix("/"),
          !source.split(separator: "/").contains(where: { $0 == ".." || $0 == "." }) else { return }
    category = .source
    openFile(prefix + source, line: line)
  }

  /// The Nix definition (default.nix), which Source edits.
  func openDefinition() {
    guard let prefix = session.definitionDir(cardKey) else { return }
    category = .source
    openFile(prefix + "default.nix")
  }

  /// The run and invocation the Log category shows; nil follows this Card's latest.
  var logSelection: CardLogSelection?

  func openLog(runId: String, invocation: UInt64?) {
    logSelection = CardLogSelection(runId: runId, invocation: invocation)
    category = .log
  }

  private(set) var sourceLine: Int?

  func openFile(_ p: String, line: Int? = nil) {
    if !openFiles.contains(p) { openFiles.append(p) }
    shownFile = p
    sourceLine = line
  }

  func closeFile(_ p: String) {
    guard let i = openFiles.firstIndex(of: p) else { return }
    openFiles.remove(at: i)
    if shownFile == p {
      shownFile = openFiles.isEmpty ? nil : openFiles[min(i, openFiles.count - 1)]
      sourceLine = nil
    }
  }

  /// Reads a source file once (later calls return the working copy).
  /// Throws when it cannot be read; nothing is recorded as its content then.
  func text(_ p: String) async throws -> String {
    if let t = texts[p] { return t }
    loads[p] = .loading
    let source = renames[p] ?? p
    let hash = session.sources[source]?.hash
    do {
      let t = try await session.sourceText(source)
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
    if !created.contains(p) { presetFiles[p] = nil }
    removed.remove(p)
    created.insert(p)
    if let hash = session.sources[p]?.hash { baseHashes[p] = hash }
    texts[p] = text
    loads[p] = .loaded
  }

  /// Stage a guarded move without decoding the source bytes.
  @discardableResult
  func rename(_ p: String, to rel: String) async -> Bool {
    let target = prefix + rel
    if target == p { return true }
    presetFiles[p] = nil
    presetFiles[target] = nil
    let origin = renames[p] ?? p
    let hash = baseHashes[p] ?? session.sources[origin]?.hash
    let text = texts[p], base = baseTexts[p], load = loads[p]
    let fresh = created.contains(p) && renames[p] == nil
    created.remove(p); renames[p] = nil; renameDestinations[p] = nil
    texts[p] = nil; baseTexts[p] = nil; loads[p] = nil
    if !fresh { removed.insert(origin) }
    if target == origin { removed.remove(origin) }
    else { created.insert(target); if !fresh { renames[target] = origin } }
    if fresh { created.insert(target) }
    texts[target] = text; baseTexts[target] = base; loads[target] = load
    baseHashes[target] = hash; baseHashes[origin] = hash
    renameDestinations[target] = session.sources[target]?.hash
    return true
  }

  func removeFile(_ p: String) {
    renames[p] = nil
    renameDestinations[p] = nil
    if baseHashes[p] == nil, let hash = session.sources[p]?.hash { baseHashes[p] = hash }
    if created.contains(p) { created.remove(p) } else { removed.insert(p) }
    texts[p] = nil
    loads[p] = nil
  }

  var recoveryText: String {
    "_card.json\n" + (definition?.serialized() ?? "") + "\nREADME.md\n" + (Documents.splitCardNotes(meta).readme ?? "") +
      texts.sorted(by: { $0.key < $1.key }).map { "\n\($0.key)\n\($0.value)" }.joined() +
      removed.sorted().map { "\n- \($0)" }.joined()
  }

  /// Copies the working copy into the project as one undoable edit (saved at once).
  func apply(overwrite: Bool = false) async -> Bool {
    guard !applying else { return false }
    guard session.isEditable else {
      message = String(localized: "実行中は Card を変更できません。実行が終わってから適用してください。")
      return false
    }
    applying = true
    defer { applying = false }
    let ends = draftEnds, bindings = rebindings
    guard !ends.contains(where: { bindings[$0.id]?.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ?? true }) else {
      message = String(localized: "各 wire の実ポート名を指定してください（ポート欄）。")
      return false
    }
    let def = definition, m = meta, k = cardKey
    let sentTexts = texts, sentCreated = created
    let moves = renames, destinations = renameDestinations
    let changedTexts = texts.filter { baseTexts[$0.key] != $0.value || created.contains($0.key) && moves[$0.key] == nil }
    let gone = removed.subtracting(moves.keys).subtracting(moves.values)
    let bases = baseHashes
    let ok = await revisionEdit.apply(session) { s in
      var edits: [Tsugumi_Host_ProjectEditT] = []
      if let def { edits.append(s.jsonEdit(prefix + "_card.json", old: revisionEdit.jsonBase(prefix + "_card.json", initial: baseDefinition, overwrite: overwrite), new: def)) }
      edits.append(contentsOf: s.notesEdits(prefix, old: revisionEdit.jsonBase(prefix + "#notes", initial: baseMeta, overwrite: overwrite), new: m))
      for (to, from) in moves.sorted(by: { $0.key < $1.key }) {
        let move = Tsugumi_Host_ProjectFileMoveT()
        move.filePath = from; move.to = to
        move.expectedHash = revisionEdit.hashBase(from, initial: bases[to], overwrite: overwrite) ?? ""
        move.expectedDestinationHash = revisionEdit.hashBase(to, initial: destinations[to], overwrite: overwrite); move.destinationMissing = move.expectedDestinationHash == nil
        let edit = Tsugumi_Host_ProjectEditT()
        edit.value = Tsugumi_Host_ProjectEditValueUnion(move, type: .projectfilemove)
        edits.append(edit)
      }
      for (p, text) in changedTexts {
        let hash = revisionEdit.hashBase(moves[p] ?? p, initial: bases[p], overwrite: overwrite)
        let put = Tsugumi_Host_ProjectFilePutT()
        put.filePath = p; put.expectedHash = hash; put.expectMissing = hash == nil; put.bytes = Array(text.utf8)
        let edit = Tsugumi_Host_ProjectEditT()
        edit.value = Tsugumi_Host_ProjectEditValueUnion(put, type: .projectfileput)
        edits.append(edit)
      }
      for p in gone {
        let remove = Tsugumi_Host_ProjectFileRemoveT()
        remove.filePath = p; remove.expectedHash = revisionEdit.hashBase(p, initial: bases[p], overwrite: overwrite) ?? ""
        let edit = Tsugumi_Host_ProjectEditT()
        edit.value = Tsugumi_Host_ProjectEditValueUnion(remove, type: .projectfileremove)
        edits.append(edit)
      }
      s.appendEdits(edits)
      let scope = Array(path(k).dropLast())
      if ends.contains(where: { $0.index != nil }) {
        s.updateGraph(scope) { g in
          var conns = g["connections"]?.array ?? []
          for e in ends {
            if let i = e.index { conns[i][e.side]?["port"] = .string(bindings[e.id]!.trimmingCharacters(in: .whitespacesAndNewlines)) }
          }
          g["connections"] = .array(conns)
        }
      }
      if ends.contains(where: { $0.boundary != nil }), let prefix = s.projectPrefix(scope) {
        s.updateExternal(prefix) { ext in
          for e in ends {
            if let name = e.boundary, var ports = ext.project["boundary"]?[e.side]?.array,
               let i = ports.firstIndex(where: { $0["name"]?.string == name }) {
              ports[i]["port"] = .string(bindings[e.id]!.trimmingCharacters(in: .whitespacesAndNewlines))
              ext.project["boundary"]?[e.side] = .array(ports)
            }
          }
        }
      }

    }
    guard ok else { return false }
    message = String(localized: "適用しました。")
    let laterDef = definition, laterMeta = meta
    let laterMoves = renames, laterDestinations = renameDestinations
    let laterTexts = texts, laterRemoved = removed, laterCreated = created
    let savedCurrent = revisionEdit.savedRev == session.rev
    reload()
    if laterDef != def { definition = laterDef; baseDefinition = def }
    if laterMeta != m { meta = laterMeta; baseMeta = m }
    for (p, t) in laterTexts {
      let sent = sentTexts[p]
      if savedCurrent || t != sent {
        texts[p] = t
        baseTexts[p] = sent
        loads[p] = .loaded
        baseHashes[p] = changedTexts[p] != nil ? sent.map { sha256Hex(Data($0.utf8)) } : bases[p]
      }
    }
    removed = laterRemoved.subtracting(gone).subtracting(moves.values)
    for (to, from) in laterMoves where moves[to] != from {
      let savedFrom = moves.first { $0.value == from }?.key ?? from
      renames[to] = savedFrom; renameDestinations[to] = laterDestinations[to]; removed.insert(savedFrom)
      baseHashes[to] = session.sources[savedFrom]?.hash ?? bases[to]
    }
    for (to, from) in moves where !laterMoves.values.contains(from) && !laterCreated.contains(to) { removed.insert(to) }
    created = laterCreated.subtracting(sentCreated)
    for p in sentTexts.keys where laterTexts[p] == nil && !gone.contains(p) { removed.insert(p) }
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
      if let model, model.exists || model.unapplied {
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
        if ref.file == "default.nix" { m.openDefinition() }
        model = m
        UIScript.shared.editors[ref.card] = m
      }
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
    case .input: return [.general, .source, .ports, .log, .executions, .artifacts, .notes]
    case .draft, .invalid: return [.general, .notes]
    case .subgraph: return [.general, .ports, .executions, .notes]
    default: return [.general, .ports, .log, .executions, .artifacts, .notes]
    }
  }

  private func content(_ m: CardEditorModel) -> some View {
    HStack(spacing: 0) {
      SettingsSidebar(items: categories(m), selection: Binding(get: { m.category }, set: { m.category = $0 }),
                      title: \.title, symbol: \.symbol, width: 160)
      Divider()
      VStack(spacing: 0) {
        // The preview belongs to General; editors, ports and history use the whole height.
        if m.category == .general {
          CardPreview(session: session, model: m)
            .frame(minHeight: 120, idealHeight: 170, maxHeight: 220)
          Divider()
        }
        Group {
          switch m.category {
          case .general: GeneralCategory(model: m)
          case .source: SourceCategory(model: m)
          case .ports: m.kind == .subgraph ? AnyView(BoundaryCategory(model: m)) : AnyView(PortsCategory(model: m))
          case .log: CardLogView(session: session, model: m)
          case .dependencies: DependenciesCategory(model: m)
          case .executions: CardExecutionView(session: session, model: m)
          case .artifacts: CardArtifactsView(session: session, cardKey: ref.card)
          case .notes: CommentsCategory(session: m.session, card: m.cardKey)
          }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        RevisionEditNotice(edit: m.revisionEdit, text: { m.recoveryText },
                           overwrite: { Task { _ = await m.apply(overwrite: true) } }, reload: { m.reload() })
          .padding(.horizontal, 8)
        Divider()
        footer(m)
      }
    }
    // Resizable: wider windows give the editor and lists the room; smaller
    // ones scroll (each category scrolls on its own).
    .frame(minWidth: 620, maxWidth: .infinity, minHeight: 420, maxHeight: .infinity)
    // What changes elsewhere while the window is open is taken in at once;
    // and when this first shows: a window restored at launch took its copy
    // before the project was read.
    .onChange(of: m.followToken, initial: true) { Task { await m.follow() } }
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
        .guideTarget("apply")
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
    let data = session.nodeData(model.cardKey, definition: model.definition, notes: model.meta,
                                collapsed: collapsed, outputs: outputs)
    if items.isEmpty {
      items = [CanvasItem(id: model.cardKey, data: data, origin: .zero)]
    } else {
      items[0].data = data
    }
  }
}

// MARK: - Categories

/// Id (the Card's only name and its folder), description, params, when it
/// runs, and the worker's permissions.
private struct GeneralCategory: View {
  @Bindable var model: CardEditorModel

  private var remote: Bool {
    { let id = model.definition?["executionService"]?.string ?? "auto"; return id != "local" && id != "auto" }()
  }

  var body: some View {
    Form {
      Section {
        LabeledContent("ID", value: model.session.displayName(model.cardKey))
        LabeledContent("種類", value: model.kind.label)
        LabeledContent("アイコン") {
          SymbolPicker(symbol: Binding(get: { model.definition?["symbol"]?.string.flatMap { $0.isEmpty ? nil : $0 } },
                                       set: { model.definition?["symbol"] = $0.map { .string($0) } }),
                       inherited: model.kind.symbol)
        }
        TextField("概要", text: metaString("description"), axis: .vertical)
          .lineLimit(2...5)
        if model.kind == .draft {
          TextField("内容", text: metaString("content"), prompt: Text("何を受け取り、何を出し、どう処理するかを書きます"), axis: .vertical)
            .lineLimit(6...16)
        }
      }
      if model.session.kind(model.cardKey) == .draft {
        Section {
          Picker("種類", selection: Binding(get: { model.implementation }, set: { model.implementAs($0) })) {
            ForEach(["draft", "code", "input",
                     DefinitionTemplate.booleanInput.rawValue, "trigger", "merge", "collect"], id: \.self) { value in
              if value == "code" {
                Text("コード").tag(value)
              } else {
                Text(value == DefinitionTemplate.booleanInput.rawValue ? DefinitionTemplate.booleanInput.label : value).tag(value)
              }
            }
          }
        } header: { Text("Draft を手動で実装") }
          footer: { Text("定義と wire の実ポート名を同じ適用で保存します。コード Card はソースと出力スキーマも作成してください。") }
      }
      if definitionKind(model.definition) == model.session.kind(model.cardKey) {
        let parameters = model.session.resolution[model.cardKey]?.parameters ?? []
        if !parameters.isEmpty {
          Section("パラメータ") {
            ForEach(parameters) { parameter in ParamRow(model: model, parameter: parameter) }
          }
        }
      }
      if model.kind != .subgraph && model.kind != .draft && model.kind != .invalid {
        Section("実行") {
          numberField("maxRuns", label: "最大発火回数", fallback: "1")
            .help("実行リスト内の呼び出しはまとめて 1 回の発火として数えます。")
          timeoutField("timeoutSeconds", label: "実行の期限（秒）")
          if model.kind == .code {
            timeoutField("buildTimeoutSeconds", label: "ビルドの期限（秒）")
            executionSettings
            toggle("reuseWorker", label: "worker の状態を残す", fallback: false)
              .help("オン: 読み込んだ worker を次の呼び出しや次の実行でも使い、状態（static 変数など）を残します。オフ: 呼び出しごとに新しい worker です。")
              .disabled(remote && model.definition?["reuseWorker"]?.bool != true)
            if remote && model.definition?["reuseWorker"]?.bool == true {
              Label("外部の実行サービスでは worker の状態を残せません。", systemImage: "exclamationmark.triangle")
                .font(.caption)
            }
            nestedToggle("sandbox", "filesystem", label: "ファイルアクセスを制限", fallback: true)
              .disabled(remote && model.definition?["sandbox"]?["filesystem"]?.bool != false)
            nestedToggle("sandbox", "pid", label: "PID 名前空間を分離", fallback: true)
            nestedToggle("sandbox", "ipc", label: "IPC 名前空間を分離", fallback: true)
            nestedToggle("sandbox", "identity", label: "実行ユーザーと権限を分離", fallback: true)
            if remote && model.definition?["sandbox"]?["filesystem"]?.bool == false {
              Label("外部の実行サービスにはファイルアクセスの制限が必要です。", systemImage: "exclamationmark.triangle")
                .font(.caption)
            }
          }
        }
        if model.kind == .code {
          Section {
            nestedToggle("grants", "network", label: "ネットワークへのアクセスを許可する", fallback: false)
            nestedToggle("grants", "gui", label: "GUI へのアクセスを許可する", fallback: false)
            HostFoldersEditor(model: model, remote: remote)
          } header: {
            Text("アクセス許可")
          }
        }
      }
      let diags = model.session.diagnostics.filter { $0.card.map(key) == model.cardKey }
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

  @ViewBuilder private var executionSettings: some View {
    let services = model.session.executionServices
    let serviceSetting = string("executionService", fallback: "auto")
    let serviceId = serviceSetting.wrappedValue
    let service = services.first { $0.id == serviceId }
    let resourceLimits = serviceId == "auto" || service?.resourceLimits == true
    let nativeSystem = service?.nativeSystem ?? ""
    let systems = service?.systems.compactMap { $0 } ?? []
    let system = string("system", fallback: "")
    let selected = system.wrappedValue

    TextField("ワーカーの条件式", text: Binding(
      get: { model.definition?["workerCondition"]?["expression"]?.string ?? "" },
      set: { value in
        var condition = JSONValue.object([])
        condition["language"] = .string("simple")
        condition["expression"] = .string(value)
        model.definition?["workerCondition"] = value.trimmingCharacters(in: .whitespaces).isEmpty ? nil : condition
      }))
      .help("x はワーカーの grade です。例: x > 1、x & 0x10 != 0。空欄はすべての値を許可します。")
    if !services.isEmpty {
      Picker("実行サービス", selection: serviceSetting) {
        Text("条件から自動選択").tag("auto")
        ForEach(services, id: \.id) { entry in
          Text(verbatim: serviceLabel(entry)).tag(entry.id).disabled(!entry.available)
        }
        if service == nil && serviceId != "auto" { Text(verbatim: serviceId).tag(serviceId).disabled(true) }
      }
    } else {
      LabeledContent("実行サービス", value: service.map(serviceLabel) ?? String(localized: "情報なし"))
    }
    if !model.session.identity.supportsExternalExecution {
      Label("このプロジェクトのフォルダで作成時刻を取得できないため、外部の実行サービスを使用できません。", systemImage: "exclamationmark.triangle")
        .font(.caption)
    }
    if serviceId != "local" && serviceId != "auto" && service?.available != true {
      Label("選択した実行サービスに接続できません。", systemImage: "exclamationmark.triangle").font(.caption)
    }
    Picker("CPU アーキテクチャ", selection: system) {
      if nativeSystem.isEmpty {
        Text("この実行環境の既定").tag("")
      } else {
        Text("この実行環境の既定（\(nativeSystem)）").tag("")
      }
      ForEach(systems, id: \.self) { value in
        Text(verbatim: value).tag(value)
      }
      if !selected.isEmpty && !systems.contains(selected) {
        Text(verbatim: selected).tag(selected).disabled(true)
      }
    }
    .help("選択した実行サービスで利用できる CPU アーキテクチャを選びます。")
    if service != nil && !selected.isEmpty && !systems.contains(selected) {
      Label("この実行環境では \(selected) を実行できません。", systemImage: "exclamationmark.triangle")
        .font(.caption)
    }
    if resourceLimits {
      Text("CPU とメモリの上限を指定します。実行先のリソースは他のジョブと共有します。").font(.caption).foregroundStyle(.secondary)
      executionResource("cpu", label: "CPU 上限（コア）", example: "0.5")
      executionResource("memoryMiB", label: "メモリ上限（MiB）", example: "1024")
    }
    if model.definition?["sandbox"]?["limits"] != nil {
      if !resourceLimits {
        Label("この実行サービスではリソースを指定できません。", systemImage: "exclamationmark.triangle").font(.caption)
      }
      Button("リソース指定を削除") { setNested("sandbox", "limits", nil) }
    }
  }

  private func serviceLabel(_ service: Tsugumi_RPC_ExecutionServiceT) -> String {
    if service.id != "local" {
      return service.available ? service.id : String(localized: "\(service.id)（接続未確認）")
    }
    if service.nativeSystem.hasSuffix("-linux") { return "Linux (\(service.nativeSystem))" }
    if service.nativeSystem.hasSuffix("-darwin") { return "macOS (\(service.nativeSystem))" }
    return service.id
  }

  private func setNested(_ group: String, _ key: String, _ value: JSONValue?) {
    var settings = JSONValue.object(model.definition?[group]?.object ?? [])
    settings[key] = value
    model.definition?[group] = settings.object?.isEmpty == false ? settings : nil
  }

  private func nestedToggle(_ group: String, _ key: String, label: LocalizedStringKey, fallback: Bool) -> some View {
    Toggle(label, isOn: Binding(
      get: { model.definition?[group]?[key]?.bool ?? fallback },
      set: { setNested(group, key, $0 == fallback ? nil : .bool($0)) }))
  }

  private func executionResource(_ name: String, label: LocalizedStringKey, example: String) -> some View {
    TextField(label, text: Binding(
      get: {
        let value = model.definition?["sandbox"]?["limits"]?[name]
        return value?.numberText ?? value?.string ?? ""
      },
      set: { value in
        let text = value.trimmingCharacters(in: .whitespaces)
        var resources = JSONValue.object(model.definition?["sandbox"]?["limits"]?.object ?? [])
        let number = name == "cpu" ? Double(text).flatMap { $0.isFinite ? String($0) : nil } : UInt64(text).map { String($0) }
        resources[name] = text.isEmpty ? nil : (number.map { .number($0) } ?? .string(text))
        setNested("sandbox", "limits", resources.object?.isEmpty == false ? resources : nil)
      }), prompt: Text(example))
  }

  private func string(_ name: String, fallback: String? = nil) -> Binding<String> {
    Binding(get: { model.definition?[name]?.string ?? fallback ?? "" },
            set: { v in model.definition?[name] = v.isEmpty || v == fallback ? nil : .string(v) })
  }

  /// A Card's intent is in its README.md.
  private func metaString(_ name: String) -> Binding<String> {
    Binding(get: { model.meta?[name]?.string ?? "" },
            set: { v in
              var meta = model.meta ?? .object([])
              meta[name] = v.isEmpty ? nil : .string(v)
              model.meta = meta
            })
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
  let remote: Bool

  var body: some View {
    let folders = model.definition?["grants"]?["hostPaths"]?.array ?? []
    VStack(alignment: .leading, spacing: 6) {
      HStack {
        Text("ホストのフォルダへのアクセスを許可する")
        Spacer()
        Button("フォルダを追加…") { add() }.disabled(remote)
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
            Text(remote ? String(localized: "外部の実行サービスにはホストのフォルダを共有できません。")
                        : present ? "TSUGUMI_GRANT_\(grantName(f["name"]?.string ?? ""))" : String(localized: "この Mac にはありません（実行すると失敗します）"))
              .font(.caption).foregroundStyle(present ? Color.secondary : Color.orange)
          }
          Spacer()
          Toggle("書き込み", isOn: Binding(get: { f["write"]?.bool ?? false }, set: { v in
            edit { $0[i]["write"] = v ? .bool(true) : nil }
          }))
          .toggleStyle(.checkbox)
          .disabled(remote)
          .help("オフ（デフォルト）: 読み取り専用")
          Button { edit { $0.remove(at: i) } } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless)
        }
      }
      Text(remote ? String(localized: "外部の実行サービスにはホストのフォルダを共有できません。")
                  : String(localized: "Card は TSUGUMI_GRANT_<名前> からフォルダのパスを取得できます。共有 VM での追加・変更はプロジェクトを開き直すと反映されます。"))
        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
    }
  }

  private func edit(_ fn: (inout [JSONValue]) -> Void) {
    var list = model.definition?["grants"]?["hostPaths"]?.array ?? []
    fn(&list)
    var grants = JSONValue.object(model.definition?["grants"]?.object ?? [])
    grants["hostPaths"] = list.isEmpty ? nil : .array(list)
    model.definition?["grants"] = grants.object?.isEmpty == false ? grants : nil
  }

  private func grantName(_ name: String) -> String {
    String(name.uppercased().map { $0.isASCII && ($0.isLetter || $0.isNumber) ? $0 : "_" })
  }

  private func add() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.allowsMultipleSelection = false
    guard panel.runModal() == .OK, let url = panel.url else { return }
    let taken = Set((model.definition?["grants"]?["hostPaths"]?.array ?? []).compactMap { $0["name"]?.string }.map(grantName))
    let base = String(url.lastPathComponent.map { $0.isASCII && ($0.isLetter || $0.isNumber || "-_.".contains($0)) ? $0 : "-" })
    var name = base.isEmpty || base == "." || base == ".." ? "folder" : base
    var n = 2
    while taken.contains(grantName(name)) { name = "\(base)-\(n)"; n += 1 }
    edit { $0.append(.object([("path", .string(url.path)), ("name", .string(name))])) }
  }
}

private struct ParamRow: View {
  @Bindable var model: CardEditorModel
  let parameter: CardParameter
  @State private var draft: String?
  @State private var edit = InlineParamEdit()

  private var session: ProjectSession { model.session }
  private var name: String { parameter.name }
  private var disabled: Bool {
    return !session.isEditable || edit.revision.applying || model.applying
  }

  var body: some View {
    let value = parameter.effectiveValue
    VStack(alignment: .leading, spacing: 4) {
      HStack {
        if parameter.type == "bool" {
          Toggle(name, isOn: Binding(get: { draft.map { $0 == "true" } ?? value.bool ?? false }, set: {
            begin()
            draft = $0 ? "true" : "false"
            save()
          }))
        } else {
          TextField(name, text: Binding(get: { draft ?? parameterText(value, type: parameter.type) }, set: {
            begin()
            draft = $0
          }))
          .onSubmit { if draft != nil { save() } }
          if draft != nil { Button("保存") { save() } }
        }
        Text(verbatim: parameter.type).font(.caption2).foregroundStyle(.secondary)
      }
      .guideTarget("param:\(name)")
      .disabled(disabled)
      RevisionEditNotice(edit: edit.revision, text: { draft ?? "" },
                         overwrite: { save(overwrite: true) }, reload: clear)
    }
  }

  private func begin() {
    if draft == nil { edit.begin(parameterText(parameter.effectiveValue, type: parameter.type), rev: session.rev) }
  }

  private func clear() {
    draft = nil
    edit.begin(parameterText(parameter.effectiveValue, type: parameter.type), rev: session.rev)
  }

  private func save(overwrite: Bool = false) {
    guard let draft else { return }
    let literal = parameterInputLiteral(draft, parameter.type)
    Task { if await edit.commitParam(session, card: model.cardKey, name: name, literal: literal, overwrite: overwrite) { clear() } }
  }
}

/// Ports, their purposes and recording.
private struct PortsCategory: View {
  @Bindable var model: CardEditorModel

  var body: some View {
    let r = model.session.resolution[model.cardKey]
    Form {
      if !model.draftEnds.isEmpty {
        Section {
          ForEach(model.draftEnds) { e in
            let output = e.side == "from" || e.side == "outputs"
            LabeledContent {
              TextField("実ポート名", text: Binding(get: { model.rebindings[e.id] ?? "" }, set: { model.rebindings[e.id] = $0 }))
            } label: {
              Text(output ? "出力ポート" : "入力ポート")
              Text(e.label).font(.caption).textSelection(.enabled)
            }
          }
        } header: { Text("wire の再接続") }
          footer: { Text("入力 wire には実入力ポート、出力 wire には実出力ポートを指定します。候補が一つでも指定が必要です。") }
      }

      Section {
        if model.definition != nil, [.code, .merge].contains(model.kind) {
          let inputs = model.definition?["inputs"]?.array ?? []
          ForEach(inputs.indices, id: \.self) { i in
            let name = inputs[i]["name"]?.string ?? ""
            HStack {
              // An editable field that looks like one (border, focus ring).
              TextField("名前", text: Binding(get: { name }, set: { v in editInputs { $0[i]["name"] = .string(v) } }))
                .textFieldStyle(.roundedBorder).labelsHidden()
                .frame(maxWidth: 140)
                .help("入力ポートの名前")
              Text(r?.inputs.first { $0.name == name }?.typeName ?? String(localized: "未解決")).font(.caption).foregroundStyle(.secondary)
              Button { editInputs { $0.remove(at: i) } } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless)
            }
          }
          Button("入力を追加") { editInputs { $0.append(.object([("name", .string("in\($0.count + 1)"))])) } }
        } else {
          ForEach(r?.inputs ?? [], id: \.name) { p in LabeledContent(p.name, value: p.typeName ?? String(localized: "未解決")) }
          if (r?.inputs ?? []).isEmpty { Text("入力はありません").foregroundStyle(.secondary) }
        }
      } header: { Text("入力ポート") } footer: {
        Text("入力の型は接続元の出力から決まります（_inputs.fbs は生成され、手で書きません）。")
          .font(.caption).foregroundStyle(.secondary)
      }
      OutputPortsSection(model: model)
      if model.kind != .subgraph && model.kind != .draft && model.kind != .invalid {
        Section {
          recordingOverride("recordLogs", label: "標準出力と標準エラーを記録")
          recordingOverride("recordIO", label: "入出力の値を記録")
        } header: { Text("記録と成果物") } footer: {
          Text("実行の要約は常に記録します。成果物は常に保存され、残すかどうかは Card が成果物を作るときに選びます。残さない成果物は実行の終了時に削除されます。").font(.caption).foregroundStyle(.secondary)
        }
      }
    }
    .formStyle(.grouped)
  }

  private func editInputs(_ fn: (inout [JSONValue]) -> Void) {
    var inputs = model.definition?["inputs"]?.array ?? []
    fn(&inputs)
    model.definition?["inputs"] = inputs.isEmpty ? nil : .array(inputs)
  }

  private func recordingOverride(_ name: String, label: LocalizedStringKey) -> some View {
    Picker(label, selection: Binding(
      get: { model.definition?[name]?.bool },
      set: { model.definition?[name] = $0.map { .bool($0) } })) {
      Text("プロジェクト設定に従う").tag(nil as Bool?)
      Text("オン").tag(true as Bool?)
      Text("オフ").tag(false as Bool?)
    }
  }
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
          let selection = p.rootType ?? ""
          if model.outputsPath == nil {
            Text(p.typeName ?? String(localized: "未解決"))
              .font(.caption).foregroundStyle(.secondary)
          } else {
            Picker("型", selection: Binding(get: { selection }, set: {
              set(p.name, type: $0)
            })) {
              if selection.isEmpty { Text("未解決").tag("").disabled(true) }
              if let type = p.rootType, !types.contains(type) {
                Text(short(type)).tag(type)
              }
              ForEach(types, id: \.self) { Text(short($0)).tag($0).help($0) }
            }
            .labelsHidden().frame(maxWidth: 150)
            .help("_outputs.fbs で定義した型から選びます（_outputs.fbs に書き戻します）")
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
  private func set(_ port: String, type: String) {
    guard !type.isEmpty, let p = model.outputsPath, let rpc = model.session.rpc else { return }
    Task {
      do {
        let text = try await model.text(p)
        let res = try await rpc.outputTypes(text: text, port: port, typeName: type)
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

/// What the user and the Agent write about this Card. Saved at once, not with Apply
/// (during a run, when the run ends, as other edits): it says nothing about how it runs.
private struct CommentsCategory: View {
  let session: ProjectSession
  let card: CardKey
  @State private var text = ""
  @State private var editing: CardComment?
  @State private var revisionEdit = InlineParamEdit()
  @State private var showResolved = false

  var body: some View {
    let comments = session.comments(card)
    let resolved = comments.filter(\.resolved)
    ScrollView {
      VStack(alignment: .leading, spacing: 8) {
        Text("この Card をどう変えたいかを書きます。Canvas の「コメントから生成」で Agent が従います（_comments.json に保存され、版の履歴に残ります）。")
          .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        TextField("例: y 軸を対数にしてください", text: $text, axis: .vertical)
          .lineLimit(3...8).textFieldStyle(.roundedBorder)
        HStack {
          Spacer()
          Button("コメントを追加", action: add)
            .keyboardShortcut(.return, modifiers: .command)
            .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !session.connected)
        }
        RevisionEditNotice(edit: revisionEdit.revision, text: { editing?.text ?? "" },
                           overwrite: { Task { await saveEdit(overwrite: true) } }, reload: {
                             editing = comments.first { $0.id == editing?.id }
                             beginCommentEdit(editing?.id ?? "", editing?.text ?? "")
                           })
        if let editing, !comments.contains(where: { $0.id == editing.id }) { row(editing) }
        ForEach(comments.filter { !$0.resolved }) { row($0) }
        if !resolved.isEmpty {
          DisclosureGroup("解決済み（\(resolved.count)）", isExpanded: $showResolved) {
            VStack(alignment: .leading, spacing: 6) { ForEach(resolved) { row($0) } }
              .padding(.top, 4)
          }
        }
      }
      .padding(12)
      .frame(maxWidth: .infinity, alignment: .leading)
    }
  }

  private func beginCommentEdit(_ id: String, _ text: String) {
    let value = session.cardMeta(card)?["comments"]?.array?.first { $0["id"]?.string == id }
    revisionEdit.beginField(text, session: session, file: (session.definitionDir(card) ?? "") + "_comments.json",
                           selector: [.string("comments"), .object([("id", .string(id))])], value: value)
  }

  private func saveEdit(overwrite: Bool = false) async {
    guard let copy = editing else { return }
    if await revisionEdit.commitField(session, text: copy.text, overwrite: overwrite, { value in
      let text = copy.text.trimmingCharacters(in: .whitespacesAndNewlines)
      guard !text.isEmpty else { return }
      value?["text"] = .string(text)
      value?["edited_by"] = .string("user")
    }) {
      editing = nil
    }
  }

  private func add() {
    let submitted = text
    session.performCardEdit {
      try await session.addComment(card, submitted)
      if text == submitted { text = "" }
    }
  }

  /// Who resolved it, what was done and which Cards changed.
  private func outcome(_ c: CardComment) -> String {
    var line = c.stateBy == "agent" ? String(localized: "Agent が解決") : c.stateBy == "user" ? String(localized: "あなたが解決") : String(localized: "解決")
    if let r = c.resolution { line += ": " + r }
    if !c.cards.isEmpty { line += String(localized: "（変更した Card: \(c.cards.joined(separator: "、"))）") }
    return line
  }

  /// Whose comment this is, when it is not simply one of the user's.
  @ViewBuilder private func record(_ c: CardComment) -> some View {
    let parts = [
      c.author == "agent" ? String(localized: "Agent が書きました") : nil,
      c.editedBy != nil && c.editedBy != (c.author ?? "user") ? (c.editedBy == "agent" ? String(localized: "Agent が編集") : String(localized: "あなたが編集")) : nil,
      !c.resolved && c.stateBy == "agent" ? String(localized: "Agent が開き直しました") : nil,
    ].compactMap { $0 }
    if !parts.isEmpty {
      HStack(spacing: 6) {
        Label { Text(verbatim: "Agent") } icon: { Image(systemName: "bubble.left") }
          .labelStyle(.titleAndIcon).font(.caption.weight(.semibold))
          .padding(.horizontal, 5).background(Color.accentColor.opacity(0.18), in: Capsule())
        Text(verbatim: parts.joined(separator: "・")).font(.caption).foregroundStyle(.secondary)
      }
    }
  }

  @ViewBuilder private func row(_ c: CardComment) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      if editing?.id == c.id {
        TextField("", text: Binding(get: { editing?.text ?? "" }, set: { editing?.text = $0 }), axis: .vertical)
          .lineLimit(3...8).textFieldStyle(.roundedBorder)
        HStack {
          Spacer()
          Button("キャンセル") { editing = nil }
          Button("保存") { Task { await saveEdit() } }
          .disabled((editing?.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !session.connected)
        }
      } else {
        record(c)
        Text(verbatim: c.text).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
        if c.resolution != nil || !c.cards.isEmpty || (c.resolved && CardComment.actor(c.stateBy) != nil) {
          Label { Text(verbatim: outcome(c)) } icon: { Image(systemName: "checkmark") }
            .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
        }
        HStack {
          Spacer()
          Button("編集") { beginCommentEdit(c.id, c.text); editing = c }
          if c.resolved {
            Button("開き直す") { session.setCommentResolved(card, id: c.id, false) }
          } else {
            Button("解決") { session.setCommentResolved(card, id: c.id, true) }
          }
          Button { session.removeComment(card, id: c.id) } label: { Image(systemName: "trash") }
            .buttonStyle(.borderless).help("削除")
        }
        .controlSize(.small)
        .disabled(!session.connected)
      }
    }
    .padding(.horizontal, 10).padding(.vertical, 8)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(Color(nsColor: .controlBackgroundColor), in: RoundedRectangle(cornerRadius: 6))
  }
}

/// What the last "build SDK" returned: the files of share/tsugumi/api/ by
/// their shown name, or the build failure and its log. Held only while the
/// editor shows the sources.
struct SdkBuild {
  struct File: Identifiable {
    let id: String
    let name: String
    let text: String
  }

  var building = false
  var files: [File]?
  var failure: String?
  var log: String?
  var truncated = false

  init(building: Bool = false, failure: String? = nil) {
    self.building = building
    self.failure = failure
  }

  init(_ result: Tsugumi_RPC_EvaluateNixResultT, prefix: String) {
    if let problem = result.failure {
      failure = problem.diagnostic ?? "\(problem.kind)"
      log = result.log
      return
    }
    truncated = result.truncated
    let directory = "share/tsugumi/api/"
    files = result.files.compactMap { file in
      guard let file, file.path.hasPrefix(directory) else { return nil }
      let relative = String(file.path.dropFirst(directory.count))
      return File(id: prefix + relative, name: relative, text: String(decoding: file.content, as: UTF8.self))
    }
  }
}

/// File tree, tabs and editor for a definition's sources, `_outputs.fbs` and
/// `default.nix`, with formatting, errors in place and the Card's generated
/// API (the "SDK" section: what the builder placed under share/tsugumi/api/).
/// The open files, the shown one and the expanded folders live on the model,
/// so switching categories or applying keeps them.
private struct SourceCategory: View {
  @Bindable var model: CardEditorModel
  @State private var newName = ""
  @State private var renaming: String?
  @State private var showNew = false
  @State private var formatting = false
  @State private var formatMessage: String?
  @State private var formatMarkers: [EditorMarker] = []
  @State private var sdk = SdkBuild()

  static let sdkPrefix = "sdk:"
  static let sdkLog = "sdk:/log"

  private var prefix: String { model.session.definitionDir(model.cardKey) ?? "" }

  var body: some View {
    let files = model.files
    HSplitView {
      VStack(spacing: 0) {
        ScrollView {
          VStack(alignment: .leading, spacing: 0) {
            FileTreeRows(nodes: FileNode.tree(files.map { String($0.dropFirst(prefix.count)) }, base: prefix), depth: 0,
                         model: model, open: open, contextMenu: menu)
            if model.kind == .code { sdkSection }
          }
          .padding(.vertical, 4)
        }
        Divider()
        HStack {
          Button { showNew = true; newName = "src/" } label: { Image(systemName: "plus") }.help("ファイルを作成")
          Button {
            if let s = model.shownFile, !s.hasPrefix(Self.sdkPrefix) { model.removeFile(s); model.closeFile(s) }
          } label: { Image(systemName: "minus") }
            .disabled(model.shownFile == nil || model.shownFile!.hasPrefix(Self.sdkPrefix)).help("表示中のファイルを削除")
          Spacer()
        }
        .buttonStyle(.borderless).padding(6)
      }
      .frame(minWidth: 170, idealWidth: 220, maxWidth: 360)
      VStack(spacing: 0) {
        if model.openFiles.count > 1 { tabs }
        editorPane(files: files)
      }
      .frame(minWidth: 320, maxWidth: .infinity, maxHeight: .infinity)
    }
    .onAppear {
      if model.shownFile == nil {
        if let f = files.first { model.openFile(f) }
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

  /// The "SDK" section: builds the saved definition with the current wiring
  /// by evaluating and building (import ./. { inherit params inputs; }).api or null, then lists share/tsugumi/api/.
  @ViewBuilder private var sdkSection: some View {
    HStack {
      Text("SDK").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
      Spacer()
      Button(sdk.building ? String(localized: "ビルドしています…") : String(localized: "SDK をビルド")) { Task { await buildSdk() } }
        .controlSize(.small).disabled(sdk.building || model.session.rpc == nil)
        .help("保存済みの定義と現在の接続で SDK をビルドし、share/tsugumi/api/ のファイルを表示します")
    }
    .padding(.leading, 10).padding(.trailing, 6).padding(.top, 12).padding(.bottom, 2)
    if let failure = sdk.failure {
      Text("ビルドできませんでした: \(failure)").font(.caption).foregroundStyle(.red).textSelection(.enabled)
        .padding(.horizontal, 10).padding(.vertical, 2)
      if let log = sdk.log, !log.isEmpty {
        FileRow(name: String(localized: "ビルドログ"), icon: "doc.text", depth: 0, folder: nil,
                selected: model.shownFile == Self.sdkLog, readOnly: true) { open(Self.sdkLog) }
      }
    }
    if sdk.truncated {
      Text("出力の上限に達したため、一部のファイルのみ表示しています").font(.caption).foregroundStyle(.secondary)
        .padding(.horizontal, 10)
    }
    if let files = sdk.files {
      if files.isEmpty {
        Text("ビルドで生成された SDK ファイルはありません").font(.caption).foregroundStyle(.secondary)
          .padding(.horizontal, 10).padding(.vertical, 2)
      }
      ForEach(files) { file in
        FileRow(name: file.name, icon: "lock.doc", depth: 0, folder: nil,
                selected: model.shownFile == file.id, readOnly: true) { open(file.id) }
          .help(file.name)
      }
    }
  }

  private func buildSdk() async {
    guard let rpc = model.session.rpc, !sdk.building else { return }
    sdk = SdkBuild(building: true)
    do {
      let result = try await rpc.evaluateNix(card: path(model.cardKey), expression: "(import ./. { inherit params inputs; }).api or null", build: true, atRev: model.session.rev)
      sdk = SdkBuild(result, prefix: Self.sdkPrefix)
    } catch {
      sdk = SdkBuild(failure: "\(error)")
    }
  }

  private func sdkName(_ f: String) -> String {
    if f == Self.sdkLog { return String(localized: "ビルドログ") }
    return sdk.files?.first(where: { $0.id == f })?.name ?? (f as NSString).lastPathComponent
  }

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
            Image(systemName: f.hasPrefix(Self.sdkPrefix) ? "lock.doc" : FileNode.icon(f)).font(.caption)
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
    ((f.hasPrefix(Self.sdkPrefix) ? sdkName(f) : f) as NSString).lastPathComponent
  }

  /// Header: the file's name and where it is (definition › folders).
  private func header(_ path: String, generatedFile: Bool) -> some View {
    let rel = generatedFile ? sdkName(path) : String(path.dropFirst(prefix.count))
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
      if generatedFile { ReadOnlyMark(why: String(localized: "Card のビルドが生成したファイルです（手で編集しません）")) }
      if model.created.contains(path) { Text("新規").font(.caption2).foregroundStyle(.orange) }
      else if model.isEdited(path) { Text("未適用").font(.caption2).foregroundStyle(.orange) }
      Spacer()
      if let formatMessage { Text(formatMessage).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
      if !generatedFile && canFormat(path) {
        Button("整形") { format(path) }.disabled(formatting || model.loadedText(path) == nil).controlSize(.small)
          .shortcut(.formatSource)
          .help(tip(String(localized: "ソースコードを整形"), .formatSource))
      }
    }
    .padding(.horizontal, 10).padding(.vertical, 6)
    // The window's own background: the header continues the title bar above it.
    .background(Color(nsColor: .windowBackgroundColor))
    .overlay(alignment: .bottom) { Divider() }
  }

  @ViewBuilder private func editorPane(files: [String]) -> some View {
    VStack(spacing: 0) {
      if let selected = model.shownFile, selected.hasPrefix(Self.sdkPrefix) {
        header(selected, generatedFile: true)
        if let text = selected == Self.sdkLog ? sdk.log : sdk.files?.first(where: { $0.id == selected })?.text {
          CodeEditor(text: .constant(text), language: selected == Self.sdkLog ? "" : (selected as NSString).pathExtension,
                     editable: false, documentID: selected)
        } else {
          ContentUnavailableView("SDK をビルドすると表示されます", systemImage: "doc.text")
        }
      } else if let selected = model.shownFile, model.files.contains(selected) {
        let markers = coreMarkers(for: selected) + formatMarkers
        let loaded = model.loadedText(selected) != nil
        header(selected, generatedFile: false)
        Group {
          if loaded {
            CodeEditor(text: Binding(get: { model.loadedText(selected) ?? "" },
                                     set: { model.texts[selected] = $0; formatMarkers = [] }),
                       language: (selected as NSString).pathExtension, markers: markers, documentID: selected, revealLine: model.sourceLine)
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

  private func canFormat(_ path: String) -> Bool {
    ["json", "nix"].contains((path as NSString).pathExtension)
  }

  private func format(_ p: String) {
    guard !formatting, canFormat(p), model.session.rpc != nil, let source = model.loadedText(p) else { return }
    formatting = true
    Task {
      do {
        let r = try await CoreHostClient.shared.format(path: p, text: source)
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
      if readOnly { ReadOnlyMark(why: String(localized: "読み取り専用")) }
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
