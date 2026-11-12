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
/// `unknown`: the records do not hold enough to tell, so it is never shown as unchanged.
enum ComparedDifference: Equatable { case same, changed, unknown }

struct ComparedRunField: Identifiable {
  var id: String { name }
  let name: String
  let left: String?
  let right: String?
  var difference: ComparedDifference
  var changed: Bool { difference != .same }
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

  private static func names<V>(_ a: [String: V], _ b: [String: V]) -> [String] { Set(a.keys).union(b.keys).sorted() }

  /// Said for a side whose Card ran but whose invocation record did not arrive:
  /// it is neither "no inputs" nor "removed".
  static var unreadable: String { String(localized: "呼び出しの記録を読み込めませんでした") }

  // MARK: Parameters

  /// A side that has the Card but no recorded parameters (or no readable record) cannot be compared.
  static func paramsUnavailable(_ c: ComparedRunCard?) -> Bool {
    guard let c else { return false }
    return (c.record?.record?.paramsJson ?? "").isEmpty
  }

  /// Parameters pair by name and compare as serialized JSON.
  static func params(_ left: ComparedRunCard?, _ right: ComparedRunCard?) -> [ComparedRunField] {
    func values(_ c: ComparedRunCard?) -> [String: String] {
      guard let text = c?.record?.record?.paramsJson, let json = try? JSONValue.parse(text), let entries = json.object else { return [:] }
      return Dictionary(entries.map { ($0.0, $0.1.serialized().trimmingCharacters(in: .whitespacesAndNewlines)) }, uniquingKeysWith: { x, _ in x })
    }
    let a = values(left), b = values(right), unknown = paramsUnavailable(left) || paramsUnavailable(right)
    return names(a, b).map {
      ComparedRunField(name: $0, left: a[$0], right: b[$0], difference: unknown ? .unknown : a[$0] == b[$0] ? .same : .changed)
    }
  }

  // MARK: Inputs and outputs

  /// The reply leaves a large or truncated value's bytes out. An empty payload counts as
  /// the value only when the record says it is empty.
  static func omitted(_ v: Tsugumi_Data_NamedValueT) -> Bool { v.inlineBytes.isEmpty && v.sizeBytes != 0 }

  /// Compares the type and content digest, or the payload when both are available.
  static func valueDifference(_ a: Tsugumi_Data_NamedValueT?, _ b: Tsugumi_Data_NamedValueT?) -> ComparedDifference {
    guard let a, let b else { return a == nil && b == nil ? .same : .changed }
    if a.kind != b.kind || a.rootType != b.rootType || a.schemaBfbs != b.schemaBfbs { return .changed }
    if let x = a.contentHash, !x.isEmpty, let y = b.contentHash, !y.isEmpty { return x == y ? .same : .changed }
    if omitted(a) || omitted(b) { return .unknown }
    return a.inlineBytes == b.inlineBytes ? .same : .changed
  }

  /// Why a side's values are not known (nil when they are, or the Card is absent there).
  private static func valuesMissing(_ c: ComparedRunCard?) -> String? {
    guard let c else { return nil }
    guard let r = c.record?.record else { return unreadable }
    return r.recordIo == false ? String(localized: "入出力は記録されていません") : nil
  }

  static func valuesUnavailable(_ left: ComparedRunCard?, _ right: ComparedRunCard?) -> Bool {
    valuesMissing(left) != nil || valuesMissing(right) != nil
  }

  static func values(_ output: Bool, _ left: ComparedRunCard?, _ right: ComparedRunCard?) -> [ComparedRunField] {
    func values(_ c: ComparedRunCard?) -> [String: Tsugumi_Data_NamedValueT] {
      let list = (output ? c?.record?.record?.outputs : c?.record?.record?.inputs) ?? []
      return Dictionary(list.compactMap { $0 }.map { ($0.name, $0) }, uniquingKeysWith: { x, _ in x })
    }
    let a = values(left), b = values(right), ma = valuesMissing(left), mb = valuesMissing(right)
    return names(a, b).map { name in
      let x = a[name], y = b[name]
      // A value absent from a side whose values are unknown is not a removed value.
      let difference: ComparedDifference = (x == nil && ma != nil) || (y == nil && mb != nil) ? .unknown : valueDifference(x, y)
      return ComparedRunField(name: name, left: x.map { describeValue($0, includeArtifact: false) } ?? ma,
                              right: y.map { describeValue($0, includeArtifact: false) } ?? mb, difference: difference)
    }
  }

  // MARK: Execution conditions

  /// Where each input came from: a Card of the same run and its invocation, or the
  /// saved input for a recorded input that no Card of the run sent.
  private static func origins(_ r: Tsugumi_RPC_InvocationRecordT) -> [String: String] {
    var out = Dictionary(r.inputs.compactMap { $0 }.map { ($0.name, String(localized: "保存した入力")) }, uniquingKeysWith: { x, _ in x })
    for s in r.inputSources.compactMap({ $0 }) { out[s.port] = "\(s.card.path.joined(separator: "/")) #\(s.invocation ?? 0)" }
    return out
  }

  /// What a Card ran under besides its parameters and code: the recorded Nix
  /// environment, the system it ran as, and where each input came from.
  static func conditions(_ left: ComparedRunCard?, _ right: ComparedRunCard?) -> [ComparedRunField] {
    func summary(_ c: ComparedRunCard?) -> Tsugumi_RPC_InvocationSummaryT? { c.map { $0.record?.record?.summary ?? $0.summary } }
    let a = summary(left), b = summary(right)
    func field(_ name: String, _ x: String?, _ y: String?, known: Bool) -> ComparedRunField {
      ComparedRunField(name: name, left: x, right: y, difference: a == nil || b == nil ? .changed : !known ? .unknown : x == y ? .same : .changed)
    }
    func nonEmpty(_ s: String?) -> String? { s.flatMap { $0.isEmpty ? nil : $0 } }
    func environment(_ v: Tsugumi_RPC_InvocationSummaryT?) -> String? {
      v.map { nonEmpty($0.environmentId) ?? String(localized: "記録なし") }
    }
    func service(_ v: Tsugumi_RPC_InvocationSummaryT?) -> String? {
      nonEmpty(v?.executionService).map { $0 == "local" ? String(localized: "プロジェクトの実行環境") : $0 }
    }
    func method(_ v: Tsugumi_RPC_InvocationSummaryT?) -> String? {
      if let e = nonEmpty(v?.emulator) { return String(localized: "エミュレーション: \(e)") }
      return service(v).map { _ in String(localized: "直接実行") }
    }
    let recordedEnvironment = { (v: Tsugumi_RPC_InvocationSummaryT?) in !(v?.environmentId ?? "").isEmpty }
    var fields = [
      // An environment missing from a record (a Card that stopped before its build, say) is not comparable.
      field(String(localized: "環境"), environment(a), environment(b), known: recordedEnvironment(a) && recordedEnvironment(b)),
      field(String(localized: "実行方式"), method(a), method(b), known: method(a) != nil && method(b) != nil),
      field(String(localized: "実行サービス"), service(a), service(b), known: service(a) != nil && service(b) != nil),
    ]

    // nil: the Card is absent from that run. `missing`: why that side's origins are not known.
    func side(_ c: ComparedRunCard?) -> (origins: [String: String], missing: String?) {
      guard let c else { return ([:], nil) }
      guard let r = c.record?.record else { return ([:], unreadable) }
      return (origins(r), r.recordIo == false ? String(localized: "入出力は記録されていません") : nil)
    }
    let x = side(left), y = side(right)
    // A port known on one side only reached the other side from no Card of its run.
    func text(_ s: (origins: [String: String], missing: String?), _ c: ComparedRunCard?, _ port: String) -> String? {
      guard c != nil else { return nil }
      return s.origins[port] ?? s.missing ?? String(localized: "この実行の Card からは届いていません")
    }
    let ports = names(x.origins, y.origins)
    for port in ports {
      let known = (x.origins[port] != nil || x.missing == nil) && (y.origins[port] != nil || y.missing == nil)
      fields.append(field(String(localized: "入力元 \(port)"), text(x, left, port), text(y, right, port), known: known))
    }
    // Neither side named a port, but one side's record is missing: that is not "no inputs".
    if ports.isEmpty, x.missing != nil || y.missing != nil {
      func none(_ s: (origins: [String: String], missing: String?), _ c: ComparedRunCard?) -> String? {
        c == nil ? nil : s.missing ?? String(localized: "なし")
      }
      fields.append(field(String(localized: "入力元"), none(x, left), none(y, right), known: false))
    }
    return fields
  }

  // MARK: Run scope

  /// A run starts at one Card and either goes on downstream or runs that Card alone
  /// with its saved inputs (`propagate` is true unless recorded false).
  static func runScope(_ run: Tsugumi_RPC_RunSummaryT?) -> String? {
    guard let run else { return nil }
    let target = run.target.path.isEmpty ? "—" : run.target.path.joined(separator: "/")
    return run.propagate == false ? String(localized: "\(target) だけ（保存した入力）") : String(localized: "\(target) から下流")
  }

  static func scope(_ left: Tsugumi_RPC_RunSummaryT?, _ right: Tsugumi_RPC_RunSummaryT?) -> ComparedRunField {
    let a = runScope(left), b = runScope(right)
    return ComparedRunField(name: String(localized: "開始した Card"), left: a, right: b,
                            difference: a == nil || b == nil ? .unknown : a == b ? .same : .changed)
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

  private func run(_ id: String) -> Tsugumi_RPC_RunSummaryT? { runs.first { $0.status.runId == id } }
  private var from: String { run(left)?.status.commit ?? "" }
  private var to: String { run(right)?.status.commit ?? "" }

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
            // Where each run started and how far it went: a partial run reuses saved inputs.
            ComparedFieldsView(title: "実行の範囲", values: [RunComparison.scope(run(left), run(right))])
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
          ComparedFieldsView(title: "実行条件", values: RunComparison.conditions(row.left, row.right))
          ComparedFieldsView(title: "パラメータ", values: RunComparison.params(row.left, row.right),
                             unavailable: RunComparison.paramsUnavailable(row.left) || RunComparison.paramsUnavailable(row.right))
          ComparedFieldsView(title: "入力", values: RunComparison.values(false, row.left, row.right),
                             unavailable: RunComparison.valuesUnavailable(row.left, row.right))
          ComparedFieldsView(title: "出力", values: RunComparison.values(true, row.left, row.right),
                             unavailable: RunComparison.valuesUnavailable(row.left, row.right))
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
        if side.record != nil, (side.record?.record?.paramsJson ?? "").isEmpty { Text("パラメータの記録がありません").foregroundStyle(.secondary) }
      }
    } else { Text("この実行にはありません").foregroundStyle(.secondary) }
  }
}

/// Named values side by side. `unknown` is said in words: an unmarked field would read as unchanged.
private struct ComparedFieldsView: View {
  let title: LocalizedStringKey
  let values: [ComparedRunField]
  /// With no fields, a side whose record cannot tell makes the section "not comparable", not "none".
  var unavailable = false
  private let changed = Color.accentColor.opacity(0.09)
  private let unknown = Color.orange.opacity(0.08)

  var body: some View {
    VStack(alignment: .leading, spacing: 5) {
      Text(title).bold()
      if values.isEmpty, unavailable { Text("比較できません").foregroundStyle(.secondary) }
      else if values.isEmpty { Text("なし").foregroundStyle(.secondary) }
      ForEach(values) { field in
        VStack(alignment: .leading, spacing: 4) {
          HStack {
            Text(field.name).font(.callout.bold())
            switch field.difference {
            case .same: EmptyView()
            case .changed: Text("相違あり").font(.caption)
            case .unknown: Text("比較できません").font(.caption).foregroundStyle(.secondary)
            }
          }
          HStack(alignment: .top, spacing: 12) {
            value(field.left).frame(maxWidth: .infinity, alignment: .leading)
            value(field.right).frame(maxWidth: .infinity, alignment: .leading)
          }
        }.padding(6).background(field.difference == .changed ? changed : field.difference == .unknown ? unknown : .clear)
      }
    }
  }

  private func value(_ text: String?) -> some View {
    Text(text ?? String(localized: "この実行にはありません")).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
      .frame(maxWidth: .infinity, alignment: .leading)
  }
}
