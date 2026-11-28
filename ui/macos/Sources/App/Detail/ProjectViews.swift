import AppKit
import SwiftUI
import TsugumiAgentClient
import TsugumiLauncher
import TsugumiRPC

// MARK: - Card database

/// Every definition folder of the project (one per Card, and those without
/// a Card): search, edit its JSON, place an unplaced one on the canvas.
struct CardDatabaseView: View {
  @Environment(\.openWindow) private var openWindow
  let session: ProjectSession
  /// The definition the window was opened for ("Edit…" in the tree).
  var initial: String? = nil
  @State private var search = ""
  @State private var showTemplates = false
  private let newCardPoint = CGPoint(x: 40, y: 40)
  @State private var selected: String?
  @State private var cardText = ""
  @State private var metaText = ""
  @State private var baseCard = ""
  @State private var baseMeta = ""
  @State private var revisionEdit = RevisionEdit()
  @State private var parseError: String?
  @State private var message: String?

  var body: some View {
    HSplitView {
      VStack(spacing: 0) {
        HStack {
          TextField("検索", text: $search).textFieldStyle(.roundedBorder)
          Menu {
            NewCardItems(add: { kind in
              session.performCardEdit {
                selected = path(try await session.addCard(template: kind, in: [], at: newCardPoint)).first
              }
            }, pickTemplate: { showTemplates = true })
          } label: { Image(systemName: "plus") }
            .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            .disabled(!session.isEditable)
        }
        .padding(6)
        List(ids, id: \.self, selection: $selected) { id in
          let def = session.definitions[id]
          HStack {
            Image(systemName: definitionIcon(def)).foregroundStyle(.secondary).frame(width: 16)
            VStack(alignment: .leading) {
              Text(session.displayName(key([id])))
              Text(definitionKind(def).label + (session.unplacedDefinitions.contains(id) ? String(localized: " · 未配置") : ""))
                .font(.caption).foregroundStyle(.secondary)
            }
          }
          .tag(id)
          .draggable(DefinitionDrag.prefix + id)
        }
      }
      .disabled(revisionEdit.applying)
      .frame(minWidth: 240, idealWidth: 280)
      detail.frame(minWidth: 420)
    }
    .sheet(isPresented: $showTemplates) {
      TemplatePicker(session: session, placement: TemplatePlacement(scope: [], point: newCardPoint))
    }
    .onChange(of: selected) { load() }
    .onAppear { if selected == nil, let initial, session.definitions[initial] != nil { selected = initial } }
    .navigationTitle("Card の定義")
  }

  private var ids: [String] {
    session.definitions.keys.sorted().filter { id in
      search.isEmpty || session.displayName(key([id])).localizedCaseInsensitiveContains(search) || id.localizedCaseInsensitiveContains(search) ||
        (session.definitionMeta[id]?["description"]?.string ?? "").localizedCaseInsensitiveContains(search)
    }
  }

  @ViewBuilder private var detail: some View {
    if let id = selected {
      let unapplied = cardText != baseCard || metaText != baseMeta
      VStack(alignment: .leading, spacing: 8) {
        HStack {
          Text(session.displayName(key([id]))).font(.title2.bold())
          Spacer()
          if unapplied { Label("未適用の変更", systemImage: "pencil.circle.fill").foregroundStyle(.orange) }
        }
        Text("Card「\(id)」の定義（definitions/\(id)/）です。").font(.callout).foregroundStyle(.secondary)
        Text("_card.json（設定・入力）").font(.headline)
        CodeEditor(text: $cardText, language: "json").frame(minHeight: 180)
        Text("README.md（説明・計画、任意）").font(.headline)
        CodeEditor(text: $metaText, language: "md").frame(minHeight: 80, maxHeight: 140)
        RevisionEditNotice(edit: revisionEdit, text: { "_card.json\n" + cardText + "\nREADME.md\n" + metaText },
                           overwrite: { Task { await apply(id, overwrite: true) } }, reload: { load() })
        if let parseError { Label(parseError, systemImage: "xmark.octagon").foregroundStyle(.red) }
        if let message { Text(message).font(.callout).foregroundStyle(.secondary) }
        HStack {
          Button("Canvas に配置") { place(id) }.disabled(!session.isEditable || !session.unplacedDefinitions.contains(id))
          Button("ソースを開く…") {
            openWindow(id: "card", value: CardRef(path: session.identity.url.path, card: key([id]), category: .source))
          }
          .disabled(session.unplacedDefinitions.contains(id))
          .help("Card の設定画面で開きます")
          Button("削除", role: .destructive) { session.removeDefinition(id); selected = nil }
            .disabled(!session.unplacedDefinitions.contains(id) || !session.isEditable)
            .help(session.unplacedDefinitions.contains(id) ? "定義とソースを削除" : "Card を削除すると定義も削除されます")
          Spacer()
          Button("再読込") { load() }.disabled(revisionEdit.applying)
          Button("適用") { Task { await apply(id) } }.disabled(!unapplied || revisionEdit.applying || !session.isEditable)
        }
      }
      .padding(12)
    } else {
      ContentUnavailableView("定義を選んでください", systemImage: "square.stack", description: Text("ドラッグして Canvas に配置できます。"))
    }
  }

  private func load() {
    guard let id = selected else { return }
    cardText = session.definitions[id]?.serialized() ?? "{}"
    metaText = Documents.splitCardNotes(session.definitionMeta[id]).readme ?? ""
    baseCard = cardText
    baseMeta = metaText
    revisionEdit.reload(session.rev)
    parseError = nil
    message = nil
  }

  private func place(_ id: String) {
    _ = session.placeDefinition(id, at: CGPoint(x: 40, y: 40))
    message = nil
  }

  private func apply(_ id: String, overwrite: Bool = false) async {
    let sentCard = cardText, sentMeta = metaText
    do {
      let card = try JSONValue.parse(sentCard)
      let meta: JSONValue? = sentMeta.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : .string(sentMeta)
      parseError = nil
      guard await revisionEdit.replaceDocuments(session, overwrite: overwrite, [
        ("definitions/\(id)/_card.json", card), ("definitions/\(id)/README.md", meta),
      ]) else { return }
      guard selected == id else { return }
      baseCard = sentCard
      baseMeta = sentMeta
      message = String(localized: "適用しました（revision \(session.rev)）。")
    } catch {
      parseError = String(localized: "JSON として読めません: \(String(describing: error))")
    }
  }
}

// MARK: - Workflow settings

/// A project's settings pane (in the settings window, ⌘,).
struct ProjectSettingsContent: View {
  let pane: ProjectSettingsPane
  let session: ProjectSession

  var body: some View {
    switch pane {
    case .general: GeneralProjectSettings(session: session)
    case .history: RunHistorySettings(session: session)
    case .cache: CacheSettings(session: session)
    case .hostPaths: HostPathsSettings(session: session)
    case .secrets: CardSecretsSettings(session: session)
    }
  }
}

private struct NixPathSettings: View {
  let session: ProjectSession
  @State private var prefix = ""

  var body: some View {
    let projects = session.externals.keys.sorted()
    Section {
      if !projects.isEmpty {
        Picker("対象プロジェクト", selection: $prefix) {
          Text("ルートプロジェクト").tag("")
          ForEach(projects, id: \.self) { Text(verbatim: String($0.dropLast())).tag($0) }
        }
        .onChange(of: projects) { if !projects.contains(prefix) { prefix = "" } }
      }
      NixPathRows(session: session, prefix: prefix).id(prefix)
    } header: {
      InfoHeader("Nix 検索パス", info: "Card は <名前/パス> でソースを参照します。「更新」で追加または URL が変わったソースを固定し、削除したソースの固定を外します。")
    }
  }
}

private struct NixPathRows: View {
  let session: ProjectSession
  let prefix: String
  @State private var name = ""
  @State private var url = ""
  @State private var editing: String?
  @State private var adding = false
  @State private var updating = false
  @State private var updateError: String?
  @State private var revisionEdit = RevisionEdit()
  @State private var original: JSONValue?
  @State private var pending: [JSONValue]?

  private var file: String { prefix + "tsugumi.json" }
  private var project: JSONValue? { prefix.isEmpty ? session.project : session.externals[prefix]?.project }
  private var entries: [JSONValue] { project?["nixPath"]?.array ?? [] }
  private var disabled: Bool { updating || revisionEdit.applying || !session.isEditable }

  private func start(_ entry: JSONValue? = nil) {
    editing = entry?["name"]?.string
    adding = entry == nil
    name = editing ?? ""
    url = entry?["url"]?.string ?? ""
    original = project?["nixPath"]
    pending = nil
    revisionEdit.reload(session.rev)
  }

  private func save(_ list: [JSONValue], overwrite: Bool = false) async {
    pending = list
    let before = overwrite ? revisionEdit.jsonBase(file, initial: nil, overwrite: true)?["nixPath"] : original
    if await revisionEdit.apply(session, { s in
      s.appendEdits([s.fieldEdit(file, [.string("nixPath")], old: before, new: .array(list))])
    }) {
      close()
    }
  }

  private func close() {
    editing = nil
    adding = false
    pending = nil
    revisionEdit.reload(session.rev)
  }

  private func update(_ name: String? = nil) async {
    updating = true
    updateError = nil
    defer { updating = false }
    do { try await session.updateNixPathLock(prefix: prefix, name: name) }
    catch { updateError = error.localizedDescription }
  }

  var body: some View {
    let lock = session.nixPathLock(prefix: prefix)
    ForEach(entries.indices, id: \.self) { index in
      let entry = entries[index]
      if let name = entry["name"]?.string, let url = entry["url"]?.string {
        LabeledContent(name) {
          HStack {
            VStack(alignment: .trailing) {
              Text(verbatim: url).font(.caption.monospaced()).textSelection(.enabled)
              status(.read(lock, name: name, url: url)).font(.caption).foregroundStyle(.secondary)
            }
            Button("編集") { start(entry) }
            Button("更新") { Task { await update(name) } }
            Button("削除", role: .destructive) {
              start(entry)
              let remaining = entries.filter { $0["name"]?.string != name }
              Task { await save(remaining) }
            }
          }
          .disabled(disabled)
        }
      }
    }
    if adding || editing != nil {
      TextField("名前", text: $name).disabled(disabled)
      TextField("URL", text: $url, prompt: Text(verbatim: "github:owner/repo")).disabled(disabled)
      HStack {
        Button(adding ? String(localized: "追加") : String(localized: "保存")) {
          let entry = JSONValue.object([("name", .string(name)), ("url", .string(url.trimmingCharacters(in: .whitespacesAndNewlines)))])
          var next = original?.array ?? []
          if let index = next.firstIndex(where: { $0["name"]?.string == editing }) { next[index] = entry }
          else { next.append(entry) }
          Task { await save(next) }
        }
        .disabled(disabled || name.wholeMatch(of: #/[A-Za-z0-9][A-Za-z0-9._-]*/#) == nil ||
          url.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ||
          entries.contains { $0["name"]?.string == name && name != editing })
        Button("キャンセル") { close() }.disabled(disabled)
      }
    }
    HStack {
      if !adding && editing == nil { Button("追加") { start() }.disabled(disabled) }
      Button("すべて更新") { Task { await update() } }.disabled(disabled)
      if updating { ProgressView().controlSize(.small) }
    }
    RevisionEditNotice(edit: revisionEdit, text: { JSONValue.array(pending ?? []).serialized() },
                       overwrite: { if let pending { Task { await save(pending, overwrite: true) } } },
                       reload: { close() })
    if let updateError { Text(updateError).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
  }

  private func status(_ value: NixPathLockStatus) -> Text {
    switch value {
    case .unlocked: Text("未ロック")
    case .urlChanged: Text("URL が変更されました")
    case .locked(let pin): Text(verbatim: pin)
    }
  }
}

private struct GeneralProjectSettings: View {
  let session: ProjectSession
  @State private var selectedProject: JSONValue = .object([])
  @State private var name = ""
  @State private var trackedPaths = ""
  @State private var agentExclude = ""
  @State private var recording: [String: Bool] = [:]
  @State private var limits: [String: String] = [:]
  private let recordingKeys = ["recordLogs", "recordIO", "pushOutputs"]
  private let limitFields: [(key: String, label: LocalizedStringKey, max: UInt64)] = [
    ("logLimitBytes", "呼び出しごとのログ上限（バイト）", 1073741824),
    ("outputLimitBytes", "出力の保存上限（バイト）", .max),
    ("outputLimitEntries", "出力の保存件数上限", .max),
  ]

  private func recordingValues(_ project: JSONValue) -> [String: Bool] {
    Dictionary(uniqueKeysWithValues: recordingKeys.compactMap { key in
      (project[key]?.bool ?? (key == "pushOutputs" ? true : nil)).map { (key, $0) }
    })
  }

  private func limitValues(_ project: JSONValue) -> [String: String] {
    Dictionary(uniqueKeysWithValues: limitFields.map { ($0.key, project[$0.key]?.numberText ?? "") })
  }

  private var validLimits: Bool {
    limitFields.allSatisfy { field in
      guard let text = limits[field.key], let value = UInt64(text),
            text.utf8.allSatisfy({ (48...57).contains($0) }) else { return false }
      return value <= field.max
    }
  }

  private func recordingToggle(_ key: String, _ label: LocalizedStringKey) -> some View {
    Toggle(label, isOn: Binding(get: { recording[key] == true }, set: { recording[key] = $0 }))
  }
  private func excludeText(_ project: JSONValue) -> String {
    (project["agentExclude"]?.array?.compactMap(\.string) ?? []).joined(separator: "\n")
  }
  @State private var message: String?
  @State private var revisionEdit = RevisionEdit()

  var body: some View {
    let savedPaths = (selectedProject["trackedPaths"]?.array ?? []).compactMap(\.string).joined(separator: "\n")
    let changed = name != (selectedProject["name"]?.string ?? "") || trackedPaths != savedPaths || agentExclude != excludeText(selectedProject) || recording != recordingValues(selectedProject) || limits != limitValues(selectedProject)
    VStack(alignment: .leading) {
      Form {
        TextField("プロジェクト名", text: $name)
        Section {
          LabeledContent {
            TextEditor(text: $trackedPaths).font(.system(.body, design: .monospaced)).frame(minHeight: 90)
          } label: {
            InfoHeader("Tsugumi の版に含める追加パス", info: "Tsugumi のプロジェクトの版に追加で残すファイルやフォルダを、プロジェクトからの相対パスで 1 行に 1 つ指定します。Git の追跡には影響しません。ワイルドカード、.tsugumi/、.git/、シンボリックリンクは指定できません。")
          }
        }
        Section {
          LabeledContent {
            TextEditor(text: $agentExclude).font(.system(.body, design: .monospaced)).frame(minHeight: 90)
          } label: {
            InfoHeader("Agent から除外するパス", info: "Agent が参照できないパスを 1 行に 1 つ指定します。プロジェクトからの相対パス、または絶対パスで、ファイルと配下の全体を対象にします。人の画面には表示されます。")
          }
        }
        Toggle(isOn: Binding(get: { session.projectTrust == 1 }, set: { trusted in
          Task { await session.setProjectTrust(trusted) }
        })) { InfoHeader("このプロジェクトを信頼する", info: "信頼すると、このプロジェクトの Skill が自動で適用され、組み込み Skill を上書きできます。信頼しなくても、プロジェクトを開いて読むことや、名前を指定して Skill を使うことはできます。") }
        .disabled(!session.trustedFolder.isEmpty)
        Section {
          recordingToggle("recordLogs", "標準出力と標準エラーを記録")
          recordingToggle("recordIO", "入出力の値を記録")
          ForEach(limitFields.indices, id: \.self) { i in
            let field = limitFields[i]
            TextField(field.label, text: Binding(get: { limits[field.key] ?? "" }, set: { limits[field.key] = $0 }))
              .help(field.key == "logLimitBytes" ? "0〜1073741824。0 の場合もログは実行中に表示されます。" : "")
          }
        } header: { Text("記録の既定値") } footer: {
          Text("実行の要約は常に記録します。成果物は常に保存され、残すかどうかは Card が成果物を作るときに選びます。残さない成果物は実行の終了時に削除されます。").font(.caption).foregroundStyle(.secondary)
        }
        Section("バイナリキャッシュ") {
          recordingToggle("pushOutputs", "ビルド出力をキャッシュに送信")
        }
        TrustedFoldersSettings()
        LabeledContent("場所", value: session.identity.url.path)
        LabeledContent("ファイル形式", value: session.project["formatVersion"]?.numberText ?? "")
        NixPathSettings(session: session)
      }
      .formStyle(.grouped)
      RevisionEditNotice(edit: revisionEdit, text: {
        (["name: \(name)", "trackedPaths:\n\(trackedPaths)", "agentExclude:\n\(agentExclude)"] +
          recording.sorted(by: { $0.key < $1.key }).map { "\($0.key): \($0.value)" } +
          limitFields.map { "\($0.key): \(limits[$0.key] ?? "")" }).joined(separator: "\n")
      }, overwrite: { Task { await save(overwrite: true) } }, reload: { load() })
        .padding(.horizontal, 12)
      HStack {
        if let message { Text(message).font(.callout).foregroundStyle(.secondary) }
        Spacer()
        Button("保存") {
          Task { await save() }
        }
        .disabled(!changed || !validLimits || revisionEdit.applying || !session.isEditable)
        .keyboardShortcut(.defaultAction)
      }
      .padding(12)
    }
    .onAppear { load() }
  }

  private func load() {
    selectedProject = session.project
    name = session.project["name"]?.string ?? ""
    trackedPaths = (session.project["trackedPaths"]?.array ?? []).compactMap(\.string).joined(separator: "\n")
    agentExclude = excludeText(session.project)
    recording = recordingValues(session.project)
    limits = limitValues(session.project)
    revisionEdit.reload(session.rev)
  }

  private func save(overwrite: Bool = false) async {
    guard validLimits else { return }
    let sentRecording = recording, sentLimits = limits
    let sentName = name, sentPaths = trackedPaths, sentExclude = agentExclude
    let paths = sentPaths.components(separatedBy: .newlines).filter { !$0.isEmpty }
    let before = revisionEdit.jsonBase("tsugumi.json", initial: selectedProject, overwrite: overwrite)
    var after = before ?? .object([])
    if sentName != (selectedProject["name"]?.string ?? "") { after["name"] = .string(sentName) }
    let originalPaths = (selectedProject["trackedPaths"]?.array ?? []).compactMap(\.string).joined(separator: "\n")
    if sentPaths != originalPaths { after["trackedPaths"] = .array(paths.map { .string($0) }) }
    if sentExclude != excludeText(selectedProject) { after["agentExclude"] = .array(sentExclude.components(separatedBy: .newlines).filter { !$0.isEmpty }.map { .string($0) }) }
    let savedRecording = recordingValues(selectedProject)
    for (key, value) in sentRecording where savedRecording[key] != value { after[key] = .bool(value) }
    let savedLimits = limitValues(selectedProject)
    for (key, text) in sentLimits where text != savedLimits[key] {
      if let value = UInt64(text) { after[key] = .number(String(value)) }
    }
    if await revisionEdit.apply(session, { s in
      s.appendEdits([s.jsonEdit("tsugumi.json", old: before, new: after)])
    }) {
      selectedProject = after
      for (key, value) in recordingValues(after) where recording[key] == sentRecording[key] { recording[key] = value }
      for (key, value) in limitValues(after) where limits[key] == sentLimits[key] { limits[key] = value }
      if trackedPaths == sentPaths { trackedPaths = paths.joined(separator: "\n") }
      message = String(localized: "保存しました。")
    }
  }
}

private struct RunHistorySettings: View {
  let session: ProjectSession
  enum Scope: Hashable { case all, before }
  @State private var scope = Scope.before
  @State private var before = Calendar.current.date(byAdding: .day, value: -30, to: Date()) ?? Date()
  @State private var working = false
  @State private var message: String?
  @State private var failed = false
  @State private var confirm = false
  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      Form {
        Section {
          Picker("対象", selection: $scope) {
            Text("指定した日時より前に終わった実行").tag(Scope.before)
            Text("すべての実行").tag(Scope.all)
          }
          if scope == .before { DatePicker("日時", selection: $before) }
        } header: {
          InfoHeader("削除", info: "実行記録、保存した値、ログ、実行が所有する成果物（.tsugumi/artifacts/<run>）を削除します。実行中は削除できません。")
        }
      }
      .formStyle(.grouped)
      HStack {
        if let message { Text(message).font(.callout).foregroundStyle(failed ? .red : .secondary).textSelection(.enabled) }
        Spacer()
        if working { ProgressView().controlSize(.small) }
        Button("削除…", role: .destructive) { confirm = true }
          .disabled(working || session.runActive || !session.connected)
      }
      .padding(12)
    }
    .confirmationDialog(scope == .all ? "すべての実行履歴を削除しますか？" : "この日時より前に終わった実行を削除しますか？", isPresented: $confirm) {
      Button("削除", role: .destructive) { delete() }
      Button("キャンセル", role: .cancel) {}
    }
  }

  private func delete() {
    working = true
    message = nil
    Task {
      do {
        let r = try await session.deleteRunHistory(endedBefore: scope == .all ? nil : before)
        failed = false
        let freed = ByteCountFormatter.string(fromByteCount: Int64(r.freedBytes ?? 0), countStyle: .file)
        message = String(localized: "\(r.deletedRuns ?? 0) 件の実行を削除し、\(freed) を解放しました。")
      } catch {
        failed = true
        message = String(localized: "削除できませんでした: \(String(describing: error))")
      }
      working = false
    }
  }
}

private struct CacheSettings: View {
  let session: ProjectSession
  @Environment(AppModel.self) private var model
  private struct CacheProject {
    let id: String
    let name: String
    let path: String?
  }

  @State private var clearTarget: CacheProject?
  @State private var environments: [String: CacheProject] = [:]
  @State private var result: Tsugumi_RPC_ProjectCacheClearResultT?
  @State private var working = false
  @State private var message: String?
  @State private var confirm = false

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      GroupBox {
        ScrollView {
          VStack(alignment: .leading, spacing: 8) {
            LabeledContent("合計（共有パスは一度だけ計上）", value: bytes(session.cacheUsage?.buildBytes))
            if let available = session.cacheUsage?.storeAvailableBytes {
              LabeledContent("store ディスクの空き", value: bytes(available))
            }

            if let measured = session.cacheUsage?.measuredAt {
              LabeledContent("計測日時", value: Date(timeIntervalSince1970: Double(measured) / 1000).formatted())
            }
            if let error = session.cacheUsage?.error { Text(error).font(.caption).textSelection(.enabled) }
            ForEach((session.cacheUsage?.projects ?? []).compactMap { $0 }, id: \.project) { project in
              let target = cacheProject(project)
              let open = model.sessions[target.id]
              Divider()
              HStack {
                Text(target.name).font(.headline)
                Spacer()
                Button("削除", role: .destructive) { clearTarget = target; confirm = true }
                  .disabled(open?.runActive == true || working)
              }
              if let path = target.path {
                Text(path).font(.caption.monospaced()).foregroundStyle(.secondary).textSelection(.enabled)
              }
              LabeledContent("ビルドキャッシュ", value: bytes(project.buildBytes))
              LabeledContent("root の最終使用", value: project.lastUsed.map { Date(timeIntervalSince1970: Double($0) / 1000).formatted() } ?? String(localized: "不明"))
              if let error = project.error { Text(error).font(.caption).textSelection(.enabled) }
            }
          }
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      } label: {
        InfoHeader("キャッシュ使用量", info: Text("このプロジェクトの Card のビルド結果の登録（Nix の GC root）と、作業用のビルドファイルを削除します。共有 Nix store にある実体は消さず、アプリ設定の「共有 store を掃除」で登録のないものとして削除されます。文書・実行履歴・保持した成果物は削除しません。次回の実行では評価し直し、store に残っていればビルドせずに使います。"), Text("実行中は削除できません。"), Text("ビルドキャッシュは NAR サイズの概算です。共有する依存関係は各プロジェクトにも含まれるため、ビルドの内訳の和と合計は異なります。設定を開いたときに計測します。"))
      }
      if let r = result {
        GroupBox("結果") {
          VStack(alignment: .leading, spacing: 4) {
            LabeledContent("削除", value: String(localized: "\(r.deleted.count) 件"))
            LabeledContent("保持", value: String(localized: "\(r.retained.count) 件"))
            if let b = r.freedBytes { LabeledContent("解放", value: ByteCountFormatter.string(fromByteCount: Int64(b), countStyle: .file)) }
          }
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
      if let message { Text(message).foregroundStyle(.red).textSelection(.enabled) }
      if working {
        ProgressView().controlSize(.small).frame(maxWidth: .infinity, alignment: .trailing)
      }
    }
    .padding(16)
    .task {
      async let usage: Void = session.refreshCacheUsage()
      async let names: Void = loadEnvironmentNames()
      _ = await (usage, names)
    }
    .confirmationDialog("ビルドキャッシュを削除しますか？", isPresented: $confirm, titleVisibility: .visible,
                        presenting: clearTarget) { target in
      Button("削除", role: .destructive) { clear(target) }
      Button("キャンセル", role: .cancel) {}
    } message: { target in
      Text([target.name, target.path].compactMap { $0 }.joined(separator: "\n"))
    }
  }

  private func bytes(_ value: UInt64?) -> String {
    value.map { ByteCountFormatter.string(fromByteCount: Int64(clamping: $0), countStyle: .file) }
      ?? String(localized: "計測中または取得できません")
  }

  private func cacheProject(_ usage: Tsugumi_RPC_ProjectCacheUsageT) -> CacheProject {
    let id = usage.project
    if let open = model.sessions[id] {
      return CacheProject(id: id, name: open.scopeName([]), path: open.identity.url.path)
    }
    if let environment = environments[id] { return environment }
    if let recent = model.recents.first(where: {
      (try? ProjectIdentity(url: URL(fileURLWithPath: $0.path)))?.id == id
    }) {
      return CacheProject(id: id, name: recent.name, path: recent.path)
    }
    return CacheProject(id: id, name: usage.name == "project" ? id : usage.name, path: nil)
  }

  private func loadEnvironmentNames() async {
    let request = Tsugumi_Host_EnvironmentListRequestT()
    guard let reply: Tsugumi_Host_EnvironmentListT = try? await CoreHostClient.shared.hostCall(.environmentlistrequest, request)
    else { return }
    for entry in reply.environments.compactMap({ $0 }) where !entry.state.project.isEmpty {
      let id = entry.state.key
      environments[id] = CacheProject(id: id, name: entry.name, path: entry.state.project)
    }
  }

  private func clear(_ target: CacheProject) {
    working = true
    message = nil
    Task {
      defer { working = false }
      do {
        let request = Tsugumi_Host_HostRuntimeRequestT()
        request.action = .clearprojectbuildcache
        request.projectId = target.id
        let reply: Tsugumi_Host_HostRuntimeT = try await CoreHostClient.shared.hostCall(.hostruntimerequest, request)
        result = reply.buildCache
        await session.refreshCacheUsage()
      } catch { message = String(localized: "削除できませんでした: \(String(describing: error))") }
    }
  }
}
