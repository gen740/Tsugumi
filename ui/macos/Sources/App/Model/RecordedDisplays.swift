import Observation
import SwiftUI
import TsugumiRPC

/// ABI 8 stores at most one display per invocation. Its revision gives arrival order.
@MainActor @Observable final class RecordedDisplays {
  struct Entry {
    let invocation: UInt64
    let revision: UInt64
  }

  private(set) var entries: [Entry] = []
  private(set) var display: DisplaySnapshot?
  private(set) var error: String?
  var selection: UInt64?
  private var latest: DisplaySnapshot?

  func load(rpc: RPCClient?, run: String, card: [String], known: Tsugumi_RPC_RecordResultT?, exact: Bool = false) async {
    entries = []; selection = nil; error = nil
    latest = known?.record?.display.flatMap(recordedDisplay)
    display = latest
    guard let rpc, !exact, (known?.aggregate.calls ?? known?.record?.summary.invocation ?? 0) > 1 else { return }
    do {
      var found: [Entry] = []
      var newest = latest
      var cursor: String?
      repeat {
        let page = try await rpc.invocations(runId: run, card: card, cursor: cursor)
        for row in page.records.compactMap({ $0 }) {
          try Task.checkCancellation()
          guard let invocation = row.invocation, invocation > 0 else { continue }
          let record = invocation == known?.record?.summary.invocation
            ? known : try await rpc.record(runId: run, card: card, invocation: invocation)
          guard let wire = record?.record?.display else { continue }
          let revision = wire.revision ?? 0
          found.append(Entry(invocation: invocation, revision: revision))
          if newest == nil || revision >= newest!.revision { newest = recordedDisplay(wire) }
        }
        cursor = page.nextCursor
      } while cursor != nil
      try Task.checkCancellation()
      entries = found.sorted { ($0.revision, $0.invocation) < ($1.revision, $1.invocation) }
      latest = newest; display = newest; selection = entries.last?.invocation
    } catch {
      if !Task.isCancelled { self.error = error.localizedDescription }
    }
  }

  func show(rpc: RPCClient?, run: String, card: [String]) async {
    guard let invocation = selection, let rpc else { return }
    if invocation == entries.last?.invocation { display = latest; return }
    do {
      let record = try await rpc.record(runId: run, card: card, invocation: invocation)
      try Task.checkCancellation()
      guard selection == invocation else { return }
      display = record.record?.display.flatMap(recordedDisplay)
      error = nil
    } catch {
      if !Task.isCancelled && selection == invocation { self.error = error.localizedDescription }
    }
  }
}

struct DisplayRecordPicker: View {
  @Bindable var history: RecordedDisplays

  var body: some View {
    if history.entries.count > 1 {
      Picker("表示の記録", selection: $history.selection) {
        ForEach(Array(history.entries.enumerated()), id: \.element.invocation) { index, entry in
          Text(verbatim: "\(index + 1) / \(history.entries.count) · #\(entry.invocation)").tag(Optional(entry.invocation))
        }
      }
    }
    if let error = history.error { Text(error).font(.caption).foregroundStyle(.red) }
  }
}
