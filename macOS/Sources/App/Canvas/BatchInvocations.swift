import SwiftUI
import TsugumiRPC

/// Coalesce member updates while an invocation list is open.
@MainActor func observeBatchHistory(_ session: ProjectSession, update: (Int) -> Void) async {
  var previous: Int?
  while !Task.isCancelled {
    let revision = session.batchHistoryRevision
    if revision != previous {
      previous = revision
      update(revision)
    }
    try? await Task.sleep(for: .milliseconds(200))
  }
}

struct BatchInvocationsBadge: View {
  let session: ProjectSession
  let card: CardKey
  let batch: Tsugumi_RPC_RunBatchT
  let onLog: (UInt64) -> Void
  let onHistory: () -> Void
  @State private var open = false
  @State private var page: Tsugumi_RPC_InvocationPageT?
  @State private var cursors: [String] = []
  @State private var problem: String?
  @State private var revision = 0

  private struct Refresh: Equatable {
    let runId: String?
    let activation: UInt64
    let cursor: String?
    let revision: Int
    let connected: Bool
  }

  var body: some View {
    Button { open.toggle() } label: {
      Text(verbatim: "×\(batch.invocationCount)").font(.system(size: 11, design: .monospaced))
    }
    .buttonStyle(.bordered)
    .controlSize(.mini)
    .accessibilityLabel(Text("呼び出し一覧"))
    .accessibilityValue(Text(verbatim: String(batch.invocationCount)))
    .popover(isPresented: $open, arrowEdge: .bottom) {
      panel
    }
    .onChange(of: session.currentRunId) { _, _ in resetPage() }
    .onChange(of: batch.activation) { _, _ in resetPage() }
  }

  private var panel: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack {
        Text("呼び出し一覧").font(.headline)
        Spacer()
        Button("実行履歴") { open = false; onHistory() }
      }
      if let problem {
        Text(problem).font(.caption).textSelection(.enabled)
      } else if session.rpc == nil {
        Text("実行環境に接続していません").font(.caption)
      } else if let page {
        let rows = page.records.compactMap { $0 }.filter { $0.invocation != nil }
        if rows.isEmpty {
          Text(batch.invocationCount == 0 ? String(localized: "空の実行リスト") : String(localized: "呼び出しの記録を待っています"))
            .font(.caption).foregroundStyle(.secondary)
        } else {
          ScrollView {
            LazyVStack(spacing: 2) {
              ForEach(rows, id: \.invocation) { row in
                if let invocation = row.invocation {
                  Button {
                    open = false
                    onLog(invocation)
                  } label: {
                    HStack {
                      Text(verbatim: "#\(invocation)").monospacedDigit()
                      if let i = row.batchIndex, let size = row.batchSize {
                        Text(verbatim: (row.batchActivation.map { "#\($0) · " } ?? "") + "\(i)/\(size)")
                          .font(.caption).foregroundStyle(.secondary)
                      }
                      Spacer()
                      Text(row.state.label)
                    }.contentShape(Rectangle())
                  }
                  .buttonStyle(.borderless)
                  .padding(.vertical, 3)
                }
              }
            }
          }.frame(maxHeight: 260)
        }
        if !cursors.isEmpty || page.nextCursor != nil {
          HStack {
            Button("前へ") { self.page = nil; cursors.removeLast() }.disabled(cursors.isEmpty)
            Button("次へ") {
              if let next = page.nextCursor { self.page = nil; cursors.append(next) }
            }.disabled(page.nextCursor == nil)
          }
        }
      } else {
        ProgressView().controlSize(.small)
      }
    }
    .padding(12).frame(width: 340, alignment: .leading)
    .task { await observeBatchHistory(session) { revision = $0 } }
    .task(id: Refresh(runId: session.currentRunId, activation: batch.activation, cursor: cursors.last,
                      revision: revision, connected: session.connected)) {
      guard let rpc = session.rpc, let runId = session.currentRunId else { return }
      do {
        let result = try await rpc.invocations(runId: runId, card: path(card), cursor: cursors.last)
        guard !Task.isCancelled, runId == session.currentRunId else { return }
        page = result
        problem = nil
      } catch {
        if !Task.isCancelled { problem = String(describing: error) }
      }
    }
  }

  private func resetPage() {
    page = nil
    cursors = []
    problem = nil
  }
}
