import SwiftUI
import TsugumiRPC

/// The environment recorded for a run, read from history only: the current Card definition is never evaluated.
struct RecordedEnvironmentView: View {
  let session: ProjectSession
  let runId: String
  let environmentId: String
  @State private var environment: Tsugumi_RPC_BuildEnvironmentT?
  @State private var loaded = false
  @State private var error: String?
  @State private var attempt = 0
  @State private var query = ""
  @State private var offsets: [UInt64] = [0]
  @State private var paths: [String] = []
  @State private var next: UInt64?
  @State private var busy = false
  @State private var total: UInt64 = 0

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 10) {
        Text("依存 package").font(.headline)
        if let error {
          Text(error).foregroundStyle(.red)
          Button("再試行") { attempt += 1 }
        }
        if !loaded { ProgressView() }
        else if let environment {
          Text(environment.system)
          Text("実行時の依存パス").font(.headline)
          Text("この呼び出しで記録されたものです。ビルド時だけに使う依存関係は含まれません。")
          TextField("依存パスを検索", text: $query).textFieldStyle(.roundedBorder)
            .onChange(of: query) { offsets = [0] }
          Text("検索結果を 100 件ずつ表示します。").font(.caption)
          Text(String(total)).font(.caption).foregroundStyle(.secondary)
          if busy { ProgressView() }
          else if paths.isEmpty { Text("一致する依存パスはありません") }
          else { ForEach(paths, id: \.self) { storePath($0) } }
          HStack {
            Button("前へ") { offsets.removeLast() }.disabled(busy || offsets.count == 1)
            Text(String(offsets.count))
            Button("次へ") { if let next { offsets.append(next) } }.disabled(busy || next == nil)
          }
        } else if error == nil {
          Text("この呼び出しの環境は記録されていません")
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading).padding(12)
    }
    .task(id: attempt) { await loadEnvironment() }
    .task(id: PathQuery(id: environment?.id, query: query, offset: offsets.last ?? 0, attempt: attempt)) {
      await loadPaths()
    }
  }

  private struct PathQuery: Equatable {
    let id: String?
    let query: String
    let offset: UInt64
    let attempt: Int
  }

  private func storePath(_ path: String) -> some View {
    DisclosureGroup(recordedStoreName(path)) {
      Text(path).font(.caption.monospaced()).textSelection(.enabled)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
  }

  private func loadEnvironment() async {
    loaded = false
    environment = nil
    error = nil
    defer { if !Task.isCancelled { loaded = true } }
    guard !environmentId.isEmpty else { return }
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
    do {
      var offset: UInt64? = 0
      while let current = offset {
        try Task.checkCancellation()
        let request = Tsugumi_RPC_RunEnvironmentsRequestT()
        request.runId = runId
        request.offset = current
        request.limit = 100
        let page: Tsugumi_RPC_BuildEnvironmentPageT = try await rpc.call(.runenvironmentsrequest, request)
        try Task.checkCancellation()
        if let found = page.environments.compactMap({ $0 }).first(where: { $0.id == environmentId }) {
          environment = found
          return
        }
        if let next = page.nextOffset, next <= current { throw EnvironmentPageError.nonAdvancing }
        offset = page.nextOffset
      }
    } catch { if !Task.isCancelled { self.error = String(describing: error) } }
  }

  private func loadPaths() async {
    paths = []; next = nil; total = 0
    guard let environment, let rpc = session.rpc else { return }
    busy = true
    defer { if !Task.isCancelled { busy = false } }
    do {
      let request = Tsugumi_RPC_RunEnvironmentPathsRequestT()
      request.runId = runId
      request.environmentId = environment.id
      request.offset = offsets.last ?? 0
      request.limit = 100
      request.query = query
      let page: Tsugumi_RPC_EnvironmentPathPageT = try await rpc.call(.runenvironmentpathsrequest, request)
      try Task.checkCancellation()
      if let next = page.nextOffset, next <= (request.offset ?? 0) { throw EnvironmentPageError.nonAdvancing }
      paths = page.paths.compactMap { $0 }
      next = page.nextOffset
      total = page.total
      error = nil
    } catch { if !Task.isCancelled { self.error = String(describing: error) } }
  }
}

enum EnvironmentPageError: Error { case nonAdvancing }

func recordedStoreName(_ path: String) -> String {
  let name = String(path.split(separator: "/").last ?? "")
  guard name.count > 33 else { return name }
  let prefix = name.prefix(32)
  guard prefix.allSatisfy({ $0.isASCII && ($0.isLowercase || $0.isNumber) }),
        name[name.index(name.startIndex, offsetBy: 32)] == "-" else { return name }
  return String(name.dropFirst(33))
}

/// The packages of one invocation in the run history. An invocation without a
/// recorded environment says so: nothing is derived from the current definition.
struct InvocationPackagesView: View {
  let session: ProjectSession
  let runId: String
  let environmentId: String

  var body: some View {
    if environmentId.isEmpty {
      ContentUnavailableView("この呼び出しの環境は記録されていません", systemImage: "shippingbox",
                             description: Text("依存 package は実行時に記録されたものだけを表示します。現在の Nix 定義からは求めません。"))
    } else {
      RecordedEnvironmentView(session: session, runId: runId, environmentId: environmentId)
        .id(runId + "\u{0}" + environmentId)
    }
  }
}
