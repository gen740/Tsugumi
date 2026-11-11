import SwiftUI
import TsugumiRPC

/// Inspect the retained environment, independently of the current Card definition.
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
          Text("ビルド時だけに使う依存関係は含まれません。")
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
          Text("この Card の環境はまだ記録されていません")
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

/// Packages are a Card setting, loaded from its latest recorded invocation.
struct CardPackagesView: View {
  let session: ProjectSession
  let cardKey: CardKey
  @State private var selection: Selection?
  @State private var loaded = false
  @State private var error: String?
  @State private var attempt = 0

  private struct Selection: Hashable {
    let run: String
    let environment: String
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      Text("直近の実行で記録された package です。Nix 定義の変更は次の実行後に反映されます。")
        .foregroundStyle(.secondary).padding(12)
      if let error {
        Text(error).foregroundStyle(.red)
        Button("再試行") { attempt += 1 }
      } else if !loaded { ProgressView() }
      else if let selection {
        RecordedEnvironmentView(session: session, runId: selection.run, environmentId: selection.environment).id(selection)
      } else { Text("この Card の環境はまだ記録されていません").padding(12) }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .task(id: ExecutionHistoryRefresh(session: session, card: cardKey)) { await load() }
    .task(id: attempt) { if attempt > 0 { await load() } }
  }

  private func load() async {
    loaded = false; error = nil; selection = nil
    defer { if !Task.isCancelled { loaded = true } }
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
    do {
      let page = try await rpc.history(card: path(cardKey), limit: 1)
      try Task.checkCancellation()
      guard let run = page.runs.compactMap({ $0 }).first?.status.runId else { return }
      let record = try await rpc.record(runId: run, card: path(cardKey), followLatest: true)
      try Task.checkCancellation()
      selection = Selection(run: run, environment: record.record?.summary.environmentId ?? "")
    } catch { if !Task.isCancelled { self.error = String(describing: error) } }
  }
}
