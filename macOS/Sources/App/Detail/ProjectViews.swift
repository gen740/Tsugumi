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
  @State private var parseError: String?
  @State private var message: String?

  var body: some View {
    HSplitView {
      VStack(spacing: 0) {
        HStack {
          TextField("検索", text: $search).textFieldStyle(.roundedBorder)
          Menu {
            ForEach(DefinitionTemplate.allCases) { t in
              Button(t.label) { selected = path(session.addCard(template: t, in: [], at: CGPoint(x: 40, y: 40))).first }
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
              Text(id)
              Text(definitionKind(def).label + (session.unplacedDefinitions.contains(id) ? String(localized: " · 未配置") : ""))
                .font(.caption).foregroundStyle(.secondary)
            }
          }
          .tag(id)
          .draggable(DefinitionDrag.prefix + id)
        }
      }
      .frame(minWidth: 240, idealWidth: 280)
      detail.frame(minWidth: 420)
    }
    .onChange(of: selected) { load() }
    .onAppear { if selected == nil, let initial, session.definitions[initial] != nil { selected = initial } }
    .navigationTitle("Card の定義")
  }

  private var ids: [String] {
    session.definitions.keys.sorted().filter { id in
      search.isEmpty || id.localizedCaseInsensitiveContains(search) ||
        (session.definitionMeta[id]?["description"]?.string ?? "").localizedCaseInsensitiveContains(search)
    }
  }

  @ViewBuilder private var detail: some View {
    if let id = selected, session.definitions[id] != nil {
      let unapplied = cardText != baseCard || metaText != baseMeta
      VStack(alignment: .leading, spacing: 8) {
        HStack {
          Text(id).font(.title2.bold())
          Spacer()
          if unapplied { Label("未適用の変更", systemImage: "pencil.circle.fill").foregroundStyle(.orange) }
        }
        Text("Card「\(id)」の定義（definitions/\(id)/）です。").font(.callout).foregroundStyle(.secondary)
        Text("_card.json（設定・params・入力）").font(.headline)
        CodeEditor(text: $cardText, language: "json").frame(minHeight: 180)
        Text("_meta.json（説明・計画・追加コンテキスト、任意）").font(.headline)
        CodeEditor(text: $metaText, language: "json").frame(minHeight: 80, maxHeight: 140)
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
          Button("再読込") { load() }
          Button("適用") { Task { await apply(id) } }.disabled(!unapplied || !session.isEditable)
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
    parseError = nil
    message = nil
  }

  private func place(_ id: String) {
    let k = session.placeDefinition(id, at: CGPoint(x: 40, y: 40))
    message = String(localized: "\(k) を Workflow に配置しました（未保存）。")
  }

  private func apply(_ id: String) async {
    do {
      let card = try JSONValue.parse(cardText)
      let meta = metaText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : try JSONValue.parse(metaText)
      parseError = nil
      session.edit { $0.setDefinition(id, card: card, meta: meta) }
      await session.save()
      message = session.saveError.map { String(localized: "保存できませんでした: \($0)") } ?? String(localized: "適用しました（revision \(session.rev)）。")
      load()
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
    case .databases: DatabaseSettings(session: session)
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

  private var sources: [JSONValue] { session.project["nixSources"]?.array ?? [] }
  private func entry(_ name: String, _ ref: String) -> JSONValue {
    .object([("name", .string(name)), ("type", .string("flake")), ("ref", .string(ref))])
  }

  private func resolve(_ ref: String) async -> (ref: String, packages: [String])? {
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

  private func save(_ list: [JSONValue]) async {
    session.edit { $0.project["nixSources"] = .array(list) }
    await session.save()
    message = session.saveError.map { String(localized: "保存できませんでした: \($0)") }
  }

  private var taken: Bool { found.map { f in sources.contains { $0["name"]?.string == f.name } } ?? false }

  var body: some View {
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
                guard let r = await resolve(NixSources.unlockedRef(ref)) else { return }
                await save(sources.map { $0["name"]?.string == name ? entry(name, r.ref) : $0 })
              }
            }
            Button("削除", role: .destructive) {
              Task { await save(sources.filter { $0["name"]?.string != name }) }
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
            if let r = await resolve(ref) { found = Found(name: NixSources.sourceName(ref), ref: r.ref, packages: r.packages) }
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
              await save(sources + [entry(f.name, f.ref)])
              found = nil
              address = ""
            }
          }
          .disabled(taken || f.name.wholeMatch(of: #/[A-Za-z_][A-Za-z0-9_-]*/#) == nil)
          if taken { Text("同じ名前があります").font(.caption).foregroundStyle(.secondary) }
        }
      }
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

  var body: some View {
    let changed = name != (session.project["name"]?.string ?? "") || pkgs != (session.project["pkgsPath"]?.string ?? "")
    VStack(alignment: .leading) {
      Form {
        TextField("プロジェクト名", text: $name)
        TextField("実行環境（Nixpkgs）", text: $pkgs, prompt: Text(NixpkgsPin.current ?? "tsugumi:default"))
        if pkgs.isEmpty || pkgs == "tsugumi:default" {
          Label("Nixpkgs が固定されていません。アプリを更新すると、このプロジェクトの Card のパッケージも変わります。", systemImage: "exclamationmark.triangle")
            .font(.caption).foregroundStyle(.orange)
        }
        if let pin = NixpkgsPin.current, pkgs != pin {
          Button(pkgs.isEmpty || pkgs == "tsugumi:default" ? String(localized: "このアプリの Nixpkgs に固定")
                                                           : String(localized: "このアプリの Nixpkgs に更新")) { pkgs = pin }
        }
        Text("このプロジェクトが使う Nixpkgs です（版の一部として残ります）。変更すると Card の再ビルドが必要になります。")
          .font(.caption).foregroundStyle(.secondary)
        LabeledContent("場所", value: session.identity.url.path)
        NixSourceSettings(session: session)
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
            message = session.saveError ?? String(localized: "保存しました。")
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
      message = session.saveError ?? String(localized: "保存しました。")
      if session.saveError == nil { load() }
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

  private var saving: Binding<Bool> {
    Binding(
      get: { session.project["saveRunHistory"]?.bool ?? true },
      set: { on in
        session.edit { $0.project["saveRunHistory"] = .bool(on) }
        Task {
          await session.save()
          failed = session.saveError != nil
          message = session.saveError.map { String(localized: "保存できませんでした: \($0)") }
        }
      })
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      Form {
        Section {
          Toggle("実行履歴をプロジェクトに保存する", isOn: saving).disabled(!session.isEditable)
          Text("実行履歴は VM の中に保存します。オンのときは、終わった実行の記録（値・型・ログ・成果物の一覧）をプロジェクトの .tsugumi/runs.sqlite にも書き出します。コピーしたり別の Mac に移したりしたプロジェクトは、開いたときに VM にない実行をここから読み込みます。オフにすると、新しい実行は VM の中だけに保存します。")
            .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        Section("削除") {
          Picker("対象", selection: $scope) {
            Text("指定した日時より前に終わった実行").tag(Scope.before)
            Text("すべての実行").tag(Scope.all)
          }
          if scope == .before { DatePicker("日時", selection: $before) }
          Text("実行の記録・値・ログと、実行ごとのファイル（成果物など、.tsugumi/runs/<run>）を、VM 内と .tsugumi/runs.sqlite の両方から削除します。最後の入力（その Card だけを再実行するための入力）は残します。実行中は削除できません。")
            .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
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
  @State private var result: Tsugumi_RPC_ProjectCacheClearResultT?
  @State private var working = false
  @State private var message: String?
  @State private var confirm = false

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("このプロジェクトの Card のビルド結果の登録（Nix の GC root）と、作業用のビルドファイルを削除します。共有 Nix store にある実体は消さず、アプリ設定の「共有 store を掃除」で登録のないものとして削除されます。文書・実行履歴・保持した成果物は削除しません。次回の実行では評価し直し、store に残っていればビルドせずに使います。")
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
      do { result = try await session.clearBuildCache() } catch { message = String(localized: "削除できませんでした: \(String(describing: error))") }
      working = false
    }
  }
}
