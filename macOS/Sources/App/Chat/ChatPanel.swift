import AppKit
import SwiftUI
import TsugumiAgent

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
        Button("会話を圧縮") { chat.compact() }.disabled(chat.busy || chat.items.count < 6)
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
          LazyVStack(alignment: .leading, spacing: 8) {
            if chat.items.isEmpty {
              VStack(alignment: .leading, spacing: 6) {
                Text("Workflow の相談・生成・編集を依頼できます。").font(.callout)
                Text("Card を右クリックして参照に追加、`$` で Skill を選べます。Agent は UI と同じ保存・実行の RPC を使います。")
                  .font(.caption).foregroundStyle(.secondary)
              }
              .padding(.top, 20)
            }
            ForEach(chat.items) { item in row(item).id(item.id) }
          }
          .padding(10)
        }
        .onChange(of: chat.items.count) { if let last = chat.items.last { proxy.scrollTo(last.id, anchor: .bottom) } }
      }
      if let s = chat.status {
        HStack {
          Text(s).font(.caption).foregroundStyle(chat.failed ? .red : .secondary).lineLimit(3)
          Spacer()
          if chat.failed { Button("再試行") { chat.retry() }.controlSize(.small) }
        }
        .padding(.horizontal, 10).padding(.vertical, 4)
      }
      Divider()
      composer
    }
  }

  private var composer: some View {
    VStack(alignment: .leading, spacing: 6) {
      if !chat.references.isEmpty {
        ScrollView(.horizontal, showsIndicators: false) {
          HStack {
            ForEach(chat.references) { r in
              HStack(spacing: 3) {
                Image(systemName: "paperclip").font(.caption2)
                Text(r.label).font(.caption)
                Button { chat.references.removeAll { $0 == r } } label: { Image(systemName: "xmark.circle.fill") }
                  .buttonStyle(.borderless).font(.caption)
              }
              .padding(.horizontal, 6).padding(.vertical, 2)
              .background(Color.accentColor.opacity(0.12), in: Capsule())
            }
          }
        }
      }
      if let q = skillQuery {
        let matches = chat.skills.filter { q.isEmpty || $0.name.localizedCaseInsensitiveContains(q) }
        VStack(alignment: .leading, spacing: 0) {
          if matches.isEmpty {
            Text(chat.skills.isEmpty ? "Skill がありません（設定 > Skills でフォルダを追加、またはプロジェクトの skills/）" : "一致する Skill はありません")
              .font(.caption).foregroundStyle(.secondary).padding(6)
          }
          ForEach(matches.prefix(8)) { s in
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
          }
        }
        .background(.background.secondary, in: RoundedRectangle(cornerRadius: 6))
      }
      TextEditor(text: $input)
        .font(.body)
        .frame(minHeight: 44, maxHeight: 120)
        .scrollContentBackground(.hidden)
        .padding(4)
        .background(.background, in: RoundedRectangle(cornerRadius: 6))
        .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(.secondary.opacity(0.3)))
        .focused($inputFocused)
        .onChange(of: input) { updateSkillQuery() }
        .onKeyPress(.return, phases: .down) { press in
          let cmd = press.modifiers.contains(.command), shift = press.modifiers.contains(.shift)
          let sends = prefs.sendKey == .returnKey ? (!shift && !cmd) : cmd
          if sends {
            send()
            return .handled
          }
          return .ignored
        }
      HStack(spacing: 6) {
        Picker("", selection: $model) {
          ForEach(Array(Set(models + [model])).sorted(), id: \.self) { Text($0).tag($0) }
        }
        .labelsHidden().frame(maxWidth: 180)
        .onChange(of: model) { UserDefaults.standard.set(model, forKey: "agent.model") }
        .disabled(chat.provider.name != ProviderConfig.anthropic.name)
        .help("モデル（接続先: \(chat.provider.name)）")
        if let tab, !tab.selection.isEmpty {
          Button { for k in tab.selection.sorted() { chat.addReference(.card(k)) } } label: {
            Label("選択を参照", systemImage: "paperclip")
          }
          .controlSize(.small)
          .help("選択した Card を参照に追加")
        }
        Spacer()
        Text("\(chat.usage.inputTokens / 1000)k").font(.caption2).foregroundStyle(.secondary)
          .help("直近の入力トークン数（上限に近づくと自動で圧縮）")
        if chat.busy {
          Button("停止", action: chat.cancel)
            .help("回答を停止（Workflow の実行は止まりません）")
        } else {
          Button("送信", action: send)
            .buttonStyle(.borderedProminent)
            .disabled(input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !session.connected || !chat.available)
            .help(prefs.sendKey == .returnKey ? "Return で送信" : "⌘Return で送信")
        }
      }
    }
    .padding(8)
    .task { await loadModels() }
  }

  @ViewBuilder private func row(_ item: ChatController.Item) -> some View {
    switch item.role {
    case "user":
      Text(item.text).textSelection(.enabled)
        .padding(8).frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
        .opacity(item.archived ? 0.6 : 1)
    case "tool":
      DisclosureGroup {
        Text(item.text).font(.caption.monospaced()).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
      } label: {
        HStack(spacing: 4) {
          if item.tool?.ok == nil { ProgressView().controlSize(.mini) } else {
            Image(systemName: item.tool!.ok! ? "checkmark.circle.fill" : "xmark.circle.fill")
              .foregroundStyle(item.tool!.ok! ? .green : .red)
          }
          Text(item.tool?.name ?? "").font(.caption.bold())
          Text(item.tool?.ok == nil ? "実行中" : (item.tool!.ok! ? "完了" : "失敗")).font(.caption).foregroundStyle(.secondary)
        }
      }
      .foregroundStyle(.secondary)
      .opacity(item.archived ? 0.6 : 1)
    case "note":
      Label(item.text, systemImage: "rectangle.compress.vertical").font(.caption).foregroundStyle(.secondary)
        .frame(maxWidth: .infinity)
    default:
      Text((try? AttributedString(markdown: item.text, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace))) ?? AttributedString(item.text))
        .textSelection(.enabled)
        .frame(maxWidth: .infinity, alignment: .leading)
        .opacity(item.archived ? 0.6 : 1)
    }
  }

  private func send() {
    let text = input
    guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, !chat.busy, session.connected else { return }
    chat.send(text)
    input = ""
    skillQuery = nil
  }

  /// `$` at the start of the word being typed opens the Skill picker.
  private func updateSkillQuery() {
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
