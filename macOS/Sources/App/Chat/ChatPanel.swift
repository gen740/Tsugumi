import AppKit
import SwiftUI
import TsugumiAgentClient
import TsugumiRPC

/// Chat: session list or the conversation (answers, progress, input, model
/// and send/stop). Usable docked beside the canvas or in its own window.
struct ChatPanel: View {
  @Environment(\.openWindow) private var openWindow
  @Environment(AppPreferences.self) private var prefs
  let session: ProjectSession
  @Bindable var chat: ChatController
  let detached: Bool
  var tab: WorkflowTab?
  /// Whether the panel is open: opening it again shows the newest message.
  var shown = true
  @State private var showList = false
  @State private var query = ""
  @State private var input = ""
  @State private var model = AgentSettings.current().model
  /// The connection chosen in Settings > API: the model menu follows it when it changes.
  @HostStorage("agent.provider") private var providerSetting = "anthropic"
  /// The model menu's effort and Fast mode (AgentSettings.chat() reads them).
  @HostStorage(AgentSettings.effortKey) private var effortSetting = ""
  @HostStorage(AgentSettings.fastModelKey) private var fastModelSetting = ""
  /// The connections set up (keys stored, ChatGPT signed in) with their models.
  @State private var connections: [Connection] = []
  struct Connection: Identifiable {
    let id: String
    let name: String
    let models: [String]
  }
  /// Display names of the ChatGPT account's models (Settings > API).
  @State private var modelNames: [String: String] = [:]
  /// The model menu: it says when it opens, and shows new lists while open.
  @State private var modelMenu = ModelMenu()
  @State private var modelLists = ModelListRefresher(fetch: { id in
    // Never the keychain's dialog: nobody asked for this read. A connection
    // that would have to ask keeps its saved list until a turn or Settings > API reads it.
    try await MessagesProvider(config: AgentSettings.config(for: id)).modelChoices(asking: false)
  })
  /// "Draw thoughts": the Agent's thinking summarized for the canvas (extra tokens).
  @HostStorage(ThoughtSummarizer.defaultsKey) private var drawThoughts = false
  @HostStorage("agent.drawThoughtsAsked") private var drawThoughtsAsked = false
  @State private var askThoughts = false
  @State private var skillQuery: String?
  /// The highlighted Skill candidate (↑ / ↓, Return or Tab takes it).
  @State private var skillIndex = 0
  @State private var foldersToAttach: [URL]?
  /// The conversation is taller than its area (only then a scroll bar shows).
  @State private var overflows = false
  @State private var follow = ChatFollow()
  @State private var scrollPosition = ScrollPosition(edge: .bottom)
  @FocusState private var inputFocused: Bool
  @State private var flashing = false

  var body: some View {
    VStack(spacing: 0) {
      header
      Divider()
      if showList { sessionList } else { conversation }
    }
    .background(Color(nsColor: .windowBackgroundColor))
    .sheet(item: Binding(get: { detached ? chat.permissionPrompt : nil }, set: { _ in })) { p in
      PermissionPromptView(prompt: p) { allow, always in chat.answerPermissions(allow: allow, always: always) }
    }
  }

  // MARK: - Header

  private var header: some View {
    HStack(spacing: 8) {
      if !detached && GuideController.shared.available { GuideNest(busy: chat.busy) }
      Button { showList.toggle() } label: { Image(systemName: showList ? "bubble.left.and.bubble.right.fill" : "list.bullet") }
        .buttonStyle(.borderless)
        .help(showList ? "会話に戻る" : "会話の一覧と検索")
      Text(showList ? String(localized: "会話") : (chat.sessions.first { $0.id == chat.currentId }?.title ?? String(localized: "新しい会話")))
        .font(.headline).lineLimit(1)
      Spacer()
      Button { chat.newSession(); showList = false } label: { Image(systemName: "square.and.pencil") }
        .buttonStyle(.borderless).help("新しい会話")
      Menu {
        // Only for the conversation that is open, never from the list.
        if !showList {
          Button("会話を圧縮") { chat.compact() }.disabled(chat.busy || chat.items.count < 6)
          Divider()
        }
        Button("Skill を再読込") { chat.reloadSkills() }
        if !detached {
          Button("別ウインドウで開く") { openWindow(id: "chat", value: ProjectRef(path: session.identity.url.path)) }
        }
        Divider()
        SettingsLink { Text("接続設定…") }
      } label: { Image(systemName: "ellipsis.circle") }
        .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
    }
    .padding(.horizontal, 10).padding(.vertical, 7)
  }

  // MARK: - Session list

  @State private var hits: [ChatController.SearchHit] = []

  private var sessionList: some View {
    VStack(spacing: 0) {
      TextField("履歴を検索", text: $query).textFieldStyle(.roundedBorder).padding(8)
        .task(id: query) { hits = await chat.search(query) }
      if query.isEmpty {
        List(chat.sessions) { s in
          Button {
            chat.select(s.id)
            showList = false
          } label: {
            VStack(alignment: .leading, spacing: 2) {
              Text(s.title).lineLimit(1).fontWeight(s.id == chat.currentId ? .semibold : .regular)
              Text("\(s.updatedAt.formatted(date: .abbreviated, time: .shortened)) · \(s.messages) 件").font(.caption).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
          .contextMenu { Button("削除", role: .destructive) { chat.delete(s.id) } }
        }
        .overlay { if chat.sessions.isEmpty { ContentUnavailableView("会話はまだありません", systemImage: "bubble.left") } }
      } else {
        List(hits) { h in
          Button {
            chat.select(h.session)
            showList = false
          } label: {
            VStack(alignment: .leading, spacing: 2) {
              Text(h.title).font(.caption.bold())
              Text(h.snippet).font(.caption).foregroundStyle(.secondary).lineLimit(3)
            }
          }
          .buttonStyle(.plain)
        }
        .overlay { if hits.isEmpty { ContentUnavailableView.search(text: query) } }
      }
    }
  }

  // MARK: - Conversation

  private var conversation: some View {
    VStack(spacing: 0) {
      ScrollViewReader { proxy in
        ScrollView {
          LazyVStack(alignment: .leading, spacing: 12) {
            if chat.items.isEmpty {
              VStack(alignment: .leading, spacing: 6) {
                Text(GuideController.shared.connected ? String(localized: "ツグミです。Workflow の相談・生成・編集も、使い方の質問もどうぞ。")
                                                       : String(localized: "Workflow の相談・生成・編集を依頼できます。")).font(.callout)
                Text("Card を右クリックして参照に追加、$ で Skill を選べます。").font(.caption).foregroundStyle(.secondary)
              }
              .padding(.top, 16)
            }
            ForEach(chat.items) { item in
              row(item).id(item.id)
                .background(alignment: .leading) {
                  // Tsugumi's bubble sent the user here: its answer is marked for a moment.
                  if flashing, item.id == chat.items.last(where: { $0.role == "assistant" })?.id {
                    RoundedRectangle(cornerRadius: 6).fill(TsugumiPalette.rufous.opacity(0.12)).padding(-6)
                      .overlay(alignment: .leading) { Rectangle().fill(TsugumiPalette.rufous).frame(width: 3).padding(.leading, -6) }
                      .transition(.opacity)
                  }
                }
            }
            if chat.busy { activity }
          }
          .padding(.horizontal, 14).padding(.vertical, 12)
          .frame(maxWidth: 720, alignment: .leading)
        }
        .scrollIndicators(overflows ? .automatic : .never)
        .onScrollGeometryChange(for: Bool.self) { g in g.contentSize.height > g.containerSize.height + 1 } action: { _, v in overflows = v }
        // Opens at the newest message and keeps it, whatever changes the height
        // (messages, Markdown and math measured, the composer, the panel's width),
        // until the reader scrolls up.
        .defaultScrollAnchor(.bottom, for: .initialOffset)
        .scrollPosition($scrollPosition)
        .onScrollGeometryChange(for: ChatFollow.View.self) { g in
          ChatFollow.View(top: g.visibleRect.minY, bottom: g.visibleRect.maxY, content: g.contentSize.height)
        } action: { _, now in
          if follow.update(now) { scrollPosition.scrollTo(edge: .bottom) }
        }
        .onScrollPhaseChange { _, phase in
          follow.readerScrolling = phase == .interacting || phase == .decelerating
          // (rows measured while the reader's hand was on it left the end further away)
          if phase == .idle, follow.following { scrollPosition.scrollTo(edge: .bottom) }
        }
        .onAppear { followEnd() }
        .onChange(of: chat.currentId) { followEnd() }
        // (the inspector keeps its views while closed: nothing appears again)
        .onChange(of: shown) { if shown { followEnd() } }
        .onChange(of: chat.items.last.flatMap { $0.role == "user" ? $0.id : nil }) { _, sent in
          if sent != nil { followEnd() }
        }
        .onChange(of: GuideController.shared.chatFlash) { _, d in
          guard d != nil, GuideController.shared.speakingChat === chat, let last = chat.items.last(where: { $0.role == "assistant" }) else { return }
          withAnimation { proxy.scrollTo(last.id, anchor: .top) }
          withAnimation(.easeOut(duration: 0.2)) { flashing = true }
          Task { @MainActor in
            try? await Task.sleep(for: .seconds(2))
            withAnimation(.easeOut(duration: 0.6)) { flashing = false }
          }
        }
      }
      if let s = chat.status {
        HStack(spacing: 6) {
          Image(systemName: chat.failed ? "exclamationmark.triangle.fill" : "info.circle")
            .foregroundStyle(chat.failed ? .red : .secondary)
          Text(s).font(.caption).foregroundStyle(chat.failed ? .red : .secondary).lineLimit(3).textSelection(.enabled)
          Spacer()
          if chat.usageLimited {
            Button("Manage usage") { NSWorkspace.shared.open(ChatGPTAuth.usageURL) }.controlSize(.small).buttonStyle(.borderedProminent)
          } else if chat.contextFull {
            Button("新しい会話") { chat.newSession() }.controlSize(.small).buttonStyle(.borderedProminent)
          } else if chat.failed { Button("再試行") { chat.retry() }.controlSize(.small) }
          if chat.interrupted && !chat.busy { Button("再開") { chat.resume() }.controlSize(.small) }
          if chat.stoppedAtLimit && !chat.busy { Button("続ける") { chat.continueWork() }.controlSize(.small) }
        }
        .padding(.horizontal, 12).padding(.vertical, 5)
      }
      if !detached { ChatChoices(chat: chat, tab: tab) }
      composer
    }
  }

  private func followEnd() {
    follow.restart()
    scrollPosition.scrollTo(edge: .bottom)
  }

  /// What the agent is doing right now (no internal reasoning, only the step).
  private var activity: some View {
    HStack(spacing: 6) {
      ProgressView().controlSize(.small)
      Text(currentStep).font(.caption).foregroundStyle(.secondary).lineLimit(1)
    }
  }

  private var currentStep: String {
    if let t = chat.items.last(where: { $0.role == "tool" && $0.tool?.ok == nil }), let name = t.tool?.name {
      return ToolStep(name: name, args: t.text).running
    }
    if chat.items.last?.role == "assistant" { return String(localized: "回答を書いています…") }
    return String(localized: "考えています…")
  }

  private var composer: some View {
    VStack(alignment: .leading, spacing: 6) {
      if !chat.folders.isEmpty {
        ScrollView(.horizontal, showsIndicators: false) {
          HStack {
            ForEach(chat.folders, id: \.self) { f in
              HStack(spacing: 3) {
                Image(systemName: "folder").font(.caption2)
                Text(chat.folders.filter { ($0 as NSString).lastPathComponent == (f as NSString).lastPathComponent }.count > 1
                     ? f : (f as NSString).lastPathComponent).font(.caption)
                ReadOnlyMark(why: String(localized: "読み取り専用で共有しています。Agent だけが読みます。"))
                Button { chat.detach(folder: f) } label: { Image(systemName: "xmark.circle.fill") }
                  .buttonStyle(.borderless).font(.caption)
                  .disabled(chat.attaching)
                  .help("添付を外す")
              }
              .help(f)
              .padding(.horizontal, 6).padding(.vertical, 2)
              .background(Color.secondary.opacity(0.12), in: Capsule())
            }
          }
        }
      }
      if !chat.references.isEmpty {
        ScrollView(.horizontal, showsIndicators: false) {
          HStack {
            ForEach(chat.references) { r in
              HStack(spacing: 3) {
                Image(systemName: "paperclip").font(.caption2)
                Text(r.label).font(.caption)
                Button { chat.references.removeAll { $0 == r } } label: { Image(systemName: "xmark.circle.fill") }
                  .buttonStyle(.borderless).font(.caption)
                  .help("参照を外す")
              }
              .padding(.horizontal, 6).padding(.vertical, 2)
              .background(Color.accentColor.opacity(0.12), in: Capsule())
            }
          }
        }
      }
      if skillQuery != nil {
        let matches = skillMatches
        VStack(alignment: .leading, spacing: 0) {
          if matches.isEmpty {
            Text(chat.skills.isEmpty ? "Skill がありません（設定 > Skills でフォルダを追加、またはプロジェクトの skills/）" : "一致する Skill はありません")
              .font(.caption).foregroundStyle(.secondary).padding(6)
          }
          ForEach(Array(matches.enumerated()), id: \.element.id) { i, s in
            Button { insertSkill(s) } label: {
              VStack(alignment: .leading) {
                Text("$" + s.name).font(.callout.monospaced())
                Text("\(s.description) · \(s.source)").font(.caption).foregroundStyle(.secondary).lineLimit(1)
              }
              .frame(maxWidth: .infinity, alignment: .leading)
              .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padding(.horizontal, 6).padding(.vertical, 3)
            .background(i == skillIndex ? Color.accentColor.opacity(0.18) : .clear, in: RoundedRectangle(cornerRadius: 4))
          }
        }
        .background(.background.secondary, in: RoundedRectangle(cornerRadius: 6))
      }
      VStack(spacing: 4) {
        // Starts one line high and grows with the text (up to a limit).
        ZStack(alignment: .topLeading) {
          // Gone while typing (and while an input method composes, which the
          // text binding does not see yet).
          if input.isEmpty && !inputFocused {
            Text("メッセージ（\(prefs.sendKey == .returnKey ? "Return" : "⌘Return") で送信）")
              .foregroundStyle(.tertiary).padding(.leading, 5).padding(.top, 1)
              .allowsHitTesting(false)
          }
          TextEditor(text: $input)
            .font(.body)
            .scrollContentBackground(.hidden)
            .frame(height: min(160, max(22, inputHeight)))
            .scrollIndicators(inputHeight > 160 ? .automatic : .never)
            .focused($inputFocused)
            .onChange(of: input) { updateSkillQuery() }
            .onKeyPress(.return, phases: .down) { press in
              let cmd = press.modifiers.contains(.command), shift = press.modifiers.contains(.shift)
              if !cmd && !shift, let s = highlightedSkill {
                insertSkill(s)
                return .handled
              }
              // ⌘Return always sends; Return too with the standard setting; ⇧Return is a new line.
              let sends = cmd || (prefs.sendKey == .returnKey && !shift)
              if sends {
                send()
                return .handled
              }
              return .ignored
            }
            .onKeyPress(.tab) {
              guard let s = highlightedSkill else { return .ignored }
              insertSkill(s)
              return .handled
            }
            .onKeyPress(.upArrow) {
              guard skillQuery != nil, !skillMatches.isEmpty else { return .ignored }
              skillIndex = (skillIndex + skillMatches.count - 1) % skillMatches.count
              return .handled
            }
            .onKeyPress(.downArrow) {
              guard skillQuery != nil, !skillMatches.isEmpty else { return .ignored }
              skillIndex = (skillIndex + 1) % skillMatches.count
              return .handled
            }
            .onKeyPress(.escape) {
              guard skillQuery != nil else { return .ignored }
              skillQuery = nil
              return .handled
            }
        }
        HStack(spacing: 8) {
          Menu {
            Button("選択した Card を参照に追加") { if let tab { for k in tab.selection.sorted() { chat.addReference(.card(k)) } } }
              .disabled(tab?.selection.isEmpty ?? true)
            Button("フォルダを添付…") { chooseFolder() }
              .disabled(chat.attaching || !session.connected)
          } label: { Image(systemName: chat.attaching ? "hourglass" : "paperclip") }
            .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            .help("参照・フォルダを添付")
          // The model, as quiet text under the input (a menu, not a pop-up box).
          Button { modelMenu.show() } label: {
            HStack(spacing: 3) {
              Text(modelLabel).lineLimit(1)
              Image(systemName: "chevron.down").font(.system(size: 8, weight: .semibold))
            }
            .font(.caption)
            .foregroundStyle(.secondary)
            .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
          .fixedSize()
          .background(ModelMenuAnchor(menu: modelMenu))
          .disabled(!chat.provider.isRemote)
          .help("モデル（接続先: \(chat.provider.name)）")
          thoughtsToggle
          Spacer()
          Text("\(chat.usage.inputTokens / 1000)k").font(.caption2).foregroundStyle(.tertiary)
            .help(String(localized: "直近の入力トークン数（上限に近づくと自動で圧縮）。この会話でキャッシュから読んだ量 \(chat.usage.cacheReadTokens)、キャッシュに書いた量 \(chat.usage.cacheWriteTokens)"))
          if chat.busy {
            Button(action: chat.cancel) { Image(systemName: "stop.circle.fill").font(.title2) }
              .buttonStyle(.borderless)
              .help("回答を停止（Workflow の実行は止まりません）")
              .accessibilityLabel("回答を停止")
          } else {
            Button(action: send) { Image(systemName: "arrow.up.circle.fill").font(.title2) }
              .buttonStyle(.borderless)
              .disabled(input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !session.connected || !chat.available || chat.attaching)
              .help(prefs.sendKey == .returnKey ? "送信（Return）" : "送信（⌘Return）")
              .accessibilityLabel("送信")
          }
        }
      }
      .padding(.horizontal, 8).padding(.vertical, 6)
      .background(.background, in: RoundedRectangle(cornerRadius: 12))
      .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(.secondary.opacity(inputFocused ? 0.5 : 0.25)))
    }
    .padding(10)
    .task {
      modelMenu.onOpen = { refreshModels() }
      modelMenu.onChoose = { provider, m in choose(provider, m) }
      UIScript.shared.modelMenu = modelMenu
      modelMenu.onEffort = { level in
        CorePreferences.shared.set(level, forKey: AgentSettings.effortKey)
        showChoices()
      }
      modelMenu.onFast = { on in
        CorePreferences.shared.set(on ? AgentSettings.current().model : "", forKey: AgentSettings.fastModelKey)
        showChoices()
      }
      await loadModels()
    }
    .onDisappear {
      modelMenu.onOpen = nil
      modelMenu.onChoose = nil
      modelMenu.onEffort = nil
      modelMenu.onFast = nil
    }
    // Settings > API switched the connection (ChatGPT, OpenAI, Anthropic): its model and list.
    .onChange(of: providerSetting) { Task { await loadModels() } }
    .confirmationDialog("思考の過程を描きますか？", isPresented: $askThoughts) {
      Button("描く") { drawThoughtsAsked = true; drawThoughts = true }
      Button("キャンセル", role: .cancel) {}
    } message: {
      Text("Agent の発言とツールの結果を、数秒ごとに小さなモデル（\(ThoughtSummarizer.config()?.model ?? "なし")、接続先: \(chat.provider.name)）に送って要約し、Canvas の鶫の吹き出しに出します。Agent とは別に token を使います。オフにすると、吹き出しはツール呼び出しから作る短い文だけになります。")
    }
    .confirmationDialog("フォルダを添付しますか？", isPresented: Binding(get: { foldersToAttach != nil }, set: { if !$0 { foldersToAttach = nil } }),
                        presenting: foldersToAttach) { urls in
      Button("添付（環境を再起動）") { Task { await chat.attach(folders: urls) } }
      Button("キャンセル", role: .cancel) {}
    } message: { urls in
      Text("「\(urls.map(\.path).joined(separator: "\n"))」を読み取り専用で実行環境に共有し、Agent が読めるようにします。Agent が読んだファイルの中身は、接続先（\(chat.provider.name)）に送られます。共有のため実行環境を再起動します（実行中の Run は止まります）。")
    }
  }

  private func chooseFolder() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.allowsMultipleSelection = true
    panel.message = String(localized: "チャットに添付するフォルダ（中身は Agent が読み、接続先に送られます）")
    if panel.runModal() == .OK, !panel.urls.isEmpty { foldersToAttach = panel.urls }
  }

  /// Rough text height for the growing input (17 pt per line).
  private var inputHeight: CGFloat {
    let lines = input.split(separator: "\n", omittingEmptySubsequences: false).reduce(0) { $0 + max(1, ($1.count + 39) / 40) }
    return CGFloat(lines) * 17 + 4
  }

  @ViewBuilder private func row(_ item: ChatController.Item) -> some View {
    switch item.role {
    case "user":
      HStack {
        Spacer(minLength: 40)
        Text(item.text).textSelection(.enabled)
          .padding(.horizontal, 11).padding(.vertical, 7)
          .background(Color.accentColor.opacity(0.14), in: RoundedRectangle(cornerRadius: 12))
      }
      .opacity(item.archived ? 0.6 : 1)
    case "tool":
      ToolRow(item: item, tab: tab, session: session).opacity(item.archived ? 0.6 : 1)
    case "helper":
      HelperRow(item: item, tint: session.helperPresences.first { $0.label == item.helper }?.tint ?? 1)
    case "note":
      Label(item.text, systemImage: "rectangle.compress.vertical").font(.caption).foregroundStyle(.secondary)
        .frame(maxWidth: .infinity)
    default:
      if chat.busy && item.id == chat.items.last?.id {
        StreamingText(text: item.text)
      } else {
        ChatMarkdown(text: item.text).modifier(EvidenceMarkup(text: item.text, session: session, tab: tab)).opacity(item.archived ? 0.6 : 1)
      }
    }
  }

  private func send() {
    let text = input
    guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, !chat.busy, !chat.attaching, session.connected else { return }
    chat.send(text)
    input = ""
    skillQuery = nil
  }

  private var skillMatches: [Skill] {
    guard let q = skillQuery else { return [] }
    return Array(chat.skills.filter { q.isEmpty || $0.name.localizedCaseInsensitiveContains(q) }.prefix(8))
  }

  private var highlightedSkill: Skill? {
    let m = skillMatches
    return m.isEmpty ? nil : m[min(skillIndex, m.count - 1)]
  }

  /// `$` at the start of the word being typed opens the Skill picker.
  private func updateSkillQuery() {
    skillIndex = 0
    guard let last = input.split(separator: " ", omittingEmptySubsequences: false).last?
      .split(separator: "\n", omittingEmptySubsequences: false).last, last.hasPrefix("$") else {
      skillQuery = nil
      return
    }
    let q = String(last.dropFirst())
    skillQuery = chat.skills.contains { $0.name == q } ? nil : q
  }

  private func insertSkill(_ s: Skill) {
    if let r = input.range(of: "$" + (skillQuery ?? ""), options: .backwards) { input.replaceSubrange(r, with: "$\(s.name) ") }
    skillQuery = nil
    inputFocused = true
  }

  /// The connections set up, each with the models saved for it. Opening the
  /// panel reads account status from agentd and Keychain attributes only.
  private func loadModels() async {
    await CorePreferences.shared.reload()
    let accounts = await ChatGPTAuth.shared().accounts()
    for id in ["anthropic", "openai", "chatgpt"] {
      _ = try? await CoreAgentClient.shared.storedModels(id)
    }
    showConnections(chatGPT: accounts.contains { $0.active && $0.signedIn && $0.planEnabled })
  }

  /// Everything saved, on the panel and in the menu. Without waiting for
  /// anything, as the main actor does not run while the menu is open.
  private func showConnections(chatGPT: Bool) {
    let current = AgentSettings.current().model
    let names = Dictionary(ModelCatalogSnapshot.shared.models("chatgpt").map { ($0.id, $0.name) }, uniquingKeysWith: { a, _ in a })
    var out: [Connection] = []
    func add(_ id: String, _ name: String) {
      let chosen = AgentSettings.config(for: id).model
      let listed = ModelCatalogSnapshot.shared.models(id).map(\.id)
      out.append(Connection(id: id, name: name, models: Array(Set(listed + [chosen])).sorted()))
    }
    if ProviderCredentials.exists(account: ProviderConfig.anthropic.name) { add("anthropic", "Anthropic API") }
    if ProviderCredentials.exists(account: ProviderConfig.openAI.name) { add("openai", "OpenAI API") }
    if chatGPT { add("chatgpt", "ChatGPT プラン") }
    if providerSetting == "fixture" { add("fixture", "Local fixture") }
    model = current
    modelNames = names
    connections = out
    modelMenu.setContents(connections: out, provider: providerSetting, model: current, names: names)
    showChoices()
  }

  /// What the requests carry, in the menu.
  private func showChoices() {
    let c = AgentSettings.chat()
    modelMenu.setChoices(effort: c.effort, fast: c.fast)
  }

  /// The model with what its requests carry beyond the defaults.
  private var modelLabel: String {
    var c = AgentSettings.config(for: providerSetting)
    c.model = model
    c = c.choosing(effort: effortSetting, fastModel: fastModelSetting)
    return ([modelName(model), c.effort, c.fast ? "Fast" : nil] as [String?]).compactMap { $0 }.joined(separator: " · ")
  }

  /// The menu opened: the models of every connection are listed again. It
  /// shows the saved lists at once, and each new list when it comes; a
  /// connection that fails or is slow keeps what it has. Never the keychain's
  /// dialog, however often it opens.
  private func refreshModels() {
    UIScript.shared.note("model menu opened")
    let wasOn = connections.contains { $0.id == "chatgpt" }
    let lists = modelLists
    let show = showConnections
    Task.detached {
      for id in ["anthropic", "openai", "chatgpt"] {
        _ = try? await CoreAgentClient.shared.storedModels(id)
      }
      let accounts = await ChatGPTAuth.shared().accountsWithoutAsking()
      // A record that cannot be read without asking says nothing: the connection stays as it was.
      let chatGPT = accounts.map { $0.contains { $0.active && $0.signedIn && $0.planEnabled } } ?? wasOn
      onMainWhileMenuTracks {
        show(chatGPT)
        var ids = ["anthropic", "openai"].filter { ProviderCredentials.exists(account: AgentSettings.config(for: $0).name) }
        if chatGPT { ids.append("chatgpt") }
        lists.refresh(ids) { id, outcome in
          UIScript.shared.note("model list \(id): \(outcome)")
          show(chatGPT)
        }
      }
    }
  }

  /// A model of a connection chosen in the menu: that connection is used from now on.
  private func choose(_ provider: String, _ m: String) {
    CorePreferences.shared.set(m, forKey: AgentSettings.modelKey(provider))
    providerSetting = provider
    model = m
    modelMenu.setCurrent(provider: provider, model: m)
    showChoices()
    GuideController.shared.refreshAvailability()
  }

  private func modelName(_ m: String) -> String { modelNames[m] ?? m }

  /// "Draw thoughts": on, a small model writes what the Agent is thinking
  /// for the tsugumi's bubble (asks once before the first time).
  private var thoughtsToggle: some View {
    Button {
      if drawThoughts { drawThoughts = false } else if drawThoughtsAsked { drawThoughts = true } else { askThoughts = true }
    } label: {
      Image(systemName: drawThoughts ? "bubble.left.and.text.bubble.right.fill" : "bubble.left.and.text.bubble.right")
        .font(.caption)
        .foregroundStyle(drawThoughts ? Color.accentColor : .secondary)
    }
    .buttonStyle(.borderless)
    .disabled(ThoughtSummarizer.config() == nil)
    .help(drawThoughts
      ? String(localized: "思考の過程を描く: オン（使った token: 入力 \(chat.thoughts.usage.inputTokens)、出力 \(chat.thoughts.usage.outputTokens)）")
      : String(localized: "思考の過程を描く: Agent の考えを要約して Canvas に出します（追加の token を使います）"))
    .accessibilityLabel("思考の過程を描く")
  }
}

/// A tool call in words: what is being done and on which Card or file.
struct ToolStep {
  let name: String
  let args: String

  private var target: String? {
    guard let v = try? JSONValue.parse(args.components(separatedBy: "\n→ ").first ?? args) else { return nil }
    if let c = v["card"]?.array?.compactMap(\.string), !c.isEmpty { return c.joined(separator: "/") }
    if let c = v["card"]?["segments"]?.array?.compactMap(\.string), !c.isEmpty { return c.joined(separator: "/") }
    if let c = v["card"]?.string { return c }
    return v["path"]?.string ?? v["definition"]?.string ?? v["query"]?.string
  }

  var card: CardKey? {
    guard let v = try? JSONValue.parse(args.components(separatedBy: "\n→ ").first ?? args) else { return nil }
    if let c = v["card"]?.array?.compactMap(\.string), !c.isEmpty { return key(c) }
    // (a CardPath as the model writes it)
    if let c = v["card"]?["segments"]?.array?.compactMap(\.string), !c.isEmpty { return key(c) }
    return v["card"]?.string
  }

  /// What the save did to the comments on Cards, one line each.
  var savedComments: [String] {
    guard let from = args.range(of: "\n→ comments: "), let end = args[from.upperBound...].firstIndex(of: "\n"),
          let list = (try? JSONValue.parse(String(args[from.upperBound..<end])))?.array else { return [] }
    return list.map { c in
      var line = String(localized: "\(c["card"]?.string ?? "") · コメントを\(CardComment.change(c["change"]?.string ?? "", by: c["by"]?.string)): \(c["text"]?.string ?? "")")
      if let r = c["resolution"]?.string, !r.isEmpty { line += " — " + r }
      let cards = c["cards"]?.array?.compactMap(\.string) ?? []
      if !cards.isEmpty { line += String(localized: "（変更した Card: \(cards.joined(separator: "、"))）") }
      return line
    }
  }

  private var verb: (running: String, done: String) {
    let n = name.lowercased()
    if n == "delegate" { return (String(localized: "手分けして進めています"), String(localized: "手分けして進めました")) }
    if n == "web_search" { return (String(localized: "Web を検索しています"), String(localized: "Web を検索")) }
    if n.contains("save") || n.contains("put") || n.contains("write") { return (String(localized: "変更を保存しています"), String(localized: "変更を保存")) }
    if n.contains("run") && !n.contains("status") { return (String(localized: "実行しています"), String(localized: "実行")) }
    if n.contains("read") || n.contains("inspect") || n.contains("get") { return (String(localized: "確認しています"), String(localized: "確認")) }
    if n.contains("log") { return (String(localized: "ログを読んでいます"), String(localized: "ログを確認")) }
    if n.contains("validate") || n.contains("check") { return (String(localized: "検証しています"), String(localized: "検証")) }
    if n.contains("format") { return (String(localized: "整形しています"), String(localized: "整形")) }
    if n.contains("status") || n.contains("record") || n.contains("history") { return (String(localized: "結果を確認しています"), String(localized: "結果を確認")) }
    return (String(localized: "\(name) を実行しています"), name)
  }

  var running: String { verb.running + (target.map { "（\($0)）" } ?? "") + "…" }
  var done: String { verb.done + (target.map { "（\($0)）" } ?? "") }
}

/// A helper at work (delegate): its name in its bird's colour and what it
/// does now, or how its work ended.
private struct HelperRow: View {
  let item: ChatController.Item
  let tint: Int

  var body: some View {
    let step = item.tool.map { ToolStep(name: $0.name, args: item.text) }
    HStack(spacing: 6) {
      Circle().fill(PresenceColor.of(tint)).frame(width: 7, height: 7)
      Text(item.helper ?? "").font(.caption.weight(.semibold)).foregroundStyle(PresenceColor.of(tint))
      Group {
        if let ended = item.ended {
          Text(ended)
        } else if let step, let t = item.tool {
          Text(t.ok == nil ? step.running : step.done + (t.ok! ? "" : String(localized: "：失敗")))
        } else {
          Text("考えています…")
        }
      }
      .font(.caption).foregroundStyle(item.tool?.ok == false && item.ended == nil ? .red : .secondary).lineLimit(1)
      Spacer()
      if item.ended == nil { ProgressView().controlSize(.mini) }
    }
    .padding(.leading, 10)
  }
}

/// A folded tool step: one line saying what was done; details on demand.
struct ToolRow: View {
  let item: ChatController.Item
  let tab: WorkflowTab?
  let session: ProjectSession
  @State private var open = false

  /// A search's details with its sources as links (only http and https).
  static func linked(_ text: String) -> AttributedString {
    var out = AttributedString(text)
    guard let detector = try? NSDataDetector(types: NSTextCheckingResult.CheckingType.link.rawValue) else { return out }
    for m in detector.matches(in: text, range: NSRange(text.startIndex..., in: text)) {
      guard let url = m.url, ["http", "https"].contains(url.scheme?.lowercased() ?? ""),
            let r = Range(m.range, in: text), let a = Range(r, in: out) else { continue }
      out[a].link = url
    }
    return out
  }

  var body: some View {
    let step = ToolStep(name: item.tool?.name ?? "", args: item.text)
    VStack(alignment: .leading, spacing: 4) {
      HStack(spacing: 5) {
        Group {
          if item.tool?.ok == nil { ProgressView().controlSize(.mini) } else {
            Image(systemName: item.tool!.ok! ? "checkmark.circle" : "xmark.circle.fill")
              .foregroundStyle(item.tool!.ok! ? Color.secondary : Color.red)
          }
        }
        .frame(width: 14)
        Text(item.tool?.ok == nil ? step.running : step.done + (item.tool!.ok! ? "" : String(localized: "：失敗")))
          .font(.caption).foregroundStyle(item.tool?.ok == false ? .red : .secondary).lineLimit(1)
        if let k = step.card, let tab, session.instance(k) != nil {
          Button("Card を表示") {
            tab.enter(Array(path(k).dropLast()))
            tab.selection = [k]
            NotificationCenter.default.post(name: .canvasReveal, object: tab.id, userInfo: ["card": k])
          }
          .buttonStyle(.link).font(.caption)
        }
        Spacer()
        Button { open.toggle() } label: { Image(systemName: open ? "chevron.up" : "chevron.down").font(.caption2) }
          .buttonStyle(.borderless).foregroundStyle(.secondary)
          .help(open ? "詳細を閉じる" : "ツールの詳細")
      }
      ForEach(Array(step.savedComments.enumerated()), id: \.offset) { _, line in
        Text(verbatim: line)
          .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
          .padding(.leading, 19)
          .fixedSize(horizontal: false, vertical: true)
      }
      if open {
        (item.tool?.name == "web_search" ? Text(Self.linked(item.text)) : Text(item.text))
          .font(.caption.monospaced()).textSelection(.enabled)
          .lineLimit(40)
          .frame(maxWidth: .infinity, alignment: .leading)
          .padding(6)
          .background(.background.secondary, in: RoundedRectangle(cornerRadius: 6))
      }
    }
  }
}

/// The answer being written: shown at a steady pace that catches up with
/// what arrived (no jumps of whole chunks), new blocks fading in.
struct StreamingText: View {
  let text: String
  @State private var shown = 0

  var body: some View {
    MarkdownView(text: String(text.prefix(shown)), fontSize: 13, codeTools: true, fadeIn: true)
      .frame(maxWidth: .infinity, alignment: .leading)
      .task(id: text.count) {
        // About 0.25 s to catch up with whatever arrived, a few characters per frame at least.
        while shown < text.count {
          let behind = text.count - shown
          shown = min(text.count, shown + max(2, behind / 15))
          try? await Task.sleep(for: .milliseconds(16))
          if Task.isCancelled { break }
        }
      }
  }
}

/// A finished answer as Markdown with math; very long answers are folded.
struct ChatMarkdown: View {
  let text: String
  @State private var expanded = false

  var body: some View {
    let long = text.count > 6000 && !expanded
    VStack(alignment: .leading, spacing: 8) {
      MarkdownView(text: long ? Self.fold(text) : text, fontSize: 13, codeTools: true)
      if long {
        Button("続きを表示") { expanded = true }.buttonStyle(.link).font(.caption)
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }

  /// The first part of a long answer, cut between paragraphs (never inside code).
  static func fold(_ text: String) -> String {
    var out = ""
    var fence = false
    for line in text.components(separatedBy: "\n") {
      if line.hasPrefix("```") { fence.toggle() }
      out += line + "\n"
      if out.count > 4000 && !fence && line.trimmingCharacters(in: .whitespaces).isEmpty { break }
    }
    return out
  }
}

struct ChatCodeBlock: View {
  let code: String
  let language: String
  @State private var copied = false
  @State private var expanded = false

  var body: some View {
    let lines = code.components(separatedBy: "\n")
    let folded = lines.count > 30 && !expanded
    VStack(alignment: .leading, spacing: 0) {
      HStack {
        Text(language.isEmpty ? "code" : language).font(.caption2).foregroundStyle(.secondary)
        Spacer()
        Button(copied ? "コピーしました" : "コピー") {
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(code, forType: .string)
          copied = true
        }
        .buttonStyle(.borderless).font(.caption2)
      }
      .padding(.horizontal, 8).padding(.vertical, 4)
      ScrollView(.horizontal, showsIndicators: false) {
        Text(folded ? lines.prefix(30).joined(separator: "\n") : code)
          .font(.system(size: 11.5, design: .monospaced))
          .textSelection(.enabled)
          .padding(.horizontal, 8).padding(.bottom, 6)
      }
      if folded {
        Button("残り \(lines.count - 30) 行を表示") { expanded = true }.buttonStyle(.link).font(.caption).padding([.horizontal, .bottom], 8)
      }
    }
    .background(.background.secondary, in: RoundedRectangle(cornerRadius: 8))
  }
}
