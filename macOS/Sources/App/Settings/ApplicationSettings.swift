import AppKit
import SwiftUI
import TsugumiAgent
import TsugumiLauncher

/// App-wide settings; the category on the left switches the content.
struct ApplicationSettings: View {
  enum Category: String, CaseIterable, Identifiable {
    case general, appearance, editor, shortcuts, skills, api, connections
    var id: String { rawValue }
    var title: String {
      switch self {
      case .general: return String(localized: "一般")
      case .appearance: return String(localized: "外観")
      case .editor: return String(localized: "エディター")
      case .shortcuts: return String(localized: "ショートカット")
      case .skills: return "Skills"
      case .api: return "API"
      case .connections: return String(localized: "外部接続")
      }
    }
    var symbol: String {
      switch self {
      case .general: return "gearshape"
      case .appearance: return "paintpalette"
      case .editor: return "text.cursor"
      case .shortcuts: return "keyboard"
      case .skills: return "wand.and.stars"
      case .api: return "key"
      case .connections: return "network"
      }
    }
    /// Where each pane's values are kept, said on the pane.
    var storage: String {
      switch self {
      case .api: return String(localized: "このアプリ（この Mac。API キーは macOS のキーチェーン）")
      case .connections: return String(localized: "表示のみ（開いているプロジェクトの接続先）")
      default: return String(localized: "このアプリ（この Mac のユーザー設定）")
      }
    }
  }

  /// "<category>" for the app's panes, "project.<pane>" for the project's.
  @AppStorage("settings.category") private var stored = Category.general.rawValue
  /// The open project whose settings are shown (several may be open).
  @AppStorage("settings.project") private var projectPath = ""
  @Environment(AppModel.self) private var model

  private var projects: [ProjectSession] { model.sessions.values.sorted { $0.scopeName([]) < $1.scopeName([]) } }
  private var project: ProjectSession? { projects.first { $0.identity.url.path == projectPath } ?? projects.first }
  private var projectPane: ProjectSettingsPane? {
    guard stored.hasPrefix("project."), project != nil else { return nil }
    return ProjectSettingsPane(rawValue: String(stored.dropFirst("project.".count)))
  }
  private var category: Category { Category(rawValue: stored) ?? .general }

  var body: some View {
    // A plain list + content (no split view): switching categories, going back
    // and reopening the window always work, whatever a category is doing.
    HStack(spacing: 0) {
      List(selection: Binding<String?>(get: { projectPane?.selection ?? category.rawValue },
                                       set: { if let v = $0 { stored = v } })) {
        Section("アプリ") {
          ForEach(Category.allCases) { c in
            Label(c.title, systemImage: c.symbol).tag(Optional(c.rawValue))
              .accessibilityIdentifier("settings.category.\(c.title)")
          }
        }
        // The open project's settings, under the app's.
        if let project {
          Section {
            // Row ids are the selection's values: the List selects by them, so a
            // pane named like an app category ("general") never selects that category.
            ForEach(ProjectSettingsPane.allCases, id: \.selection) { p in
              Label(p.title, systemImage: p.symbol).tag(Optional(p.selection))
            }
          } header: {
            if projects.count > 1 {
              Picker("プロジェクト", selection: Binding(get: { project.identity.url.path }, set: { projectPath = $0 })) {
                ForEach(projects, id: \.identity.id) { s in Text(s.scopeName([])).tag(s.identity.url.path) }
              }
              .labelsHidden().pickerStyle(.menu).controlSize(.small)
              .help("設定を表示するプロジェクト（開いているプロジェクトから選びます）")
            } else {
              VStack(alignment: .leading, spacing: 1) {
                Text("このプロジェクト")
                Text(project.scopeName([])).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail)
              }
            }
          }
        }
      }
      .listStyle(.sidebar)
      .scrollContentBackground(.hidden)
      .frame(width: 210)
      Divider()
      VStack(alignment: .leading, spacing: 0) {
        if let pane = projectPane, let project {
          StorageNote(text: pane.storage(project))
          ProjectSettingsContent(pane: pane, session: project)
            .id("\(project.identity.id).\(pane.rawValue)")
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        } else {
          StorageNote(text: category.storage)
          Group {
            switch category {
            case .general: GeneralSettings()
            case .appearance: AppearanceSettings()
            case .editor: EditorSettings()
            case .shortcuts: ShortcutSettings()
            case .skills: SkillSettings()
            case .api: APISettings()
            case .connections: ConnectionSettings()
            }
          }
          .id(category)
          .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
      }
    }
    // The pane is the window's title (with the project for its panes), on the
    // window's own background, like the Card settings windows.
    .navigationTitle(projectPane.map { "\($0.title) — \(project?.scopeName([]) ?? "")" } ?? category.title)
    .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
    .containerBackground(Color(nsColor: .windowBackgroundColor), for: .window)
  }
}

/// Where the values of a settings pane are kept.
private struct StorageNote: View {
  let text: String

  var body: some View {
    Label(String(localized: "保存先: ") + text, systemImage: "externaldrive")
      .font(.caption).foregroundStyle(.secondary)
      .padding(.horizontal, 20).padding(.top, 10)
  }
}

/// The settings of one project: kept in the project (or its environment).
enum ProjectSettingsPane: String, CaseIterable, Identifiable {
  case general, canvas, databases, history, cache
  var id: String { rawValue }
  /// The settings window's selection for this pane.
  var selection: String { "project." + rawValue }
  var title: String {
    switch self {
    case .general: return String(localized: "名前と実行環境")
    case .canvas: return "Canvas"
    case .databases: return String(localized: "データベース接続先")
    case .history: return String(localized: "実行履歴")
    case .cache: return String(localized: "ビルドキャッシュ")
    }
  }
  var symbol: String {
    switch self {
    case .general: return "folder.badge.gearshape"
    case .canvas: return "rectangle.3.group"
    case .databases: return "cylinder.split.1x2"
    case .history: return "clock.arrow.circlepath"
    case .cache: return "shippingbox"
    }
  }
  func storage(_ s: ProjectSession) -> String {
    let name = s.identity.url.lastPathComponent
    switch self {
    case .general, .databases: return String(localized: "このプロジェクト（\(name)/tsugumi.json）")
    case .canvas: return String(localized: "このプロジェクト（\(name)/layout.json）")
    case .history: return String(localized: "このプロジェクトの実行環境（書き出しは \(name)/.tsugumi/runs.sqlite）")
    case .cache: return String(localized: "このプロジェクトの実行環境（VM の中）")
    }
  }

  /// Opens the settings window at a project's pane.
  @MainActor static func open(_ s: ProjectSession, _ pane: ProjectSettingsPane = .general, with openSettings: OpenSettingsAction) {
    UserDefaults.standard.set(pane.selection, forKey: "settings.category")
    UserDefaults.standard.set(s.identity.url.path, forKey: "settings.project")
    openSettings()
  }
}

private struct GeneralSettings: View {
  @Environment(AppPreferences.self) private var prefs

  var body: some View {
    @Bindable var prefs = prefs
    Form {
      // Explanations on hover, not as text under each control.
      Picker("言語", selection: $prefs.language) {
        ForEach(AppPreferences.Language.allCases) { Text($0.label).tag($0) }
      }
      .help("言語の変更はアプリの再起動後に反映されます。")
      Stepper("並列実行数: \(prefs.maxConcurrency)", value: $prefs.maxConcurrency, in: 1...64)
        .help("別々の Card を同時に実行する上限です（同じ Card は常に直列）。次の実行から使われます。")
      Picker("チャットの送信キー", selection: $prefs.sendKey) {
        ForEach(AppPreferences.SendKey.allCases) { Text($0.label).tag($0) }
      }
      Picker("Tsugumi のアニメーション", selection: $prefs.tsugumiMotion) {
        ForEach(AppPreferences.Motion.allCases) { Text($0.label).tag($0) }
      }
      .help("Home と読み込み中の Tsugumi を動かすかどうか。デフォルトは macOS の「視差効果を減らす」に合わせます。")
      UpdateSettings()
    }
    .formStyle(.grouped)
  }
}

/// The version, and updates from the GitHub releases (Updater).
private struct UpdateSettings: View {
  @Bindable private var updater = Updater.shared

  var body: some View {
    Section("アップデート") {
      LabeledContent("版", value: updater.build.isEmpty ? updater.currentVersion : "\(updater.currentVersion) (\(updater.build))")
      if let why = updater.unavailable {
        Text(why).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      } else {
        Toggle("アップデートを自動的に確認する", isOn: $updater.automatic)
          .help("起動後と 6 時間ごとに GitHub の最新リリースを確認し、新しい版があればダウンロードして、次に終了したときに更新します。")
        HStack {
          status
          Spacer()
          if case .ready = updater.state {
            Button("再起動して更新") { updater.installAndRelaunch() }
          } else {
            Button("今すぐ確認") { Task { await updater.check() } }.disabled(updater.busy)
          }
        }
      }
      Link("リリースの一覧（GitHub）", destination: Updater.releasesPage).font(.callout)
    }
  }

  @ViewBuilder private var status: some View {
    switch updater.state {
    case .idle:
      if updater.automatic { Text("まもなく確認します").foregroundStyle(.secondary) } else { Text("未確認").foregroundStyle(.secondary) }
    case .checking:
      HStack(spacing: 6) { ProgressView().controlSize(.small); Text("確認しています") }
    case .upToDate:
      Text("最新の版です（\(updater.lastCheck.map { $0.formatted(date: .omitted, time: .shortened) } ?? "") に確認）").foregroundStyle(.secondary)
    case .downloading(let v):
      HStack(spacing: 6) { ProgressView().controlSize(.small); Text("\(v) をダウンロードしています") }
    case .ready(let v):
      Text("\(v) の準備ができました。次に終了したときに更新します。")
    case .failed(let m):
      Text(m).font(.caption).foregroundStyle(.red).textSelection(.enabled).lineLimit(3)
    }
  }
}

private struct AppearanceSettings: View {
  @Environment(AppPreferences.self) private var prefs
  @State private var items = PreviewSamples.items()

  var body: some View {
    @Bindable var prefs = prefs
    HStack(alignment: .top, spacing: 0) {
      Form {
        Picker("テーマ", selection: $prefs.theme) {
          ForEach(AppPreferences.Theme.allCases) { Text($0.label).tag($0) }
        }
        Picker("新しいプロジェクトの流れの向き", selection: $prefs.flowDirection) {
          ForEach(FlowDirection.allCases) { Text($0.label).tag($0) }
        }
        .help("新しく作るプロジェクトの Card を並べる向きです。開いているプロジェクトの向きは、設定の「このプロジェクト」→「Canvas」で変えます。")
        Toggle("グリッドを表示（移動時に吸着）", isOn: $prefs.showGrid)
        Slider(value: $prefs.gridSize, in: 10...60, step: 5) { Text("グリッド間隔 \(Int(prefs.gridSize))") }
        Toggle("実行中の接続線を動かす", isOn: $prefs.animateLines)
        Picker("作業中の Agent", selection: $prefs.agentAvatar) {
          ForEach(AgentAvatar.allCases) { Text($0.label).tag($0) }
        }
        .help("Agent が作業している間、Canvas の上でその様子を見せる姿です。表示しなくても、Card や接続の変化は見えます。")
      }
      .formStyle(.grouped)
      .frame(width: 300)
      VStack(alignment: .leading, spacing: 4) {
        Text("プレビュー（選択・ドラッグ・折りたたみ・拡大縮小を試せます。プロジェクトは変わりません）")
          .font(.caption).foregroundStyle(.secondary)
        CanvasSurface(items: $items, wires: PreviewSamples.wires, flow: prefs.flowDirection, grid: prefs.showGrid,
                      gridSize: prefs.gridSize, animateWires: prefs.animateLines)
          .clipShape(RoundedRectangle(cornerRadius: 8))
          .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.secondary.opacity(0.3)))
      }
      .padding(12)
    }
  }
}

private struct EditorSettings: View {
  @Environment(AppPreferences.self) private var prefs
  @State private var language = "python"
  @State private var sample = EditorSettings.samples["python"]!

  static let samples: [String: String] = [
    "python": """
    # Collatz steps for one starting value
    def steps(n: int, limit: int = 1000) -> list[int]:
        \"\"\"Returns the sequence down to 1.\"\"\"
        seen = [n]
        while n != 1 and len(seen) < limit:
            if n % 2 == 0:
                n = n // 2
            else:
                n = 3 * n + 1
            seen.append(n)
        return seen

    print("27 ->", len(steps(27)) - 1, "steps")

    """,
    "cpp": """
    // Collatz steps for one starting value
    #include <cstdint>
    #include <vector>

    std::vector<uint64_t> steps(uint64_t n, size_t limit = 1000) {
      std::vector<uint64_t> seen{n};
      while (n != 1 && seen.size() < limit) {
        if (n % 2 == 0) {
          n /= 2;
        } else {
          n = 3 * n + 1;  // may overflow for large n
        }
        seen.push_back(n);
      }
      return seen;
    }

    """,
    "nix": """
    # A Python Card with one extra package
    { pkgs, inputs, params }:
    {
      run = inputs.tsugumi.pythonCard {
        src = ./src;
        packages = ps: [ ps.numpy ];
      };
      exports = { scale = x: x * 2; };
    }

    """,
  ]

  var body: some View {
    @Bindable var prefs = prefs
    HStack(alignment: .top, spacing: 0) {
      Form {
        Picker("インデント幅", selection: $prefs.indentWidth) {
          Text("2").tag(2)
          Text("4").tag(4)
          Text("8").tag(8)
        }
        .pickerStyle(.segmented)
        Toggle("インデントガイドを表示", isOn: $prefs.showIndentGuides)
        Stepper("文字サイズ: \(Int(prefs.editorFontSize))", value: $prefs.editorFontSize, in: 9...24)
        Picker("配色", selection: $prefs.editorTheme) {
          ForEach(EditorTheme.allCases) { Text($0.label).tag($0) }
        }
      }
      .formStyle(.grouped)
      .frame(width: 280)
      VStack(alignment: .leading, spacing: 6) {
        Picker("", selection: $language) {
          Text("Python").tag("python")
          Text("C++").tag("cpp")
          Text("Nix").tag("nix")
        }
        .pickerStyle(.segmented).labelsHidden().frame(width: 240)
        .onChange(of: language) { sample = Self.samples[language] ?? "" }
        // The same editor as Card settings: what you see here is what you get there.
        CodeEditor(text: $sample, language: language)
          .id("\(prefs.editorFontSize)-\(prefs.indentWidth)-\(prefs.showIndentGuides)-\(prefs.editorTheme.rawValue)")
          .clipShape(RoundedRectangle(cornerRadius: 6))
          .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(.secondary.opacity(0.3)))
        Text("サンプルは編集して試せます（保存されません）。Tab はインデント幅の空白を入れます。")
          .font(.caption).foregroundStyle(.secondary)
      }
      .padding(12)
    }
  }
}

private struct ShortcutSettings: View {
  @Environment(AppPreferences.self) private var prefs
  @State private var recording: ShortcutAction?

  var body: some View {
    VStack(alignment: .leading) {
      Table(ShortcutAction.allCases) {
        TableColumn("操作") { a in Text(a.title) }
        TableColumn("キー") { a in
          HStack {
            Text(recording == a ? String(localized: "キーを押してください…") : prefs.shortcut(a).label).monospaced()
              .foregroundStyle(recording == a ? Color.accentColor : .primary)
            if let c = prefs.conflict(prefs.shortcut(a), except: a) {
              Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow).help("「\(c.title)」と重複しています")
            }
          }
        }
        TableColumn("") { a in
          HStack {
            Button(recording == a ? "キャンセル" : "変更") { recording = recording == a ? nil : a }
            Button("デフォルト") { prefs.shortcuts[a] = nil }.disabled(prefs.shortcuts[a] == nil)
          }
        }
      }
      Text("変更はメニューに反映されます（反映されない場合はウインドウを開き直してください）。").font(.caption).foregroundStyle(.secondary)
    }
    .padding()
    .background(KeyRecorder(active: recording != nil) { binding in
      if let a = recording { prefs.shortcuts[a] = binding }
      recording = nil
    })
  }
}

/// Captures the next key press with modifiers (while recording a shortcut).
private struct KeyRecorder: NSViewRepresentable {
  let active: Bool
  let onKey: (KeyBinding) -> Void

  func makeNSView(context: Context) -> NSView { RecorderView() }
  func updateNSView(_ v: NSView, context: Context) {
    guard let v = v as? RecorderView else { return }
    v.onKey = onKey
    v.active = active
    if active { DispatchQueue.main.async { v.window?.makeFirstResponder(v) } }
  }

  final class RecorderView: NSView {
    var onKey: ((KeyBinding) -> Void)?
    var active = false
    override var acceptsFirstResponder: Bool { active }
    override func keyDown(with e: NSEvent) {
      guard active, let ch = e.charactersIgnoringModifiers?.lowercased(), !ch.isEmpty else { return super.keyDown(with: e) }
      let f = e.modifierFlags
      guard f.contains(.command) || f.contains(.control) || f.contains(.option) else { return }
      onKey?(KeyBinding(key: ch, command: f.contains(.command), option: f.contains(.option), shift: f.contains(.shift),
                        control: f.contains(.control)))
    }
  }
}

private struct SkillSettings: View {
  @Environment(AppPreferences.self) private var prefs
  @State private var query = ""
  @State private var state = LoadState.idle
  @State private var found: [Skill] = []

  enum LoadState: Equatable {
    case idle, loading, loaded, failed(String)
  }

  var body: some View {
    @Bindable var prefs = prefs
    VStack(alignment: .leading, spacing: 8) {
      Text("チャットで `$名前` として使う Skill のフォルダです。各フォルダの `<名前>/SKILL.md` または `<名前>.md` を読み込みます。プロジェクトの `skills/` も常に読み込みます。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      GroupBox("フォルダ") {
        VStack(alignment: .leading, spacing: 4) {
          if prefs.skillFolders.isEmpty {
            Text("フォルダはありません").foregroundStyle(.secondary).frame(maxWidth: .infinity, alignment: .leading)
          }
          ForEach($prefs.skillFolders) { $f in
            HStack {
              Toggle("", isOn: $f.enabled).labelsHidden().toggleStyle(.checkbox)
              Image(systemName: FileManager.default.fileExists(atPath: f.path) ? "folder" : "folder.badge.questionmark")
                .foregroundStyle(FileManager.default.fileExists(atPath: f.path) ? Color.secondary : Color.orange)
              Text(f.path).lineLimit(1).truncationMode(.middle).help(f.path)
              Spacer()
              Button { prefs.skillFolders.removeAll { $0.path == f.path } } label: { Image(systemName: "minus.circle") }
                .buttonStyle(.borderless).help("一覧から外す")
            }
          }
          HStack {
            Button("フォルダを追加…") { add() }
            Button("再読込") { load() }.disabled(state == .loading)
          }
        }
      }
      HStack {
        TextField("Skill を検索", text: $query).textFieldStyle(.roundedBorder)
        if state == .loading { ProgressView().controlSize(.small) }
      }
      Group {
        switch state {
        case .failed(let m):
          Label(m, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
        case .loading where found.isEmpty:
          Text("読み込み中…").foregroundStyle(.secondary)
        default:
          let shown = found.filter { query.isEmpty || $0.name.localizedCaseInsensitiveContains(query) || $0.description.localizedCaseInsensitiveContains(query) }
          if shown.isEmpty {
            Text(found.isEmpty ? "Skill はまだありません" : "一致する Skill はありません").foregroundStyle(.secondary)
          } else {
            List(shown) { sk in
              VStack(alignment: .leading, spacing: 1) {
                Text("$" + sk.name).font(.callout.monospaced())
                Text("\(sk.description) · \(sk.source)").font(.caption).foregroundStyle(.secondary).lineLimit(1)
              }
            }
            .listStyle(.inset)
          }
        }
      }
      .frame(maxHeight: .infinity, alignment: .top)
    }
    .padding(16)
    .task(id: prefs.skillFolders) { load() }
  }

  private func add() {
    let p = NSOpenPanel()
    p.canChooseDirectories = true
    p.canChooseFiles = false
    guard p.runModal() == .OK, let url = p.url, !prefs.skillFolders.contains(where: { $0.path == url.path }) else { return }
    prefs.skillFolders.append(SkillFolder(path: url.path))
  }

  /// Reads the folders off the main thread; a slow or missing folder never blocks the window.
  private func load() {
    let folders = prefs.skillFolders.filter(\.enabled)
    state = .loading
    Task {
      let result = await Task.detached(priority: .userInitiated) { () -> (skills: [Skill], missing: [String]) in
        var out: [Skill] = []
        var missing: [String] = []
        for f in folders {
          let url = URL(fileURLWithPath: f.path)
          if !FileManager.default.fileExists(atPath: url.path) { missing.append(f.path); continue }
          out += loadSkills(in: url, source: url.lastPathComponent)
        }
        return (out, missing)
      }.value
      found = result.skills
      state = result.missing.isEmpty ? .loaded : .failed(String(localized: "見つからないフォルダ: ") + result.missing.joined(separator: ", "))
      for chat in AppModel.shared.openChats { chat.reloadSkills() }
    }
  }
}

/// Model connections. API keys go to the Keychain only (one item per
/// provider); they are never written to project files, settings or logs.
private struct APISettings: View {
  @Environment(AppPreferences.self) private var prefs
  @AppStorage("agent.provider") private var provider = "anthropic"
  @AppStorage("agent.model") private var anthropicModel = ProviderConfig.anthropic.model
  @AppStorage("agent.model.openai") private var openAIModel = ProviderConfig.openAI.model
  @AppStorage("agent.fixturePort") private var port = 8765
  @AppStorage(AgentSettings.webSearchKey) private var webSearch = false

  var body: some View {
    Form {
      // Every connection can be set up; this picks the one the chat and Tsugumi use
      // (the chat's model menu switches it too).
      Section {
        Picker("使う接続先", selection: $provider) {
          Text("Anthropic API（API キー）").tag("anthropic")
          Text("OpenAI API（API キー）").tag("openai")
          Text("ChatGPT プラン（Sign in with ChatGPT）").tag("chatgpt")
          Text("ローカル fixture（テスト用・通信なし）").tag("fixture")
        }
        if provider == "fixture" {
          TextField("fixture のポート", value: $port, format: .number)
          Text("起動: python3 tests/fixtures/model_fixture.py --port \(port)").font(.caption.monospaced())
        }
      } footer: {
        Text("API キーと ChatGPT のサインインは、下でそれぞれ同時に設定しておけます。チャットのモデルの一覧からも切り替えられます。")
          .font(.caption).foregroundStyle(.secondary)
      }
      APIKeySection(id: "anthropic", config: .anthropic, model: $anthropicModel)
      APIKeySection(id: "openai", config: .openAI, model: $openAIModel)
      ChatGPTSettings()
      Section {
        ForEach(CardPermission.allCases) { k in
          Toggle(isOn: Binding(get: { prefs.allowedPermissions.contains(k.rawValue) },
                               set: { on in if on { prefs.allowedPermissions.insert(k.rawValue) } else { prefs.allowedPermissions.remove(k.rawValue) } })) {
            Label(k.label, systemImage: k.symbol)
          }
        }
      } header: {
        Text("Agent が確認なしに Card に与えてよい権限")
      } footer: {
        Text("オフの権限を Agent が Card に与えようとすると、保存の前に確認します。あなた自身が Card の設定で変えるときは確認しません。")
          .font(.caption).foregroundStyle(.secondary)
      }
      Section {
        Toggle("Agent が自分の判断で Web を検索する", isOn: $webSearch)
      } header: {
        Text("Web 検索")
      } footer: {
        Text(provider == "fixture"
             ? "ローカル fixture には検索がないため、オンでも使えません。"
             : "オンにすると、検索語が接続先のプロバイダを通して検索サービスへ送られます。Agent は未公開のデータを検索語に入れないよう指示されていますが、確実に防げるものではありません。検索語と出典はチャットに残ります。検索には利用料がかかることがあります。")
          .font(.caption).foregroundStyle(.secondary)
      }
      Section {
        LabeledContent("状態", value: GuideController.shared.available
                       ? String(localized: "右下に表示中（\(AgentSettings.current().name)）")
                       : String(localized: "API キーが未設定のため表示していません"))
        // Pressed, it says what happens next and waits until it has happened.
        Button("初回の案内（デモの勧誘）をもう一度表示") { GuideController.shared.resetIntroduction() }
          .disabled(GuideController.shared.offersDemo)
        if GuideController.shared.offersDemo {
          Label("設定しました。Home に戻ると、ツグミが「何に興味がありますか？」と尋ねます。", systemImage: "checkmark.circle.fill")
            .font(.callout).foregroundStyle(.green)
        }
      } header: {
        Text("ガイドのツグミ")
      } footer: {
        Text("ガイドは使う接続先を使います。使える接続（キー、ChatGPT のサインイン）があると現れ、なくなると消えます。質問に答えるだけではプロジェクトを変えません。")
          .font(.caption).foregroundStyle(.secondary)
      }
      Text("チャットは任意機能です。通常の操作はすべて UI から行えます。").font(.caption).foregroundStyle(.secondary)
    }
    .formStyle(.grouped)
    .onAppear { GuideController.shared.refreshAvailability() }
    .onChange(of: provider) { GuideController.shared.refreshAvailability() }
  }
}

/// One API key connection: its key (in the Keychain only: never in project
/// files, settings or logs), its model and the models it offers.
private struct APIKeySection: View {
  let id: String
  let config: ProviderConfig
  @Binding var model: String
  @AppStorage("agent.provider") private var provider = "anthropic"
  @State private var key = ""
  @State private var stored = false
  @State private var models: [String] = []
  @State private var message: String?

  var body: some View {
    Section {
      LabeledContent("保存先", value: stored ? String(localized: "macOS Keychain（このデバイスのみ・ロック解除中に使用）") : String(localized: "未設定"))
      SecureField("新しい API キー", text: $key)
      HStack {
        Button("キーを保存") {
          do {
            try Keychain.save(key.trimmingCharacters(in: .whitespacesAndNewlines), account: config.name)
            key = ""
            refresh(saved: true)
            message = String(localized: "Keychain に保存しました。キーは core や Card には渡されず、プロジェクト・設定・ログにも残りません。")
          } catch { message = "\(error)" }
        }
        .disabled(key.isEmpty)
        Button("キーを削除", role: .destructive) {
          Keychain.delete(account: config.name)
          refresh()
          message = String(localized: "\(config.name) のキーを Keychain から削除しました。")
        }
        .disabled(!stored)
        Button("モデル一覧を取得") {
          Task {
            do {
              models = try await MessagesProvider(config: AgentSettings.config(for: id)).models()
              UserDefaults.standard.set(models, forKey: AgentSettings.modelsKey(id))
              message = String(localized: "\(models.count) 件のモデルを取得しました。")
            } catch { message = "\(error)" }
          }
        }
        .disabled(!stored)
      }
      TextField("モデル", text: $model)
      if !models.isEmpty {
        Picker("一覧から選択", selection: $model) {
          ForEach(models, id: \.self) { Text($0).tag($0) }
        }
      }
      if let message { Text(message).font(.caption).textSelection(.enabled) }
    } header: {
      HStack {
        Text(id == "openai" ? "OpenAI API キー" : "Anthropic API キー")
        if provider == id { Text("使用中").font(.caption).foregroundStyle(.green) }
      }
    }
    .onAppear {
      refresh()
      models = UserDefaults.standard.stringArray(forKey: AgentSettings.modelsKey(id)) ?? []
    }
  }

  private func refresh(saved: Bool = false) {
    stored = Keychain.exists(account: config.name)
    // The guide is there while the connection in use works.
    if saved { GuideController.shared.connectionSaved() } else { GuideController.shared.refreshAvailability() }
  }
}

/// RPC endpoints of the environments this app has open (loopback only).
private struct ConnectionSettings: View {
  @Environment(AppModel.self) private var model

  var body: some View {
    let sessions = model.sessions.values.sorted { $0.scopeName([]) < $1.scopeName([]) }
    VStack(alignment: .leading, spacing: 10) {
      Text("開いているプロジェクトの tsugumid への接続先です。127.0.0.1 にだけ公開しています。外部クライアント（別の UI・Agent）は同じ RPC 契約で接続でき、認証 token は control フォルダの server.ready（0600）にだけ置かれます。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      List(sessions, id: \.identity.id) { s in
        VStack(alignment: .leading, spacing: 4) {
          HStack {
            Image(systemName: s.connected ? "circle.fill" : "circle").foregroundStyle(s.connected ? .green : .secondary).font(.caption)
            Text(s.scopeName([])).bold()
            Text(s.connected ? String(localized: "接続中") : s.environment.label).foregroundStyle(.secondary)
          }
          if case .ready(let ep) = s.environment {
            HStack {
              Text(ep.url.absoluteString).font(.caption.monospaced()).textSelection(.enabled)
              Button("コピー") { copy(ep.url.absoluteString) }.controlSize(.small)
            }
            let ready = model.environments.controlDir(s.identity).appendingPathComponent("server.ready").path
            HStack {
              Text(ready).font(.caption.monospaced()).lineLimit(1).truncationMode(.middle).textSelection(.enabled)
              Button("パスをコピー") { copy(ready) }.controlSize(.small)
            }
            Text("project_id \(ep.projectId)").font(.caption2).foregroundStyle(.secondary)
          }
        }
      }
      .overlay { if sessions.isEmpty { ContentUnavailableView("開いているプロジェクトはありません", systemImage: "network.slash") } }
      EnvironmentStorageSettings()
      VMHistorySettings()
    }
    .padding()
  }

  private func copy(_ s: String) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(s, forType: .string)
  }
}

/// Where environments keep their data, and removing it. Deleting checks
/// every step; on a failure the environment's files are kept and the
/// message says what is left, so the deletion can be retried.
private struct EnvironmentStorageSettings: View {
  @Environment(AppModel.self) private var model
  @State private var working = false
  @State private var message: String?
  @State private var failed = false
  @State private var confirmReset = false
  @State private var confirmCleanup = false

  var body: some View {
    GroupBox("実行環境のデータ") {
      VStack(alignment: .leading, spacing: 8) {
        Text("実行環境（全プロジェクト共通の VM、その Nix store と実行履歴のディスクイメージ、各プロジェクトの control フォルダ）は次のフォルダにあります。プロジェクトの文書・成果物と、プロジェクトに保存した実行履歴（.tsugumi/runs.sqlite）はプロジェクトのフォルダにあり、ここには含まれません。")
          .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        HStack {
          Text(model.environments.support.path).font(.caption.monospaced()).lineLimit(1).truncationMode(.middle).textSelection(.enabled)
          Button("Finder で表示") { NSWorkspace.shared.activateFileViewerSelecting([model.environments.support]) }.controlSize(.small)
        }
        Text("共有 Nix store の掃除は、どのプロジェクトのビルド結果としても登録されていない（GC root のない）ものを削除します。登録中の Card のビルド結果と、そのビルドに使ったコンパイラなどは残ります。各プロジェクトの「ビルドキャッシュを削除」は登録を外すだけで、store の掃除はここでだけ行います。")
          .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        HStack {
          Button("共有 store を掃除…") { confirmCleanup = true }
            .disabled(working)
          Button("すべての実行環境を初期化…", role: .destructive) { confirmReset = true }
            .disabled(working)
          if working { ProgressView().controlSize(.small) }
          if let message {
            Text(message).font(.caption).foregroundStyle(failed ? .red : .secondary).textSelection(.enabled)
              .fixedSize(horizontal: false, vertical: true)
          }
        }
      }
      .padding(6)
    }
    .confirmationDialog("共有 Nix store を掃除しますか？", isPresented: $confirmCleanup) {
      Button("掃除") { cleanup() }
      Button("キャンセル", role: .cancel) {}
    } message: {
      Text("登録されていないビルド結果を共有 store から削除します。登録を外したプロジェクトの Card は、次の実行でビルドし直します。")
    }
    .confirmationDialog("すべての実行環境を初期化しますか？", isPresented: $confirmReset) {
      Button("初期化", role: .destructive) { reset() }
      Button("キャンセル", role: .cancel) {}
    } message: {
      Text("開いているプロジェクトの実行を止めて VM を停止し、共通の Nix store・VM イメージの展開先・各プロジェクトの control フォルダを削除します（以前の版が Apple container に作った環境も削除します）。VM 内の実行履歴（別のディスク）、プロジェクトの文書・成果物、ホストの /nix/store は削除しません。次に開いたときに作り直します。VM 内の実行履歴は下の「VM 内の実行履歴」で削除できます。")
    }
  }

  private func cleanup() {
    working = true
    message = nil
    Task {
      do {
        let r = try await model.environments.cleanupStore()
        failed = false
        message = String(localized: "\(r.paths) 件を削除し、\(ByteCountFormatter.string(fromByteCount: Int64(r.bytes), countStyle: .file)) を解放しました。")
      } catch {
        failed = true
        message = String(localized: "掃除できませんでした: ") + "\(error)"
      }
      working = false
    }
  }

  private func reset() {
    working = true
    message = nil
    Task {
      do {
        let r = try await model.environments.resetAll()
        failed = false
        var parts = [String(localized: "プロジェクトの環境 \(r.environments) 件を削除しました")]
        if r.vmRemoved { parts.append(String(localized: "VM と Nix store を削除")) }
        message = parts.joined(separator: "。") + "。"
      } catch {
        failed = true
        message = String(localized: "初期化を完了できませんでした（残りは保持しています。再試行できます）: ") + "\(error)"
      }
      working = false
      await model.refreshSetup()
    }
  }
}

/// Run histories kept in the VM, one per project id: size, last start and
/// the folder when the app knows it. Deleting one also removes that
/// project's build roots; a project still on disk reads its runs back from
/// .tsugumi/runs.sqlite when it is opened again.
private struct VMHistorySettings: View {
  @Environment(AppModel.self) private var model
  @State private var rows: [EnvironmentManager.VMHistory]?
  @State private var folders: [String: String] = [:]
  @State private var working = false
  @State private var message: String?
  @State private var pending: EnvironmentManager.VMHistory?

  var body: some View {
    GroupBox("VM 内の実行履歴") {
      VStack(alignment: .leading, spacing: 8) {
        Text("実行履歴はプロジェクトごとに VM のディスクに保存します。自動では削除しません。フォルダが見つからないプロジェクトの分も、ここから削除できます（そのプロジェクトのビルドの登録も外します）。")
          .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        if let rows {
          if rows.isEmpty { Text("ありません。").foregroundStyle(.secondary) }
          ForEach(rows) { r in
            HStack {
              VStack(alignment: .leading) {
                Text(folders[r.id] ?? String(localized: "フォルダが見つからないプロジェクト")).lineLimit(1).truncationMode(.middle)
                Text("\(r.id)  \(r.lastStart.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "")")
                  .font(.caption.monospaced()).foregroundStyle(.secondary)
              }
              Spacer()
              Text(ByteCountFormatter.string(fromByteCount: Int64(r.bytes), countStyle: .file)).monospacedDigit()
              Button("削除…", role: .destructive) { pending = r }
                .disabled(working || r.running)
                .help(r.running ? String(localized: "開いているプロジェクトは削除できません") : "")
            }
          }
        }
        HStack {
          Button(rows == nil ? "一覧を表示" : "再読込") { load() }.disabled(working)
          if working { ProgressView().controlSize(.small) }
          if let message { Text(message).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
        }
      }
      .padding(6)
    }
    .confirmationDialog("VM 内の実行履歴を削除しますか？", isPresented: Binding(get: { pending != nil }, set: { if !$0 { pending = nil } }),
                        presenting: pending) { r in
      Button("削除", role: .destructive) { delete(r) }
      Button("キャンセル", role: .cancel) {}
    } message: { r in
      Text(folders[r.id] != nil
           ? String(localized: "プロジェクトの .tsugumi/runs.sqlite にある実行は、次に開いたときに読み込み直します。")
           : String(localized: "このプロジェクトの実行履歴とビルドの登録を削除します。"))
    }
  }

  private func load() {
    working = true
    message = nil
    Task {
      var known: [String: String] = [:]
      for r in model.recents {
        if let id = try? ProjectIdentity(url: URL(fileURLWithPath: r.path)).id { known[id] = r.path }
      }
      for s in model.sessions.values { known[s.identity.id] = s.identity.url.path }
      folders = known
      do { rows = try await model.environments.vmHistories().sorted { ($0.lastStart ?? .distantPast) > ($1.lastStart ?? .distantPast) } }
      catch { message = String(localized: "読めませんでした: ") + "\(error)" }
      working = false
    }
  }

  private func delete(_ r: EnvironmentManager.VMHistory) {
    working = true
    message = nil
    Task {
      do { try await model.environments.deleteVMData(id: r.id) } catch { message = String(localized: "削除できませんでした: ") + "\(error)" }
      working = false
      load()
    }
  }
}
