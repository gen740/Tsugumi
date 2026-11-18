import AppKit
import SwiftUI
import TsugumiAgentClient

/// Sign in with ChatGPT: the Agent uses the user's ChatGPT plan. Accounts
/// (registrations) and their tokens stay in hostd, with a separate Web host
/// account profile. Tokens never reach tsugumid, Cards, project files, settings
/// or logs.
struct ChatGPTSettings: View {
  @HostStorage("agent.model.chatgpt") private var model = ProviderConfig.chatGPT.model
  @HostStorage("agent.provider") private var provider = "anthropic"
  @State private var connection = ChatGPTSignInModel()
  @State private var message: String?
  @State private var models: [(id: String, name: String)] = []

  var body: some View {
    Section {
      ChatGPTConnection(model: connection) { GuideController.shared.connectionSaved() }
    } header: {
      HStack {
        Text("ChatGPT プラン（Sign in with ChatGPT）")
        InfoButton(Text("ChatGPT の Plus・Pro プランで Agent を使います。リクエストは OpenAI の Responses API に送られ、プランの利用枠から使われます（API キーの課金とは別で、Tsugumi に料金はかかりません）。Plus の 5 時間ごとの利用上限は、ほかのアプリ（オープンソースのツールを含む）と共有です。Pro にはこの上限はありません。"), Text("サインインはブラウザで行い、Tsugumi は 127.0.0.1 で結果を受け取ります。認証とトークンの保存は hostd が行います。トークンは Card やプロジェクト・設定・ログには残りません。"))
        if provider == "chatgpt" { Text("使用中").font(.caption).foregroundStyle(.green) }
      }
    }
    .onAppear { Task { models = (try? await CoreHostClient.shared.storedModels("chatgpt")) ?? [] } }
    Section("モデル") {
      TextField("モデル", text: $model)
      HStack {
        Button("モデル一覧を取得") {
          Task {
            do {
              models = try await MessagesProvider(config: AgentSettings.config(for: "chatgpt")).modelChoices()
              message = String(localized: "\(models.count) 件のモデルを取得しました。")
            } catch { message = "\(error)" }
          }
        }
        .disabled(!(connection.active?.signedIn ?? false))
      }
      if !models.isEmpty {
        Picker("一覧から選択", selection: $model) {
          ForEach(models, id: \.id) { Text($0.name).tag($0.id) }
        }
      }
    }
    if let message { Text(message).font(.caption).textSelection(.enabled) }
  }
}

/// Shared presentation state for Settings and first start. OAuth and the
/// once-only welcome record remain in the host service.
@MainActor @Observable
final class ChatGPTSignInModel {
  // Includes dismissed sheets until the host acknowledges their cancellation.
  private static var pending: [UUID: Task<Void, Never>] = [:]
  static func cancelPendingSignIns() async {
    let tasks = Array(pending.values)
    for task in tasks { task.cancel() }
    for task in tasks { await task.value }
  }

  var accounts: [ChatGPTAccountInfo] = []
  var signingIn: Task<Void, Never>?
  var message: String?
  var welcome: String?
  var active: ChatGPTAccountInfo? { accounts.first(where: \.active) }
  private var connected: (() async throws -> Void)?
  private let authenticate: (String?, Bool) async throws -> ChatGPTAuth.SignInResult
  private let load: () async -> ChatGPTAuth.Status
  private let acknowledge: (String) async throws -> Void

  init(
    authenticate: @escaping (String?, Bool) async throws -> ChatGPTAuth.SignInResult = { client, consent in
      try await ChatGPTAuth.shared().signIn(clientId: client, consent: consent) { url in
        await MainActor.run { _ = NSWorkspace.shared.open(url) }
      }
    },
    load: @escaping () async -> ChatGPTAuth.Status = { await ChatGPTAuth.shared().status() },
    acknowledge: @escaping (String) async throws -> Void = { try await ChatGPTAuth.shared().markWelcomed(clientId: $0) }
  ) {
    self.authenticate = authenticate
    self.load = load
    self.acknowledge = acknowledge
  }

  func refresh(onConnected: (() async throws -> Void)? = nil) async {
    if let onConnected { connected = onConnected }
    let status = await load()
    accounts = status.accounts
    if let message = status.message { self.message = message }
    if let result = status.result, result.account.signedIn, result.account.planEnabled, result.welcome {
      welcome = result.account.clientId
    }
  }

  func signIn(clientId: String? = nil, consent: Bool = false, onConnected: @escaping () async throws -> Void) {
    guard signingIn == nil else { return }
    message = nil
    connected = onConnected
    let id = UUID()
    let task = Task {
      defer { Self.pending[id] = nil; signingIn = nil }
      do {
        let r = try await authenticate(clientId, consent)
        try Task.checkCancellation()
        message = r.account.planEnabled
          ? String(localized: "\(r.account.email ?? r.account.clientId) でサインインしました。")
          : String(localized: "サインインしましたが、ChatGPT プランの利用は許可されませんでした。「プランの利用を許可」から許可するか、API キーの接続を使ってください。")
        await refresh()
        try Task.checkCancellation()
        if r.account.planEnabled {
          if r.welcome {
            connected = onConnected
            welcome = r.account.clientId
          } else { try await onConnected() }
        }
      } catch is CancellationError {
        message = String(localized: "サインインを中止しました。")
      } catch { message = "\(error)" }
    }
    signingIn = task
    Self.pending[id] = task
  }

  func welcomed() async {
    guard let id = welcome else { return }
    do {
      try await acknowledge(id)
      welcome = nil
      let done = connected
      connected = nil
      try await done?()
    } catch { message = "\(error)" }
  }
}

/// The same sign-in controls and welcome in both entry points.
struct ChatGPTConnection: View {
  @Bindable var model: ChatGPTSignInModel
  var onboarding = false
  var onConnected: () async throws -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      ForEach(model.accounts.filter { !onboarding || $0.active }) { a in
        HStack {
          VStack(alignment: .leading, spacing: 2) {
            Text(a.email ?? a.clientId).textSelection(.enabled)
            Text(!a.signedIn ? String(localized: "ChatGPT にもう一度サインインしてください。") : a.planEnabled ? String(localized: "サインイン中") : String(localized: "サインイン中（プランの利用は未許可）"))
              .font(.caption).foregroundStyle(.secondary)
          }
          Spacer()
          if !onboarding && a.signedIn && !a.active {
            Button("このアカウントを使う") {
              Task { try? await ChatGPTAuth.shared().select(clientId: a.clientId); await model.refresh(); GuideController.shared.refreshAvailability() }
            }
          }
          if !a.signedIn {
            continueButton { signIn(clientId: a.clientId) }
          } else if !a.planEnabled {
            Button("プランの利用を許可") { signIn(clientId: a.clientId, consent: true) }.disabled(model.signingIn != nil)
          }
          if !onboarding && a.signedIn {
            Button("サインアウト", role: .destructive) {
              Task {
                do {
                  let revoked = try await ChatGPTAuth.shared().signOut(clientId: a.clientId)
                  model.message = revoked
                    ? String(localized: "\(a.email ?? a.clientId) からサインアウトしました。")
                    : String(localized: "このデバイスのトークンは消しましたが、OpenAI 側での取り消しを確かめられませんでした。ChatGPT の設定で Tsugumi の接続を解除できます。")
                } catch { model.message = "\(error)" }
                await model.refresh()
                GuideController.shared.refreshAvailability()
              }
            }
          }
        }
      }
      HStack {
        if model.signingIn != nil {
          ProgressView().controlSize(.small)
          Text("ブラウザでサインインを続けてください…").font(.caption).foregroundStyle(.secondary)
          Button("中止") { model.signingIn?.cancel() }
        } else {
          continueButton { signIn() }
        }
        Spacer()
        Link("Manage usage", destination: ChatGPTAuth.usageURL)
      }
      if let message = model.message {
        Text(message).font(.caption).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
      }
    }
    .task { await model.refresh(onConnected: onConnected) }
    .onDisappear { model.signingIn?.cancel() }
    .alert(Text("ChatGPT プランを使っています"), isPresented: Binding(get: { model.welcome != nil }, set: { _ in })) {
      Button("Got it") { Task { await model.welcomed() } }
      Button("Manage usage") {
        NSWorkspace.shared.open(ChatGPTAuth.usageURL)
        Task { await model.welcomed() }
      }
    } message: {
      Text("Agent のリクエストは ChatGPT プランの利用枠から使われます。利用状況は ChatGPT の設定（Usage）で確認・管理できます。")
    }
  }

  private func signIn(clientId: String? = nil, consent: Bool = false) {
    model.signIn(clientId: clientId, consent: consent, onConnected: onConnected)
  }

  private func continueButton(action: @escaping () -> Void) -> some View {
    Button("Continue with ChatGPT", action: action)
      .buttonStyle(.borderedProminent).tint(.black).foregroundStyle(.white)
      .disabled(model.signingIn != nil)
  }
}
