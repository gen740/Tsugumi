import SwiftUI
import TsugumiRPC

struct RecordedStorePath: View {
  let path: String
  var body: some View {
    DisclosureGroup(recordedStoreName(path)) {
      Text(path).font(.caption.monospaced()).textSelection(.enabled)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
  }
}

struct RecordedDependencyPaths: View {
  let session: ProjectSession
  let identity: RecordedEnvironmentIdentity
  var excluding: RecordedEnvironmentIdentity?
  @State private var query = ""
  @State private var offsets: [UInt64] = [0]
  @State private var page: Tsugumi_RPC_EnvironmentPathPageT?
  @State private var error: String?
  @State private var attempt = 0

  private struct Query: Equatable {
    let identity: RecordedEnvironmentIdentity
    let excluding: RecordedEnvironmentIdentity?
    let query: String
    let offset: UInt64
    let attempt: Int
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      TextField("依存パスを検索", text: $query).textFieldStyle(.roundedBorder)
        .onChange(of: query) { offsets = [0] }
      Text("検索結果を 100 件ずつ表示します。").font(.caption)
      if let error { Text(error).foregroundStyle(.red); Button("再試行") { attempt += 1 } }
      else if let page {
        Text(String(page.total)).font(.caption)
        if page.paths.isEmpty { Text("一致する依存パスはありません") }
        ForEach(page.paths.compactMap { $0 }, id: \.self) { RecordedStorePath(path: $0) }
      } else { ProgressView() }
      HStack {
        Button("前へ") { offsets.removeLast() }.disabled(page == nil || offsets.count == 1)
        Text(String(offsets.count))
        Button("次へ") { if let next = page?.nextOffset { offsets.append(next) } }.disabled(page == nil || page?.nextOffset == nil)
      }
    }.task(id: Query(identity: identity, excluding: excluding, query: query, offset: offsets.last ?? 0, attempt: attempt)) {
      page = nil; error = nil
      guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
      do {
        let result = try await EnvironmentHistorySource(rpc: rpc).paths(identity, offset: offsets.last ?? 0, query: query, excluding: excluding)
        try Task.checkCancellation()
        page = result
      } catch { if !Task.isCancelled { self.error = String(describing: error) } }
    }
  }
}

private struct RecordedEnvironmentUses: View {
  let session: ProjectSession
  let identity: RecordedEnvironmentIdentity
  @State private var offsets: [UInt64] = [0]
  @State private var page: Tsugumi_RPC_EnvironmentUsePageT?
  @State private var error: String?
  @State private var attempt = 0

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      Text("この環境を共有する Card と呼び出し").font(.headline)
      if let error { Text(error).foregroundStyle(.red); Button("再試行") { attempt += 1 } }
      else if let page {
        ForEach(Array(page.uses.compactMap { $0 }.enumerated()), id: \.offset) { _, use in
          Text("\(session.displayPath(path(use.card))) · #\(use.invocation)")
        }
      } else { ProgressView() }
      HStack {
        Button("前へ") { offsets.removeLast() }.disabled(page == nil || offsets.count == 1)
        Text(String(offsets.count))
        Button("次へ") { if let next = page?.nextOffset { offsets.append(next) } }.disabled(page == nil || page?.nextOffset == nil)
      }
    }.task(id: "\(offsets.last ?? 0):\(attempt)") {
      page = nil; error = nil
      guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
      do {
        let result = try await EnvironmentHistorySource(rpc: rpc).uses(identity, offset: offsets.last ?? 0)
        try Task.checkCancellation()
        page = result
      } catch { if !Task.isCancelled { self.error = String(describing: error) } }
    }
  }
}

/// The environment recorded for a run: no evaluation of the current Card definition.
struct RecordedEnvironmentView: View {
  let session: ProjectSession
  let runId: String
  let environmentId: String
  @State private var environment: Tsugumi_RPC_BuildEnvironmentT?
  @State private var loaded = false
  @State private var error: String?
  @State private var attempt = 0
  private var identity: RecordedEnvironmentIdentity { .init(run: runId, id: environmentId) }

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      if let error { Text(error).foregroundStyle(.red); Button("再試行") { attempt += 1 } }
      if !loaded { ProgressView() }
      else if let environment {
        LabeledContent("対象システム", value: environment.system)
        Text("Manifest の出力パス").font(.headline)
        RecordedStorePath(path: environment.manifest)
        Text("Derivation").font(.headline)
        RecordedStorePath(path: environment.derivation)
        RecordedEnvironmentUses(session: session, identity: identity).id(identity.run + ":" + identity.id)
        Text("実行時の依存パス").font(.headline)
        Text(String(environment.pathCount)).font(.caption)
        Text("実行時の closure は、ビルド時の依存関係の完全な一覧ではありません。")
        RecordedDependencyPaths(session: session, identity: identity).id(identity.run + ":" + identity.id)
      } else if error == nil { Text("この呼び出しの環境は記録されていません") }
    }.task(id: attempt) {
      loaded = false; environment = nil; error = nil
      defer { if !Task.isCancelled { loaded = true } }
      guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
      do {
        let result = try await EnvironmentHistorySource(rpc: rpc).find(identity)
        try Task.checkCancellation()
        environment = result
      } catch { if !Task.isCancelled { self.error = String(describing: error) } }
    }
  }
}

private struct RecordedRunEnvironments: View {
  let session: ProjectSession
  let runId: String
  @State var selected: String
  @State private var offsets: [UInt64] = [0]
  @State private var page: Tsugumi_RPC_BuildEnvironmentPageT?
  @State private var error: String?
  @State private var attempt = 0

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 10) {
        Text("この実行の共有環境").font(.headline)
        if let error { Text(error).foregroundStyle(.red); Button("再試行") { attempt += 1 } }
        if let page {
          ForEach(page.environments.compactMap { $0 }, id: \.id) { environment in
            Button { selected = environment.id } label: {
              Text("\(recordedStoreName(environment.manifest)) · \(environment.system) · \(environment.id.prefix(8))")
                .fontWeight(selected == environment.id ? .bold : .regular)
            }
          }
        } else if error == nil { ProgressView() }
        HStack {
          Button("前へ") { offsets.removeLast() }.disabled(page == nil || offsets.count == 1)
          Text(String(offsets.count))
          Button("次へ") { if let next = page?.nextOffset { offsets.append(next) } }.disabled(page == nil || page?.nextOffset == nil)
        }
        Divider()
        RecordedEnvironmentView(session: session, runId: runId, environmentId: selected).id(runId + ":" + selected)
      }.frame(maxWidth: .infinity, alignment: .leading).padding(12)
    }.task(id: "\(offsets.last ?? 0):\(attempt)") {
      page = nil; error = nil
      guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
      do {
        let result = try await EnvironmentHistorySource(rpc: rpc).page(run: runId, offset: offsets.last ?? 0)
        try Task.checkCancellation()
        page = result
      } catch { if !Task.isCancelled { self.error = String(describing: error) } }
    }
  }
}

struct InvocationPackagesView: View {
  let session: ProjectSession
  let runId: String
  let environmentId: String
  var body: some View {
    if environmentId.isEmpty {
      ContentUnavailableView("この呼び出しの環境は記録されていません", systemImage: "shippingbox",
                             description: Text("依存 package は実行時に記録されたものだけを表示します。現在の Nix 定義からは求めません。"))
    } else {
      RecordedRunEnvironments(session: session, runId: runId, selected: environmentId).id(runId + ":" + environmentId)
    }
  }
}

struct RecordedEnvironmentComparison: View {
  let session: ProjectSession
  let left: RecordedEnvironmentIdentity
  let right: RecordedEnvironmentIdentity
  private var difference: ComparedDifference { EnvironmentHistorySource.change(left.id, right.id) }
  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack {
        Text("環境").bold()
        if difference == .changed { Text("環境が変わった Card") }
        else if difference == .unknown { Text("比較できません") }
        else { Text("同じ環境です") }
      }
      if difference == .unknown { Text("この呼び出しの環境は記録されていません") }
      if difference == .changed {
        Text("実行時の closure は、ビルド時の依存関係の完全な一覧ではありません。")
        HStack(alignment: .top, spacing: 12) {
          VStack(alignment: .leading) {
            Text("削除された実行時の依存パス").bold()
            RecordedDependencyPaths(session: session, identity: left, excluding: right)
          }.frame(maxWidth: .infinity, alignment: .leading)
          VStack(alignment: .leading) {
            Text("追加された実行時の依存パス").bold()
            RecordedDependencyPaths(session: session, identity: right, excluding: left)
          }.frame(maxWidth: .infinity, alignment: .leading)
        }
      }
    }.padding(6).background(difference == .changed ? Color.accentColor.opacity(0.09) : .clear)
  }
}

func recordedStoreName(_ path: String) -> String {
  let name = String(path.split(separator: "/").last ?? "")
  guard name.count > 33 else { return name }
  let prefix = name.prefix(32)
  guard prefix.allSatisfy({ $0.isASCII && ($0.isLowercase || $0.isNumber) }),
        name[name.index(name.startIndex, offsetBy: 32)] == "-" else { return name }
  return String(name.dropFirst(33))
}
