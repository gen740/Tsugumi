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

  @State private var category: Category = .general

  var body: some View {
    NavigationSplitView {
      List(Category.allCases, selection: $category) { c in
        Label(c.title, systemImage: c.symbol).tag(c)
      }
      .navigationSplitViewColumnWidth(180)
    } detail: {
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
      .navigationTitle(category.title)
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

  var body: some View {
    @Bindable var prefs = prefs
    HStack(alignment: .top) {
      Form {
        Picker("テーマ", selection: $prefs.theme) {
          ForEach(AppPreferences.Theme.allCases) { Text($0.label).tag($0) }
        }
        Toggle("グリッドを表示（移動時に吸着）", isOn: $prefs.showGrid)
        Slider(value: $prefs.gridSize, in: 10...60, step: 5) { Text("グリッド間隔 \(Int(prefs.gridSize))") }
        Toggle("実行中の接続線を動かす", isOn: $prefs.animateLines)
      }
      .formStyle(.grouped)
      VStack(alignment: .leading) {
        Text("プレビュー").font(.headline)
        CanvasPreview(grid: prefs.showGrid, gridSize: prefs.gridSize, animate: prefs.animateLines)
          .frame(width: 240, height: 160)
          .clipShape(RoundedRectangle(cornerRadius: 8))
          .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(.secondary.opacity(0.3)))
      }
      .padding()
    }
  }
}

private struct CanvasPreview: View {
  let grid: Bool
  let gridSize: Double
  let animate: Bool

  var body: some View {
    SwiftUI.TimelineView(.animation(minimumInterval: 1 / 30, paused: !animate)) { t in
      Canvas { ctx, size in
        ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Color(nsColor: .textBackgroundColor)))
        if grid {
          var x = 0.0
          while x < size.width {
            var y = 0.0
            while y < size.height {
              ctx.fill(Path(ellipseIn: CGRect(x: x, y: y, width: 1.6, height: 1.6)), with: .color(.secondary.opacity(0.35)))
              y += gridSize
            }
            x += gridSize
          }
        }
        let a = CGRect(x: 16, y: 40, width: 80, height: 44), b = CGRect(x: 144, y: 80, width: 80, height: 44)
        for r in [a, b] {
          ctx.fill(Path(roundedRect: r, cornerRadius: 6), with: .color(Color(nsColor: .controlBackgroundColor)))
          ctx.stroke(Path(roundedRect: r, cornerRadius: 6), with: .color(.secondary.opacity(0.5)))
        }
        var p = Path()
        p.move(to: CGPoint(x: a.maxX, y: a.midY))
        p.addCurve(to: CGPoint(x: b.minX, y: b.midY), control1: CGPoint(x: a.maxX + 40, y: a.midY), control2: CGPoint(x: b.minX - 40, y: b.midY))
        let phase = animate ? -CGFloat(t.date.timeIntervalSinceReferenceDate * 30).truncatingRemainder(dividingBy: 20) : 0
        ctx.stroke(p, with: .color(.orange), style: StrokeStyle(lineWidth: 1.8, dash: animate ? [8, 6] : [], dashPhase: phase))
      }
    }
  }
}

private struct EditorSettings: View {
  @Environment(AppPreferences.self) private var prefs

  var body: some View {
    @Bindable var prefs = prefs
    HStack(alignment: .top) {
      Form {
        Picker("インデント幅", selection: $prefs.indentWidth) {
          Text("2").tag(2)
          Text("4").tag(4)
          Text("8").tag(8)
        }
        .pickerStyle(.segmented)
        Toggle("インデントガイドを表示", isOn: $prefs.showIndentGuides)
        Stepper("文字サイズ: \(Int(prefs.editorFontSize))", value: $prefs.editorFontSize, in: 9...24)
      }
      .formStyle(.grouped)
      VStack(alignment: .leading) {
        Text("プレビュー").font(.headline)
        CodeEditor(text: .constant("def main(ctx):\n    if ctx:\n        return 1\n    return 0\n"), language: "python", editable: false)
          .frame(width: 260, height: 140)
          .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(.secondary.opacity(0.3)))
      }
      .padding()
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
  @State private var counts: [String: Int] = [:]

  var body: some View {
    @Bindable var prefs = prefs
    VStack(alignment: .leading, spacing: 10) {
      Text("チャットで `$名前` として使う Skill のフォルダです。各フォルダの `<名前>/SKILL.md` または `<名前>.md` を読み込みます。プロジェクトの `skills/` も常に読み込みます。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      List {
        ForEach($prefs.skillFolders) { $f in
          HStack {
            Toggle("", isOn: $f.enabled).labelsHidden()
            VStack(alignment: .leading) {
              Text(URL(fileURLWithPath: f.path).lastPathComponent)
              Text(f.path).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            }
            Spacer()
            Text("\(counts[f.path] ?? 0) 件").foregroundStyle(.secondary)
            Button { prefs.skillFolders.removeAll { $0.path == f.path } } label: { Image(systemName: "minus.circle") }
              .buttonStyle(.borderless)
          }
        }
      }
      .overlay { if prefs.skillFolders.isEmpty { ContentUnavailableView("フォルダはありません", systemImage: "folder.badge.plus") } }
      HStack {
        Button("フォルダを追加…") { add() }
        Button("再読込") { reload() }
        Spacer()
      }
    }
    .padding()
    .onAppear(perform: reload)
  }

  private func add() {
    let p = NSOpenPanel()
    p.canChooseDirectories = true
    p.canChooseFiles = false
    guard p.runModal() == .OK, let url = p.url, !prefs.skillFolders.contains(where: { $0.path == url.path }) else { return }
    prefs.skillFolders.append(SkillFolder(path: url.path))
    reload()
  }

  private func reload() {
    for f in prefs.skillFolders { counts[f.path] = loadSkills(in: URL(fileURLWithPath: f.path), source: "").count }
    for s in AppModel.shared.sessions.values { AppModel.shared.chat(for: s).reloadSkills() }
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
    }
    .padding()
  }

  private func copy(_ s: String) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(s, forType: .string)
  }
}
