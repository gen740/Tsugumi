import AppKit
import SwiftUI
import TsugumiAgent

/// Sign in with ChatGPT: the Agent uses the user's ChatGPT plan. Accounts
/// (registrations) and their tokens stay in the login keychain (the web UI's
/// server keeps its own); nothing of them reaches tsugumid, Cards, project
/// files, settings or logs.
struct ChatGPTSettings: View {
  @AppStorage("agent.model.chatgpt") private var model = ProviderConfig.chatGPT.model
  @AppStorage("agent.provider") private var provider = "anthropic"
  @State private var accounts: [ChatGPTAccountInfo] = []
  @State private var signingIn: Task<Void, Never>?
  @State private var message: String?
  @State private var welcome: String?
  @State private var models: [(id: String, label: String)] = []

  private var auth: ChatGPTAuth { ChatGPTAuth.shared() }
  private var active: ChatGPTAccountInfo? { accounts.first(where: \.active) }

  var body: some View {
    Section {
      Text("ChatGPT の Plus・Pro プランで Agent を使います。リクエストは OpenAI の Responses API に送られ、プランの利用枠から使われます（API キーの課金とは別で、Tsugumi に料金はかかりません）。Plus の 5 時間ごとの利用上限は、ほかのアプリ（オープンソースのツールを含む）と共有です。Pro にはこの上限はありません。")
        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
      if let a = active {
        LabeledContent("使用中のアカウント") {
          Text(a.email ?? a.clientId).textSelection(.enabled)
        }
        if a.signedIn && !a.planEnabled {
          Text("このアカウントでは ChatGPT プランの利用が許可されていないため、Agent は使えません。許可するか、API キーの接続を選んでください。")
            .font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
        }
      }
      ForEach(accounts) { a in row(a) }
      HStack {
        if signingIn != nil {
          ProgressView().controlSize(.small)
          Text("ブラウザでサインインを続けてください…").font(.caption).foregroundStyle(.secondary)
          Button("中止") { signingIn?.cancel() }
        } else {
          Button(accounts.isEmpty ? "Continue with ChatGPT" : "Continue with ChatGPT（別のアカウント）") { signIn(clientId: nil) }
            .buttonStyle(.borderedProminent)
        }
        Spacer()
        Link("Manage usage", destination: ChatGPTAuth.usageURL)
      }
    } header: {
      HStack {
        Text("ChatGPT プラン（Sign in with ChatGPT）")
        if provider == "chatgpt" { Text("使用中").font(.caption).foregroundStyle(.green) }
      }
    } footer: {
      Text("サインインはブラウザで行い、Tsugumi は 127.0.0.1 で結果を受け取ります。トークンは macOS Keychain（このデバイスのみ）に置かれ、core や Card には渡されず、プロジェクト・設定・ログにも残りません。")
        .font(.caption).foregroundStyle(.secondary)
    }
    .onAppear { Task { await refresh() } }
    .alert(Text("ChatGPT プランを使っています"), isPresented: Binding(get: { welcome != nil }, set: { if !$0 { welcome = nil } })) {
      Button("Got it") {
        if let id = welcome { Task { try? await auth.markWelcomed(clientId: id) } }
        welcome = nil
      }
      Button("Manage usage") {
        NSWorkspace.shared.open(ChatGPTAuth.usageURL)
        if let id = welcome { Task { try? await auth.markWelcomed(clientId: id) } }
        welcome = nil
      }
    } message: {
      Text("Agent のリクエストは ChatGPT プランの利用枠から使われます。利用状況は ChatGPT の設定（Usage）で確認・管理できます。")
    }
    Section("モデル") {
      TextField("モデル", text: $model)
      HStack {
        Button("モデル一覧を取得") {
          Task {
            do {
              models = try await MessagesProvider(config: AgentSettings.config(for: "chatgpt")).modelChoices()
              UserDefaults.standard.set(models.map(\.id), forKey: AgentSettings.modelsKey("chatgpt"))
              UserDefaults.standard.set(Dictionary(models.map { ($0.id, $0.label) }, uniquingKeysWith: { a, _ in a }),
                                        forKey: "agent.modelNames")
              message = String(localized: "\(models.count) 件のモデルを取得しました。")
            } catch { message = "\(error)" }
          }
        }
        .disabled(!(active?.signedIn ?? false))
      }
      if !models.isEmpty {
        Picker("一覧から選択", selection: $model) {
          ForEach(models, id: \.id) { Text($0.label).tag($0.id) }
        }
      }
    }
    if let message { Text(message).font(.caption).textSelection(.enabled) }
  }

  @ViewBuilder private func row(_ a: ChatGPTAccountInfo) -> some View {
    HStack {
      Image(systemName: a.active ? "checkmark.circle.fill" : "circle").foregroundStyle(a.active ? Color.accentColor : .secondary)
      VStack(alignment: .leading, spacing: 2) {
        Text(a.email ?? a.clientId)
        Text(!a.signedIn ? String(localized: "サインアウト済み") : a.planEnabled ? String(localized: "サインイン中") : String(localized: "サインイン中（プランの利用は未許可）"))
          .font(.caption).foregroundStyle(.secondary)
      }
      Spacer()
      if a.signedIn && !a.active {
        Button("このアカウントを使う") { Task { try? await auth.select(clientId: a.clientId); await refresh() } }
      }
      if !a.signedIn {
        Button("Continue with ChatGPT") { signIn(clientId: a.clientId) }.disabled(signingIn != nil)
      } else if !a.planEnabled {
        Button("プランの利用を許可") { signIn(clientId: a.clientId, consent: true) }.disabled(signingIn != nil)
      }
      if a.signedIn {
        Button("サインアウト", role: .destructive) {
          Task {
            do {
              let revoked = try await auth.signOut(clientId: a.clientId)
              message = revoked
                ? String(localized: "\(a.email ?? a.clientId) からサインアウトしました。")
                : String(localized: "このデバイスのトークンは消しましたが、OpenAI 側での取り消しを確かめられませんでした。ChatGPT の設定で Tsugumi の接続を解除できます。")
            } catch { message = "\(error)" }
            await refresh()
          }
        }
      }
    }
  }

  private func signIn(clientId: String?, consent: Bool = false) {
    message = nil
    signingIn = Task {
      var signedIn = false
      do {
        let r = try await auth.signIn(clientId: clientId, consent: consent) { url in
          await MainActor.run { _ = NSWorkspace.shared.open(url) }
        }
        message = r.account.planEnabled
          ? String(localized: "\(r.account.email ?? r.account.clientId) でサインインしました。")
          : String(localized: "サインインしましたが、ChatGPT プランの利用は許可されませんでした。「プランの利用を許可」から許可するか、API キーの接続を使ってください。")
        if r.welcome { welcome = r.account.clientId }
        signedIn = true
      } catch is CancellationError {
        message = String(localized: "サインインを中止しました。")
      } catch {
        message = "\(error)"
      }
      signingIn = nil
      await refresh(signedIn: signedIn)
    }
  }

  private func refresh(signedIn: Bool = false) async {
    accounts = await auth.accounts()
    // The guide is there while the active account may use the plan.
    if signedIn { GuideController.shared.connectionSaved() } else { GuideController.shared.refreshAvailability() }
  }
}
