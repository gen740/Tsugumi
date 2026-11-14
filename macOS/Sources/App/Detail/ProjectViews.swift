import AppKit
import SwiftUI
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
            ForEach(DefinitionTemplate.allCases) { t in
              Button(t.label) { session.performCardEdit { selected = path(try await session.addCard(template: t, in: [], at: CGPoint(x: 40, y: 40))).first } }
            }
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
        Text("_card.json（設定・params・入力）").font(.headline)
        CodeEditor(text: $cardText, language: "json").frame(minHeight: 180)
        Text("_meta.json（説明・計画・追加コンテキスト、任意）").font(.headline)
        CodeEditor(text: $metaText, language: "json").frame(minHeight: 80, maxHeight: 140)
        RevisionEditNotice(edit: revisionEdit, text: { "_card.json\n" + cardText + "\n_meta.json\n" + metaText },
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
          Button("削除", role: .destructive) { session.edit { $0.removeDefinition(id) }; selected = nil }
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
    metaText = session.definitionMeta[id]?.serialized() ?? ""
    baseCard = cardText
    baseMeta = metaText
    revisionEdit.reload(session.rev)
    parseError = nil
    message = nil
  }

  private func place(_ id: String) {
    let k = session.placeDefinition(id, at: CGPoint(x: 40, y: 40))
    message = String(localized: "\(k) を Workflow に配置しました（未保存）。")
  }

  private func apply(_ id: String, overwrite: Bool = false) async {
    let sentCard = cardText, sentMeta = metaText
    do {
      let card = try JSONValue.parse(sentCard)
      let meta = sentMeta.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : try JSONValue.parse(sentMeta)
      parseError = nil
      guard await revisionEdit.apply(session, overwrite: overwrite, { s in
        var d = s.cardDefinitionDraft(key([id])) ?? DefinitionDraft(card: card, meta: meta)
        if d.link == nil { d.card = card }
        d.meta = meta
        s.setCardDefinition(key([id]), d)
      }) else { return }
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
    case .canvas: CanvasProjectSettings(session: session)
    case .history: RunHistorySettings(session: session)
    case .cache: CacheSettings(session: session)
    }
  }
}

/// Flakes this project opts in to: an address gives its commit, hash and
/// packages, which Cards read as inputs.
private struct NixSourceSettings: View {
  let session: ProjectSession
  private struct Found { var name: String; let ref: String; let packages: [String] }
  @State private var address = ""
  @State private var found: Found?
  @State private var working = false
  @State private var message: String?
  @State private var revisionEdit = RevisionEdit()
  @State private var pending: [JSONValue]?

  private var sources: [JSONValue] { session.project["nixSources"]?.array ?? [] }
  private func entry(_ name: String, _ ref: String) -> JSONValue {
    .object([("name", .string(name)), ("type", .string("flake")), ("ref", .string(ref))])
  }

  private func resolve(_ ref: String, baseRev: UInt64) async -> (ref: String, packages: [String])? {
    revisionEdit.reload(baseRev)
    working = true
    message = nil
    defer { working = false }
    do {
      let r = try await session.resolveNixSource(ref: ref)
      return (r.ref, r.packages.compactMap { $0 })
    } catch {
      message = String(localized: "取得できませんでした: \(error.localizedDescription)")
      return nil
    }
  }

  private func save(_ list: [JSONValue], overwrite: Bool = false) async {
    pending = list
    if await revisionEdit.apply(session, overwrite: overwrite, { $0.project["nixSources"] = .array(list) }) {
      pending = nil
    }
  }

  private var taken: Bool { found.map { f in sources.contains { $0["name"]?.string == f.name } } ?? false }

  var body: some View {
    let baseRev = session.rev
    let shownSources = sources
    Section {
      ForEach(sources.indices, id: \.self) { i in
        let name = sources[i]["name"]?.string ?? ""
        let ref = sources[i]["ref"]?.string ?? ""
        LabeledContent(name) {
          HStack {
            Text("\(NixSources.unlockedRef(ref)) \(NixSources.shortCommit(ref) ?? "")")
              .font(.caption.monospaced()).foregroundStyle(.secondary).help(ref)
            Button("更新") {
              Task {
                let baseSources = shownSources
                guard let r = await resolve(NixSources.unlockedRef(ref), baseRev: baseRev) else { return }
                await save(baseSources.map { source in
                  guard source["name"]?.string == name else { return source }
                  var updated = source
                  updated["ref"] = .string(r.ref)
                  return updated
                })
              }
            }
            Button("削除", role: .destructive) {
              Task { revisionEdit.reload(baseRev); await save(shownSources.filter { $0["name"]?.string != name }) }
            }
          }
          .disabled(working || !session.isEditable)
        }
      }
      HStack {
        TextField("flake", text: $address, prompt: Text(verbatim: "github:owner/repo"))
          .onChange(of: address) { found = nil }
        Button("取得") {
          let ref = address.trimmingCharacters(in: .whitespaces)
          Task {
            if let r = await resolve(ref, baseRev: baseRev) { found = Found(name: NixSources.sourceName(ref), ref: r.ref, packages: r.packages) }
          }
        }
        .disabled(working || address.trimmingCharacters(in: .whitespaces).isEmpty || !session.isEditable)
        if working { ProgressView().controlSize(.small) }
      }
      if let f = found {
        TextField("名前", text: Binding(get: { found?.name ?? "" }, set: { found?.name = $0 }))
        LabeledContent("コミット") { Text(NixSources.shortCommit(f.ref) ?? f.ref).font(.caption.monospaced()).help(f.ref) }
        LabeledContent("package") {
          Text(f.packages.isEmpty ? String(localized: "この環境の package はありません") : f.packages.joined(separator: ", "))
            .font(.caption.monospaced())
        }
        HStack {
          Button("追加") {
            Task {
              await save(shownSources + [entry(f.name, f.ref)])
              found = nil
              address = ""
            }
          }
          .disabled(taken || f.name.wholeMatch(of: #/[A-Za-z_][A-Za-z0-9_-]*/#) == nil)
          if taken { Text("同じ名前があります").font(.caption).foregroundStyle(.secondary) }
        }
      }
      RevisionEditNotice(edit: revisionEdit, text: { JSONValue.array(pending ?? []).serialized() },
                         overwrite: { if let pending { Task { await save(pending, overwrite: true) } } },
                         reload: { pending = nil; revisionEdit.reload(session.rev) })
      if let message { Text(message).font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true) }
    } header: {
      Text("外部ソース")
    } footer: {
      Text("追加した flake の package は、Card の default.nix で inputs.sources.ciao.packages.ciao のように使えます。コミットと hash で固定します。追加・更新・削除はすぐに保存されます。flake の式は Card の隔離の外で評価するので、信頼できるものだけを追加してください。")
        .font(.caption).foregroundStyle(.secondary)
    }
  }
}

private struct GeneralProjectSettings: View {
  let session: ProjectSession
  @State private var name = ""
  @State private var pkgs = ""
  @State private var message: String?
  @State private var revisionEdit = RevisionEdit()

  var body: some View {
    let changed = name != (session.project["name"]?.string ?? "") || pkgs != (session.project["pkgsPath"]?.string ?? "")
    VStack(alignment: .leading) {
      Form {
        TextField("プロジェクト名", text: $name)
        TextField("実行環境（Nixpkgs）", text: $pkgs, prompt: Text(AppModel.shared.nixpkgsPin ?? "tsugumi:default"))
        if pkgs.isEmpty || pkgs == "tsugumi:default" {
          Label("Nixpkgs が固定されていません。アプリを更新すると、このプロジェクトの Card のパッケージも変わります。", systemImage: "exclamationmark.triangle")
            .font(.caption).foregroundStyle(.orange)
        }
        if let pin = AppModel.shared.nixpkgsPin, pkgs != pin {
          Button(pkgs.isEmpty || pkgs == "tsugumi:default" ? String(localized: "このアプリの Nixpkgs に固定")
                                                           : String(localized: "このアプリの Nixpkgs に更新")) { pkgs = pin }
        }
        Text("このプロジェクトが使う Nixpkgs です（版の一部として残ります）。変更すると Card の再ビルドが必要になります。")
          .font(.caption).foregroundStyle(.secondary)
        LabeledContent("場所", value: session.identity.url.path)
        NixSourceSettings(session: session)
      }
      .formStyle(.grouped)
      RevisionEditNotice(edit: revisionEdit, text: { "name: \(name)\npkgsPath: \(pkgs)" },
                         overwrite: { Task { await save(overwrite: true) } }, reload: { load() })
        .padding(.horizontal, 12)
      HStack {
        if let message { Text(message).font(.callout).foregroundStyle(.secondary) }
        Spacer()
        Button("保存") {
          Task { await save() }
        }
        .disabled(!changed || revisionEdit.applying || !session.isEditable)
        .keyboardShortcut(.defaultAction)
      }
      .padding(12)
    }
    .onAppear { load() }
  }

  private func load() {
    name = session.project["name"]?.string ?? ""
    pkgs = session.project["pkgsPath"]?.string ?? ""
    revisionEdit.reload(session.rev)
  }

  private func save(overwrite: Bool = false) async {
    let sentName = name, sentPkgs = pkgs
    if await revisionEdit.apply(session, overwrite: overwrite, { s in
      s.project["name"] = .string(sentName)
      s.project["pkgsPath"] = .string(sentPkgs.isEmpty ? "tsugumi:default" : sentPkgs)
    }) {
      if pkgs == sentPkgs { pkgs = sentPkgs.isEmpty ? "tsugumi:default" : sentPkgs }
      message = String(localized: "保存しました。")
    }
  }
}

/// How the project's Cards are laid out (layout.json, applied at once).
private struct CanvasProjectSettings: View {
  let session: ProjectSession

  var body: some View {
    Form {
      Picker("流れの向き", selection: Binding(get: { session.flow }, set: { session.setFlow($0) })) {
        ForEach(FlowDirection.allCases) { Text($0.label).tag($0) }
      }
      .help("このプロジェクトの Card を並べた向きです（layout.json に保存され、すぐ反映されます）。線の引き方と「整列」がこの向きに合わせます。")
      Label("変えると Card の配置と線が合わなくなることがあります（「整列」で並べ直せます）。", systemImage: "exclamationmark.triangle")
        .font(.caption).foregroundStyle(.orange)
    }
    .formStyle(.grouped)
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
  @State private var revisionEdit = RevisionEdit()
  @State private var pendingSaving = true

  private var saving: Binding<Bool> {
    let baseRev = session.rev
    return Binding(
      get: { session.project["saveRunHistory"]?.bool ?? true },
      set: { on in
        revisionEdit.reload(baseRev)
        pendingSaving = on
        Task { await saveHistorySetting() }
      })
  }

  private func saveHistorySetting(overwrite: Bool = false) async {
    _ = await revisionEdit.apply(session, overwrite: overwrite) { $0.project["saveRunHistory"] = .bool(pendingSaving) }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      Form {
        Section {
          Toggle("実行履歴をプロジェクトに保存する", isOn: saving).disabled(!session.isEditable)
          Text("実行履歴は VM の中に保存します。オンのときは、終わった実行の記録（実行したコードのコミット・値・型・ログ・成果物の一覧）をプロジェクトの .tsugumi/runs.sqlite にも書き出します。コピーしたり別の Mac に移したりしたプロジェクトは、開いたときに VM にない実行をここから読み込みます。オフにすると、新しい実行は VM の中だけに保存します。")
            .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        Section("削除") {
          Picker("対象", selection: $scope) {
            Text("指定した日時より前に終わった実行").tag(Scope.before)
            Text("すべての実行").tag(Scope.all)
          }
          if scope == .before { DatePicker("日時", selection: $before) }
          Text("実行の記録・値・ログと、実行ごとのファイル（成果物など、.tsugumi/runs/<run>）を、VM 内と .tsugumi/runs.sqlite の両方から削除します。対象の実行の scratch も削除します。最後の入力（その Card だけを再実行するための入力）は残します。実行中は削除できません。")
            .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
      }
      .formStyle(.grouped)
      RevisionEditNotice(edit: revisionEdit, text: { "saveRunHistory: \(pendingSaving)" },
                         overwrite: { Task { await saveHistorySetting(overwrite: true) } },
                         reload: { revisionEdit.reload(session.rev) })
        .padding(.horizontal, 12)
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

/// A selection can include an unreadable folder, whose bytes do not count as zero.
struct ScratchSelection {
  var paths: Set<String> = []
  func rows(_ candidates: [Tsugumi_RPC_RunScratchUsageT]) -> [Tsugumi_RPC_RunScratchUsageT] { candidates.filter { paths.contains($0.path) } }
  func knownBytes(_ candidates: [Tsugumi_RPC_RunScratchUsageT]) -> UInt64 { rows(candidates).filter { !$0.sizeUnknown }.reduce(0) { $0 + $1.bytes } }
  func hasUnknown(_ candidates: [Tsugumi_RPC_RunScratchUsageT]) -> Bool { rows(candidates).contains { $0.sizeUnknown } }
}

private struct CacheSettings: View {
  let session: ProjectSession
  @Environment(AppModel.self) private var model
  @State private var scratchSelection: [String] = []
  @State private var selectedScratch = ScratchSelection()
  @State private var confirmScratch = false
  @State private var scratchResult: Tsugumi_RPC_RunScratchDeleteResultT?
  private var scratchCandidates: [Tsugumi_RPC_RunScratchUsageT] {
    (session.cacheUsage?.projects.compactMap { $0 }.first {
      $0.project == session.cacheUsage?.currentProject
    }?.runScratch.compactMap { $0 } ?? []).filter {
      $0.path != "runs/" + (session.cacheUsage?.protectedRun ?? "")
    }
  }
  @State private var clearTarget: ProjectSession?
  @State private var result: Tsugumi_RPC_ProjectCacheClearResultT?
  @State private var working = false
  @State private var message: String?
  @State private var confirm = false

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("このプロジェクトの Card のビルド結果の登録（Nix の GC root）と、作業用のビルドファイルを削除します。共有 Nix store にある実体は消さず、アプリ設定の「共有 store を掃除」で登録のないものとして削除されます。文書・実行履歴・保持した成果物は削除しません。次回の実行では評価し直し、store に残っていればビルドせずに使います。")
        .fixedSize(horizontal: false, vertical: true)
      Text("実行中・SDK 準備中は削除できません。").font(.callout).foregroundStyle(.secondary)
      GroupBox("キャッシュ使用量") {
        ScrollView {
          VStack(alignment: .leading, spacing: 8) {
            LabeledContent("合計（共有パスは一度だけ計上）", value: bytes(session.cacheUsage?.buildBytes))
            if let available = session.cacheUsage?.storeAvailableBytes {
              LabeledContent("store ディスクの空き", value: bytes(available))
            }
            if session.cacheUsage?.large == true {
              Text("ビルドキャッシュまたは実行の scratch が大きくなっています。内容を確認して、削除するものを選んでください。")
            }
            Text("ビルドキャッシュは NAR サイズの概算、scratch はリンクをたどらずに合計したファイルサイズです。共有する依存関係は各プロジェクトにも含まれるため、ビルドの内訳の和と合計は異なります。最大 1 分ごとに再計測します。")
              .font(.caption).foregroundStyle(.secondary)
            if let measured = session.cacheUsage?.measuredAt {
              LabeledContent("計測日時", value: Date(timeIntervalSince1970: Double(measured) / 1000).formatted())
            }
            if let error = session.cacheUsage?.error { Text(error).font(.caption).textSelection(.enabled) }
            ForEach((session.cacheUsage?.projects ?? []).compactMap { $0 }, id: \.project) { project in
              let target = project.project == session.cacheUsage?.currentProject ? session : model.sessions.values.first { $0.identity.id == project.project }
              Divider()
              HStack {
                Text(project.name).font(.headline)
                Spacer()
                Button("削除", role: .destructive) { clearTarget = target; confirm = true }
                  .disabled(target == nil || target?.connected != true || target?.runActive == true || working)
              }
              LabeledContent("ビルドキャッシュ", value: bytes(project.buildBytes))
              LabeledContent("実行の scratch（合計）", value: bytes(project.scratchBytes))
              LabeledContent("root の最終使用", value: project.lastUsed.map { Date(timeIntervalSince1970: Double($0) / 1000).formatted() } ?? String(localized: "不明"))
              if target == nil { Text("削除するにはこのプロジェクトを開いてください。").font(.caption).foregroundStyle(.secondary) }
              if let error = project.error { Text(error).font(.caption).textSelection(.enabled) }
            }
          }
          .frame(maxWidth: .infinity, alignment: .leading)
        }
      }
      Text("過去の scratch は自動では削除しません。削除するフォルダと容量を確認してください。最新の実行の scratch は再実行のため残します。保持した記録・成果物・最後の入力は別に残ります。")
        .font(.caption).foregroundStyle(.secondary)
      GroupBox("このプロジェクトの過去の scratch") {
        VStack(alignment: .leading, spacing: 4) {
          ForEach(scratchCandidates, id: \.path) { item in
            HStack {
              Toggle(item.path, isOn: Binding(get: { selectedScratch.paths.contains(item.path) }, set: {
                if $0 { selectedScratch.paths.insert(item.path) } else { selectedScratch.paths.remove(item.path) }
              }))
              Spacer()
              Text(item.sizeUnknown ? String(localized: "不明") : bytes(item.bytes))
            }
          }
          LabeledContent("削除対象の合計", value: bytes(selectedScratch.knownBytes(scratchCandidates)))
          if selectedScratch.hasUnknown(scratchCandidates) { Text("合計は容量不明のフォルダを除きます。").font(.caption).foregroundStyle(.secondary) }
          Button("選択した scratch を削除…", role: .destructive) {
            scratchSelection = selectedScratch.rows(scratchCandidates).map(\.path)
            confirmScratch = true
          }.disabled(working || session.runActive || !session.connected || selectedScratch.rows(scratchCandidates).isEmpty)
          if let r = scratchResult {
            LabeledContent("解放", value: bytes(r.freedBytes))
            if !r.retained.isEmpty {
              Text(String(localized: "削除できず残ったフォルダ") + ": " + r.retained.compactMap { $0 }.joined(separator: ", "))
            }
          }
        }.frame(maxWidth: .infinity, alignment: .leading)
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
      Spacer()
      HStack {
        Spacer()
        if working { ProgressView().controlSize(.small) }
        Button("削除", role: .destructive) { clearTarget = session; confirm = true }
          .disabled(working || session.runActive || !session.connected)
      }
    }
    .padding(16)
    .confirmationDialog("選択した過去の scratch を削除しますか？", isPresented: $confirmScratch) {
      Button("削除", role: .destructive) {
        working = true
        message = nil
        Task {
          do { scratchResult = try await session.deleteRunScratch(paths: scratchSelection); selectedScratch.paths = [] }
          catch { message = String(localized: "削除できませんでした: \(String(describing: error))") }
          working = false
        }
      }
      Button("キャンセル", role: .cancel) {}
    }
    .confirmationDialog("「\((clearTarget ?? session).scopeName([]))」のビルドキャッシュを削除しますか？", isPresented: $confirm) {
      Button("削除", role: .destructive) { clear() }
      Button("キャンセル", role: .cancel) {}
    }
  }

  private func bytes(_ value: UInt64?) -> String {
    value.map { ByteCountFormatter.string(fromByteCount: Int64(clamping: $0), countStyle: .file) }
      ?? String(localized: "計測中または取得できません")
  }

  private func clear() {
    working = true
    message = nil
    Task {
      do {
        result = try await (clearTarget ?? session).clearBuildCache()
        await session.refreshCacheUsage()
      } catch { message = String(localized: "削除できませんでした: \(String(describing: error))") }
      working = false
    }
  }
}
