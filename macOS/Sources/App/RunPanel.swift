import AppKit
import SwiftUI
import TsugumiRPC
import UniformTypeIdentifiers

/// History, invocations, logs and artifacts. Logs are read incrementally with
/// LogRange from each view's own next_offset when LogAvailable arrives.
struct RunPanel: View {
  @Bindable var session: ProjectSession
  @State private var runs: [Tsugumi_RPC_RunSummaryT] = []
  @State private var selectedRun: String?
  @State private var stream: Tsugumi_Data_LogStream = .stdout
  @State private var log = ""
  @State private var logOffset: UInt64 = 0
  @State private var logScope = ""
  @State private var artifacts: [Tsugumi_RPC_ArtifactInfoT] = []
  @State private var preview: String?
  @State private var record: Tsugumi_RPC_RecordResultT?
  @State private var error: String?

  private var cardScope: [String]? { session.selection.count == 1 ? path(session.selection.first!) : nil }

  var body: some View {
    HSplitView {
      runList.frame(minWidth: 200, idealWidth: 230)
      logView.padding(6)
      artifactView.frame(minWidth: 220, idealWidth: 260).padding(6)
    }
    .task(id: session.lastFinished) { await loadRuns() }
    .task(id: logKey) { await resetLog() }
    .onChange(of: session.logRevision) { Task { await readMoreLog() } }
    .onChange(of: session.runStatus?.runId) { _, id in
      if let id { selectedRun = id }
    }
  }

  private var logKey: String { "\(selectedRun ?? "")|\(stream.rawValue)|\(cardScope?.joined(separator: "/") ?? "")" }

  private var runList: some View {
    List(selection: $selectedRun) {
      ForEach(runs, id: \.status.runId) { (r: Tsugumi_RPC_RunSummaryT) in
        VStack(alignment: .leading) {
          Text("\(String(describing: r.status.state)) · \(r.target.path.joined(separator: "/"))")
          Text(date(r.status.acceptedAt)).font(.caption).foregroundStyle(.secondary)
        }
        .tag(r.status.runId)
      }
    }
  }

  private var logView: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack {
        Picker("", selection: $stream) {
          Text("stdout").tag(Tsugumi_Data_LogStream.stdout)
          Text("stderr").tag(Tsugumi_Data_LogStream.stderr)
          Text("journal").tag(Tsugumi_Data_LogStream.journal)
        }
        .pickerStyle(.segmented).frame(width: 240).labelsHidden()
        Text(cardScope.map { "Card \($0.joined(separator: "/"))" } ?? "Whole run").font(.caption)
        Spacer()
        Button("Export Log…") { exportLog() }.disabled(log.isEmpty)
      }
      if let record {
        Text(recordSummary(record)).font(.caption.monospaced()).foregroundStyle(.secondary)
      }
      ScrollView {
        Text(log.isEmpty ? "No log output" : log)
          .font(.system(size: 11, design: .monospaced))
          .textSelection(.enabled)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
      if let error { Text(error).font(.caption).foregroundStyle(.red) }
    }
  }

  private var artifactView: some View {
    VStack(alignment: .leading) {
      Text("Artifacts").font(.headline)
      List(artifacts, id: \.artifactId) { (a: Tsugumi_RPC_ArtifactInfoT) in
        VStack(alignment: .leading) {
          Text("\(a.card.path.joined(separator: "/")) · \(a.output)").font(.caption.bold())
          ForEach(a.members.compactMap { $0 }, id: \.path) { (m: Tsugumi_RPC_ArtifactMemberT) in
            HStack {
              Text(m.path).font(.caption)
              Spacer()
              Button("Preview") { Task { await previewMember(a, m.path) } }.buttonStyle(.borderless)
              Button("Export…") { Task { await exportMember(a, m.path) } }.buttonStyle(.borderless)
            }
          }
        }
      }
      if let preview {
        ScrollView { Text(preview).font(.system(size: 11, design: .monospaced)).textSelection(.enabled) }
          .frame(maxHeight: 160)
      }
    }
  }

  private func date(_ ms: Int64?) -> String {
    guard let ms else { return "" }
    return Date(timeIntervalSince1970: Double(ms) / 1000).formatted(date: .abbreviated, time: .standard)
  }

  private func recordSummary(_ r: Tsugumi_RPC_RecordResultT) -> String {
    var s = "state \(String(describing: r.status.state ?? .notStarted)) · calls \(r.aggregate.calls ?? 0)"
    s += " · ok \(r.aggregate.succeeded ?? 0) · failed \(r.aggregate.failed ?? 0)"
    if r.recordingDisabled == true { s += " · I/O not recorded" }
    if r.orphaned == true { s += " · Card removed from the document" }
    if let f = r.status.failure { s += "\n\(f.kind): \(f.diagnostic ?? "")" }
    return s
  }

  private func loadRuns() async {
    guard let rpc = session.rpc else { return }
    do {
      runs = try await rpc.history(limit: 50).runs.compactMap { $0 }
      if selectedRun == nil { selectedRun = runs.first?.status.runId }
    } catch { self.error = "\(error)" }
  }

  private func resetLog() async {
    log = ""
    logOffset = 0
    logScope = "\(selectedRun ?? "")|\(stream)|\(cardScope?.joined(separator: "/") ?? "")"
    record = nil
    await readMoreLog()
    guard let rpc = session.rpc, let run = selectedRun else { return }
    artifacts = (try? await rpc.artifacts(runId: run)) ?? []
    if let card = cardScope { record = try? await rpc.record(runId: run, card: card, followLatest: true) }
  }

  private func readMoreLog() async {
    guard let rpc = session.rpc, let run = selectedRun else { return }
    let scope = logScope
    do {
      // Whole-card logs: the latest invocation; otherwise the run level stream.
      var invocation: UInt64?
      if let card = cardScope {
        invocation = session.cardStates[key(card)]?.invocation
        if invocation == nil { invocation = try? await rpc.record(runId: run, card: card).selectedInvocation }
      }
      while true {
        let slice = try await rpc.logRange(runId: run, card: cardScope, invocation: cardScope == nil ? nil : invocation,
                                           stream: stream, offset: logOffset, limit: 256 * 1024)
        guard scope == logScope else { return }  // the selection changed meanwhile
        log += slice.text
        let next = slice.nextOffset ?? logOffset
        if next == logOffset { break }
        logOffset = next
        if next >= (slice.sizeBytes ?? 0) { break }
      }
      error = nil
    } catch let f as RPCFailure where f.kind == .notFound {
      error = nil
    } catch {
      self.error = "\(error)"
    }
  }

  private func exportLog() {
    let p = NSSavePanel()
    p.nameFieldStringValue = "\(selectedRun ?? "run")-\(stream).log"
    if p.runModal() == .OK, let url = p.url { try? Data(log.utf8).write(to: url) }
  }

  private func previewMember(_ a: Tsugumi_RPC_ArtifactInfoT, _ member: String) async {
    guard let rpc = session.rpc else { return }
    do {
      let data = try await rpc.readArtifact(id: a.artifactId, member: a.kind == .directory ? member : nil)
      preview = String(data: data.prefix(64 * 1024), encoding: .utf8) ?? "(binary, \(data.count) bytes)"
    } catch { self.error = "\(error)" }
  }

  /// Exports through ArtifactRead; the core never receives a macOS path.
  private func exportMember(_ a: Tsugumi_RPC_ArtifactInfoT, _ member: String) async {
    guard let rpc = session.rpc else { return }
    let p = NSSavePanel()
    p.nameFieldStringValue = (member as NSString).lastPathComponent
    guard p.runModal() == .OK, let url = p.url else { return }
    do {
      let data = try await rpc.readArtifact(id: a.artifactId, member: a.kind == .directory ? member : nil)
      try data.write(to: url)
    } catch { self.error = "\(error)" }
  }
}
