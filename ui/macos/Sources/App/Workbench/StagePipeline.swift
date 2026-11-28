import SwiftUI
import TsugumiRPC

/// The log view's pipeline line: the invocation's stages as the Card's face
/// draws them (the same circles and colours), side by side on a thin line,
/// each with its name and short time, and under them what the running stage
/// does now in plain words.
struct StagePipeline: Equatable {
  struct Step: Equatable, Identifiable {
    var stage: Stage
    var status: StageProgress.Status
    /// Running, but waiting (for a free worker slot or its Nix store).
    var waiting = false
    var seconds: Double?
    /// The core records no build of its own: the worker builds the Card as
    /// part of its execution, and this circle stands for that.
    var onWorker = false
    var id: Int { stage.rawValue }
  }

  var steps: [Step]
  /// What the running stage does now, e.g. "ttl で Nix ビルド中（hello-1.0）· CPU 5:41 · 最後の出力 1 分 25 秒前".
  var detail: String?

  /// - Parameters:
  ///   - stages: the stages shown, in order.
  ///   - records: what the core recorded for the invocation.
  ///   - service: the execution service ("local", a worker, or nil).
  ///   - live: the invocation's live state while the run goes on.
  ///   - now: the core's clock (ms since 1970).
  init(stages shown: [Stage], records: [StageRecord], service: String?, live: CardRunState?, now: Int64) {
    func record(_ st: Stage) -> StageRecord? { records.last { $0.stage == st } }
    func seconds(_ r: StageRecord?) -> Double? {
      guard let r, let start = r.start else { return nil }
      if let end = r.end { return Double(end - start) / 1000 }
      return r.outcome == .running ? Double(max(0, now - start)) / 1000 : nil
    }
    let activity = live.flatMap(ExecutionActivityText.init)
    var steps = shown.map { st in
      let r = record(st)
      return Step(stage: st, status: Self.status(r?.outcome), seconds: seconds(r))
    }
    func update(_ st: Stage, _ change: (inout Step) -> Void) {
      if let i = steps.firstIndex(where: { $0.stage == st }) { change(&steps[i]) }
    }
    let remote = service.map { !$0.isEmpty && $0 != "local" } ?? false
    let execute = record(.execute)
    if remote, shown.contains(.build), record(.build) == nil, let execute {
      // The worker builds the Card's derivation before it runs it: shown on
      // the build circle, not as an execution that has started.
      update(.build) { $0.onWorker = true }
      switch execute.outcome {
      case .running:
        if let activity, !activity.queued {
          update(.build) {
            $0.status = .running
            $0.waiting = activity.waiting
            $0.seconds = seconds(execute)
          }
          update(.execute) {
            $0.status = .pending
            $0.seconds = nil
          }
        } else if live?.executionActivity == .running {
          update(.build) { $0.status = .done }
        }
      case .done:
        update(.build) { $0.status = .done }
      default:
        break
      }
    }
    if activity?.queued == true { update(.execute) { $0.waiting = $0.status == .running } }
    self.steps = steps
    detail = Self.detail(steps.first { $0.status == .running }, service: remote ? service : nil,
                         activity: activity, live: live, now: now)
  }

  static func status(_ outcome: Tsugumi_Data_StageOutcome?) -> StageProgress.Status {
    switch outcome {
    case .running?: return .running
    case .failed?: return .failed
    case .cancelled?: return .cancelled
    case .cached?: return .cached
    case .reused?: return .reused
    case nil, .unspecified?: return .pending
    default: return .done
    }
  }

  private static func detail(_ current: Step?, service: String?, activity: ExecutionActivityText?,
                             live: CardRunState?, now: Int64) -> String? {
    guard let current, let live else { return nil }
    guard current.onWorker || current.stage == .execute else { return current.stage.activeLabel }
    let worker = service ?? String(localized: "ワーカー")
    var parts: [String] = []
    if let activity {
      let what = activity.detail
      if activity.queued { parts.append(String(localized: "\(worker) の空きを待っています")) } else {
        switch activity.activity {
        case .waitingStore: parts.append(String(localized: "\(worker) の Nix ストアを待っています（別の実行が使用中）"))
        case .preparing: parts.append(String(localized: "\(worker) でビルドの準備中"))
        case .building:
          parts.append(what.isEmpty ? String(localized: "\(worker) で Nix ビルド中") : String(localized: "\(worker) で Nix ビルド中（\(what)）"))
        case .substituting:
          parts.append(what.isEmpty ? String(localized: "\(worker) でビルド済みのものを取得中") : String(localized: "\(worker) でビルド済みのものを取得中（\(what)）"))
        default: break
        }
      }
    } else {
      parts.append(service.map { String(localized: "\($0) で実行中") } ?? String(localized: "実行中"))
    }
    if let cpu = live.cpuMs { parts.append(String(localized: "CPU \(formatShortDuration(Double(cpu) / 1000))")) }
    if let at = live.lastOutputAt {
      parts.append(String(localized: "最後の出力 \(formatAgo(Double(max(0, now - at)) / 1000))"))
    } else if current.stage == .execute && activity == nil {
      parts.append(String(localized: "出力はまだありません"))
    }
    return parts.joined(separator: " · ")
  }
}

/// How long ago, in words: "12 秒前", "1 分 25 秒前", "2 時間 5 分前".
func formatAgo(_ seconds: Double) -> String {
  let s = Int(max(0, seconds).rounded(.down))
  if s < 60 { return String(localized: "\(s) 秒前") }
  if s < 3600 { return String(localized: "\(s / 60) 分 \(s % 60) 秒前") }
  return String(localized: "\(s / 3600) 時間 \(s / 60 % 60) 分前")
}

/// The pipeline line as drawn: circles joined by a thin line, a click on a
/// stage opens its section.
struct StagePipelineView: View {
  let line: StagePipeline
  var onTap: (Stage) -> Void = { _ in }

  static let dot: CGFloat = 10

  var body: some View {
    VStack(alignment: .leading, spacing: 3) {
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 0) {
          ForEach(Array(line.steps.enumerated()), id: \.element.id) { index, step in
            if index > 0 {
              Rectangle().fill(.secondary.opacity(0.35)).frame(width: 16, height: 1).padding(.horizontal, 5)
            }
            stepView(step)
          }
        }
        .font(.caption).lineLimit(1)
      }
      .fixedSize(horizontal: false, vertical: true)
      if let detail = line.detail {
        Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
          .monospacedDigit().textSelection(.enabled)
      }
    }
    .padding(.horizontal, 8).padding(.vertical, 6)
  }

  private func stepView(_ step: StagePipeline.Step) -> some View {
    let current = step.status == .running
    let reached = step.status != .pending && step.status != .notRun
    let label = SectionStatus(step.status, waiting: step.waiting).label
    return Button { onTap(step.stage) } label: {
      HStack(spacing: 4) {
        StageDot(status: step.status, size: Self.dot, waiting: step.waiting)
        Text(StageLogView.stageLabel(step.stage))
          .fontWeight(current ? .semibold : .regular)
          .foregroundStyle(reached ? Color.primary : Color.secondary)
        if step.onWorker { Text("ワーカー").foregroundStyle(.secondary) }
        if let s = step.seconds { Text(formatShortDuration(s)).foregroundStyle(.secondary).monospacedDigit() }
      }
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .help(StageLogView.stageLabel(step.stage) + "：" + label
          + (step.onWorker ? String(localized: "（ワーカーが実行の一部としてビルドします）") : ""))
    .accessibilityLabel(Text(StageLogView.stageLabel(step.stage) + "：" + label))
  }
}
