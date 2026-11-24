import SwiftUI
import AppKit
import TsugumiAgentClient

struct ExecutionServicesSettings: View {
  @Environment(AppModel.self) private var model
  @State private var hostPublicKey = ""
  @State private var entries: [ExecutionServiceConnection] = []
  @State private var draft: ExecutionServiceRegistration?
  @State private var original: String?
  @State private var busy = false
  @State private var message: String?
  @State private var failed = false

  var body: some View {
    Form {
      Section {
        Text(hostPublicKey).monospaced().textSelection(.enabled)
        Button("公開鍵をコピー") {
          NSPasteboard.general.clearContents()
          NSPasteboard.general.setString(hostPublicKey, forType: .string)
        }.disabled(hostPublicKey.isEmpty)
      } header: {
        InfoHeader("このホストの公開鍵", info: "ホストの公開鍵を実行デーモンの authorized_keys に追加してから、接続を確認してください。")
      }
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
          LabeledContent("ワーカーの対応ビット") {
            WorkerBitsEditor(bits: Binding(get: { draft?.workerCapabilities ?? [] }, set: { value in
              draft?.workerCapabilities = value
              message = nil
            }))
          }
          .help("このワーカーが持つビットを 0〜63 から選びます。")
          TextField("サービス ID", text: field(\.id)).disabled(original != nil).monospaced()
          TextField("実行デーモンの接続先", text: field(\.endpoint), prompt: Text(verbatim: "tls://worker.example:8765"))
            .disabled(entry.id == "local").monospaced().help("TLS 接続先を入力してください（例: tls://worker.example:8765）。")
          TextField("実行デーモンの公開鍵", text: field(\.workerPublicKey))
            .disabled(entry.id == "local").monospaced().help("公開鍵は ed25519: で始まる SPKI 形式です。")
          if let caps = saved?.capabilities, !caps.serverId.isEmpty {
            LabeledContent("CPU アーキテクチャ", value: caps.systems.joined(separator: ", "))
            LabeledContent("隔離方式", value: String(localized: "PID 名前空間"))
            LabeledContent("CPU とメモリの上限", value: caps.resourceLimits ? String(localized: "対応") : String(localized: "非対応"))
            LabeledContent("最大同時ジョブ数", value: String(caps.maxConcurrentJobs))
          }
          HStack {
            Button(entry.id == "local" ? "保存" : original == nil ? "登録" : "接続を確認して更新") {
              busy = true
              message = nil
              Task {
                defer { busy = false }
                do {
                  var input = entry
                  let verifying = original != nil && entry.id != "local"
                  input.verify = verifying
                  adopt(try await ExecutionServices.put(input))
                  choose(entries.first { $0.id == entry.id })
                  refreshServices()
                  report(entry.id == "local" ? String(localized: "保存しました。") : verifying ? String(localized: "接続を確認しました。") : String(localized: "公開鍵を登録しました。authorized_keys にホストの公開鍵を追加してから接続を確認してください。"))
                } catch { report(error.localizedDescription, failed: true) }
              }
            }.buttonStyle(.borderedProminent).disabled(!valid(entry))
            if let original, original != "local" {
              Button("削除", role: .destructive) {
                Task {
                  busy = true
                  defer { busy = false }
                  do {
                    adopt(try await ExecutionServices.remove(original))
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
      do { adopt(try await ExecutionServices.list()) }
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
      next.workerPublicKey = entry.workerPublicKey
      next.workerCapabilities = entry.workerCapabilities
    }
    draft = next
    original = entry?.id
    message = nil
  }

  private func field(_ key: WritableKeyPath<ExecutionServiceRegistration, String>) -> Binding<String> {
    Binding(get: { draft?[keyPath: key] ?? "" }, set: { value in
      draft?[keyPath: key] = value
      message = nil
    })
  }

  private func valid(_ entry: ExecutionServiceRegistration) -> Bool {
    guard [entry.id, entry.name].allSatisfy({ !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }) else { return false }
    guard WorkerBits.valid(entry.workerCapabilities) else { return false }
    return (entry.id == "local" || (!entry.endpoint.isEmpty && !entry.workerPublicKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty))
  }

  private func adopt(_ result: ExecutionServicesState) {
    entries = result.services
    hostPublicKey = result.hostPublicKey
  }

  private func refreshServices() {
    for session in model.sessions.values { Task {
      _ = try? await model.environments.start(session.identity)
      await session.refreshExecutionServices()
    } }
  }

  private func report(_ message: String, failed: Bool = false) {
    self.message = message
    self.failed = failed
  }
}
