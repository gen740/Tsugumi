import SwiftUI
import TsugumiRPC

struct CardEnvironment: Equatable {
  let card: String
  let environment: String?
}
struct CardEnvironmentChange: Identifiable {
  let card: String
  let before: String?
  let after: String?
  let state: State
  var id: String { card }
  enum State { case added, removed, changed, same, unrecorded }
  var label: String {
    switch state {
    case .added: String(localized: "追加された Card")
    case .removed: String(localized: "削除された Card")
    case .changed: String(localized: "環境が変わりました")
    case .same: String(localized: "同じ環境です")
    case .unrecorded: String(localized: "環境の記録がありません")
    }
  }
}
func compareCardEnvironments(_ before: [CardEnvironment], _ after: [CardEnvironment]) -> [CardEnvironmentChange] {
  let old = Dictionary(uniqueKeysWithValues: before.map { ($0.card, $0) })
  let current = Dictionary(uniqueKeysWithValues: after.map { ($0.card, $0) })
  return Set(old.keys).union(current.keys).sorted().map { card in
    let a = old[card]?.environment, b = current[card]?.environment
    let state: CardEnvironmentChange.State = old[card] == nil ? .added : current[card] == nil ? .removed
      : a == nil || b == nil ? .unrecorded : a == b ? .same : .changed
    return CardEnvironmentChange(card: card, before: a, after: b, state: state)
  }
}

/// Defer RPC work until the user opens a group, and cancel it when they close it.
struct EnvironmentDisclosure<Content: View>: View {
  let label: String
  @ViewBuilder var content: () -> Content
  @State private var expanded = false
  var body: some View {
    DisclosureGroup(isExpanded: $expanded) { if expanded { content() } } label: { Text(label) }
  }
}

struct EnvironmentHistoryView: View {
  let session: ProjectSession
  let runId: String
  @State private var rows: [CardEnvironment] = []
  @State private var before: [CardEnvironment] = []
  @State private var names: [String: String] = [:]
  @State private var runs: [Tsugumi_RPC_RunSummaryT] = []
  @State private var cursor: String?
  @State private var compare = ""
  @State private var error: String?
  @State private var busy = false
  @State private var loadingRuns = false
  @State private var attempt = 0

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 12) {
        Text("実行全体の環境").font(.headline)
        Text("各 Card の最後の呼び出しを比較します。")
        Picker("比較元の実行", selection: $compare) {
          Text("比較しない").tag("")
          ForEach(runs, id: \.status.runId) { run in
            Text("\(formatMillis(run.status.acceptedAt)) · \(run.status.runId ?? "")").tag(run.status.runId ?? "")
          }
        }
        if cursor != nil { Button("以前の実行を読み込む") { Task { await loadRuns(more: true) } }.disabled(loadingRuns) }
        if let error {
          Text(error).foregroundStyle(.red)
          Button("再試行") { attempt += 1 }
        }
        if busy { ProgressView() }
        else if compare.isEmpty {
          let groups = Dictionary(grouping: rows, by: { $0.environment ?? "" })
          ForEach(groups.keys.sorted(), id: \.self) { id in
            EnvironmentDisclosure(label: names[id] ?? String(localized: "環境の記録がありません")) {
              Text(id).font(.caption.monospaced()).textSelection(.enabled)
              Text((groups[id] ?? []).map(\.card).sorted().joined(separator: ", "))
              if !id.isEmpty { RecordedEnvironmentView(session: session, runId: runId, environmentId: id) }
            }
            Text((groups[id] ?? []).map(\.card).sorted().joined(separator: ", ")).font(.caption)
          }
        } else {
          ForEach(compareCardEnvironments(before, rows)) { row in
            EnvironmentDisclosure(label: "\(row.card) · \(row.label)") {
              if let after = row.after, row.state == .changed || row.state == .added {
                Text("追加された依存パス").font(.headline)
                EnvironmentDifferenceView(session: session, runId: runId, environmentId: after,
                                          excludedRun: row.before == nil ? nil : compare, excludedEnvironment: row.before)
              }
              if let before = row.before, row.state == .changed || row.state == .removed {
                Text("削除された依存パス").font(.headline)
                EnvironmentDifferenceView(session: session, runId: compare, environmentId: before,
                                          excludedRun: row.after == nil ? nil : runId, excludedEnvironment: row.after)
              }
              if let before = row.before {
                EnvironmentDisclosure(label: String(localized: "比較元の実行")) {
                  RecordedEnvironmentView(session: session, runId: compare, environmentId: before)
                }
              }
              if let after = row.after {
                EnvironmentDisclosure(label: String(localized: "記録された実行環境")) {
                  RecordedEnvironmentView(session: session, runId: runId, environmentId: after)
                }
              }
            }.id("\(compare):\(row.card)")
          }
        }
      }.frame(maxWidth: .infinity, alignment: .leading).padding(12)
    }
    .task(id: "\(compare):\(attempt)") { await load() }
    .task(id: attempt) { await loadRuns(more: false) }
  }

  private func loadRuns(more: Bool) async {
    guard !loadingRuns, let rpc = session.rpc else { return }
    loadingRuns = true
    defer { loadingRuns = false }
    do {
      let page = try await rpc.history(cursor: more ? cursor : nil, limit: 100)
      try Task.checkCancellation()
      runs = (more ? runs : []) + page.runs.compactMap { $0 }
      cursor = page.nextCursor
    } catch { if !Task.isCancelled { self.error = String(describing: error) } }
  }

  private func load() async {
    rows = []; before = []; names = [:]; error = nil
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
    busy = true
    defer { if !Task.isCancelled { busy = false } }
    do {
      func latest(_ run: String) async throws -> [CardEnvironment] {
        let page = try await rpc.latestInvocations(runId: run)
        return page.records.compactMap { $0 }.map { CardEnvironment(card: $0.card.segments.compactMap { $0 }.joined(separator: "/"), environment: $0.environmentId) }
      }
      let current = try await latest(runId)
      let old = compare.isEmpty ? [] : try await latest(compare)
      var labels: [String: String] = [:]
      var offset: UInt64? = 0
      while let current = offset {
        try Task.checkCancellation()
        let request = Tsugumi_RPC_RunEnvironmentsRequestT()
        request.runId = runId; request.offset = current; request.limit = 100
        let page: Tsugumi_RPC_BuildEnvironmentPageT = try await rpc.call(.runenvironmentsrequest, request)
        for environment in page.environments.compactMap({ $0 }) { labels[environment.id] = recordedStoreName(environment.manifest) }
        if let next = page.nextOffset, next <= current { throw EnvironmentPageError.nonAdvancing }
        offset = page.nextOffset
      }
      try Task.checkCancellation()
      rows = current; before = old; names = labels
    } catch { if !Task.isCancelled { self.error = String(describing: error) } }
  }
}

struct EnvironmentDifferenceView: View {
  let session: ProjectSession
  let runId: String
  let environmentId: String
  let excludedRun: String?
  let excludedEnvironment: String?
  @State private var query = ""
  @State private var offsets: [UInt64] = [0]
  @State private var page: Tsugumi_RPC_EnvironmentPathPageT?
  @State private var error: String?
  @State private var attempt = 0

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      TextField("依存パスを検索", text: $query).onChange(of: query) { offsets = [0] }
      if let error {
        Text(error).foregroundStyle(.red)
        Button("再試行") { attempt += 1 }
      } else if let page {
        Text(String(page.total)).font(.caption)
        ForEach(page.paths.compactMap { $0 }, id: \.self) { path in
          DisclosureGroup(recordedStoreName(path)) { Text(path).font(.caption.monospaced()).textSelection(.enabled) }
        }
        HStack {
          Button("前へ") { offsets.removeLast() }.disabled(offsets.count == 1)
          Button("次へ") { if let next = page.nextOffset { offsets.append(next) } }.disabled(page.nextOffset == nil)
        }
      } else { ProgressView() }
    }.task(id: "\(query):\(offsets.last ?? 0):\(attempt)") { await load() }
  }

  private func load() async {
    page = nil; error = nil
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
    do {
      let r = Tsugumi_RPC_RunEnvironmentPathsRequestT()
      r.runId = runId; r.environmentId = environmentId; r.excludeRunId = excludedRun
      r.excludeEnvironmentId = excludedEnvironment; r.query = query; r.offset = offsets.last ?? 0; r.limit = 100
      let result: Tsugumi_RPC_EnvironmentPathPageT = try await rpc.call(.runenvironmentpathsrequest, r)
      try Task.checkCancellation()
      if let next = result.nextOffset, next <= (r.offset ?? 0) { throw EnvironmentPageError.nonAdvancing }
      page = result
    } catch { if !Task.isCancelled { self.error = String(describing: error) } }
  }
}
