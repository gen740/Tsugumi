import AppKit
import SwiftUI
import TsugumiAgentClient
import TsugumiRPC

struct HostPathsSettings: View {
  let session: ProjectSession
  @State private var scope = ""
  @State private var name = ""
  @State private var purpose = ""
  @State private var bindings: Tsugumi_Host_HostPathBindingsT?
  @State private var busy = false
  @State private var problem: String?

  private var declarations: [JSONValue] {
    let project = scope.isEmpty ? session.project : session.external(scope)?.project
    return project?["hostPaths"]?.array ?? []
  }

  var body: some View {
    Form {
      Section {
        if !session.externals.isEmpty {
          Picker("プロジェクト", selection: $scope) {
            Text("最上位のプロジェクト").tag("")
            ForEach(session.externals.keys.sorted(), id: \.self) { Text($0).tag($0) }
          }
          .onChange(of: scope) { _, _ in name = ""; purpose = "" }
        }
        ForEach(declarations.compactMap { $0["name"]?.string }, id: \.self) { declared in
          let description = declarations.first { $0["name"]?.string == declared }?["description"]?.string ?? ""
          let folder = bindings?.bindings.compactMap { $0 }.first { ($0.scope ?? "") == scope && $0.name == declared }?.path
          LabeledContent {
            HStack {
              if let folder {
                Text(folder).lineLimit(1).truncationMode(.middle).help(folder)
              } else {
                Text("未割り当て").foregroundStyle(.secondary)
              }
              Button("編集") { name = declared; purpose = description }
              Button("フォルダを選ぶ…") {
                let selectedScope = scope
                perform {
                  guard let project = session.projectId,
                        let url = await Self.chooseFolder(declared, description: description, initial: folder) else { return }
                  bindings = try await Self.setBinding(project: project, scope: selectedScope, name: declared, folder: url)
                  session.reconnect()
                }
              }
              if folder != nil {
                Button("割り当てを解除") {
                  let selectedScope = scope
                  perform {
                    guard let project = session.projectId else { return }
                    bindings = try await Self.setBinding(project: project, scope: selectedScope, name: declared, folder: nil)
                    session.reconnect()
                  }
                }
              }
              Button("削除", role: .destructive) { perform { try await update(remove: declared) } }
            }
            .disabled(!session.isEditable || busy)
          } label: {
            VStack(alignment: .leading) {
              Text(declared)
              Text(description).foregroundStyle(.secondary)
            }
          }
        }
      } header: {
        Text("ホストパス")
      } footer: {
        Text("名前と用途をプロジェクトに保存します。フォルダの割り当てはこのホストに保存します。")
      }
      Section {
        TextField("名前", text: $name, prompt: Text(verbatim: "saves"))
        TextField("用途", text: $purpose)
        Button("保存") { perform { try await update() } }
          .disabled(!session.isEditable || !validName(name) || purpose.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || busy)
      }
      if let problem { Text(problem).foregroundStyle(.red).textSelection(.enabled) }
    }
    .formStyle(.grouped)
    .task(id: session.connected ? session.projectId : nil) {
      guard session.connected, let project = session.projectId else { return }
      let request = Tsugumi_Host_HostPathListRequestT()
      request.projectId = project
      do { bindings = try await CoreHostClient.shared.hostCall(.hostpathlistrequest, request) }
      catch { problem = error.localizedDescription }
    }
  }

  private func perform(_ action: @escaping @MainActor () async throws -> Void) {
    busy = true
    problem = nil
    Task {
      defer { busy = false }
      do { try await action() }
      catch { problem = error.localizedDescription }
    }
  }

  private func update(remove: String? = nil) async throws {
    let selectedScope = scope
    let target = remove ?? name
    let replacement = JSONValue.object([("name", .string(target)), ("description", .string(purpose))])
    _ = try await session.applyMutation { model in
      let project = selectedScope.isEmpty ? model.project : model.external(selectedScope)?.project
      var rows = project?["hostPaths"]?.array ?? []
      if let remove {
        rows.removeAll { $0["name"]?.string == remove }
      } else if let index = rows.firstIndex(where: { $0["name"]?.string == target }) {
        rows[index] = replacement
      } else {
        rows.append(replacement)
      }
      model.setProjectField("hostPaths", rows.isEmpty ? nil : .array(rows), scope: selectedScope)
    }
    name = ""
    purpose = ""
  }

  @MainActor
  static func chooseFolder(_ name: String, description: String?, initial: String? = nil) async -> URL? {
    let panel = NSOpenPanel()
    panel.canChooseDirectories = true
    panel.canChooseFiles = false
    panel.allowsMultipleSelection = false
    panel.message = String(localized: "ホストパス「\(name)」のフォルダを選んでください。")
    if let description, !description.isEmpty { panel.message += " " + description }
    if let initial { panel.directoryURL = URL(fileURLWithPath: initial, isDirectory: true) }
    return await panel.begin() == .OK ? panel.url : nil
  }

  @MainActor
  static func setBinding(project: String, scope: String, name: String, folder: URL?) async throws -> Tsugumi_Host_HostPathBindingsT {
    let request = Tsugumi_Host_HostPathSetRequestT()
    request.projectId = project
    request.scope = scope.isEmpty ? nil : scope
    request.name = name
    request.path = folder?.path
    request.remove = folder == nil
    return try await CoreHostClient.shared.hostCall(.hostpathsetrequest, request)
  }
}
