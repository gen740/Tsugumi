import AppKit
import SwiftUI
import TsugumiAgent
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
  @State private var showList = false
  @State private var query = ""
  @State private var input = ""
  @State private var model = UserDefaults.standard.string(forKey: "agent.model") ?? ProviderConfig.anthropic.model
  @State private var models: [String] = []
  @State private var skillQuery: String?
  /// The highlighted Skill candidate (↑ / ↓, Return or Tab takes it).
  @State private var skillIndex = 0
  @State private var folderToAttach: URL?
  /// The conversation is taller than its area (only then a scroll bar shows).
  @State private var overflows = false
  @FocusState private var inputFocused: Bool

  var body: some View {
    VStack(spacing: 0) {
      header
      Divider()
      if showList { sessionList } else { conversation }
    }
    .background(Color(nsColor: .windowBackgroundColor))
  }

  // MARK: - Header

  private var header: some View {
    HStack(spacing: 8) {
      Button { showList.toggle() } label: { Image(systemName: showList ? "bubble.left.and.bubble.right.fill" : "list.bullet") }
        .buttonStyle(.borderless)
        .help(showList ? "会話に戻る" : "会話の一覧と検索")
      Text(showList ? "会話" : (chat.sessions.first { $0.id == chat.currentId }?.title ?? "新しい会話"))
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

  private var sessionList: some View {
    VStack(spacing: 0) {
      TextField("履歴を検索", text: $query).textFieldStyle(.roundedBorder).padding(8)
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
        let hits = chat.search(query)
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
                Text("Workflow の相談・生成・編集を依頼できます。").font(.callout)
                Text("Card を右クリックして参照に追加、$ で Skill を選べます。").font(.caption).foregroundStyle(.secondary)
              }
              .padding(.top, 16)
            }
            ForEach(chat.items) { item in row(item).id(item.id) }
            if chat.busy { activity.id("activity") }
          }
          .padding(.horizontal, 14).padding(.vertical, 12)
          .frame(maxWidth: 720, alignment: .leading)
        }
        .scrollIndicators(overflows ? .automatic : .never)
        .onScrollGeometryChange(for: Bool.self) { g in g.contentSize.height > g.containerSize.height + 1 } action: { _, v in overflows = v }
        .onChange(of: chat.items.count) { scrollToEnd(proxy) }
        .onChange(of: chat.items.last?.text) { scrollToEnd(proxy) }
      }
      if let s = chat.status {
        HStack(spacing: 6) {
          Image(systemName: chat.failed ? "exclamationmark.triangle.fill" : "info.circle")
            .foregroundStyle(chat.failed ? .red : .secondary)
          Text(s).font(.caption).foregroundStyle(chat.failed ? .red : .secondary).lineLimit(3).textSelection(.enabled)
          Spacer()
          if chat.failed { Button("再試行") { chat.retry() }.controlSize(.small) }
        }
        .padding(.horizontal, 12).padding(.vertical, 5)
      }
      composer
    }
  }

  private func scrollToEnd(_ proxy: ScrollViewProxy) {
    if chat.busy { proxy.scrollTo("activity", anchor: .bottom) } else if let last = chat.items.last { proxy.scrollTo(last.id, anchor: .bottom) }
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
                Text((f as NSString).lastPathComponent).font(.caption)
                ReadOnlyMark(why: String(localized: "読み取り専用で共有しています。Agent だけが読みます。"))
                Button { chat.detach(folder: f) } label: { Image(systemName: "xmark.circle.fill") }
                  .buttonStyle(.borderless).font(.caption)
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
          Menu {
            ForEach(Array(Set(models + [model])).sorted(), id: \.self) { m in
              Button { model = m } label: {
                if m == model { Label(m, systemImage: "checkmark") } else { Text(m) }
              }
            }
          } label: {
            HStack(spacing: 3) {
              Text(model).lineLimit(1)
              Image(systemName: "chevron.down").font(.system(size: 8, weight: .semibold))
            }
            .font(.caption)
            .foregroundStyle(.secondary)
          }
          .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
          .tint(.secondary)
          .foregroundStyle(.secondary)
          .onChange(of: model) {
            UserDefaults.standard.set(model, forKey: chat.provider.api == .openAIResponses ? "agent.model.openai" : "agent.model")
          }
          .disabled(!chat.provider.usesKeychain)
          .help("モデル（接続先: \(chat.provider.name)）")
          Spacer()
          Text("\(chat.usage.inputTokens / 1000)k").font(.caption2).foregroundStyle(.tertiary)
            .help("直近の入力トークン数（上限に近づくと自動で圧縮）")
          if chat.busy {
            Button(action: chat.cancel) { Image(systemName: "stop.circle.fill").font(.title2) }
              .buttonStyle(.borderless)
              .help("回答を停止（Workflow の実行は止まりません）")
              .accessibilityLabel("回答を停止")
          } else {
            Button(action: send) { Image(systemName: "arrow.up.circle.fill").font(.title2) }
              .buttonStyle(.borderless)
              .disabled(input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !session.connected || !chat.available)
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
    .task { await loadModels() }
    .confirmationDialog("フォルダを添付しますか？", isPresented: Binding(get: { folderToAttach != nil }, set: { if !$0 { folderToAttach = nil } }),
                        presenting: folderToAttach) { url in
      Button("添付（環境を再起動）") { Task { await chat.attach(folder: url) } }
      Button("キャンセル", role: .cancel) {}
    } message: { url in
      Text("「\(url.lastPathComponent)」を読み取り専用で実行環境に共有し、Agent が読めるようにします。Agent が読んだファイルの中身は、接続先（\(chat.provider.name)）に送られます。共有のため実行環境を再起動します（実行中の Run は止まります）。")
    }
  }

  private func chooseFolder() {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.allowsMultipleSelection = false
    panel.message = String(localized: "チャットに添付するフォルダ（中身は Agent が読み、接続先に送られます）")
    if panel.runModal() == .OK, let url = panel.url { folderToAttach = url }
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
    case "note":
      Label(item.text, systemImage: "rectangle.compress.vertical").font(.caption).foregroundStyle(.secondary)
        .frame(maxWidth: .infinity)
    default:
      if chat.busy && item.id == chat.items.last?.id {
        StreamingText(text: item.text)
      } else {
        MarkdownBlocks(text: item.text).opacity(item.archived ? 0.6 : 1)
      }
    }
  }

  private func send() {
    let text = input
    guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, !chat.busy, session.connected else { return }
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

  /// Models listed earlier in Settings > API (no network request from here).
  private func loadModels() async {
    models = UserDefaults.standard.stringArray(forKey: "agent.models") ?? []
  }
}

/// A tool call in words: what is being done and on which Card or file.
struct ToolStep {
  let name: String
  let args: String

  private var target: String? {
    guard let v = try? JSONValue.parse(args.components(separatedBy: "\n→ ").first ?? args) else { return nil }
    if let c = v["card"]?.array?.compactMap(\.string), !c.isEmpty { return c.joined(separator: "/") }
    if let c = v["card"]?.string { return c }
    return v["path"]?.string ?? v["definition"]?.string
  }

  var card: CardKey? {
    guard let v = try? JSONValue.parse(args.components(separatedBy: "\n→ ").first ?? args) else { return nil }
    if let c = v["card"]?.array?.compactMap(\.string), !c.isEmpty { return key(c) }
    return v["card"]?.string
  }

  private var verb: (running: String, done: String) {
    let n = name.lowercased()
    if n.contains("save") || n.contains("put") || n.contains("write") { return ("変更を保存しています", "変更を保存") }
    if n.contains("run") && !n.contains("status") { return ("実行しています", "実行") }
    if n.contains("read") || n.contains("inspect") || n.contains("get") { return ("確認しています", "確認") }
    if n.contains("log") { return ("ログを読んでいます", "ログを確認") }
    if n.contains("validate") || n.contains("check") { return ("検証しています", "検証") }
    if n.contains("format") { return ("整形しています", "整形") }
    if n.contains("status") || n.contains("record") || n.contains("history") { return ("結果を確認しています", "結果を確認") }
    return ("\(name) を実行しています", name)
  }

  var running: String { verb.running + (target.map { "（\($0)）" } ?? "") + "…" }
  var done: String { verb.done + (target.map { "（\($0)）" } ?? "") }
}

/// A folded tool step: one line saying what was done; details on demand.
private struct ToolRow: View {
  let item: ChatController.Item
  let tab: WorkflowTab?
  let session: ProjectSession
  @State private var open = false

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
      if open {
        Text(item.text).font(.caption.monospaced()).textSelection(.enabled)
          .lineLimit(40)
          .frame(maxWidth: .infinity, alignment: .leading)
          .padding(6)
          .background(.background.secondary, in: RoundedRectangle(cornerRadius: 6))
      }
    }
  }
}

/// Markdown of an answer: paragraphs, headings, lists and code blocks (with
/// copy). Very long answers are folded.
/// The answer being written: shown at a steady pace that catches up with
/// what arrived (no jumps of whole chunks), new blocks fading in.
struct StreamingText: View {
  let text: String
  @State private var shown = 0

  var body: some View {
    let visible = String(text.prefix(shown))
    let blocks = MarkdownBlocks.blocks(visible)
    VStack(alignment: .leading, spacing: 8) {
      ForEach(Array(blocks.enumerated()), id: \.offset) { _, b in
        MarkdownBlocks.view(b).transition(.opacity.animation(.easeOut(duration: 0.25)))
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
    .textSelection(.enabled)
    .animation(.easeOut(duration: 0.25), value: blocks.count)
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

struct MarkdownBlocks: View {
  let text: String
  @State private var expanded = false

  enum Block: Hashable {
    case paragraph(String)
    case heading(String, Int)
    case bullet(String)
    case code(String, String)
  }

  static func blocks(_ text: String) -> [Block] {
    var out: [Block] = []
    var para: [String] = []
    var code: [String]?
    var lang = ""
    func flush() {
      if !para.isEmpty { out.append(.paragraph(para.joined(separator: "\n"))); para = [] }
    }
    for line in text.components(separatedBy: "\n") {
      if line.hasPrefix("```") {
        if let c = code {
          out.append(.code(c.joined(separator: "\n"), lang))
          code = nil
        } else {
          flush()
          lang = String(line.dropFirst(3)).trimmingCharacters(in: .whitespaces)
          code = []
        }
        continue
      }
      if code != nil { code!.append(line); continue }
      let t = line.trimmingCharacters(in: .whitespaces)
      if t.isEmpty { flush(); continue }
      if t.hasPrefix("#") {
        flush()
        let level = t.prefix { $0 == "#" }.count
        out.append(.heading(t.drop { $0 == "#" }.trimmingCharacters(in: .whitespaces), level))
      } else if t.hasPrefix("- ") || t.hasPrefix("* ") {
        flush()
        out.append(.bullet(String(t.dropFirst(2))))
      } else {
        para.append(line)
      }
    }
    if let c = code { out.append(.code(c.joined(separator: "\n"), lang)) }
    flush()
    return out
  }

  var body: some View {
    let all = Self.blocks(text)
    let long = text.count > 6000 && !expanded
    let shown = long ? Array(all.prefix(12)) : all
    VStack(alignment: .leading, spacing: 8) {
      ForEach(Array(shown.enumerated()), id: \.offset) { _, b in block(b) }
      if long {
        Button("続きを表示（\(all.count - shown.count) ブロック）") { expanded = true }.buttonStyle(.link).font(.caption)
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
    .textSelection(.enabled)
  }

  private func inline(_ s: String) -> AttributedString {
    (try? AttributedString(markdown: s, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(s)
  }

  @ViewBuilder static func view(_ b: Block) -> some View {
    MarkdownBlocks(text: "").block(b)
  }

  @ViewBuilder fileprivate func block(_ b: Block) -> some View {
    switch b {
    case .paragraph(let s): Text(inline(s)).lineSpacing(2).fixedSize(horizontal: false, vertical: true)
    case .heading(let s, let level): Text(inline(s)).font(level <= 1 ? .title3.bold() : .headline)
    case .bullet(let s):
      HStack(alignment: .firstTextBaseline, spacing: 6) {
        Text("•").foregroundStyle(.secondary)
        Text(inline(s)).fixedSize(horizontal: false, vertical: true)
      }
    case .code(let code, let lang): CodeBlock(code: code, language: lang)
    }
  }
}

private struct CodeBlock: View {
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
