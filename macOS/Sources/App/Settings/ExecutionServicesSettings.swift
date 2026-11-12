import SwiftUI
import TsugumiLauncher

struct ExecutionServicesSettings: View {
  @Environment(AppModel.self) private var model
  @State private var entries: [ExecutionServiceConnection] = []
  @State private var draft: ExecutionServiceConnection?
  @State private var original: String?
  @State private var busy = false
  @State private var message: String?
  @State private var failed = false

  var body: some View {
    Form {
      Section("実行サービス") {
        ForEach(entries) { entry in
          LabeledContent {
            Text(entry.nativeSystem ?? String(localized: "接続未確認"))
          } label: {
            Button("\(entry.name) (\(entry.id))") { choose(entry, original: entry.id) }
          }
        }
        Button { choose(ExecutionServiceConnection()) } label: { Label("実行サービスを追加", systemImage: "plus") }
      }
      if let entry = draft {
        Section {
          TextField("名前", text: field(\.name))
          TextField("サービス ID", text: field(\.id)).disabled(original != nil).monospaced()
          LabeledContent("接続方法", value: "SSH")
          LabeledContent("実行方式", value: String(localized: "SSH ホストで直接実行"))
          TextField("SSH ホスト", text: field(\.sshHost, connection: true)).monospaced()
            .help("SSH 設定ファイルにあるホストの別名を使います。")
          TextField("ゲートウェイ実行ファイル", text: Binding(get: { draft?.gateway ?? "" }, set: { path in
            draft?.gateway = path.isEmpty ? nil : path
            draft?.nativeSystem = nil
            draft?.systems = nil
            message = nil
          }), prompt: Text(verbatim: "tsugumi-remote-gateway")).monospaced()
            .help("既定名で起動できない場合は、SSH 先の絶対パスを指定します。")
          if let systems = entry.systems, !systems.isEmpty {
            LabeledContent("CPU アーキテクチャ", value: systems.joined(separator: ", "))
          }
          HStack {
            Button("接続をテスト") {
              busy = true
              message = nil
              Task {
                defer { busy = false }
                do {
                  draft = try await ExecutionServiceRegistry.probe(entry)
                  report(String(localized: "接続を確認しました。"))
                } catch { report(error.localizedDescription, failed: true) }
              }
            }.disabled(!valid(entry))
            Button("保存") {
              save(entries.filter { $0.id != original } + [entry])
              if !failed { original = entry.id }
            }.buttonStyle(.borderedProminent).disabled(!valid(entry))
            if let original {
              Button("削除", role: .destructive) {
                save(entries.filter { $0.id != original })
                if !failed { draft = nil; self.original = nil }
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
      do { entries = try ExecutionServiceRegistry.load(support: model.environments.support) }
      catch { report(error.localizedDescription, failed: true) }
    }
  }

  private func choose(_ entry: ExecutionServiceConnection, original: String? = nil) {
    draft = entry
    self.original = original
    message = nil
  }

  private func field(_ key: WritableKeyPath<ExecutionServiceConnection, String>, connection: Bool = false) -> Binding<String> {
    Binding(get: { draft?[keyPath: key] ?? "" }, set: { value in
      draft?[keyPath: key] = value
      if connection { draft?.nativeSystem = nil; draft?.systems = nil }
      message = nil
    })
  }

  private func valid(_ entry: ExecutionServiceConnection) -> Bool {
    [entry.id, entry.name, entry.sshHost].allSatisfy { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
  }

  private func save(_ next: [ExecutionServiceConnection]) {
    do {
      try ExecutionServiceRegistry.save(next, support: model.environments.support)
      entries = next
      for session in model.sessions.values { Task { await session.refreshExecutionServices() } }
      report(String(localized: "保存しました。"))
    } catch { report(error.localizedDescription, failed: true) }
  }

  private func report(_ message: String, failed: Bool = false) {
    self.message = message
    self.failed = failed
  }
}
