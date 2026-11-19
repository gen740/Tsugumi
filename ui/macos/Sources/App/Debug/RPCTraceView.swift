import Foundation
import SwiftUI
import TsugumiAgentClient
import TsugumiRPC
import UniformTypeIdentifiers

private struct TraceRow: Identifiable {
  let source: String
  let entry: Tsugumi_Trace_EntryT

  var id: String { "\(source):\(entry.sequence)" }
  var pair: String { "\(entry.from) → \(entry.to)" }
  var date: Date { Date(timeIntervalSince1970: Double(entry.unixMicros) / 1_000_000) }
  var direction: String {
    switch entry.direction {
    case .request: return String(localized: "要求")
    case .reply: return String(localized: "応答")
    case .event: return String(localized: "イベント")
    case .notification: return String(localized: "通知")
    }
  }
  var result: String { entry.failure ?? (entry.direction == .reply ? "OK" : "") }

  var exportObject: [String: Any] {
    var value: [String: Any] = [
      "recorder": source.isEmpty ? "hostd" : source,
      "sequence": entry.sequence, "unix_micros": entry.unixMicros,
      "from": entry.from, "to": entry.to, "direction": String(describing: entry.direction),
      "message_type": entry.messageType, "bytes": entry.bytes,
    ]
    value["request_id"] = entry.requestId
    value["elapsed_micros"] = entry.elapsedMicros
    value["status"] = entry.direction == .reply ? entry.failure ?? "ok" : nil
    value["content_omission"] = entry.contentOmission
    if let json = entry.contentJson {
      value["content"] = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) ?? json
    }
    return value
  }
}

private struct TraceExport: FileDocument {
  static var readableContentTypes: [UTType] { [.json] }
  let data: Data

  init(data: Data) { self.data = data }
  init(configuration: ReadConfiguration) throws {
    guard let data = configuration.file.regularFileContents else { throw CocoaError(.fileReadCorruptFile) }
    self.data = data
  }
  func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
    FileWrapper(regularFileWithContents: data)
  }
}

struct RPCTraceView: View {
  @State private var mode: Tsugumi_Trace_Mode = .off
  @State private var rows: [TraceRow] = []
  @State private var unavailable: [String] = []
  @State private var error: String?
  @State private var updating = false
  @State private var pair = ""
  @State private var type = ""
  @State private var failuresOnly = false
  @State private var selection: TraceRow.ID?
  @State private var export: TraceExport?
  @State private var exporting = false

  private var filtered: [TraceRow] {
    rows.filter { row in
      (pair.isEmpty || row.pair == pair) &&
      (type.isEmpty || row.entry.messageType == type) &&
      (!failuresOnly || row.entry.failure != nil)
    }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack {
        Picker("記録モード", selection: Binding(get: { mode }, set: { requested in
          Task { await refresh(mode: requested) }
        })) {
          Text("オフ").tag(Tsugumi_Trace_Mode.off)
          Text("トレース").tag(Tsugumi_Trace_Mode.trace)
          Text("キャプチャ").tag(Tsugumi_Trace_Mode.capture)
        }
        .pickerStyle(.segmented)
        .fixedSize()
        Button("更新") { Task { await refresh() } }
        Spacer()
        if updating { ProgressView().controlSize(.small) }
        Button("表示中の記録をエクスポート…", action: prepareExport)
          .disabled(filtered.isEmpty)
      }
      .disabled(updating)
      if mode == .capture {
        Text("キャプチャにはファイルの内容や入力が含まれる場合があります。")
          .font(.caption).foregroundStyle(.secondary)
      }
      if let error { Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
      if !unavailable.isEmpty {
        Text("取得できないプロジェクト: \(unavailable.joined(separator: ", "))")
          .font(.caption).foregroundStyle(.secondary)
      }
      filters
      HSplitView {
        Table(filtered, selection: $selection) {
          TableColumn("時刻") { row in
            Text(row.date, format: .iso8601
              .year().month().day().time(includingFractionalSeconds: true))
              .font(.caption.monospacedDigit())
          }
          .width(min: 125, ideal: 165)
          TableColumn("エンドポイント") { Text($0.pair) }.width(min: 110, ideal: 190)
          TableColumn("種別") { Text($0.direction) }.width(min: 50, ideal: 65)
          TableColumn("メッセージ型") { Text($0.entry.messageType) }.width(min: 105, ideal: 155)
          TableColumn("要求 ID") { Text($0.entry.requestId.map(String.init) ?? "—").monospacedDigit() }
            .width(min: 55, ideal: 65)
          TableColumn("バイト数") { Text($0.entry.bytes, format: .number).monospacedDigit() }
            .width(min: 55, ideal: 65)
          TableColumn("経過時間 (µs)") { Text($0.entry.elapsedMicros.map(String.init) ?? "—").monospacedDigit() }
            .width(min: 70, ideal: 90)
          TableColumn("結果") { Text($0.result) }.width(min: 55, ideal: 80)
        }
        .frame(minWidth: 550)
        detail
          .frame(minWidth: 250, minHeight: 320)
      }
    }
    .padding()
    .task { await refresh() }
    .fileExporter(isPresented: $exporting, document: export, contentType: .json,
                  defaultFilename: "rpc-trace.json") { result in
      export = nil
      if case .failure(let failure) = result { error = failure.localizedDescription }
    }
  }

  private var filters: some View {
    HStack {
      Picker("エンドポイント", selection: $pair) {
        Text("すべて").tag("")
        ForEach(Set(rows.map(\.pair)).sorted(), id: \.self) { Text($0).tag($0) }
      }
      Picker("メッセージ型", selection: $type) {
        Text("すべて").tag("")
        ForEach(Set(rows.map(\.entry.messageType)).sorted(), id: \.self) { Text($0).tag($0) }
      }
      Toggle("失敗のみ", isOn: $failuresOnly)
      Spacer()
    }
  }

  private var detail: some View {
    let counts = Dictionary(grouping: rows, by: \.pair).mapValues(\.count)
    let row = filtered.first { $0.id == selection }
    return VStack(alignment: .leading, spacing: 8) {
      Text("エンドポイント別の件数").font(.headline)
      ScrollView {
        VStack(alignment: .leading) {
          ForEach(counts.keys.sorted(), id: \.self) { pair in
            LabeledContent(pair, value: String(counts[pair, default: 0]))
          }
        }
      }
      .frame(maxHeight: 110)
      Divider()
      Text("メッセージの内容").font(.headline)
      if let row {
        if !row.source.isEmpty {
          Text("プロジェクト: \(row.source)").font(.caption).textSelection(.enabled)
        }
        if let status = row.entry.contentOmission {
          Text(omission(status)).font(.caption).foregroundStyle(.secondary)
        }
        if let json = row.entry.contentJson {
          ScrollView([.horizontal, .vertical]) {
            Text(json).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
              .frame(maxWidth: .infinity, alignment: .leading)
          }
        } else if row.entry.contentOmission == nil {
          Text("本文の記録はありません。").foregroundStyle(.secondary)
        }
      } else {
        Text("一覧からメッセージを選択してください。").foregroundStyle(.secondary)
      }
      Spacer(minLength: 0)
    }
    .padding(.leading, 8)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
  }

  private func omission(_ reason: String) -> String {
    switch reason {
    case "redacted", "secret": return String(localized: "認証情報を含むフィールドを除外しました。")
    case "size": return String(localized: "サイズ上限を超えたため、本文を省略しました。")
    case "evicted": return String(localized: "バッファの上限に達したため、本文を消去しました。")
    default: return String(localized: "本文をデコードできませんでした。")
    }
  }

  private func prepareExport() {
    do {
      let data = try JSONSerialization.data(withJSONObject: filtered.map(\.exportObject),
                                            options: [.prettyPrinted, .sortedKeys])
      export = TraceExport(data: data)
      exporting = true
    } catch { self.error = error.localizedDescription }
  }

  private func query(mode: Tsugumi_Trace_Mode? = nil, source: String? = nil) async throws -> Tsugumi_Trace_HostTraceT {
    let request = Tsugumi_Trace_HostTraceRequestT()
    request.mode = mode?.rawValue
    request.source = source
    return try await CoreHostClient.shared.hostCall(.hosttracerequest, request, existingOnly: true)
  }

  private func refresh(mode requested: Tsugumi_Trace_Mode? = nil) async {
    guard !updating else { return }
    updating = true
    defer { updating = false }
    error = nil
    // Drop displayed content before waiting for the daemons to clear theirs.
    if requested != .capture { clearContent() }
    do {
      let catalog = try await query(mode: requested)
      mode = catalog.snapshot.mode
      var missing: [String] = []
      var fetched: [TraceRow] = []
      let recorders = [("", true)] + catalog.recorders.compactMap { recorder in
        recorder.map { ($0.id, $0.available) }
      }
      for (source, available) in recorders {
        do {
          guard available else { missing.append(source); continue }
          let response = try await query(source: source)
          fetched += response.snapshot.entries.compactMap { entry in
            entry.map { TraceRow(source: source, entry: $0) }
          }
        } catch {
          if source.isEmpty { throw error }
          missing.append(source)
        }
      }
      rows = fetched.sorted { ($0.entry.unixMicros, $0.source, $0.entry.sequence) >
                              ($1.entry.unixMicros, $1.source, $1.entry.sequence) }
      unavailable = missing
      if mode != .capture { clearContent() }
      if !rows.contains(where: { $0.pair == pair }) { pair = "" }
      if !rows.contains(where: { $0.entry.messageType == type }) { type = "" }
    } catch {
      self.error = error.localizedDescription
    }
  }

  private func clearContent() {
    for row in rows {
      row.entry.contentJson = nil
      row.entry.contentOmission = nil
    }
    export = nil
  }
}
