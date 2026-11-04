import SwiftUI
import TsugumiAgent

/// What a main window lets the guide's buttons do (MainWindow sets it).
struct GuideWindowActions {
  /// Opens a project in this window (leaving the open one first).
  var openProject: (URL) -> Void
  /// Back to Home.
  var home: () -> Void
  /// The new project sheet on Home.
  var newProject: () -> Void
}

private struct GuideWindowKey: EnvironmentKey { static let defaultValue: GuideWindowActions? = nil }

extension EnvironmentValues {
  var guideWindow: GuideWindowActions? {
    get { self[GuideWindowKey.self] }
    set { self[GuideWindowKey.self] = newValue }
  }
}

// MARK: - Pointing

/// A ring the guide draws around a part of the screen it talks about.
private struct GuideTarget: ViewModifier {
  let id: String
  let radius: CGFloat

  func body(content: Content) -> some View {
    content.overlay {
      if GuideController.shared.points(at: id) {
        GuideRing(radius: radius).allowsHitTesting(false).accessibilityHidden(true)
      }
    }
  }
}

private struct GuideRing: View {
  let radius: CGFloat
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var pulse = false

  var body: some View {
    let still = AppPreferences.shared.tsugumiStill(reduceMotion: reduceMotion)
    RoundedRectangle(cornerRadius: radius + 4)
      .strokeBorder(TsugumiPalette.rufous, lineWidth: 2.5)
      .padding(-5)
      .shadow(color: TsugumiPalette.rufous.opacity(0.5), radius: pulse ? 6 : 2)
      .scaleEffect(pulse ? 1.04 : 1)
      .onAppear {
        guard !still else { return }
        withAnimation(.easeInOut(duration: 0.8).repeatForever(autoreverses: true)) { pulse = true }
      }
  }
}

extension View {
  /// Lets the guide point at this view (guide_show "highlight").
  func guideTarget(_ id: String, radius: CGFloat = 8) -> some View { modifier(GuideTarget(id: id, radius: radius)) }
}

// MARK: - The corner

/// The guide bird in the bottom-right corner, with its panel above it. Shown
/// only with a model connection; the screen it is on is what it explains.
struct GuideCorner: View {
  let screen: GuideController.Screen
  var session: ProjectSession? = nil
  var tab: WorkflowTab? = nil
  /// In a project the panel is docked in the inspector column (GuideDock),
  /// so it never covers the canvas; on Home it floats above the bird.
  var docked = false
  @Environment(AppModel.self) private var model
  @Environment(\.guideWindow) private var window
  @Environment(\.openWindow) private var openWindow
  @Environment(\.openSettings) private var openSettings
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  private var guide: GuideController { .shared }

  var body: some View {
    VStack(alignment: .trailing, spacing: 8) {
      if guide.available, window != nil {
        if guide.open && !docked {
          GuidePanel(screen: screen, session: session, tab: tab, perform: perform)
            .transition(.opacity.combined(with: .scale(scale: 0.92, anchor: .bottomTrailing)))
        }
        HStack(alignment: .bottom, spacing: 6) {
          if !guide.open, let line = bubble {
            Text(line).font(.caption).lineLimit(2)
              .padding(.horizontal, 9).padding(.vertical, 5)
              .background(.regularMaterial, in: Capsule())
              .transition(.opacity)
          }
          Button {
            withAnimation(.easeOut(duration: 0.2)) { guide.open.toggle() }
          } label: {
            GuideAvatar(mood: mood, still: AppPreferences.shared.tsugumiStill(reduceMotion: reduceMotion))
          }
          .buttonStyle(.plain)
          .help(String(localized: "ガイドのツグミに聞く（\(AppPreferences.shared.shortcut(.toggleGuide).label)）"))
          .accessibilityLabel(String(localized: "ガイドのツグミ"))
          .accessibilityValue(guide.activity ?? (guide.open ? String(localized: "会話を表示中") : ""))
          .accessibilityHint(guide.open ? String(localized: "会話を閉じます") : String(localized: "質問できる会話を開きます"))
        }
      }
    }
    .animation(.easeOut(duration: 0.2), value: guide.open)
    .onChange(of: runKey, initial: true) { if let session { guide.observe(session, tab: tab) } }
    .onChange(of: guide.offersDemo, initial: true) { _, offers in if offers, window != nil { guide.open = true } }
  }

  private var perform: GuidePerformer {
    GuidePerformer(model: model, window: window, session: session, tab: tab, openWindow: openWindow, openSettings: openSettings)
  }

  /// What changes in the project that the guide follows during a tour.
  private var runKey: String {
    guard let s = session else { return "" }
    return "\(s.connected)|\(s.currentRunId ?? "")|\(s.runStatus.map { String(describing: $0.state) } ?? "")"
  }

  private var mood: GuideAvatar.Mood {
    if guide.busy { return .thinking }
    if guide.failed { return .worried }
    if let s = session, s.runActive, let t = guide.tour, GuideController.same(t.path, s.identity.url) { return .watching }
    return .idle
  }

  /// A word under the closed panel: what the guide does now.
  private var bubble: String? {
    if let a = guide.activity { return a + "…" }
    if guide.failed { return String(localized: "うまく答えられませんでした") }
    return nil
  }
}

/// What a button of the guide does, with the app's own controls in this window.
@MainActor
struct GuidePerformer {
  let model: AppModel
  let window: GuideWindowActions?
  let session: ProjectSession?
  let tab: WorkflowTab?
  let openWindow: OpenWindowAction
  let openSettings: OpenSettingsAction
  private var guide: GuideController { .shared }

  func callAsFunction(_ a: GuideAction) {
    switch a.kind {
    case .openDemo:
      openDemo(a.demo ?? "report")
    case .showDemos:
      guide.demosRequested = true
      if session != nil { window?.home() }
    case .run:
      guard let s = session, let tab, s.connected, !s.runActive else { return }
      RunActions.runAll(s, scope: tab.scope)
    case .stop:
      guard let s = session, s.runActive else { return }
      Task { await s.cancelRun() }
    case .openCard:
      guard let s = session, let id = a.card else { return }
      let scope = tab?.scope ?? []
      let k = s.instance(key(scope + [id])) != nil ? key(scope + [id]) : key([id])
      guard s.instance(k) != nil else { return }
      openWindow(id: "card", value: CardRef(path: s.identity.url.path, card: k))
    case .openSettings:
      UserDefaults.standard.set(ApplicationSettings.Category.api.rawValue, forKey: "settings.category")
      openSettings()
    case .newProject:
      window?.newProject()
    case .home:
      window?.home()
    }
  }

  private func openDemo(_ id: String) {
    guard let demo = model.demos.first(where: { $0.id == id }) ?? model.demos.first(where: { $0.id == "report" }) else { return }
    if session?.runActive == true {
      model.error = String(localized: "実行中はデモを開けません。停止するか終了を待ってください。")
      return
    }
    do {
      let url = try model.copyDemo(demo)
      guide.startTour(demo, copy: url)
      window?.openProject(url)
    } catch {
      model.error = "\(error)"
    }
  }
}

// MARK: - The bird

/// The guide's bird in a round badge. It breathes and blinks; while the
/// guide thinks it tilts its head, while a tour's run goes it hops, and when
/// something went wrong it droops. Still when motion is reduced.
struct GuideAvatar: View {
  enum Mood { case idle, thinking, watching, worried }
  let mood: Mood
  let still: Bool

  var body: some View {
    TimelineView(.animation(minimumInterval: 1 / 30, paused: still)) { ctx in
      let t = still ? 0 : ctx.date.timeIntervalSinceReferenceDate
      Canvas { g, size in
        TsugumiBird.draw(g, at: CGPoint(x: size.width * 0.47, y: size.height * 0.82), scale: 0.066, pose: pose(t))
      }
      .scaleEffect(x: -1, y: 1)  // facing the window's content
    }
    .frame(width: 52, height: 52)
    .background(Circle().fill(TsugumiPalette.cream))
    .overlay(Circle().strokeBorder(mood == .worried ? Color.orange : TsugumiPalette.rufous.opacity(0.7), lineWidth: 1.5))
    .shadow(color: .black.opacity(0.2), radius: 4, y: 2)
    .contentShape(Circle())
  }

  private func pose(_ t: Double) -> BirdPose {
    var p = BirdPose()
    let blinkPhase = t.truncatingRemainder(dividingBy: 4.2)
    p.blink = blinkPhase < 0.12 ? 1 : 0
    switch mood {
    case .idle:
      p.squash = CGFloat(sin(t * 1.6) * 0.02)
    case .thinking:
      p.tilt = 0.12 * sin(t * 3.2)
    case .watching:
      p.hop = CGFloat(max(0, sin(t * 5.5)) * 4)
      p.flap = max(0, sin(t * 11)) * 0.5
    case .worried:
      p.tilt = -0.12
      p.squash = 0.06
    }
    return p
  }
}

// MARK: - The panel

/// The guide's panel in a project's inspector column: above the Agent chat
/// when it is open (`split`), the whole column otherwise.
struct GuideDock: View {
  let session: ProjectSession
  let tab: WorkflowTab?
  @Environment(AppModel.self) private var model
  @Environment(\.guideWindow) private var window
  @Environment(\.openWindow) private var openWindow
  @Environment(\.openSettings) private var openSettings

  var body: some View {
    GuidePanel(screen: .project, session: session, tab: tab,
               perform: GuidePerformer(model: model, window: window, session: session, tab: tab,
                                       openWindow: openWindow, openSettings: openSettings),
               docked: true)
  }
}

/// The conversation with the guide: its greeting and demo offer, the tour
/// being shown, the replies with their buttons, and a field to ask in.
private struct GuidePanel: View {
  let screen: GuideController.Screen
  let session: ProjectSession?
  let tab: WorkflowTab?
  let perform: GuidePerformer
  var docked = false
  @Environment(\.guideWindow) private var window
  @Environment(\.openSettings) private var openSettings
  @State private var text = ""
  @FocusState private var focused: Bool

  private var guide: GuideController { .shared }

  var body: some View {
    VStack(spacing: 0) {
      header
      Divider()
      if let t = guide.tour { tourBar(t) }
      ScrollViewReader { proxy in
        ScrollView {
          LazyVStack(alignment: .leading, spacing: 10) {
            if guide.offersDemo { offer }
            if guide.items.isEmpty && !guide.offersDemo { hints }
            ForEach(guide.items) { item in row(item).id(item.id) }
            if let s = guide.status { statusRow(s) }
            Color.clear.frame(height: 1).id("end")
          }
          .padding(12)
        }
        .onChange(of: guide.items.last?.text) { proxy.scrollTo("end", anchor: .bottom) }
        .onChange(of: guide.items.count) { proxy.scrollTo("end", anchor: .bottom) }
      }
      Divider()
      input
    }
    .modifier(GuidePanelFrame(docked: docked))
    .onExitCommand { guide.open = false }
    .onAppear { focused = !guide.offersDemo }
    .accessibilityElement(children: .contain)
    .accessibilityLabel(String(localized: "ガイドのツグミとの会話"))
  }

  private var header: some View {
    HStack(spacing: 8) {
      VStack(alignment: .leading, spacing: 1) {
        Text("ツグミ").font(.headline)
        Text(guide.activity.map { $0 + "…" } ?? String(localized: "使い方の質問に答えます"))
          .font(.caption).foregroundStyle(.secondary).lineLimit(1)
      }
      Spacer()
      Menu {
        Button("新しい会話") { guide.newConversation() }.disabled(guide.busy)
        Button("デモを見る…") { perform(GuideAction(kind: .showDemos, label: "")) }
        Button("API の設定…") { perform(GuideAction(kind: .openSettings, label: "")) }
      } label: { Image(systemName: "ellipsis.circle") }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
        .accessibilityLabel("その他")
      Button { guide.open = false } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary) }
        .buttonStyle(.plain)
        .help("閉じる（Esc）。ツグミは右下に残ります")
        .accessibilityLabel("閉じる")
    }
    .padding(.horizontal, 12).padding(.vertical, 9)
  }

  private func tourBar(_ t: GuideController.Tour) -> some View {
    let here = session.map { GuideController.same(t.path, $0.identity.url) } ?? false
    return HStack(spacing: 6) {
      Image(systemName: "sparkles").foregroundStyle(TsugumiPalette.rufous)
      Text(here ? String(localized: "デモ「\(t.title)」を案内中") : String(localized: "デモ「\(t.title)」の案内は中断中"))
        .font(.caption).lineLimit(1)
      Spacer()
      if !here {
        Button("続ける") { if let url = guide.resumeTour() { window?.openProject(url) } }
          .controlSize(.small).disabled(session?.runActive == true)
      }
      Button("終える") { guide.endTour() }.controlSize(.small)
    }
    .padding(.horizontal, 12).padding(.vertical, 6)
    .background(TsugumiPalette.rufous.opacity(0.1))
  }

  private var offer: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text("こんにちは、ツグミです。Tsugumi の使い方を、ここからいつでもご案内します。")
      Text("まず、動くデモを一緒に見てみますか？ デモは編集できるコピーとして開き、実際に実行しながら Card・接続・結果の見方をお見せします。")
        .foregroundStyle(.secondary)
      HStack {
        Button("見る") { perform(GuideAction(kind: .openDemo, label: "", demo: "report")) }
          .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
        Button("あとで") {
          guide.answerDemoOffer()
          focused = true
        }
      }
    }
    .font(.callout)
    .padding(10)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(TsugumiPalette.cream.opacity(0.35), in: RoundedRectangle(cornerRadius: 10))
  }

  private var hints: some View {
    VStack(alignment: .leading, spacing: 6) {
      Text("いま見ている画面について、気軽に聞いてください。").font(.callout).foregroundStyle(.secondary)
      ForEach(hintQuestions, id: \.self) { q in
        Button(q) { send(q) }.buttonStyle(.link).font(.callout)
      }
    }
  }

  private var hintQuestions: [String] {
    switch screen {
    case .project:
      return [String(localized: "この Card は何をしている？"), String(localized: "いま何が起きている？"),
              String(localized: "グラフを表示するには？"), String(localized: "デモを見せて")]
    default:
      return [String(localized: "Tsugumi で何ができる？"), String(localized: "デモを見せて"), String(localized: "新しいプロジェクトの作り方は？")]
    }
  }

  @ViewBuilder private func row(_ item: GuideController.Item) -> some View {
    switch item.role {
    case "user":
      HStack {
        Spacer(minLength: 40)
        Text(item.text).font(.callout).textSelection(.enabled)
          .padding(.horizontal, 10).padding(.vertical, 6)
          .background(Color.accentColor.opacity(0.15), in: RoundedRectangle(cornerRadius: 10))
      }
    case "note":
      Label(item.text, systemImage: "info.circle").font(.caption).foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .center)
    default:
      VStack(alignment: .leading, spacing: 8) {
        if !item.text.isEmpty { MarkdownView(text: item.text, fontSize: 13) }
        if !item.actions.isEmpty {
          HStack(spacing: 6) {
            ForEach(Array(item.actions.enumerated()), id: \.element.id) { i, a in
              if i == 0 {
                Button(a.label) { perform(a) }.buttonStyle(.borderedProminent).disabled(!enabled(a))
              } else {
                Button(a.label) { perform(a) }.disabled(!enabled(a))
              }
            }
          }
          .controlSize(.small)
        }
      }
    }
  }

  private func statusRow(_ s: String) -> some View {
    VStack(alignment: .leading, spacing: 6) {
      Label(s, systemImage: guide.failed ? "exclamationmark.triangle" : "info.circle")
        .font(.caption).foregroundStyle(guide.failed ? Color.orange : .secondary).textSelection(.enabled)
      if guide.failed {
        HStack {
          Button("再試行") { guide.retry() }.disabled(guide.busy)
          Button("API の設定を開く") { perform(GuideAction(kind: .openSettings, label: "")) }
        }
        .controlSize(.small)
      }
    }
  }

  /// A button that cannot do anything now is shown but disabled (a run while one runs, ...).
  private func enabled(_ a: GuideAction) -> Bool {
    switch a.kind {
    case .run: return session.map { $0.connected && !$0.runActive } ?? false
    case .stop: return session?.runActive ?? false
    case .openCard: return session != nil
    case .openDemo, .home, .newProject: return !(session?.runActive ?? false)
    default: return true
    }
  }

  private var input: some View {
    HStack(alignment: .bottom, spacing: 6) {
      TextField("ツグミに質問", text: $text, axis: .vertical)
        .textFieldStyle(.plain).lineLimit(1...4)
        .focused($focused)
        .onSubmit { send(text) }
        .accessibilityLabel("ツグミへの質問")
      if guide.busy {
        Button { guide.cancel() } label: { Image(systemName: "stop.circle.fill") }
          .buttonStyle(.plain).help("回答を止める").accessibilityLabel("回答を止める")
      } else {
        Button { send(text) } label: { Image(systemName: "arrow.up.circle.fill") }
          .buttonStyle(.plain).foregroundStyle(TsugumiPalette.rufous)
          .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
          .help("送信").accessibilityLabel("送信")
      }
    }
    .font(.title3)
    .padding(10)
  }

  private func send(_ q: String) {
    guard !guide.busy, !q.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
    if guide.offersDemo { guide.answerDemoOffer() }
    guide.ask(q, screen: screen, session: session, tab: tab)
    text = ""
  }
}

// MARK: - First start

/// The first start: choose Anthropic or OpenAI and enter that provider's API
/// key (one is enough), checked by connecting, or start without a guide.
/// The key goes to the Keychain the same way as in Settings > API.
struct GuideSetupView: View {
  let onDone: () -> Void
  @State private var provider = UserDefaults.standard.string(forKey: "agent.provider") == "openai" ? "openai" : "anthropic"
  @State private var key = ""
  @State private var working = false
  @State private var message: String?
  @State private var saved = false

  private var config: ProviderConfig { provider == "openai" ? .openAI : .anthropic }

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      HStack(spacing: 12) {
        GuideAvatar(mood: .idle, still: true)
        VStack(alignment: .leading, spacing: 2) {
          Text("ガイドのツグミを呼ぶ準備").font(.title2.bold())
          Text("右下のツグミが、使い方の質問に答えたりデモを案内したりします。").foregroundStyle(.secondary)
        }
      }
      Text("ツグミとチャットの Agent は、あなたの API キーで OpenAI か Anthropic のモデルを使います。どちらか一方のキーがあれば十分です。キーはこの Mac のキーチェーンにだけ保存し、プロジェクト・会話の記録・ログには残しません。")
        .font(.callout).fixedSize(horizontal: false, vertical: true)
      Picker("使うモデル", selection: $provider) {
        Text("Anthropic").tag("anthropic")
        Text("OpenAI").tag("openai")
      }
      .pickerStyle(.segmented)
      .onChange(of: provider) { message = nil; saved = false }
      SecureField(provider == "openai" ? "OpenAI の API キー" : "Anthropic の API キー", text: $key)
        .textFieldStyle(.roundedBorder)
        .onSubmit(connect)
      if let message {
        Label(message, systemImage: "exclamationmark.triangle").font(.caption).foregroundStyle(.orange)
          .textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
      }
      Text("キーがなくても、Tsugumi はすべて UI から使えます（その間ツグミは現れません）。あとから設定の「API」でキーを登録すると現れます。")
        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      HStack {
        Button("スキップして始める") {
          GuideController.shared.finishSetup()
          onDone()
        }
        Spacer()
        if working { ProgressView().controlSize(.small) }
        if saved && message != nil {
          Button("再試行", action: check).disabled(working)
        }
        Button("接続して始める", action: connect)
          .keyboardShortcut(.defaultAction).buttonStyle(.borderedProminent)
          .disabled(working || key.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
      }
    }
    .padding(24)
    .frame(width: 500)
    .tint(TsugumiPalette.rufous)
  }

  private func connect() {
    let k = key.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !k.isEmpty, !working else { return }
    do {
      try Keychain.save(k, account: config.name)
    } catch {
      message = String(localized: "キーを保存できませんでした: \(String(describing: error))")
      return
    }
    key = ""
    saved = true
    UserDefaults.standard.set(provider, forKey: "agent.provider")
    check()
  }

  /// A connection check: the provider lists its models with the key.
  private func check() {
    working = true
    message = nil
    Task {
      do {
        _ = try await MessagesProvider(config: AgentSettings.current()).models()
        working = false
        GuideController.shared.finishSetup()
        onDone()
      } catch {
        working = false
        message = String(localized: "接続を確かめられませんでした（\(String(describing: error))）。キーを入れ直すか、ネットワークを確かめて再試行してください。")
      }
    }
  }
}

/// A floating panel above the bird, or a docked one filling its place in a column.
private struct GuidePanelFrame: ViewModifier {
  let docked: Bool

  func body(content: Content) -> some View {
    if docked {
      content.frame(maxWidth: .infinity, maxHeight: .infinity)
    } else {
      content
        .frame(width: 340, height: 440)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(.secondary.opacity(0.25)))
        .shadow(color: .black.opacity(0.18), radius: 12, y: 4)
    }
  }
}
