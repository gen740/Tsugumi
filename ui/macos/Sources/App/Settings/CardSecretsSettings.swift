import SwiftUI
import TsugumiAgentClient

/// A project's Card secrets: hostd hands each one, as an environment variable of
/// the same name, to the processes of this project's Cards that declare it.
/// Values are bound to the project id and write-only.
struct CardSecretsSettings: View {
  let session: ProjectSession
  @State private var names: [String] = []
  @State private var name = ""
  @State private var value = ""
  @State private var busy = false
  @State private var message: String?
  @State private var failed = false

  var body: some View {
    if let project = session.projectId {
      form(project).task(id: project) {
        do { names = try await CardSecrets.names(project: project) }
        catch { report(error.localizedDescription, failed: true) }
      }
    } else {
      Text("プロジェクトの実行環境に接続すると設定できます").foregroundStyle(.secondary)
    }
  }

  private func form(_ project: String) -> some View {
    Form {
      Section {
        if names.isEmpty {
          Text("設定されたシークレットはありません").foregroundStyle(.secondary)
        }
        ForEach(names, id: \.self) { secret in
          LabeledContent {
            HStack {
              Text("設定済み").foregroundStyle(.secondary)
              Button("置き換え") {
                name = secret
                value = ""
                message = nil
              }
              Button("削除", role: .destructive) {
                perform(String(localized: "\(secret) を削除しました。")) { try await CardSecrets.remove(project: project, secret) }
              }
            }
          } label: {
            Text(verbatim: secret).monospaced()
          }
        }
      } header: {
        InfoHeader("Card シークレット", info: "Card の定義（_card.json の \"secrets\"）が名前を挙げたシークレットを、その Card のプロセスに同じ名前の環境変数として渡します。値はこのホストの hostd がこのプロジェクト用にだけ保存し、表示されることはありません。")
      }
      Section {
        TextField("名前", text: $name, prompt: Text(verbatim: "database_password"))
          .monospaced()
          .help("小文字、数字、_ で 64 文字まで（先頭は小文字）。")
        SecureField("値", text: $value)
        HStack {
          Button(names.contains(name) ? "置き換え" : "保存") {
            let saved = name
            perform(String(localized: "\(saved) を保存しました。")) {
              let result = try await CardSecrets.set(project: project, saved, value: value)
              value = ""
              return result
            }
          }
          .buttonStyle(.borderedProminent)
          .disabled(!validName(name) || value.isEmpty)
          if busy { ProgressView().controlSize(.small) }
        }
      } header: {
        InfoHeader("シークレットを設定", info: "Card がその値をどう扱うか（子プロセスへの継承、ログ、出力）は Card の作者の責任です。")
      }
      if let message {
        Text(message).foregroundStyle(failed ? .red : .secondary).textSelection(.enabled)
      }
    }
    .formStyle(.grouped)
    .disabled(busy)
  }

  private func perform(_ done: String, _ action: @escaping @MainActor () async throws -> [String]) {
    busy = true
    message = nil
    Task {
      defer { busy = false }
      do {
        names = try await action()
        report(done)
      } catch { report(error.localizedDescription, failed: true) }
    }
  }

  private func report(_ message: String, failed: Bool = false) {
    self.message = message
    self.failed = failed
  }
}
