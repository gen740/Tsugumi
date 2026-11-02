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
  }

  @AppStorage("settings.category") private var stored = Category.general.rawValue

  private var category: Category { Category(rawValue: stored) ?? .general }

  var body: some View {
    // A plain list + content (no split view): switching categories, going back
    // and reopening the window always work, whatever a category is doing.
    HStack(spacing: 0) {
      SettingsSidebar(items: Category.allCases, selection: Binding(get: { category }, set: { stored = $0.rawValue }),
                      title: \.title, symbol: \.symbol, width: 180)
      Divider()
      VStack(alignment: .leading, spacing: 0) {
        Text(category.title).font(.title2.bold()).padding(.horizontal, 20).padding(.top, 14).padding(.bottom, 4)
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
}

private struct GeneralSettings: View {
  @Environment(AppPreferences.self) private var prefs

  var body: some View {
    @Bindable var prefs = prefs
    Form {
      Picker("言語", selection: $prefs.language) {
        ForEach(AppPreferences.Language.allCases) { Text($0.label).tag($0) }
      }
      Text("言語の変更はアプリの再起動後に反映されます。").font(.caption).foregroundStyle(.secondary)
      Stepper("並列実行数: \(prefs.maxConcurrency)", value: $prefs.maxConcurrency, in: 1...64)
      Text("別々の Card を同時に実行する上限です（同じ Card は常に直列）。次の実行から使われます。")
        .font(.caption).foregroundStyle(.secondary)
      Picker("チャットの送信キー", selection: $prefs.sendKey) {
        ForEach(AppPreferences.SendKey.allCases) { Text($0.label).tag($0) }
      }
    }
    .formStyle(.grouped)
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
        Picker("流れの向き", selection: $prefs.flowDirection) {
          ForEach(FlowDirection.allCases) { Text($0.label).tag($0) }
        }
        Toggle("グリッドを表示（移動時に吸着）", isOn: $prefs.showGrid)
        Slider(value: $prefs.gridSize, in: 10...60, step: 5) { Text("グリッド間隔 \(Int(prefs.gridSize))") }
        Toggle("実行中の接続線を動かす", isOn: $prefs.animateLines)
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
      package = inputs.tsugumi.pythonCard {
        src = ./src;
        packages = ps: [ ps.numpy ];
      };
      reuseWorker = true;
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
            Text(recording == a ? "キーを押してください…" : prefs.shortcut(a).label).monospaced()
              .foregroundStyle(recording == a ? Color.accentColor : .primary)
            if let c = prefs.conflict(prefs.shortcut(a), except: a) {
              Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow).help("「\(c.title)」と重複しています")
            }
          }
        }
        TableColumn("") { a in
          HStack {
            Button(recording == a ? "キャンセル" : "変更") { recording = recording == a ? nil : a }
            Button("既定") { prefs.shortcuts[a] = nil }.disabled(prefs.shortcuts[a] == nil)
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

private struct APISettings: View {
  @AppStorage("agent.provider") private var provider = "anthropic"
  @AppStorage("agent.model") private var model = ProviderConfig.anthropic.model
  @AppStorage("agent.fixturePort") private var port = 8765
  @State private var key = ""
  @State private var hasKey = false
  @State private var models: [String] = UserDefaults.standard.stringArray(forKey: "agent.models") ?? []
  @State private var message: String?

  var body: some View {
    Form {
      Picker("接続先", selection: $provider) {
        Text("Anthropic API").tag("anthropic")
        Text("ローカル fixture（テスト用・通信なし）").tag("fixture")
      }
      if provider == "anthropic" {
        TextField("モデル", text: $model)
        LabeledContent("API キー", value: hasKey ? "Keychain に保存済み" : "未設定")
        SecureField("新しい API キー", text: $key)
        HStack {
          Button("キーを保存") {
            do {
              try Keychain.save(key, account: ProviderConfig.anthropic.name)
              key = ""
              hasKey = true
              message = "Keychain に保存しました。キーは core や Card には渡されません。"
            } catch { message = "\(error)" }
          }
          .disabled(key.isEmpty)
          Button("モデル一覧を取得") {
            Task {
              do {
                models = try await MessagesProvider(config: AgentSettings.current()).models()
                UserDefaults.standard.set(models, forKey: "agent.models")
                message = "\(models.count) 件のモデルを取得しました。"
              } catch { message = "\(error)" }
            }
          }
        }
        if !models.isEmpty {
          Picker("一覧から選択", selection: $model) { ForEach(models, id: \.self) { Text($0).tag($0) } }
        }
      } else {
        TextField("fixture のポート", value: $port, format: .number)
        Text("起動: python3 tests/fixtures/model_fixture.py --port \(port)").font(.caption.monospaced())
      }
      if let message { Text(message).font(.caption) }
      Text("チャットは任意機能です。通常の操作はすべて UI から行えます。").font(.caption).foregroundStyle(.secondary)
    }
    .formStyle(.grouped)
    .onAppear { hasKey = Keychain.load(account: ProviderConfig.anthropic.name) != nil }
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
            Text(s.connected ? "接続中" : s.environment.label).foregroundStyle(.secondary)
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

  var body: some View {
    GroupBox("実行環境のデータ") {
      VStack(alignment: .leading, spacing: 8) {
        Text("各プロジェクトの実行環境（Nix store のディスクイメージと control フォルダ）は次のフォルダにあります。プロジェクトの文書・履歴・成果物はプロジェクトのフォルダにあり、ここには含まれません。")
          .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        HStack {
          Text(model.environments.support.path).font(.caption.monospaced()).lineLimit(1).truncationMode(.middle).textSelection(.enabled)
          Button("Finder で表示") { NSWorkspace.shared.activateFileViewerSelecting([model.environments.support]) }.controlSize(.small)
        }
        HStack {
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
    .confirmationDialog("すべての実行環境を初期化しますか？", isPresented: $confirmReset) {
      Button("初期化", role: .destructive) { reset() }
      Button("キャンセル", role: .cancel) {}
    } message: {
      Text("実行中の環境を止め、Tsugumi が作った container・Nix store・環境イメージを削除します。プロジェクトの文書・履歴・成果物、他のアプリの container・image・volume、ホストの /nix/store は削除しません。次に開いたときに作り直します。")
    }
  }

  private func reset() {
    working = true
    message = nil
    Task {
      do {
        let r = try await model.environments.resetAll()
        failed = false
        var parts = [String(localized: "container \(r.containers.count) 件・環境 \(r.environments) 件を削除しました")]
        if !r.volumes.isEmpty { parts.append(String(localized: "旧 volume \(r.volumes.count) 件を削除")) }
        if r.imageRemoved { parts.append(String(localized: "環境イメージを削除")) }
        if let kept = r.imageKept { parts.append(String(localized: "環境イメージは使用中のため残しました（\(kept)）")) }
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
