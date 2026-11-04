import AppKit
import SwiftUI
import TsugumiRPC
import TsugumiRenderer

/// Right-click menu of a Card.
struct CardMenu: View {
  @Environment(\.openWindow) private var openWindow
  let workspace: Workspace
  let session: ProjectSession
  let tab: WorkflowTab
  let cardKey: CardKey
  let onGroup: () -> Void

  var body: some View {
    let ref = CardRef(path: session.identity.url.path, card: cardKey)
    let kind = session.kind(cardKey)
    let targets = tab.selection.contains(cardKey) ? tab.selection : [cardKey]
    if kind == .subgraph {
      Button("開く") { tab.enter(path(cardKey)) }
      Button("新しいタブで開く") { workspace.openTab(path(cardKey)) }
      Button("別ウインドウで開く") { openWindow(id: "workflow", value: WorkflowRef(path: ref.path, scope: path(cardKey))) }
      Divider()
    }
    if kind == .draft { Button("Draft を編集…") { openWindow(id: "draft", value: ref) } }
    Button("設定…") { openWindow(id: "card", value: ref) }
    Button("実行履歴…") { openWindow(id: "executions", value: ref) }
    Button("成果物…") { openWindow(id: "artifacts", value: ref) }
    Button("ログを表示") { workspace.showLog(for: cardKey) }
    Button("チャットに参照を追加") {
      for k in targets.sorted() { AppModel.shared.chat(for: session).addReference(.card(k)) }
      workspace.showChat = true
    }
    Divider()
    Button("ここから下流を実行") { RunActions.run(session, card: cardKey, propagate: true) }
      .disabled(!session.connected || session.runActive)
    Button("この Card だけ実行（最後の入力）") { RunActions.run(session, card: cardKey, propagate: false) }
      .disabled(!session.connected || session.runActive)
    Divider()
    Button("複製") { Task { tab.selection = await session.duplicate(targets, in: tab.scope) } }
      .disabled(!session.isEditable)
    Button(session.collapsed(cardKey) ? "展開" : "折りたたむ") { session.setCollapsed(targets, !session.collapsed(cardKey)) }
    if session.displayFrame(cardKey)?["hidden"]?.bool == true {
      Button("出力表示を戻す") { session.updateDisplayFrame(cardKey) { $0["hidden"] = nil } }
    }
    Button("グループ化（見た目だけ）") {
      if !tab.selection.contains(cardKey) { tab.selection = [cardKey] }
      onGroup()
    }
    Button("Card in Card にまとめる（新しい external プロジェクト）") {
      Task { if let k = await session.makeSubgraph(targets, in: tab.scope) { tab.selection = [k] } }
    }
    .disabled(!session.isEditable)
    Divider()
    Button("入力を切断") { session.disconnect(cardKey, inputs: true, in: tab.scope) }.disabled(!session.isEditable)
    Button("出力を切断") { session.disconnect(cardKey, inputs: false, in: tab.scope) }.disabled(!session.isEditable)
    Button("削除", role: .destructive) {
      session.deleteCards(targets, in: tab.scope)
      tab.selection.subtract(targets)
    }
    .disabled(!session.isEditable)
  }
}

func noteColor(_ id: String?) -> Color {
  switch id {
  case "blue": return .blue
  case "green": return .green
  case "pink": return .pink
  case "gray": return .gray
  default: return .yellow
  }
}

/// Visual grouping frame drawn behind its Cards.
struct GroupFrameView: View {
  let session: ProjectSession
  let group: JSONValue
  let scope: [String]
  @Environment(\.canvasZoom) private var z

  var body: some View {
    let color = noteColor(group["color"]?.string)
    let outputs = (group["outputs"]?.array ?? []).compactMap { o -> String? in
      guard let c = o["card"]?.string, let p = o["port"]?.string else { return nil }
      return "\(session.displayName(key(scope + [c]))).\(p)"
    }
    ZStack(alignment: .topLeading) {
      RoundedRectangle(cornerRadius: 12 * z).fill(color.opacity(0.07))
      RoundedRectangle(cornerRadius: 12 * z).strokeBorder(color.opacity(0.5), style: StrokeStyle(lineWidth: 1.2, dash: [6, 4]))
      HStack(spacing: 6 * z) {
        Image(systemName: "rectangle.3.group").font(.system(size: 10 * z))
        Text(group["name"]?.string ?? String(localized: "グループ")).font(.system(size: 12 * z, weight: .semibold))
        Spacer()
      }
      .foregroundStyle(color)
      .padding(.horizontal, 10 * z).padding(.top, 5 * z)
      if !outputs.isEmpty {
        VStack(alignment: .trailing, spacing: 2 * z) {
          ForEach(outputs, id: \.self) { o in
            Label(o, systemImage: "arrow.right.circle").font(.system(size: 10 * z))
              .padding(.horizontal, 6 * z).padding(.vertical, 2 * z)
              .background(color.opacity(0.15), in: Capsule())
          }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
        .padding(6 * z)
        .help("グループの公開出力")
      }
    }
    .accessibilityLabel("グループ \(group["name"]?.string ?? "")")
  }
}

/// Markdown sticky note (with TeX math); it does not take part in runs. Its
/// text size is its own (`fontSize` in layout.json).
struct StickyNoteView: View {
  let note: JSONValue
  let onResize: (CGSize) -> Void
  @Environment(\.canvasZoom) private var z

  static func fontSize(_ note: JSONValue) -> CGFloat { CGFloat(note["fontSize"]?.double ?? 13) }

  var body: some View {
    // Cut at the note's size (no scroll bar on the canvas; resize to see more).
    MarkdownView(text: note["text"]?.string ?? "", fontSize: Self.fontSize(note) * z)
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      .clipped()
    .padding(10 * z)
    .background(noteColor(note["color"]?.string).opacity(0.28), in: RoundedRectangle(cornerRadius: 6 * z))
    .overlay(RoundedRectangle(cornerRadius: 6 * z).strokeBorder(noteColor(note["color"]?.string).opacity(0.5)))
    .overlay(alignment: .bottomTrailing) { ResizeHandle(onEnd: onResize) }
    .help("ダブルクリックで編集")
  }
}

struct ResizeHandle: View {
  let onEnd: (CGSize) -> Void
  @State private var live: CGSize = .zero

  var body: some View {
    Image(systemName: "arrow.down.right.and.arrow.up.left")
      .font(.system(size: 8))
      .foregroundStyle(.secondary)
      .frame(width: 16, height: 16)
      .contentShape(Rectangle())
      .offset(x: live.width, y: live.height)
      .gesture(DragGesture().onChanged { live = $0.translation }.onEnded { v in
        live = .zero
        onEnd(v.translation)
      })
      .help("ドラッグしてサイズ変更")
  }
}

/// A Card's result in its own movable frame.
struct OutputFrameView: View {
  let session: ProjectSession
  let cardKey: CardKey
  let onExpand: () -> Void
  let onClose: () -> Void
  let onResize: (CGSize) -> Void
  @Environment(\.canvasZoom) private var z

  var body: some View {
    VStack(alignment: .leading, spacing: 4 * z) {
      HStack(spacing: 6 * z) {
        Image(systemName: "chart.xyaxis.line").font(.system(size: 10 * z)).foregroundStyle(.purple)
        Text(session.displayName(cardKey)).font(.system(size: 10 * z, weight: .bold)).lineLimit(1)
        if let d = session.displays[cardKey] {
          Text("Run \(String(d.runId.prefix(6))) · 実行 #\(d.invocation)").font(.system(size: 9 * z).monospacedDigit()).foregroundStyle(.secondary)
            .help("Run \(d.runId) の中で、この Card の \(d.invocation) 回目の実行の結果")
        }
        Spacer()
        Button(action: onExpand) { Image(systemName: "arrow.up.left.and.arrow.down.right").font(.system(size: 11 * z)) }
          .buttonStyle(.borderless).help("拡大して表示")
        Button(action: onClose) { Image(systemName: "xmark").font(.system(size: 11 * z)) }
          .buttonStyle(.borderless).help("枠を削除（Card は残ります）")
      }
      .foregroundStyle(.secondary)
      DisplayContent(display: session.displays[cardKey], zoom: z)
    }
    .padding(8 * z)
    .background(RoundedRectangle(cornerRadius: 8 * z).fill(Color(nsColor: .windowBackgroundColor)))
    .overlay(RoundedRectangle(cornerRadius: 8 * z).strokeBorder(Color.purple.opacity(0.45)))
    .overlay(alignment: .bottomTrailing) { ResizeHandle(onEnd: onResize) }
  }
}

/// Renders a Display value (text, Markdown, scalar, line chart, table, …) at
/// `zoom` (on the canvas) or larger (`large`, the expanded viewer).
struct DisplayContent: View {
  let display: DisplaySnapshot?
  var large = false
  var zoom: CGFloat = 1

  @State private var tablePage = 0

  private func size(_ small: CGFloat, _ big: CGFloat) -> CGFloat { (large ? big : small) * zoom }

  /// Scrolls in the large viewer; on the canvas (where the wheel moves the
  /// canvas) the content is cut at the frame, with no scroll bar.
  @ViewBuilder private func scrolling(axes: Axis.Set = .vertical, @ViewBuilder _ content: () -> some View) -> some View {
    if large {
      ScrollView(axes) { content() }
    } else {
      content().frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading).clipped()
    }
  }

  var body: some View {
    switch display?.value {
    case .text(let t)?:
      scrolling {
        Text(t).font(.system(size: size(11, 13), design: .monospaced)).textSelection(.enabled)
          .frame(maxWidth: .infinity, alignment: .leading)
      }
    case .markdown(let t)?:
      scrolling {
        MarkdownView(text: t, fontSize: size(12, 14)).frame(maxWidth: .infinity, alignment: .leading)
      }
    case .scalar(let v, let label)?:
      VStack {
        Text(label).font(.system(size: size(10, 17)))
        Text(v, format: .number.precision(.significantDigits(8))).font(.system(size: size(26, 64)).monospacedDigit())
          .textSelection(.enabled)
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
    case .lineChart(let series)?:
      VStack(spacing: 4 * zoom) {
        DawnLineChart(series: series.map { ChartSeries(id: $0.id, label: $0.label, x: $0.x, y: $0.y) })
          .clipShape(RoundedRectangle(cornerRadius: 4 * zoom))
        HStack {
          ForEach(Array(series.enumerated()), id: \.offset) { _, s in
            Label("\(s.label) (\(s.x.count))", systemImage: "line.diagonal").font(.system(size: size(9, 11)))
          }
        }
      }
    case .table(let columns, let rows)?:
      let page = TablePage(count: rows.count, requested: large ? tablePage : 0)
      VStack(alignment: .leading, spacing: 4 * zoom) {
        if large {
          HStack {
            Text("\(page.start == page.end ? 0 : page.start + 1)–\(page.end) / \(rows.count) 行").font(.caption.monospacedDigit())
            Spacer()
            Button("前へ") { tablePage = page.index - 1 }.disabled(page.index == 0)
            Picker("ページ", selection: Binding(get: { page.index }, set: { tablePage = $0 })) {
              ForEach(0..<page.count, id: \.self) { Text("\($0 + 1)").tag($0) }
            }.frame(maxWidth: 130)
            Button("次へ") { tablePage = page.index + 1 }.disabled(page.index + 1 >= page.count)
          }.controlSize(.small)
        } else {
          Text("全 \(rows.count) 行・先頭 \(page.end) 行のプレビュー（拡大して全行表示）")
            .font(.system(size: size(9, 11))).foregroundStyle(.secondary)
        }
        scrolling(axes: [.horizontal, .vertical]) {
          Grid(alignment: .leading, horizontalSpacing: 12 * zoom, verticalSpacing: 3 * zoom) {
            GridRow {
              ForEach(Array(columns.enumerated()), id: \.offset) { _, c in Text(c).font(.system(size: size(10, 12), weight: .bold)) }
            }
            Divider()
            ForEach(page.start..<page.end, id: \.self) { i in
              GridRow {
                ForEach(Array(rows[i].enumerated()), id: \.offset) { _, cell in
                  Text(cell).font(.system(size: size(10, 12), design: .monospaced)).textSelection(.enabled)
                }
              }
            }
          }.padding(4 * zoom)
        }.id(page.index)
      }
      .onChange(of: display?.identity) { tablePage = 0 }
    case .image(_, let data, let alt)?:
      if let img = NSImage(data: data) {
        Image(nsImage: img).resizable().interpolation(.none).scaledToFit()
          .accessibilityLabel(alt)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        Text("画像を表示できません").font(.system(size: size(10, 12))).foregroundStyle(.secondary)
      }
    case .points(let x, let y, let z, let colors)?:
      PointCloudView(identity: display?.identity, x: x, y: y, z: z, colors: colors)
    case .module(let name, let json)?:
      ModuleDisplayView(name: name, json: json, large: large, zoom: zoom)
    case nil where large:
      ContentUnavailableView("まだ結果がありません", systemImage: "chart.xyaxis.line", description: Text("実行すると表示されます。"))
    case nil:
      Text("まだ結果がありません（実行すると表示されます）")
        .font(.system(size: size(10, 13))).foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
  }
}

/// A 3D point cloud: drag to rotate, scroll or pinch to zoom.
///
/// Each frame writes the projected points straight into a bitmap (about
/// 5 ms for 100 000 points); drawing every point as a SwiftUI path took
/// about 120 ms a frame, which made dragging crawl.
struct PointCloudView: View {
  let identity: DisplaySnapshot.Identity?
  let x: [Float], y: [Float], z: [Float], colors: [UInt32]
  @State private var yaw = 0.6
  @State private var pitch = 0.4
  @State private var scale = 1.0
  @State private var dragStart: (Double, Double)?
  @State private var cloud: PreparedCloud?
  @Environment(\.displayScale) private var displayScale

  var body: some View {
    Canvas { ctx, size in
      guard let cloud, let img = cloud.render(yaw: yaw, pitch: pitch, zoom: scale, size: size, pixelScale: displayScale) else { return }
      ctx.draw(Image(decorative: img, scale: displayScale), in: CGRect(origin: .zero, size: size))
    }
    .background(Color(nsColor: .textBackgroundColor))
    .contentShape(Rectangle())
    .gesture(DragGesture().onChanged { v in
      if dragStart == nil { dragStart = (yaw, pitch) }
      yaw = dragStart!.0 + v.translation.width / 120
      pitch = max(-1.5, min(1.5, dragStart!.1 + v.translation.height / 120))
    }.onEnded { _ in dragStart = nil })
    .gesture(MagnifyGesture().onChanged { scale = max(0.2, min(8, $0.magnification)) })
    .overlay(alignment: .bottomTrailing) {
      Text("\(x.count) 点 · ドラッグで回転").font(.caption2).foregroundStyle(.secondary).padding(4)
    }
    .accessibilityLabel(String(localized: "点群 \(x.count) 点"))
    .onAppear { cloud = PreparedCloud(x: x, y: y, z: z, colors: colors) }
    .onChange(of: identity) { cloud = PreparedCloud(x: x, y: y, z: z, colors: colors) }
  }
}

/// A point cloud centered and measured once, projected per frame into an
/// RGBA bitmap (3×3 pixels per point at the screen's scale).
final class PreparedCloud {
  let x: [Float], y: [Float], z: [Float]
  let colors: [UInt32]
  let extent: Float

  init(x: [Float], y: [Float], z: [Float], colors: [UInt32]) {
    let n = min(x.count, y.count, z.count)
    var cx: Double = 0, cy: Double = 0, cz: Double = 0
    for i in 0..<n { cx += Double(x[i]); cy += Double(y[i]); cz += Double(z[i]) }
    let d = Double(max(n, 1))
    let (mx, my, mz) = (Float(cx / d), Float(cy / d), Float(cz / d))
    var e: Float = 1e-9
    var px = [Float](repeating: 0, count: n), py = px, pz = px
    for i in 0..<n {
      px[i] = x[i] - mx; py[i] = y[i] - my; pz[i] = z[i] - mz
      e = max(e, abs(px[i]), abs(py[i]), abs(pz[i]))
    }
    self.x = px; self.y = py; self.z = pz
    self.colors = colors
    extent = e
  }

  func render(yaw: Double, pitch: Double, zoom: Double, size: CGSize, pixelScale: CGFloat) -> CGImage? {
    let w = Int(size.width * pixelScale), h = Int(size.height * pixelScale)
    guard w > 2, h > 2, !x.isEmpty else { return nil }
    var pixels = [UInt32](repeating: 0, count: w * h)
    let s = Float(Double(min(w, h)) * 0.42 * zoom) / extent
    let (cyw, syw, cp, sp) = (Float(cos(yaw)), Float(sin(yaw)), Float(cos(pitch)), Float(sin(pitch)))
    let (hw, hh) = (Float(w) / 2, Float(h) / 2)
    let n = x.count, nc = colors.count
    pixels.withUnsafeMutableBufferPointer { out in
      guard let buf = out.baseAddress else { return }
      x.withUnsafeBufferPointer { xs in y.withUnsafeBufferPointer { ys in z.withUnsafeBufferPointer { zs in
        colors.withUnsafeBufferPointer { cs in
          for i in 0..<n {
            let rx = xs[i] * cyw + zs[i] * syw
            let rz = -xs[i] * syw + zs[i] * cyw
            let ry = ys[i] * cp - rz * sp
            let sx = Int(hw + rx * s), sy = Int(hh - ry * s)
            guard sx >= 1, sy >= 1, sx < w - 1, sy < h - 1 else { continue }
            let c = i < nc ? cs[i] : 0x4A90E2
            // Premultiplied little-endian BGRA, opaque.
            let px: UInt32 = 0xFF00_0000 | (c & 0x00FF_FFFF)
            var p = buf + (sy - 1) * w + sx - 1
            for _ in 0..<3 {
              p[0] = px; p[1] = px; p[2] = px
              p += w
            }
          }
        }
      } } }
    }
    let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
    guard let provider = CGDataProvider(data: Data(bytes: pixels, count: pixels.count * 4) as CFData) else { return nil }
    return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                   space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info, provider: provider, decode: nil,
                   shouldInterpolate: false, intent: .defaultIntent)
  }
}

/// Data for a UI Canvas module. Known modules are drawn natively; any other
/// module shows its data as formatted JSON.
struct ModuleDisplayView: View {
  let name: String
  let json: String
  var large = false
  var zoom: CGFloat = 1

  var body: some View {
    let value = try? JSONValue.parse(json)
    VStack(alignment: .leading, spacing: 4) {
      if name == "histogram", let bins = value?["bins"]?.array?.compactMap(\.double), !bins.isEmpty {
        let top = bins.max() ?? 1
        GeometryReader { g in
          HStack(alignment: .bottom, spacing: 2) {
            ForEach(Array(bins.enumerated()), id: \.offset) { _, b in
              RoundedRectangle(cornerRadius: 2).fill(Color.accentColor.opacity(0.7))
                .frame(height: max(1, g.size.height * b / max(top, 1e-9)))
                .help("\(b)")
            }
          }
          .frame(maxHeight: .infinity, alignment: .bottom)
        }
      } else {
        Text("モジュール \(name)").font(.system(size: 10 * zoom, weight: .bold)).foregroundStyle(.secondary)
        let text = Text(value?.serialized() ?? json).font(.system(size: (large ? 12 : 10) * zoom, design: .monospaced))
          .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
        if large { ScrollView { text } } else { text.frame(maxHeight: .infinity, alignment: .top).clipped() }
      }
    }
  }
}

/// Edits a sticky note: Markdown text, color, size.
struct NoteEditor: View {
  @Environment(\.dismiss) private var dismiss
  let session: ProjectSession
  let noteId: String
  @State private var text = ""
  @State private var color = "yellow"
  @State private var width = 240.0
  @State private var height = 120.0
  @State private var fontSize = 13.0

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Text("付箋を編集").font(.title2.bold())
      HSplitView {
        TextEditor(text: $text).font(.system(size: 13, design: .monospaced)).frame(minWidth: 220)
        ScrollView {
          MarkdownView(text: text, fontSize: fontSize)
            .frame(maxWidth: .infinity, alignment: .leading).padding(8)
        }
        .background(noteColor(color).opacity(0.2))
        .frame(minWidth: 200)
      }
      .frame(height: 220)
      Text("Markdown（見出し・太字・リスト・表・コード）と数式（$…$、$$…$$）を使えます。付箋は実行には参加しません。").font(.caption).foregroundStyle(.secondary)
      Stepper("文字の大きさ \(Int(fontSize))", value: $fontSize, in: 8...48, step: 1)
      Picker("色", selection: $color) {
        ForEach(NoteColor.all, id: \.id) { c in Text(c.label).tag(c.id) }
      }
      .pickerStyle(.segmented)
      HStack {
        Stepper("幅 \(Int(width))", value: $width, in: 120...800, step: 20)
        Stepper("高さ \(Int(height))", value: $height, in: 60...600, step: 20)
      }
      HStack {
        Button("削除", role: .destructive) { session.removeNote(noteId); dismiss() }
        Spacer()
        Button("キャンセル") { dismiss() }
        Button("適用") { apply(); dismiss() }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 560)
    .onAppear {
      let n = session.note(noteId)
      text = n?["text"]?.string ?? ""
      color = n?["color"]?.string ?? "yellow"
      width = n?["width"]?.double ?? 240
      height = n?["height"]?.double ?? 120
      fontSize = n?["fontSize"]?.double ?? 13
    }
  }

  private func apply() {
    session.updateNote(noteId) { n in
      n["text"] = .string(text)
      n["color"] = .string(color)
      n["width"] = .real(width)
      n["height"] = .real(height)
      n["fontSize"] = fontSize == 13 ? nil : .real(fontSize)
    }
  }
}

/// Edits a group: name, members and the outputs shown on the frame.
struct GroupEditor: View {
  @Environment(\.dismiss) private var dismiss
  let session: ProjectSession
  let scope: [String]
  let groupId: String
  @State private var name = ""
  @State private var color = "blue"
  @State private var members: Set<String> = []
  @State private var outputs: Set<String> = []  // "card\tport"

  var body: some View {
    let ids = session.cards(in: scope).compactMap { $0["id"]?.string }
    VStack(alignment: .leading, spacing: 12) {
      Text("グループを編集").font(.title2.bold())
      TextField("名前", text: $name)
      Picker("色", selection: $color) {
        ForEach(NoteColor.all, id: \.id) { c in Text(c.label).tag(c.id) }
      }
      .pickerStyle(.segmented)
      HStack(alignment: .top) {
        VStack(alignment: .leading) {
          Text("所属する Card").font(.headline)
          List(ids, id: \.self) { id in
            Toggle(session.displayName(key(scope + [id])), isOn: Binding(
              get: { members.contains(id) }, set: { if $0 { members.insert(id) } else { members.remove(id) } }))
          }
        }
        VStack(alignment: .leading) {
          Text("公開する出力").font(.headline)
          List {
            ForEach(members.sorted(), id: \.self) { id in
              ForEach(session.ports(key(scope + [id])).outputs, id: \.self) { p in
                let tag = "\(id)\t\(p)"
                Toggle("\(session.displayName(key(scope + [id]))).\(p)", isOn: Binding(
                  get: { outputs.contains(tag) }, set: { if $0 { outputs.insert(tag) } else { outputs.remove(tag) } }))
              }
            }
          }
        }
      }
      .frame(height: 240)
      Text("グループは見た目のまとまりです。実行順序は変わらず、解除しても Card は残ります。").font(.caption).foregroundStyle(.secondary)
      HStack {
        Button("グループを解除", role: .destructive) { session.ungroup(groupId); dismiss() }
        Spacer()
        Button("キャンセル") { dismiss() }
        Button("適用") { apply(); dismiss() }.keyboardShortcut(.defaultAction)
      }
    }
    .padding(20)
    .frame(width: 600)
    .onAppear {
      let g = session.groups(in: scope).first { $0["id"]?.string == groupId }
      name = g?["name"]?.string ?? ""
      color = g?["color"]?.string ?? "blue"
      members = Set((g?["cards"]?.array ?? []).compactMap { $0.string })
      outputs = Set((g?["outputs"]?.array ?? []).compactMap { o in
        guard let c = o["card"]?.string, let p = o["port"]?.string else { return nil }
        return "\(c)\t\(p)"
      })
    }
  }

  private func apply() {
    session.updateGroup(groupId) { g in
      g["name"] = .string(name)
      g["color"] = .string(color)
      g["cards"] = .array(members.sorted().map { .string($0) })
      let outs: [JSONValue] = outputs.sorted().compactMap { t in
        let parts = t.split(separator: "\t").map(String.init)
        guard parts.count == 2, members.contains(parts[0]) else { return nil }
        return .object([("card", .string(parts[0])), ("port", .string(parts[1]))])
      }
      g["outputs"] = outs.isEmpty ? nil : .array(outs)
    }
  }
}
