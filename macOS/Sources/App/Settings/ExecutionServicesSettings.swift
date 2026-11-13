import SwiftUI
import TsugumiAgentClient

struct ExecutionServicesSettings: View {
  @Environment(AppModel.self) private var model
  @State private var entries: [ExecutionServiceConnection] = []
  @State private var draft: ExecutionServiceRegistration?
  @State private var original: String?
  @State private var busy = false
  @State private var message: String?
  @State private var failed = false

  var body: some View {
    Form {
      Section("実行サービス") {
        ForEach(entries) { entry in
          LabeledContent {
            Text(entry.capabilities.nativeSystem)
          } label: {
            Button("\(entry.name) (\(entry.id))") { choose(entry) }
          }
        }
        Button { choose(nil) } label: { Label("実行サービスを追加", systemImage: "plus") }
      }
      if let entry = draft {
        Section {
          TextField("名前", text: field(\.name))
          TextField("サービス ID", text: field(\.id)).disabled(original != nil).monospaced()
          TextField("実行デーモンの接続先", text: field(\.endpoint), prompt: Text(verbatim: "tcp://127.0.0.1:8765"))
            .monospaced().help("Unix ソケット、またはループバックアドレスの TCP 接続先を入力してください。")
          SecureField("認証トークン", text: Binding(get: { draft?.token ?? "" }, set: { value in
            draft?.token = value.isEmpty ? nil : value
            message = nil
          }), prompt: saved == nil ? nil : Text("保存済みトークンを使用"))
            .help("認証トークンは小文字の16進数64文字です。")
          if let caps = saved?.capabilities {
            LabeledContent("CPU アーキテクチャ", value: caps.systems.joined(separator: ", "))
            LabeledContent("隔離方式", value: String(localized: "PID 名前空間"))
            LabeledContent("CPU とメモリの上限", value: caps.resourceLimits ? String(localized: "対応") : String(localized: "非対応"))
            LabeledContent("最大同時ジョブ数", value: String(caps.maxConcurrentJobs))
          }
          HStack {
            Button(original == nil ? "接続を確認して登録" : "接続を確認して更新") {
              busy = true
              message = nil
              Task {
                defer { busy = false }
                do {
                  entries = try await ExecutionServices.put(entry)
                  choose(entries.first { $0.id == entry.id })
                  refreshServices()
                  report(String(localized: "接続を確認しました。"))
                } catch { report(error.localizedDescription, failed: true) }
              }
            }.buttonStyle(.borderedProminent).disabled(!valid(entry))
            if let original {
              Button("削除", role: .destructive) {
                Task {
                  busy = true
                  defer { busy = false }
                  do {
                    entries = try await ExecutionServices.remove(original)
                    draft = nil
                    self.original = nil
                    refreshServices()
                  } catch { report(error.localizedDescription, failed: true) }
                }
              }
            }
            if busy { ProgressView().controlSize(.small) }
          }
        }
      }
      if let message {
        Text(message).foregroundStyle(failed ? .red : .secondary).textSelection(.enabled)
      }
    }
    .formStyle(.grouped)
    .disabled(busy)
    .task {
      do { entries = try await ExecutionServices.list() }
      catch { report(error.localizedDescription, failed: true) }
    }
  }

  private var saved: ExecutionServiceConnection? {
    entries.first { $0.id == original && $0.endpoint == draft?.endpoint.trimmingCharacters(in: .whitespacesAndNewlines) }
  }

  private func choose(_ entry: ExecutionServiceConnection?) {
    var next = ExecutionServiceRegistration()
    if let entry {
      next.id = entry.id
      next.name = entry.name
      next.endpoint = entry.endpoint
    }
    draft = next
    original = entry?.id
    message = nil
  }

  private func field(_ key: WritableKeyPath<ExecutionServiceRegistration, String>) -> Binding<String> {
    Binding(get: { draft?[keyPath: key] ?? "" }, set: { value in
      draft?[keyPath: key] = value
      if key == \.endpoint { draft?.token = nil }
      message = nil
    })
  }

  private func valid(_ entry: ExecutionServiceRegistration) -> Bool {
    guard [entry.id, entry.name, entry.endpoint].allSatisfy({ !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }) else { return false }
    if let token = entry.token { return token.range(of: #"\A[a-f0-9]{64}\z"#, options: .regularExpression) != nil }
    return saved != nil
  }

  private func refreshServices() {
    for session in model.sessions.values { Task { await session.refreshExecutionServices() } }
  }

  private func report(_ message: String, failed: Bool = false) {
    self.message = message
    self.failed = failed
  }
}
