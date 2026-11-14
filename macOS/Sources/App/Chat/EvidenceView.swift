import SwiftUI
import TsugumiRPC

private struct EvidenceLinksKey: EnvironmentKey {
  static let defaultValue: Set<String> = []
}
extension EnvironmentValues {
  var evidenceLinks: Set<String> {
    get { self[EvidenceLinksKey.self] }
    set { self[EvidenceLinksKey.self] = newValue }
  }
}

/// Resolve citations against this session before enabling them in the answer.
struct EvidenceMarkup: ViewModifier {
  let text: String
  let session: ProjectSession
  var tab: WorkflowTab?
  @Environment(\.openWindow) private var openWindow
  @State private var allowed: Set<String> = []
  @State private var selected: EvidenceTarget?

  func body(content: Content) -> some View {
    content
      .environment(\.evidenceLinks, allowed)
      .environment(\.openURL, OpenURLAction { url in
        let href = url.absoluteString
        guard EvidenceTarget.recognizes(href) else {
          return ["https", "http", "mailto"].contains(url.scheme?.lowercased() ?? "") ? .systemAction : .discarded
        }
        guard allowed.contains(href), let target = EvidenceTarget.parse(href) else { return .discarded }
        Task { @MainActor in
          guard await target.exists(in: session) else { allowed.remove(href); return }
          if target.kind == "card", let card = target.card {
            if let tab {
              tab.enter(Array(card.dropLast())); tab.selection = [key(card)]
              DispatchQueue.main.async { NotificationCenter.default.post(name: .canvasReveal, object: tab.id, userInfo: ["card": key(card)]) }
            } else if let workspace = AppModel.shared.mainWindows.first(where: { $0.session === session }) {
              workspace.openTab(Array(card.dropLast()))
              if let active = workspace.active {
                active.selection = [key(card)]
                DispatchQueue.main.async { NotificationCenter.default.post(name: .canvasReveal, object: active.id, userInfo: ["card": key(card)]) }
              }
            }
            else { openWindow(id: "workflow", value: WorkflowRef(path: session.identity.url.path, scope: Array(card.dropLast()), selected: key(card))) }
          } else { selected = target }
        }
        return .handled
      })
      .task(id: "\(text)|\(session.rev)|\(String(describing: session.lastFinished))") {
        allowed = []
        for href in Self.links(MarkdownDocument.parse(text).blocks) {
          guard !Task.isCancelled else { return }
          if let target = EvidenceTarget.parse(href), await target.exists(in: session) { allowed.insert(href) }
        }
      }
      .sheet(item: $selected) { target in EvidenceView(session: session, target: target) }
  }

  static func links(_ blocks: [MarkdownDocument.Block]) -> Set<String> {
    var result: Set<String> = []
    func inline(_ values: [MarkdownDocument.Inline]) {
      for case .link(_, let href) in values where EvidenceTarget.recognizes(href) { result.insert(href) }
    }
    for block in blocks {
      switch block {
      case .paragraph(let values), .heading(_, let values): inline(values)
      case .quote(let children): result.formUnion(links(children))
      case .list(_, _, let items): for item in items { result.formUnion(links(item.blocks)) }
      case .table(let header, let rows): for values in header + rows.flatMap({ $0 }) { inline(values) }
      default: break
      }
    }
    return result
  }
}

struct EvidenceView: View {
  let session: ProjectSession
  let target: EvidenceTarget
  @Environment(\.dismiss) private var dismiss
  @State private var preview: NSImage?
  @State private var bodyText = String(localized: "読み込み中…")
  @State private var rows: [Tsugumi_RPC_InvocationSummaryT] = []
  @State private var record: Tsugumi_RPC_RecordResultT?
  @State private var selected: EvidenceTarget?
  @State private var artifact: Tsugumi_RPC_ArtifactInfoT?
  @State private var artifactMember: String?
  @State private var exporting = false
  @State private var exportError: String?

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack { Text("根拠の記録").font(.title2); Spacer(); Button("閉じる") { dismiss() } }
      Text(target.href).font(.caption).textSelection(.enabled)
      if let artifact, let artifactMember {
        Button("書き出す…") { Task { await export(artifact, artifactMember) } }.disabled(exporting)
      }
      if let exportError { Text(exportError).foregroundStyle(.red).textSelection(.enabled) }
      ScrollView {
        VStack(alignment: .leading, spacing: 12) {
          if let preview { Image(nsImage: preview).resizable().scaledToFit() }
          Text(bodyText).font(.system(.body, design: target.kind == "log" ? .monospaced : .default)).textSelection(.enabled)
          ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
            Button("\(row.card.path.joined(separator: "/")) · #\(row.invocation ?? 0) · \(row.state.label)") {
              selected = EvidenceTarget.parse("run:\(target.run!)/\(row.card.path.joined(separator: "/"))#\(row.invocation ?? 0)")
            }.buttonStyle(.link).disabled((row.invocation ?? 0) == 0)
          }
          if let r = record?.record {
            if let display = r.display.flatMap(recordedDisplay) {
              DisplayContent(display: display, large: true).frame(height: 360)
            }
            Text("入力").font(.headline)
            ForEach(Array(r.inputs.compactMap { $0 }.enumerated()), id: \.offset) { _, value in Text(describeValue(value)).monospaced().textSelection(.enabled) }
            Text("出力").font(.headline)
            ForEach(Array(r.outputs.compactMap { $0 }.enumerated()), id: \.offset) { _, value in Text(describeValue(value)).monospaced().textSelection(.enabled) }
          }
        }.frame(maxWidth: .infinity, alignment: .leading)
      }
    }.padding(20).frame(minWidth: 650, minHeight: 450)
      .task { await load() }
      .sheet(item: $selected) { EvidenceView(session: session, target: $0) }
  }
  private func load() async {
    guard let rpc = session.rpc, await target.exists(in: session), let run = target.run else {
      bodyText = String(localized: "このプロジェクトに根拠の記録がありません"); return
    }
    do {
      if target.kind == "log" {
        let log = try await rpc.logRange(runId: run, card: target.card, invocation: target.invocation, stream: target.logStream, offset: target.offset)
        bodyText = "\(target.stream) · \(String(localized: "位置")) \(log.offset ?? 0)\n\n\(log.text)"
      } else if target.kind == "artifact" {
        guard let a = try await rpc.artifacts(runId: run).first(where: { $0.artifactId == target.artifact }) else { return }
        let member = target.member ?? a.members.first.flatMap { $0?.path }
        artifact = a
        artifactMember = member
        let request = Tsugumi_RPC_ArtifactReadRequestT()
        request.artifactId = a.artifactId
        request.member = a.kind == .directory ? member : nil
        request.limitBytes = 65536
        let data: Tsugumi_RPC_FileContentT = try await rpc.call(.artifactreadrequest, request)
        if data.eof == true { preview = NSImage(data: Data(data.bytes)) }
        bodyText = "\(session.displayPath(a.card.path)) · \(a.output)\n\(member ?? "")\n\n" + (preview == nil ? (String(bytes: data.bytes, encoding: .utf8) ?? String(localized: "バイナリの成果物です")) : "")
        if data.eof != true { bodyText += "\n" + String(localized: "先頭 64 KiB を表示") }
      } else if let card = target.card {
        let r = try await rpc.record(runId: run, card: card, invocation: target.invocation)
        record = r
        let summary = r.record?.summary
        let state = summary?.state ?? r.status.state
        let diagnostic = summary.map { $0.failure?.diagnostic ?? "" } ?? (r.status.failure?.diagnostic ?? "")
        bodyText = "\(session.displayPath(card)) · #\(summary?.invocation ?? 0)\n\(state.label)\n\(diagnostic)"
      } else {
        let status = try await rpc.runStatus(runId: run)
        bodyText = status.state.label
        let request = Tsugumi_RPC_RunLatestInvocationsRequestT(); request.runId = run
        let page: Tsugumi_RPC_InvocationPageT = try await rpc.call(.runlatestinvocationsrequest, request)
        rows = page.records.compactMap { $0 }
      }
    } catch { bodyText = String(describing: error) }
  }

  private func export(_ artifact: Tsugumi_RPC_ArtifactInfoT, _ member: String) async {
    guard let rpc = session.rpc else { return }
    let panel = NSSavePanel()
    panel.nameFieldStringValue = (member as NSString).lastPathComponent
    guard panel.runModal() == .OK, let url = panel.url else { return }
    exporting = true
    exportError = nil
    defer { exporting = false }
    do {
      guard await target.exists(in: session) else {
        exportError = String(localized: "このプロジェクトに根拠の記録がありません"); return
      }
      let data = try await rpc.readArtifact(id: artifact.artifactId, member: artifact.kind == .directory ? member : nil)
      try data.write(to: url, options: .atomic)
    } catch { exportError = String(describing: error) }
  }
}
