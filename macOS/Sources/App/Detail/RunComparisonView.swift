import SwiftUI
import TsugumiRPC

struct ComparedRunCard {
  let summary: Tsugumi_RPC_InvocationSummaryT
  var record: Tsugumi_RPC_RecordResultT?
  var error: String?
}
struct ComparedRunRow: Identifiable {
  var id: String { card }
  let card: String
  let left: ComparedRunCard?
  let right: ComparedRunCard?
}
struct ComparedRunField: Identifiable {
  var id: String { name }
  let name: String
  let left: String?
  let right: String?
  var different = false
  var changed: Bool { different || left != right }
}

@MainActor enum RunComparison {
  static func load(_ rpc: RPCClient, left: String, right: String) async throws -> [ComparedRunRow] {
    @Sendable func side(_ run: String) async throws -> [String: ComparedRunCard] {
      let summaries = try await rpc.latestInvocations(runId: run).records.compactMap { $0 }
      var out: [String: ComparedRunCard] = [:]
      for summary in summaries {
        try Task.checkCancellation()
        var item = ComparedRunCard(summary: summary)
        do { item.record = try await rpc.record(runId: run, card: summary.card.path, invocation: summary.invocation) }
        catch { item.error = String(describing: error) }
        out[key(summary.card.path)] = item
      }
      return out
    }
    async let a = side(left)
    async let b = side(right)
    let (first, second) = try await (a, b)
    return Set(first.keys).union(second.keys).sorted().map { ComparedRunRow(card: $0, left: first[$0], right: second[$0]) }
  }

  static func paired(_ a: [String: String], _ b: [String: String]) -> [ComparedRunField] {
    Set(a.keys).union(b.keys).sorted().map { ComparedRunField(name: $0, left: a[$0], right: b[$0]) }
  }

  static func params(_ left: Tsugumi_RPC_RecordResultT?, _ right: Tsugumi_RPC_RecordResultT?) -> [ComparedRunField] {
    func values(_ r: Tsugumi_RPC_RecordResultT?) -> [String: String] {
      guard let text = r?.record?.paramsJson, let json = try? JSONValue.parse(text), let entries = json.object else { return [:] }
      return Dictionary(uniqueKeysWithValues: entries.map { ($0.0, $0.1.serialized().trimmingCharacters(in: .whitespacesAndNewlines)) })
    }
    return paired(values(left), values(right))
  }

  static func values(_ output: Bool, _ left: Tsugumi_RPC_RecordResultT?, _ right: Tsugumi_RPC_RecordResultT?) -> [ComparedRunField] {
    func values(_ r: Tsugumi_RPC_RecordResultT?) -> [String: String] {
      let list = (output ? r?.record?.outputs : r?.record?.inputs) ?? []
      return Dictionary(uniqueKeysWithValues: list.compactMap { $0 }.map { ($0.name, describeValue($0, includeArtifact: false)) })
    }
    var fields = paired(values(left), values(right))
    let a = (output ? left?.record?.outputs : left?.record?.inputs) ?? []
    let b = (output ? right?.record?.outputs : right?.record?.inputs) ?? []
    for i in fields.indices {
      if let x = a.compactMap({ $0 }).first(where: { $0.name == fields[i].name }),
         let y = b.compactMap({ $0 }).first(where: { $0.name == fields[i].name }) {
        fields[i].different = x.inlineBytes != y.inlineBytes || x.schemaBfbs != y.schemaBfbs || x.kind != y.kind
      }
    }
    return fields
  }

  static func sameDisplay(_ a: DisplaySnapshot?, _ b: DisplaySnapshot?) -> Bool {
    switch (a?.value, b?.value) {
    case (nil, nil): return true
    case let (.text(x)?, .text(y)?), let (.markdown(x)?, .markdown(y)?): return x == y
    case let (.scalar(x, xl)?, .scalar(y, yl)?): return x == y && xl == yl
    case let (.table(xc, xr)?, .table(yc, yr)?): return xc == yc && xr == yr
    case let (.image(xm, xd, xa)?, .image(ym, yd, ya)?): return xm == ym && xd == yd && xa == ya
    case let (.module(xn, xj)?, .module(yn, yj)?): return xn == yn && xj == yj
    case let (.points(xx, xy, xz, xc)?, .points(yx, yy, yz, yc)?): return xx == yx && xy == yy && xz == yz && xc == yc
    case let (.lineChart(x)?, .lineChart(y)?):
      return x.count == y.count && zip(x, y).allSatisfy { $0.0 == $1.0 && $0.1 == $1.1 && $0.2 == $1.2 && $0.3 == $1.3 }
    default: return false
    }
  }
}

struct RunComparisonView: View {
  let session: ProjectSession
  let runs: [Tsugumi_RPC_RunSummaryT]
  let onClose: () -> Void
  @State private var left: String
  @State private var right: String
  @State private var rows: [ComparedRunRow]?
  @State private var error: String?
  @State private var diff: Tsugumi_RPC_VersionDiffT?
  @State private var diffError: String?
  @State private var attempt = 0

  init(session: ProjectSession, runs: [Tsugumi_RPC_RunSummaryT], initial: String?, onClose: @escaping () -> Void) {
    self.session = session; self.runs = runs; self.onClose = onClose
    let first = initial ?? runs.first?.status.runId ?? ""
    _left = State(initialValue: first)
    _right = State(initialValue: runs.first { $0.status.runId != first }?.status.runId ?? "")
  }

  private var from: String { runs.first { $0.status.runId == left }?.status.commit ?? "" }
  private var to: String { runs.first { $0.status.runId == right }?.status.commit ?? "" }

  var body: some View {
    VStack(spacing: 0) {
      HStack { Text("実行を比較").font(.headline); Spacer(); Button("閉じる", action: onClose) }.padding(10)
      Divider()
      HStack(spacing: 12) {
        runPicker("左の実行", selection: $left)
        runPicker("右の実行", selection: $right)
      }.padding(10)
      Divider()
      ScrollView {
        VStack(alignment: .leading, spacing: 12) {
          Text("各 Card の最後の呼び出しを比較します。記録や現在の Workflow は変更しません。").foregroundStyle(.secondary)
          if left.isEmpty || right.isEmpty || left == right {
            ContentUnavailableView("異なる 2 つの実行を選んでください", systemImage: "arrow.triangle.swap")
          } else if let error {
            Text(error).foregroundStyle(.red)
            Button("再試行") { attempt += 1 }
          } else if let rows {
            versionDifference
            if rows.isEmpty { ContentUnavailableView("記録がありません", systemImage: "tray") }
            ForEach(rows) { ComparedRunCardView(row: $0) }
          } else { ProgressView() }
        }.padding(12).frame(maxWidth: .infinity, alignment: .leading)
      }
    }
    .frame(minWidth: 800, idealWidth: 1100, minHeight: 500, idealHeight: 760)
    .task(id: "\(left)|\(right)|\(attempt)") { await load() }
  }

  private func runPicker(_ label: LocalizedStringKey, selection: Binding<String>) -> some View {
    Picker(label, selection: selection) {
      ForEach(runs, id: \.status.runId) { r in
        Text("\(formatMillis(r.status.acceptedAt)) · \(r.status.state.label) · \(String((r.status.runId ?? "").prefix(8)))").tag(r.status.runId ?? "")
      }
    }.frame(maxWidth: .infinity)
  }

  private var versionDifference: some View {
    DisclosureGroup {
      if from.isEmpty || to.isEmpty { Text("版が記録されていないため差分を表示できません。") }
      else if from == to { Text("同じ版です") }
      else if let diffError { Text(diffError).foregroundStyle(.red) }
      else if let diff {
        Text(diff.text.isEmpty ? String(localized: "変更はありません") : diff.text).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
        if diff.truncated { Text("差分が大きいため一部を表示しています。").foregroundStyle(.secondary) }
      } else { ProgressView() }
    } label: { Text("コードと設定の差分"); Text("\(from.prefix(8)) → \(to.prefix(8))").foregroundStyle(.secondary) }
  }

  private func load() async {
    rows = nil; error = nil; diff = nil; diffError = nil
    guard let rpc = session.rpc else { error = String(localized: "実行環境に接続していません"); return }
    guard !left.isEmpty, !right.isEmpty, left != right else { return }
    do {
      let result = try await RunComparison.load(rpc, left: left, right: right)
      guard !Task.isCancelled else { return }
      rows = result
    } catch {
      guard !Task.isCancelled else { return }
      self.error = String(describing: error)
    }
    if !from.isEmpty, !to.isEmpty, from != to {
      do {
        let result = try await rpc.versionDiff(commit: to, fromCommit: from)
        guard !Task.isCancelled else { return }
        diff = result
      } catch {
        guard !Task.isCancelled else { return }
        diffError = String(describing: error)
      }
    }
  }
}

private struct ComparedRunCardView: View {
  let row: ComparedRunRow
  @State private var expanded = true
  private var a: Tsugumi_RPC_RecordResultT? { row.left?.record }
  private var b: Tsugumi_RPC_RecordResultT? { row.right?.record }
  private var leftDisplay: DisplaySnapshot? { a?.record?.display.flatMap(recordedDisplay) }
  private var rightDisplay: DisplaySnapshot? { b?.record?.display.flatMap(recordedDisplay) }
  private let changed = Color.accentColor.opacity(0.09)

  var body: some View {
    GroupBox {
      DisclosureGroup(isExpanded: $expanded) {
        VStack(alignment: .leading, spacing: 10) {
          HStack(alignment: .top, spacing: 12) {
            status(row.left).frame(maxWidth: .infinity, alignment: .leading)
            status(row.right).frame(maxWidth: .infinity, alignment: .leading)
          }.padding(6).background(row.left?.summary.state != row.right?.summary.state || row.left?.summary.durationMs != row.right?.summary.durationMs ? changed : .clear)
          fields("パラメータ", RunComparison.params(a, b))
          fields("入力", RunComparison.values(false, a, b))
          fields("出力", RunComparison.values(true, a, b))
          if leftDisplay != nil || rightDisplay != nil {
            HStack { Text("表示").bold(); if !RunComparison.sameDisplay(leftDisplay, rightDisplay) { Text("相違あり").font(.caption) } }
            HStack(alignment: .top, spacing: 12) {
              retainedDisplay(leftDisplay)
              retainedDisplay(rightDisplay)
            }.background(RunComparison.sameDisplay(leftDisplay, rightDisplay) ? .clear : changed)
          }
        }.padding(.top, 8)
      } label: { Text(row.card).font(.headline) }
    }
  }

  @ViewBuilder private func retainedDisplay(_ display: DisplaySnapshot?) -> some View {
    Group {
      if let display { DisplayContent(display: display, large: true) }
      else { Text("表示の記録がありません").foregroundStyle(.secondary) }
    }.frame(maxWidth: .infinity).frame(height: displayHeight(display)).clipped()
  }

  private func displayHeight(_ display: DisplaySnapshot?) -> CGFloat {
    switch display?.value {
    case .table(_, let rows)?: return min(400, 50 + CGFloat(rows.prefix(100).reduce(1) { $0 + max(1, $1.map { $0.components(separatedBy: "\n").count }.max() ?? 1) }) * 20)
    case .text(let text)?, .markdown(let text)?:
      let lines = text.components(separatedBy: "\n").reduce(0) { $0 + max(1, Int(ceil(Double($1.count) / 60))) }
      return min(400, max(60, CGFloat(lines) * 22 + 16))
    case .scalar?: return 120
    case nil: return 60
    default: return 400
    }
  }

  @ViewBuilder private func status(_ side: ComparedRunCard?) -> some View {
    if let side {
      let v = side.record?.record?.summary ?? side.summary
      VStack(alignment: .leading, spacing: 4) {
        Text("\(v.state.label) · #\(v.invocation ?? 0) · \(v.durationMs.map { formatDuration(Double($0) / 1000) } ?? "—")").bold()
        if let aggregate = side.record?.aggregate { Text("呼び出し \(aggregate.calls ?? 0) · 成功 \(aggregate.succeeded ?? 0) · 失敗 \(aggregate.failed ?? 0)").font(.caption) }
        if let error = side.error ?? v.failure?.diagnostic { Text(error).foregroundStyle(.red) }
        if side.record?.record?.recordIo == false { Text("入出力は記録されていません").foregroundStyle(.secondary) }
        if side.record?.truncated == true { Text("大きな値は概要のみ表示しています。").foregroundStyle(.secondary) }
        if side.record != nil, side.record?.record?.paramsJson == nil { Text("パラメータの記録がありません").foregroundStyle(.secondary) }
      }
    } else { Text("この実行にはありません").foregroundStyle(.secondary) }
  }

  private func fields(_ title: LocalizedStringKey, _ values: [ComparedRunField]) -> some View {
    VStack(alignment: .leading, spacing: 5) {
      Text(title).bold()
      if values.isEmpty { Text("なし").foregroundStyle(.secondary) }
      ForEach(values) { field in
        VStack(alignment: .leading, spacing: 4) {
          HStack { Text(field.name).font(.callout.bold()); if field.changed { Text("相違あり").font(.caption) } }
          HStack(alignment: .top, spacing: 12) {
            value(field.left).frame(maxWidth: .infinity, alignment: .leading)
            value(field.right).frame(maxWidth: .infinity, alignment: .leading)
          }
        }.padding(6).background(field.changed ? changed : .clear)
      }
    }
  }

  private func value(_ text: String?) -> some View {
    Text(text ?? String(localized: "この実行にはありません")).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
      .frame(maxWidth: .infinity, alignment: .leading)
  }
}
