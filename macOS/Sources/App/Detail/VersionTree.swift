import SwiftUI
import TsugumiRPC

extension Tsugumi_RPC_VersionGraphT {
  var commitList: [Tsugumi_RPC_VersionCommitT] { commits.compactMap { $0 } }
}

extension Tsugumi_RPC_VersionCommitT {
  var parentIds: [String] { parents.compactMap { $0 } }
  var runList: [Tsugumi_RPC_VersionRunT] { runs.compactMap { $0 } }
  var tagList: [String] { tags.compactMap { $0 } }
  var branchList: [String] { branches.compactMap { $0 } }
  var changedList: [String] { changed.compactMap { $0 } }
}

/// Lanes of a version graph, newest first: each commit's column and, for each
/// parent, the column its line runs down before it turns into the parent.
struct VersionLayout {
  struct Edge {
    let lane: Int       // the column the line runs down
    let toRow: Int
    let toLane: Int
  }
  struct Row {
    let lane: Int
    var edges: [Edge] = []
  }
  private(set) var rows: [Row] = []
  private(set) var lanes = 1

  init(_ commits: [(id: String, parents: [String])]) {
    var waiting: [String?] = []  // the commit each column waits for
    var rowOf: [String: Int] = [:]
    for (i, c) in commits.enumerated() { rowOf[c.id] = i }
    var laneOf: [String: Int] = [:]
    var pending: [(row: Int, lane: Int, parent: String)] = []
    func free() -> Int {
      if let f = waiting.firstIndex(where: { $0 == nil }) { return f }
      waiting.append(nil)
      return waiting.count - 1
    }
    for (i, c) in commits.enumerated() {
      let lane = waiting.firstIndex(where: { $0 == c.id }) ?? free()
      for j in waiting.indices where j != lane && waiting[j] == c.id { waiting[j] = nil }
      laneOf[c.id] = lane
      rows.append(Row(lane: lane))
      waiting[lane] = nil
      for (k, p) in c.parents.enumerated() where rowOf[p] != nil {
        let l: Int
        if k == 0 { l = lane } else if let w = waiting.firstIndex(where: { $0 == p }) { l = w } else { l = free() }
        waiting[l] = p
        pending.append((i, l, p))
      }
      lanes = max(lanes, waiting.count)
    }
    for e in pending {
      guard let r = rowOf[e.parent], let pl = laneOf[e.parent] else { continue }
      rows[e.row].edges.append(Edge(lane: e.lane, toRow: r, toLane: pl))
    }
  }
}

/// Where a version's Cards go in a minimap at most width x height, each with
/// the outline it has on the canvas: the whole graph fitted and centered
/// across, as web/src/ui/versions.tsx does.
struct VersionMiniMapLayout {
  struct Card {
    var id: String
    var x: Double
    var y: Double
    var kind = CardKind.code
    var fan = 0
    var collapsed = false

    /// A Card as far as a version says: its shape and the size the canvas gives
    /// it with one port on each side and no parameters, never more than it
    /// really takes, so what does not overlap on the canvas does not overlap here.
    var data: CardNodeData {
      func ports(_ n: Int) -> [PortData] { (0..<n).map { PortData(name: "p\($0)") } }
      var d = CardNodeData(title: id, kind: kind, summary: nil,
                           inputs: ports(kind == .merge ? fan : 1), outputs: ports(kind == .fork ? fan : 1))
      d.collapsed = collapsed
      return d
    }
  }
  struct Placed {
    let box: CGRect
    let outline: Path
    let shape: CardShape
    let draft: Bool
  }
  static let padding = 8.0
  var cards: [String: Placed] = [:]
  var height = 0.0
  var named = false

  init(_ cards: [Card], flow: FlowDirection, width: Double, height maxHeight: Double) {
    let placed = cards.map { (card: $0, g: CardGeometry($0.data, flow: flow)) }
    guard let left = cards.map(\.x).min(), let top = cards.map(\.y).min(),
          let right = placed.map({ $0.card.x + $0.g.size.width }).max(),
          let bottom = placed.map({ $0.card.y + $0.g.size.height }).max() else { return }
    let spanX = right - left, spanY = bottom - top
    let k = min((width - Self.padding * 2) / spanX, (maxHeight - Self.padding * 2) / spanY, 0.3)
    let ox = (width - spanX * k) / 2
    for (c, g) in placed {
      let x = ox + (c.x - left) * k, y = Self.padding + (c.y - top) * k
      let o = g.outlineRect
      let outline = g.outline(in: CGRect(x: x + o.minX * k, y: y + o.minY * k, width: o.width * k, height: o.height * k), cornerScale: k)
      self.cards[c.id] = Placed(box: CGRect(x: x, y: y, width: g.size.width * k, height: g.size.height * k), outline: outline, shape: g.shape, draft: c.kind == .draft)
    }
    height = spanY * k + Self.padding * 2
    named = CardGeometry.width * k >= 34
  }
}

/// A version's Cards and how they connect; those it changed stand out.
struct VersionMiniMap: View {
  let canvas: Tsugumi_RPC_VersionCanvasT
  let changed: [String]
  static let width = 284.0
  static let maxHeight = 132.0

  var body: some View {
    let cards = canvas.cards.compactMap { $0 }
    let map = VersionMiniMapLayout(
      cards.map { .init(id: $0.id, x: $0.x, y: $0.y, kind: CardKind(rawValue: $0.kind) ?? .code, fan: Int($0.fan), collapsed: $0.collapsed) },
      flow: canvas.flow == "topToBottom" ? .topToBottom : .leftToRight, width: Self.width, height: Self.maxHeight)
    let touched = Set(changed.map { String($0.split(separator: "/").first ?? "") })
    Canvas { g, _ in
      for w in canvas.wires.compactMap({ $0 }) {
        guard let a = map.cards[w.from]?.box, let b = map.cards[w.to]?.box else { continue }
        var p = Path()
        p.move(to: CGPoint(x: a.midX, y: a.midY))
        p.addLine(to: CGPoint(x: b.midX, y: b.midY))
        g.stroke(p, with: .color(.secondary), lineWidth: 1)
      }
      for card in cards {
        let id = card.id
        guard let c = map.cards[id] else { continue }
        let r = c.box
        g.fill(c.outline, with: .color(Color(nsColor: .windowBackgroundColor)))
        g.fill(c.outline, with: .color(touched.contains(id) ? Color.accentColor.opacity(0.45) : Color.secondary.opacity(0.22)))
        g.stroke(c.outline, with: .color(touched.contains(id) ? Color.accentColor : .secondary),
                 style: StrokeStyle(lineWidth: touched.contains(id) ? 1.25 : 0.75, dash: c.draft ? [3, 2] : []))
        if map.named && (c.shape == .card || c.shape == .start) {
          let fit = max(1, Int(r.width / 5) - 1)
          g.draw(Text(id.count * 5 > Int(r.width) ? id.prefix(fit) + "…" : id).font(.system(size: 8.5)), at: CGPoint(x: r.midX, y: r.midY))
        }
      }
    }
    .frame(width: Self.width, height: map.height)
  }
}

/// The project's versions as a tree: each commit with its branches, tags,
/// runs (from its note) and changed Cards; hovering one shows what it changed
/// and a minimap of its Cards. A version can be named, and the project can
/// start again from any of them (a change there opens a branch).
struct VersionTreeView: View {
  let session: ProjectSession
  @State private var selected: String?
  @State private var error: String?
  @State private var naming = false
  @State private var name = ""
  @State private var message = ""
  @State private var confirmStart: String?
  @HostStorage("versions.noteMode") private var noteMode = "mechanical"
  // The version under the pointer, where the pointer came in, and the one
  // whose panel shows (a moment later).
  @State private var pointed: String?
  @State private var pointerY: CGFloat = 0
  @State private var hovered: String?

  private static let treeSpace = "versionTree"
  private static let hoverHeight: CGFloat = 190

  static let rowHeight: CGFloat = 58
  static let laneWidth: CGFloat = 18

  var body: some View {
    let g = session.versionGraph
    VStack(spacing: 0) {
      header(g)
      Divider()
      if let g, g.enabled {
        HSplitView {
          tree(g)
            .coordinateSpace(name: Self.treeSpace)
            .overlay { GeometryReader { hoverPanel(g, in: $0.size) }.allowsHitTesting(false) }
            .frame(minWidth: 420)
          detail(g).frame(minWidth: 260, idealWidth: 320)
        }
      } else if g != nil {
        ContentUnavailableView {
          Label("版管理されていません", systemImage: "point.3.connected.trianglepath.dotted")
        } description: {
          Text("このプロジェクトを git のリポジトリにすると、実行するたびにその版が残り、いつでも前の版から始め直せます。")
        } actions: {
          Button("版管理を始める") { act { try await session.makeRepository() } }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
      }
    }
    .task { await session.refreshVersions() }
    .onChange(of: session.rev) { Task { await session.refreshVersions() } }
    .onChange(of: session.lastFinished) {
      // The run's note is written as the run ends: read it a moment later.
      Task {
        try? await Task.sleep(for: .milliseconds(600))
        await session.refreshVersions()
      }
    }
    .alert("版を操作できませんでした", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
      Button("OK") { error = nil }
    } message: { Text(error ?? "") }
    .alert("この版から始めますか？", isPresented: Binding(get: { confirmStart != nil }, set: { if !$0 { confirmStart = nil } })) {
      Button("この版から始める") {
        if let c = confirmStart { act { try await session.startFrom(c) } }
        confirmStart = nil
      }
      Button("キャンセル", role: .cancel) { confirmStart = nil }
    } message: {
      Text("今の定義と追加パスは版として残ってから、この版の内容に戻ります。ここから変更して実行すると、新しい枝になります。")
    }
    .onChange(of: noteMode) {
      session.versionNotes.removeAll()
      session.versionNoteRequests.removeAll()
      session.versionNoteBusy.removeAll()
      session.versionNoteErrors.removeAll()
      Task {
        for commit in Set([selected, pointed].compactMap { $0 }) { await session.loadVersionNote(commit) }
      }
    }
    .task(id: selected) {
      if let selected { await session.loadVersionNote(selected) }
    }
    .task(id: pointed) {
      hovered = nil
      guard let commit = pointed else { return }
      try? await Task.sleep(for: .milliseconds(250))
      guard !Task.isCancelled, pointed == commit else { return }
      hovered = commit
      await session.loadVersionCanvas(commit)
      await session.loadVersionNote(commit)
    }
    .onReceive(NotificationCenter.default.publisher(for: .versionHover)) { n in
      guard let row = n.object as? Int, let commits = session.versionGraph?.commitList, commits.indices.contains(row) else { return }
      pointed = commits[row].id
      pointerY = CGFloat(row) * Self.rowHeight
    }
    .sheet(isPresented: $naming) { nameSheet }
  }

  private func act(_ f: @escaping () async throws -> Void) {
    Task {
      do { try await f() } catch { self.error = error.localizedDescription }
    }
  }

  private func header(_ g: Tsugumi_RPC_VersionGraphT?) -> some View {
    HStack(spacing: 10) {
      Image(systemName: "point.3.connected.trianglepath.dotted").foregroundStyle(.secondary)
      if let g, g.enabled {
        Text(g.branch.map { String(localized: "枝: \($0)") } ?? String(localized: "枝の外（次の変更で新しい枝）"))
          .font(.callout)
        if g.dirty || session.dirty {
          Label("最後の版から変更あり（次の実行で版になります）", systemImage: "pencil.circle")
            .font(.caption).foregroundStyle(.orange)
        }
      }
      Spacer()
      VStack(alignment: .leading, spacing: 3) {
        Picker("版のノート", selection: $noteMode) {
          Text("変更の記録").tag("mechanical")
          Text("Agent の要約").tag("agent")
        }
        .fixedSize()
        Text("変更の記録は Card の追加・削除と定義・ソース・パラメータ・接続の変更を無料で記録します。Agent の要約は差分をモデルのプロバイダへ送り、token を使います。この設定は全プロジェクトに適用します。")
          .font(.caption).foregroundStyle(.secondary)
        if let error = CorePreferences.shared.error { Text(error).font(.caption).foregroundStyle(.red) }
      }
      Button("版として残す…") {
        name = ""
        message = ""
        naming = true
      }
      .disabled(!(g?.enabled ?? false) || !session.connected)
      Button { Task { await session.refreshVersions() } } label: { Image(systemName: "arrow.clockwise") }
        .help("読み直す")
    }
    .padding(.horizontal, 12).padding(.vertical, 8)
  }

  private func tree(_ g: Tsugumi_RPC_VersionGraphT) -> some View {
    let commits = g.commitList
    let layout = VersionLayout(commits.map { (id: $0.id, parents: $0.parentIds) })
    let graphWidth = CGFloat(layout.lanes) * Self.laneWidth + 12
    return ScrollView {
      // A row runs under its node, so the hover and the click work on either.
      ZStack(alignment: .topLeading) {
        LazyVStack(spacing: 0) {
          ForEach(Array(commits.enumerated()), id: \.element.id) { _, c in
            row(c, head: c.id == g.head)
              .padding(.leading, graphWidth)
              .frame(height: Self.rowHeight)
              .contentShape(Rectangle())
              .background(selected == c.id ? Color.accentColor.opacity(0.14) : .clear)
              .onTapGesture { selected = c.id }
              .onContinuousHover(coordinateSpace: .named(Self.treeSpace)) { phase in
                switch phase {
                case .active(let p):
                  if pointed != c.id {
                    pointed = c.id
                    pointerY = p.y
                  }
                case .ended:
                  if pointed == c.id { pointed = nil }
                }
              }
          }
        }
        graph(layout, commits, head: g.head)
          .frame(width: graphWidth, height: CGFloat(commits.count) * Self.rowHeight)
          .allowsHitTesting(false)
      }
    }
  }

  /// What shows while the pointer is on a version: what it changed, in a
  /// selected note style, and its Cards. Below the pointer, or above it near the
  /// bottom of the tree.
  @ViewBuilder
  private func hoverPanel(_ g: Tsugumi_RPC_VersionGraphT, in size: CGSize) -> some View {
    if let c = g.commitList.first(where: { $0.id == hovered }) {
      let below = pointerY + 20
      let top = below + Self.hoverHeight > size.height ? max(4, pointerY - 20 - Self.hoverHeight) : below
      VStack(alignment: .leading, spacing: 8) {
        if let summary = session.versionNotes[c.id], !summary.isEmpty {
          Text(summary)
        } else if session.versionNoteBusy.contains(c.id) {
          Text("ノートを読み込んでいます…").foregroundStyle(.secondary)
        } else {
          Text(c.summary ?? "")
        }
        if let error = session.versionNoteErrors[c.id] {
          Text("ノートを読み込めませんでした: \(error)").font(.caption).foregroundStyle(.red)
        }
        if let canvas = session.versionCanvases[c.id], !canvas.cards.isEmpty {
          VersionMiniMap(canvas: canvas, changed: c.changedList)
        }
      }
      .font(.callout)
      .fixedSize(horizontal: false, vertical: true)
      .padding(.horizontal, 12).padding(.vertical, 10)
      .frame(width: VersionMiniMap.width + 24, alignment: .leading)
      .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
      .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color.secondary.opacity(0.3), lineWidth: 0.5))
      .shadow(radius: 8, y: 3)
      .frame(maxWidth: .infinity, alignment: .trailing)
      .padding(.trailing, 12)
      .offset(y: top)
    }
  }

  private func x(_ lane: Int) -> CGFloat { 10 + CGFloat(lane) * Self.laneWidth + Self.laneWidth / 2 }
  private func y(_ row: Int) -> CGFloat { CGFloat(row) * Self.rowHeight + 20 }

  private func graph(_ layout: VersionLayout, _ commits: [Tsugumi_RPC_VersionCommitT], head: String?) -> some View {
    Canvas { g, _ in
      for (i, r) in layout.rows.enumerated() {
        for e in r.edges {
          var p = Path()
          p.move(to: CGPoint(x: x(r.lane), y: y(i)))
          if e.lane != r.lane {
            p.addCurve(to: CGPoint(x: x(e.lane), y: y(i) + Self.rowHeight * 0.6),
                       control1: CGPoint(x: x(r.lane), y: y(i) + Self.rowHeight * 0.35),
                       control2: CGPoint(x: x(e.lane), y: y(i) + Self.rowHeight * 0.25))
          }
          if e.lane != e.toLane {
            p.addLine(to: CGPoint(x: x(e.lane), y: y(e.toRow) - Self.rowHeight * 0.6))
            p.addCurve(to: CGPoint(x: x(e.toLane), y: y(e.toRow)),
                       control1: CGPoint(x: x(e.lane), y: y(e.toRow) - Self.rowHeight * 0.25),
                       control2: CGPoint(x: x(e.toLane), y: y(e.toRow) - Self.rowHeight * 0.35))
          } else {
            p.addLine(to: CGPoint(x: x(e.toLane), y: y(e.toRow)))
          }
          g.stroke(p, with: .color(Self.laneColor(e.lane)), lineWidth: 2)
        }
      }
      for (i, r) in layout.rows.enumerated() {
        let c = commits[i]
        let center = CGPoint(x: x(r.lane), y: y(i))
        let rect = CGRect(x: center.x - 6, y: center.y - 6, width: 12, height: 12)
        if let state = c.runList.last?.state {
          g.fill(Path(ellipseIn: rect), with: .color(Self.stateColor(state)))
        } else {
          g.fill(Path(ellipseIn: rect), with: .color(Color(nsColor: .windowBackgroundColor)))
          g.stroke(Path(ellipseIn: rect), with: .color(Self.laneColor(r.lane)), lineWidth: 2)
        }
        if c.id == head {
          g.stroke(Path(ellipseIn: rect.insetBy(dx: -4, dy: -4)), with: .color(.accentColor), lineWidth: 2)
        }
      }
    }
  }

  static func laneColor(_ lane: Int) -> Color {
    [Color.blue, .purple, .teal, .pink, .indigo, .brown][lane % 6].opacity(0.75)
  }

  static func stateColor(_ s: String) -> Color {
    switch s {
    case "succeeded": return .green
    case "failed": return .red
    default: return .gray
    }
  }

  static func stateLabel(_ s: String) -> String {
    switch s {
    case "succeeded": return String(localized: "成功")
    case "failed": return String(localized: "失敗")
    case "cancelled": return String(localized: "停止")
    default: return s
    }
  }

  static func secondsText(_ seconds: Double) -> String { String(format: "%.1f 秒", seconds) }

  private func row(_ c: Tsugumi_RPC_VersionCommitT, head: Bool) -> some View {
    VStack(alignment: .leading, spacing: 3) {
      HStack(spacing: 6) {
        Text(c.summary ?? "").font(.callout.weight(head ? .semibold : .regular)).lineLimit(1)
        ForEach(c.tagList, id: \.self) { t in
          Label(t, systemImage: "tag").font(.caption2).padding(.horizontal, 5).padding(.vertical, 1)
            .background(Color.purple.opacity(0.15), in: Capsule())
        }
        ForEach(c.branchList, id: \.self) { b in
          Text(b).font(.caption2.monospaced()).padding(.horizontal, 5).padding(.vertical, 1)
            .background((b == session.versionGraph?.branch ? Color.accentColor : Color.secondary).opacity(0.18), in: Capsule())
        }
        Spacer(minLength: 4)
        Text(Date(timeIntervalSince1970: Double(c.time ?? 0) / 1000), style: .relative)
          .font(.caption2).foregroundStyle(.secondary)
      }
      HStack(spacing: 8) {
        let runs = c.runList
        if let last = runs.last {
          let ok = runs.filter { $0.state == "succeeded" }.count
          let ng = runs.filter { $0.state == "failed" }.count
          Text("実行 \(runs.count) 回（成功 \(ok)・失敗 \(ng)）· 最後: \(Self.stateLabel(last.state))")
            .font(.caption).foregroundStyle(Self.stateColor(last.state))
        }
        if !c.changedList.isEmpty {
          Text(Self.changedText(c.changedList)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
        }
      }
    }
    .padding(.horizontal, 8)
  }

  static func changedText(_ changed: [String]) -> String {
    let names = changed.map { $0.isEmpty ? String(localized: "Workflow") : $0 }
    let shown = names.prefix(4).joined(separator: "、")
    return String(localized: "変更: \(shown)") + (names.count > 4 ? String(localized: " ほか \(names.count - 4)") : "")
  }

  @ViewBuilder
  private func detail(_ g: Tsugumi_RPC_VersionGraphT) -> some View {
    if let c = g.commitList.first(where: { $0.id == selected }) {
      ScrollView {
        VStack(alignment: .leading, spacing: 10) {
          Text(c.summary ?? "").font(.headline)
          if let summary = session.versionNotes[c.id], !summary.isEmpty { Text(summary).font(.callout).textSelection(.enabled) }
          if session.versionNoteBusy.contains(c.id) { Text("ノートを読み込んでいます…").foregroundStyle(.secondary) }
          if let error = session.versionNoteErrors[c.id] {
            Text("ノートを読み込めませんでした: \(error)").font(.caption).foregroundStyle(.red)
            Button("再試行") { Task { await session.loadVersionNote(c.id) } }
          }
          // The message's body: what follows its first line (the summary).
          let body = (c.message ?? "").split(separator: "\n", maxSplits: 1, omittingEmptySubsequences: false).dropFirst()
            .joined().trimmingCharacters(in: .whitespacesAndNewlines)
          if !body.isEmpty { Text(body).font(.callout).textSelection(.enabled) }
          LabeledContent("版", value: String(c.id.prefix(12))).font(.caption.monospaced())
          LabeledContent("作成", value: Date(timeIntervalSince1970: Double(c.time ?? 0) / 1000).formatted(date: .abbreviated, time: .shortened))
            .font(.caption)
          if !c.changedList.isEmpty { Text(Self.changedText(c.changedList)).font(.caption) }
          if !(session.versionComments[c.id] ?? []).isEmpty {
            Text("コメント").font(.subheadline.weight(.semibold))
            ForEach(Array((session.versionComments[c.id] ?? []).enumerated()), id: \.offset) { _, x in
              VStack(alignment: .leading, spacing: 1) {
                Text(verbatim: "\(x.card) · \(CardComment.change(x.change, by: x.by))").foregroundStyle(.secondary)
                Text(verbatim: x.text).textSelection(.enabled)
                if let line = x.resolution, !line.isEmpty {
                  Text(verbatim: line).foregroundStyle(.secondary).textSelection(.enabled)
                }
                let changed = x.cards.compactMap { $0 }
                if !changed.isEmpty {
                  Text("変更した Card: \(changed.joined(separator: "、"))").foregroundStyle(.secondary).textSelection(.enabled)
                }
              }
              .font(.caption)
            }
          }
          if !c.runList.isEmpty {
            Text("実行の結果").font(.subheadline.weight(.semibold))
            ForEach(Array(c.runList.enumerated()), id: \.offset) { _, r in
              HStack {
                Circle().fill(Self.stateColor(r.state)).frame(width: 8, height: 8)
                Text(Self.stateLabel(r.state))
                Text(Self.secondsText(r.seconds)).foregroundStyle(.secondary)
                Spacer()
                Text(r.ended ?? "").font(.caption2.monospaced()).foregroundStyle(.secondary)
              }
              .font(.caption)
              if let summary = r.summary, !summary.isEmpty {
                Text(summary).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
              }
            }
          } else {
            Text("この版での実行はありません。").font(.caption).foregroundStyle(.secondary)
          }
          Divider()
          Button("この版から始める…") { confirmStart = c.id }
            .disabled(c.id == g.head && !g.dirty || session.runActive || !session.connected)
          Text("git の履歴でも同じ情報を見られます（git log --notes=tsugumi、git tag）。")
            .font(.caption2).foregroundStyle(.secondary)
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
      }
    } else {
      Text("版を選ぶと、その実行の結果と変わった Card を表示します。").font(.callout).foregroundStyle(.secondary)
        .padding().frame(maxWidth: .infinity, maxHeight: .infinity)
    }
  }

  private var nameSheet: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("版として残す").font(.title3.bold())
      Text("今の定義と追加パスの内容に名前を付けて残します（git の tag）。").font(.callout).foregroundStyle(.secondary)
      TextField("名前（例: thin-bands）", text: $name)
      TextField("メモ", text: $message, axis: .vertical).lineLimit(3...6)
      HStack {
        Spacer()
        Button("キャンセル") { naming = false }.keyboardShortcut(.cancelAction)
        Button("残す") {
          naming = false
          let n = name.trimmingCharacters(in: .whitespaces), m = message
          act { try await session.markVersion(name: n, message: m) }
        }
        .keyboardShortcut(.defaultAction)
        .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
      }
    }
    .padding(20)
    .frame(width: 420)
  }
}
