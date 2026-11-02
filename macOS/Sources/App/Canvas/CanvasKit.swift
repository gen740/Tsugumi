import AppKit
import SwiftUI
import TsugumiRPC

// The Card canvas building blocks shared by the main canvas, the appearance
// preview in Settings and the preview in Card settings: Card data, geometry,
// the Card view, wires and a small self-contained canvas (CanvasSurface).

/// Direction the work flows in: inputs enter on the top (or left) edge and
/// outputs leave on the bottom (or right) edge.
enum FlowDirection: String, CaseIterable, Identifiable {
  case topToBottom, leftToRight
  var id: String { rawValue }
  var label: String { self == .topToBottom ? String(localized: "上から下") : String(localized: "左から右") }
}

/// One execution stage of an invocation, in the order the core runs them.
enum Stage: Int, CaseIterable, Identifiable {
  case waiting, prepare, evaluate, build, execute, validate, save, done
  var id: Int { rawValue }

  var label: String {
    switch self {
    case .waiting: return String(localized: "入力待ち")
    case .prepare: return String(localized: "SDK 準備")
    case .evaluate: return String(localized: "Nix 評価")
    case .build: return String(localized: "build")
    case .execute: return String(localized: "実行")
    case .validate: return String(localized: "出力検証")
    case .save: return String(localized: "保存")
    case .done: return String(localized: "完了")
    }
  }

  var symbol: String {
    switch self {
    case .waiting: return "tray.and.arrow.down"
    case .prepare: return "shippingbox"
    case .evaluate: return "snowflake"
    case .build: return "hammer"
    case .execute: return "play"
    case .validate: return "checkmark.shield"
    case .save: return "externaldrive"
    case .done: return "flag.checkered"
    }
  }

  init?(phase: Tsugumi_Data_Phase) {
    switch phase {
    case .prepare: self = .prepare
    case .evaluate: self = .evaluate
    case .build: self = .build
    case .execute: self = .execute
    case .validate: self = .validate
    case .save: self = .save
    default: return nil
    }
  }
}

/// Progress of the current invocation as the stage strip shows it.
struct StageProgress: Equatable {
  /// Stages that apply to this Card (code Cards build, builtins do not).
  var stages: [Stage]
  var current: Stage?
  var reached: Set<Stage> = []
  var failedAt: Stage?
  var cached = false
}

struct PortData: Hashable {
  var name: String
  var typeName: String?
  var optional = false
  var note: String?
}

struct ParamData: Hashable {
  var name: String
  var type: String
  var value: String
}

/// Everything a Card view needs, with no dependency on a session.
struct CardNodeData: Equatable {
  var title: String
  var kind: CardKind
  var subtitle: String
  var symbol: String?
  var summary: String?
  var inputs: [PortData]
  var outputs: [PortData]
  var params: [ParamData] = []
  var result: String?
  var resultFailed = false
  var state: Tsugumi_Data_CardState?
  var stage: StageProgress?
  var errors: [String] = []
  var warnings: [String] = []
  var collapsed = false

  var isDraft: Bool { kind == .draft }
}

/// Size and port positions of a Card (world units, relative to its top-left
/// corner). Shared by drawing and hit-testing.
struct CardGeometry {
  static let width: CGFloat = 220
  static let header: CGFloat = 44
  static let summaryRow: CGFloat = 16
  static let portRow: CGFloat = 18
  static let paramRow: CGFloat = 26
  static let resultRow: CGFloat = 20
  static let stageRow: CGFloat = 22

  let inputs: [String]
  let outputs: [String]
  let collapsed: Bool
  let summary: Bool
  let params: Int
  let result: Bool
  let stages: Bool
  let flow: FlowDirection

  init(_ d: CardNodeData, flow: FlowDirection) {
    inputs = d.inputs.map(\.name)
    outputs = d.outputs.map(\.name)
    collapsed = d.collapsed
    summary = d.summary != nil
    params = d.params.count
    result = d.result != nil
    stages = d.stage != nil
    self.flow = flow
  }

  /// Rows for port labels: one row per port side-by-side (left to right),
  /// or one label row at the top and one at the bottom (top to bottom).
  var portBand: CGFloat {
    flow == .leftToRight ? CGFloat(max(inputs.count, outputs.count, 1)) * Self.portRow : 0
  }
  var topLabels: CGFloat { flow == .topToBottom && !inputs.isEmpty ? Self.portRow : 0 }
  var bottomLabels: CGFloat { flow == .topToBottom && !outputs.isEmpty ? Self.portRow : 0 }

  var height: CGFloat {
    if collapsed { return Self.header }
    return topLabels + Self.header + (summary ? Self.summaryRow : 0) + portBand + CGFloat(params) * Self.paramRow +
      (result ? Self.resultRow : 0) + (stages ? Self.stageRow : 0) + bottomLabels + 6
  }

  var size: CGSize { CGSize(width: Self.width, height: height) }

  func port(_ name: String, output: Bool) -> CGPoint {
    let list = output ? outputs : inputs
    let i = CGFloat(list.firstIndex(of: name) ?? 0)
    switch flow {
    case .topToBottom:
      let x = Self.width * (i + 1) / CGFloat(list.count + 1)
      return CGPoint(x: x, y: output ? height : 0)
    case .leftToRight:
      let x = output ? Self.width : 0
      if collapsed { return CGPoint(x: x, y: Self.header / 2) }
      let top = Self.header + (summary ? Self.summaryRow : 0)
      return CGPoint(x: x, y: top + i * Self.portRow + Self.portRow / 2)
    }
  }
}

/// Curved wire from an output to an input, following the flow direction.
func wirePath(_ a: CGPoint, _ b: CGPoint, flow: FlowDirection) -> Path {
  var path = Path()
  path.move(to: a)
  switch flow {
  case .topToBottom:
    let dy = max(40, abs(b.y - a.y) / 2)
    path.addCurve(to: b, control1: CGPoint(x: a.x, y: a.y + dy), control2: CGPoint(x: b.x, y: b.y - dy))
  case .leftToRight:
    let dx = max(40, abs(b.x - a.x) / 2)
    path.addCurve(to: b, control1: CGPoint(x: a.x + dx, y: a.y), control2: CGPoint(x: b.x - dx, y: b.y))
  }
  return path
}

struct CanvasCamera: Equatable {
  var x: Double = 40
  var y: Double = 40
  var zoom: Double = 1

  func screen(_ w: CGPoint) -> CGPoint { CGPoint(x: w.x * zoom + x, y: w.y * zoom + y) }
  func world(_ s: CGPoint) -> CGPoint { CGPoint(x: (s.x - x) / zoom, y: (s.y - y) / zoom) }

  /// Zooms keeping `anchor` (a screen point) fixed.
  mutating func zoom(to z: Double, around anchor: CGPoint) {
    let nz = max(0.2, min(3, z))
    x = anchor.x - (anchor.x - x) * nz / zoom
    y = anchor.y - (anchor.y - y) * nz / zoom
    zoom = nz
  }

  /// Frames `rect` (world units) in a view of `size`.
  mutating func fit(_ rect: CGRect, in size: CGSize, maxZoom: Double = 1.5) {
    guard !rect.isNull, size.width > 0, size.height > 0 else { return }
    let r = rect.insetBy(dx: -50, dy: -60)
    let z = max(0.2, min(maxZoom, min(size.width / r.width, size.height / r.height)))
    zoom = z
    x = size.width / 2 - r.midX * z
    y = size.height / 2 - r.midY * z
  }
}

// MARK: - Card view

/// A Card as drawn on every canvas.
struct CardNodeView: View {
  let data: CardNodeData
  let flow: FlowDirection
  var selected = false
  /// Committing an edited param of an input Card (nil: read-only).
  var onParamCommit: ((String, String) -> Void)?
  var paramsEditable = true
  var onPortDrag: ((String, CGPoint) -> Void)?
  var onPortDrop: ((String, CGPoint) -> Void)?
  var onStageTap: ((Stage) -> Void)?
  var onToggleCollapse: (() -> Void)?

  var body: some View {
    let g = CardGeometry(data, flow: flow)
    VStack(alignment: .leading, spacing: 0) {
      if !g.collapsed, g.topLabels > 0 { portLabels(data.inputs, output: false).frame(height: CardGeometry.portRow) }
      header.frame(height: CardGeometry.header)
      if !g.collapsed {
        if let s = data.summary {
          Text(s).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
            .padding(.horizontal, 10).frame(height: CardGeometry.summaryRow, alignment: .top)
        }
        if flow == .leftToRight { sidePorts.frame(height: g.portBand) }
        ForEach(data.params, id: \.name) { p in
          ParamFieldView(param: p, editable: onParamCommit != nil && paramsEditable) { onParamCommit?(p.name, $0) }
            .frame(height: CardGeometry.paramRow)
            .padding(.horizontal, 10)
        }
        if let r = data.result {
          HStack(spacing: 4) {
            Image(systemName: data.resultFailed ? "xmark.octagon" : "text.below.photo").font(.system(size: 9))
            Text(r).font(.system(size: 10, design: .monospaced)).lineLimit(1)
          }
          .foregroundStyle(data.resultFailed ? .red : .secondary)
          .padding(.horizontal, 10)
          .frame(height: CardGeometry.resultRow)
        }
        if let st = data.stage { StageStrip(progress: st, onTap: onStageTap).frame(height: CardGeometry.stageRow).padding(.horizontal, 8) }
        if g.bottomLabels > 0 { portLabels(data.outputs, output: true).frame(height: CardGeometry.portRow) }
      }
    }
    .frame(width: CardGeometry.width, height: g.height, alignment: .topLeading)
    .background(RoundedRectangle(cornerRadius: 9).fill(Color(nsColor: .controlBackgroundColor)))
    .overlay { portDots(g) }
    .overlay(RoundedRectangle(cornerRadius: 9).strokeBorder(borderColor,
                                                            style: StrokeStyle(lineWidth: selected ? 2.5 : 1, dash: data.isDraft ? [5, 3] : [])))
    .shadow(color: .black.opacity(selected ? 0.22 : 0.08), radius: selected ? 5 : 2, y: 1)
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Card \(data.title), \(data.kind.label), \(data.state?.label ?? String(localized: "未実行"))")
  }

  private var header: some View {
    HStack(spacing: 7) {
      Image(systemName: data.symbol ?? data.kind.symbol).font(.system(size: 13)).foregroundStyle(.secondary).frame(width: 16)
      VStack(alignment: .leading, spacing: 1) {
        Text(data.title).font(.system(size: 12, weight: .semibold)).lineLimit(1)
        HStack(spacing: 4) {
          if data.isDraft {
            Text("未実装").font(.system(size: 9, weight: .semibold)).padding(.horizontal, 4)
              .background(Color.orange.opacity(0.2), in: Capsule())
          }
          if let s = data.state {
            Image(systemName: s.symbol).foregroundStyle(s.color)
          }
          Text(data.subtitle)
        }
        .font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
      }
      Spacer(minLength: 0)
      if !data.errors.isEmpty {
        Image(systemName: "xmark.octagon.fill").foregroundStyle(.red).help(data.errors.joined(separator: "\n"))
      } else if !data.warnings.isEmpty {
        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow).help(data.warnings.joined(separator: "\n"))
      }
      if let toggle = onToggleCollapse {
        Button(action: toggle) {
          Image(systemName: data.collapsed ? "chevron.down.circle" : "chevron.up.circle").font(.system(size: 11))
        }
        .buttonStyle(.borderless).foregroundStyle(.secondary)
        .help(data.collapsed ? "展開" : "折りたたむ（実行には影響しません）")
      }
    }
    .padding(.horizontal, 10)
  }

  private func portLabels(_ ports: [PortData], output: Bool) -> some View {
    HStack(spacing: 0) {
      ForEach(ports, id: \.name) { p in
        Text(p.name).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
          .frame(maxWidth: .infinity)
          .help(portHelp(p, output: output))
      }
    }
    .padding(.horizontal, 4)
  }

  private var sidePorts: some View {
    HStack(alignment: .top, spacing: 0) {
      VStack(alignment: .leading, spacing: 0) {
        ForEach(data.inputs, id: \.name) { p in
          Text(p.name).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
            .padding(.leading, 10).frame(height: CardGeometry.portRow).help(portHelp(p, output: false))
        }
      }
      Spacer(minLength: 4)
      VStack(alignment: .trailing, spacing: 0) {
        ForEach(data.outputs, id: \.name) { p in
          Text(p.name).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
            .padding(.trailing, 10).frame(height: CardGeometry.portRow).help(portHelp(p, output: true))
        }
      }
    }
  }

  /// Port dots on the edges; outputs can be dragged to an input to connect.
  private func portDots(_ g: CardGeometry) -> some View {
    ZStack(alignment: .topLeading) {
      ForEach(data.inputs, id: \.name) { p in
        let pt = g.port(p.name, output: false)
        Circle().strokeBorder(Color.secondary, lineWidth: 1.5).background(Circle().fill(Color(nsColor: .controlBackgroundColor)))
          .frame(width: 10, height: 10)
          .position(pt)
          .help(portHelp(p, output: false))
      }
      ForEach(data.outputs, id: \.name) { p in
        let pt = g.port(p.name, output: true)
        Circle().fill(Color.accentColor).frame(width: 10, height: 10)
          .contentShape(Circle().inset(by: -6))
          .position(pt)
          .gesture(DragGesture(coordinateSpace: .named("canvas"))
            .onChanged { onPortDrag?(p.name, $0.location) }
            .onEnded { onPortDrop?(p.name, $0.location) })
          .help(portHelp(p, output: true) + (onPortDrag != nil ? String(localized: "（ドラッグして入力ポートへ接続）") : ""))
      }
    }
    .frame(width: CardGeometry.width, height: g.height, alignment: .topLeading)
  }

  private func portHelp(_ p: PortData, output: Bool) -> String {
    var s = (output ? String(localized: "出力 ") : String(localized: "入力 ")) + p.name
    if let t = p.typeName { s += " : \(t)" }
    if p.optional { s += String(localized: "（省略可）") }
    if let n = p.note, !n.isEmpty { s += "\n" + n }
    return s
  }

  private var borderColor: Color {
    if selected { return .accentColor }
    if let s = data.state, s != .notStarted { return s.color.opacity(0.7) }
    return .secondary.opacity(data.isDraft ? 0.7 : 0.35)
  }
}

/// Stages of the current invocation; finished ones are checked, the current
/// one is highlighted, a failed one is red and opens the log when clicked.
struct StageStrip: View {
  let progress: StageProgress
  var onTap: ((Stage) -> Void)?

  var body: some View {
    HStack(spacing: 2) {
      ForEach(progress.stages) { st in
        let failed = progress.failedAt == st
        let current = progress.current == st && !failed
        let reached = progress.reached.contains(st)
        let skippedByCache = progress.cached && (st == .evaluate || st == .build)
        Button { onTap?(st) } label: {
          Image(systemName: failed ? "xmark.circle.fill" : skippedByCache ? "arrow.triangle.2.circlepath" :
                  current ? st.symbol + (st == .waiting ? "" : ".fill") : reached ? "checkmark.circle.fill" : st.symbol)
            .font(.system(size: 10))
            .foregroundStyle(failed ? .red : current ? .orange : reached || skippedByCache ? .green : .secondary.opacity(0.5))
            .frame(maxWidth: .infinity)
        }
        .buttonStyle(.plain)
        .disabled(onTap == nil)
        .help(st.label + (skippedByCache ? String(localized: "（キャッシュを利用）") : "") +
              (failed ? String(localized: "：失敗（クリックでログ）") : current ? String(localized: "：進行中") : reached ? String(localized: "：完了") : ""))
      }
      if progress.cached {
        Text("cache").font(.system(size: 8, weight: .semibold)).foregroundStyle(.green)
          .help("ビルドはキャッシュを利用しました")
      }
    }
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(progress.current.map { String(localized: "段階: ") + $0.label } ?? String(localized: "段階なし"))
  }
}

/// A param shown on an input Card; committed on Return or focus loss.
struct ParamFieldView: View {
  let param: ParamData
  let editable: Bool
  let commit: (String) -> Void
  @State private var text = ""
  @FocusState private var focused: Bool

  var body: some View {
    HStack(spacing: 6) {
      Text(param.name).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
      if param.type == "bool" {
        Toggle("", isOn: Binding(get: { param.value == "true" }, set: { commit($0 ? "true" : "false") }))
          .toggleStyle(.switch).controlSize(.mini).labelsHidden()
          .disabled(!editable)
      } else {
        TextField("", text: $text)
          .textFieldStyle(.roundedBorder)
          .font(.system(size: 11, design: .monospaced))
          .controlSize(.small)
          .focused($focused)
          .onSubmit { if text != param.value { commit(text) } }
          .onChange(of: focused) { if !focused && text != param.value { commit(text) } }
          .disabled(!editable)
          .help(editable ? "\(param.type)・Return で反映" : String(localized: "実行中は変更できません"))
      }
    }
    .onAppear { text = param.value }
    .onChange(of: param.value) { if !focused { text = param.value } }
  }
}

// MARK: - Self-contained canvas (previews)

struct CanvasItem: Identifiable, Equatable {
  var id: String
  var data: CardNodeData
  var origin: CGPoint
}

struct CanvasWire: Equatable {
  var from: String
  var fromPort: String
  var to: String
  var toPort: String
  var active = false
}

/// A small canvas with the main canvas' Card view, wires, selection, drag,
/// collapse and zoom, over local state only (it never edits a project).
struct CanvasSurface: View {
  @Binding var items: [CanvasItem]
  var wires: [CanvasWire]
  var flow: FlowDirection
  var grid = true
  var gridSize: Double = 20
  var animateWires = false
  @State private var camera = CanvasCamera()
  @State private var selection: Set<String> = []
  @State private var drag: (id: String, offset: CGSize)?
  @State private var zoomStart: Double?
  @State private var fitted = false

  var body: some View {
    GeometryReader { geo in
      ZStack(alignment: .topLeading) {
        CanvasGrid(camera: camera, show: grid, size: gridSize)
          .contentShape(Rectangle())
          .onTapGesture { selection = [] }
          .gesture(DragGesture(minimumDistance: 2).onChanged { v in
            camera.x += v.translation.width - (lastPan?.width ?? 0)
            camera.y += v.translation.height - (lastPan?.height ?? 0)
            lastPan = v.translation
          }.onEnded { _ in lastPan = nil })
        WireLayer(wires: wires.map { w in (start: point(w.from, w.fromPort, output: true), end: point(w.to, w.toPort, output: false), active: w.active, selected: false) },
                  flow: flow, animate: animateWires)
        ForEach(items) { item in
          let o = origin(item)
          CardNodeView(data: item.data, flow: flow, selected: selection.contains(item.id),
                       onToggleCollapse: { toggle(item.id) })
            .scaleEffect(camera.zoom, anchor: .topLeading)
            .place(at: camera.screen(o), size: CardGeometry(item.data, flow: flow).size, zoom: camera.zoom)
            .gesture(DragGesture(minimumDistance: 2, coordinateSpace: .named("canvas"))
              .onChanged { v in
                if !selection.contains(item.id) { selection = [item.id] }
                drag = (item.id, v.translation)
              }
              .onEnded { v in
                for i in items.indices where selection.contains(items[i].id) {
                  items[i].origin.x += v.translation.width / camera.zoom
                  items[i].origin.y += v.translation.height / camera.zoom
                }
                drag = nil
              })
            .onTapGesture { selection = NSEvent.modifierFlags.contains(.shift) ? selection.symmetricDifference([item.id]) : [item.id] }
        }
        HStack(spacing: 4) {
          Button { camera.zoom(to: camera.zoom / 1.2, around: CGPoint(x: geo.size.width / 2, y: geo.size.height / 2)) } label: { Image(systemName: "minus.magnifyingglass") }
          Text("\(Int((camera.zoom * 100).rounded()))%").font(.caption.monospacedDigit()).frame(minWidth: 36)
          Button { camera.zoom(to: camera.zoom * 1.2, around: CGPoint(x: geo.size.width / 2, y: geo.size.height / 2)) } label: { Image(systemName: "plus.magnifyingglass") }
          Button { fit(geo.size) } label: { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }.help("全体表示")
        }
        .buttonStyle(.borderless)
        .controlSize(.small)
        .padding(3)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 6))
        .padding(6)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
      }
      .coordinateSpace(name: "canvas")
      .clipped()
      .gesture(MagnifyGesture().onChanged { v in
        if zoomStart == nil { zoomStart = camera.zoom }
        camera.zoom(to: (zoomStart ?? 1) * v.magnification, around: CGPoint(x: geo.size.width / 2, y: geo.size.height / 2))
      }.onEnded { _ in zoomStart = nil })
      .onAppear { if !fitted { fit(geo.size); fitted = true } }
      .onChange(of: flow) { fit(geo.size) }
    }
  }

  @State private var lastPan: CGSize?

  private func origin(_ item: CanvasItem) -> CGPoint {
    var o = item.origin
    if let d = drag, selection.contains(item.id) {
      o.x += d.offset.width / camera.zoom
      o.y += d.offset.height / camera.zoom
    }
    return o
  }

  private func point(_ id: String, _ port: String, output: Bool) -> CGPoint {
    guard let item = items.first(where: { $0.id == id }) else { return .zero }
    let o = origin(item), p = CardGeometry(item.data, flow: flow).port(port, output: output)
    return camera.screen(CGPoint(x: o.x + p.x, y: o.y + p.y))
  }

  private func toggle(_ id: String) {
    guard let i = items.firstIndex(where: { $0.id == id }) else { return }
    items[i].data.collapsed.toggle()
  }

  private func fit(_ size: CGSize) {
    var r = CGRect.null
    for item in items { r = r.union(CGRect(origin: item.origin, size: CardGeometry(item.data, flow: flow).size)) }
    camera.fit(r, in: size, maxZoom: 1)
  }
}

/// Dot grid behind a canvas.
struct CanvasGrid: View {
  let camera: CanvasCamera
  let show: Bool
  let size: Double

  var body: some View {
    Canvas { ctx, sz in
      guard show else { return }
      let step = size * camera.zoom
      guard step >= 6 else { return }
      var x = camera.x.truncatingRemainder(dividingBy: step)
      let dot = Path(ellipseIn: CGRect(x: 0, y: 0, width: 1.6, height: 1.6))
      while x < sz.width {
        var y = camera.y.truncatingRemainder(dividingBy: step)
        while y < sz.height {
          ctx.fill(dot.offsetBy(dx: x, dy: y), with: .color(.secondary.opacity(0.35)))
          y += step
        }
        x += step
      }
    }
    .background(Color(nsColor: .textBackgroundColor))
  }
}

/// Wires between port positions (screen points); active ones are highlighted
/// and, when enabled, animated.
struct WireLayer: View {
  let wires: [(start: CGPoint, end: CGPoint, active: Bool, selected: Bool)]
  let flow: FlowDirection
  var animate = false

  var body: some View {
    let anyActive = animate && wires.contains { $0.active }
    SwiftUI.TimelineView(.animation(minimumInterval: 1 / 30, paused: !anyActive)) { t in
      Canvas { ctx, _ in
        let phase = anyActive ? -CGFloat(t.date.timeIntervalSinceReferenceDate * 30).truncatingRemainder(dividingBy: 20) : 0
        for w in wires {
          let path = wirePath(w.start, w.end, flow: flow)
          let color: Color = w.selected ? .accentColor : (w.active ? .orange : .secondary.opacity(0.8))
          ctx.stroke(path, with: .color(color),
                     style: StrokeStyle(lineWidth: w.selected ? 3 : 1.7, lineCap: .round,
                                        dash: w.active && anyActive ? [8, 6] : [], dashPhase: phase))
        }
      }
    }
    .allowsHitTesting(false)
  }
}

/// Sample Cards for previews of appearance settings.
enum PreviewSamples {
  static func items() -> [CanvasItem] {
    func port(_ n: String) -> PortData { PortData(name: n, typeName: "t.Number") }
    let input = CardNodeData(title: String(localized: "初期値"), kind: .input, subtitle: String(localized: "入力"), summary: nil,
                             inputs: [port("start")], outputs: [port("value")],
                             params: [ParamData(name: "value", type: "uint64", value: "27")])
    var step = CardNodeData(title: String(localized: "n / 2"), kind: .code, subtitle: String(localized: "実行中"),
                            summary: String(localized: "偶数を半分にする"), inputs: [port("number")], outputs: [port("next")],
                            state: .running)
    step.stage = StageProgress(stages: [.prepare, .evaluate, .build, .execute, .validate, .save], current: .execute,
                               reached: [.prepare, .evaluate, .build], cached: true)
    let out = CardNodeData(title: String(localized: "結果"), kind: .logger, subtitle: String(localized: "成功"), summary: nil,
                           inputs: [port("in")], outputs: [], result: "[27, 82, 41, …]", state: .succeeded)
    return [
      CanvasItem(id: "input", data: input, origin: CGPoint(x: 0, y: 0)),
      CanvasItem(id: "step", data: step, origin: CGPoint(x: 0, y: 190)),
      CanvasItem(id: "out", data: out, origin: CGPoint(x: 0, y: 380)),
    ]
  }

  static let wires = [
    CanvasWire(from: "input", fromPort: "value", to: "step", toPort: "number", active: true),
    CanvasWire(from: "step", fromPort: "next", to: "out", toPort: "in"),
  ]
}
