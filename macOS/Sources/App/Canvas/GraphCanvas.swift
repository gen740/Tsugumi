import AppKit
import SwiftUI
import TsugumiRPC

/// The canvas of one scope: grid, groups, connections, Cards, sticky notes
/// and output frames, with pan/zoom, selection, wiring and drag & drop.
struct GraphCanvas: View {
  @Environment(\.openWindow) private var openWindow
  @Environment(AppPreferences.self) private var prefs
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab
  @State private var drag: (key: CardKey, offset: CGSize)?
  @State private var pending: (from: (CardKey, String), point: CGPoint)?
  @State private var panStart: (x: Double, y: Double)?
  @State private var marquee: (start: CGPoint, end: CGPoint)?
  @State private var zoomStart: Double?
  @State private var size: CGSize = .zero
  @State private var editingNote: String?
  @State private var editingGroup: String?

  private var scopeKey: CardKey { key(tab.scope) }
  var camera: (x: Double, y: Double, zoom: Double) { session.cameras[scopeKey] ?? (40, 40, 1) }

  var body: some View {
    GeometryReader { geo in
      ZStack(alignment: .topLeading) {
        background
        ForEach(session.groups(in: tab.scope).compactMap { $0["id"]?.string }, id: \.self) { id in
          groupFrame(id)
        }
        wires
        ForEach(cardKeys, id: \.self) { k in cardView(k) }
        ForEach(outputKeys, id: \.self) { k in outputFrame(k) }
        ForEach(noteIds, id: \.self) { id in noteView(id) }
        if let p = pending {
          Path { path in
            path.move(to: screen(portWorld(p.from.0, p.from.1, output: true)))
            path.addLine(to: p.point)
          }
          .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 2, dash: [6, 4]))
          .allowsHitTesting(false)
        }
        if let m = marquee {
          let r = CGRect(x: min(m.start.x, m.end.x), y: min(m.start.y, m.end.y), width: abs(m.end.x - m.start.x), height: abs(m.end.y - m.start.y))
          Rectangle().fill(Color.accentColor.opacity(0.08)).overlay(Rectangle().strokeBorder(Color.accentColor.opacity(0.6)))
            .frame(width: r.width, height: r.height).offset(x: r.minX, y: r.minY)
            .allowsHitTesting(false)
        }
        if let i = tab.selectedConnection, let mid = connectionMid(i) {
          Button { if session.isEditable { session.removeConnection(at: i, in: tab.scope); tab.selectedConnection = nil } } label: {
            Label("切断", systemImage: "scissors").font(.caption)
          }
          .buttonStyle(.borderedProminent).controlSize(.small)
          .disabled(!session.isEditable)
          .position(mid)
        }
      }
      .coordinateSpace(name: "canvas")
      .clipped()
      .overlay(ScrollCatcher { dx, dy, zoomDelta, at in
        if zoomDelta != 0 { zoom(by: 1 + zoomDelta, around: at) } else {
          var c = camera
          c.x += dx
          c.y += dy
          session.cameras[scopeKey] = c
        }
        saveView()
      })
      .gesture(MagnifyGesture().onChanged { v in
        if zoomStart == nil { zoomStart = camera.zoom }
        setZoom((zoomStart ?? 1) * v.magnification, around: CGPoint(x: geo.size.width / 2, y: geo.size.height / 2))
      }.onEnded { _ in
        zoomStart = nil
        saveView()
      })
      .onDeleteCommand { deleteSelection() }
      .dropDestination(for: String.self) { items, loc in
        guard session.isEditable else { return false }
        var placed: [CardKey] = []
        for (i, item) in items.enumerated() {
          guard let id = DefinitionDrag.id(item) else { continue }
          let w = world(loc)
          placed.append(session.addCard(definition: id, in: tab.scope, at: CGPoint(x: w.x + Double(i) * 24, y: w.y + Double(i) * 24)))
        }
        if !placed.isEmpty { tab.selection = Set(placed) }
        return !placed.isEmpty
      }
      .onAppear { size = geo.size }
      .onChange(of: geo.size) { size = geo.size }
      .onReceive(NotificationCenter.default.publisher(for: .canvasFitAll)) { n in
        if (n.object as? UUID) == tab.id || n.object == nil { fitAll() }
      }
      .onReceive(NotificationCenter.default.publisher(for: .canvasAddNote)) { n in
        if (n.object as? UUID) == tab.id { addNote() }
      }
      .sheet(item: Binding(get: { editingNote.map(IdBox.init) }, set: { editingNote = $0?.id })) { box in
        NoteEditor(session: session, noteId: box.id)
      }
      .sheet(item: Binding(get: { editingGroup.map(IdBox.init) }, set: { editingGroup = $0?.id })) { box in
        GroupEditor(session: session, scope: tab.scope, groupId: box.id)
      }
    }
  }

  // MARK: - Data

  private var cardKeys: [CardKey] { session.cards(in: tab.scope).compactMap { $0["id"]?.string }.map { key(tab.scope + [$0]) } }

  private var outputKeys: [CardKey] {
    cardKeys.filter { session.cardSetting($0, "canvasEnabled")?.bool == true && session.displayFrame($0)?["hidden"]?.bool != true }
  }

  private var noteIds: [String] {
    (session.layout["notes"]?.array ?? []).filter { ($0["scope"]?.string ?? "") == scopeKey }.compactMap { $0["id"]?.string }
  }

  func origin(_ k: CardKey) -> CGPoint {
    var p = session.position(k)
    if let d = drag, d.key == k || (tab.selection.contains(k) && tab.selection.contains(d.key)) {
      p.x += d.offset.width / camera.zoom
      p.y += d.offset.height / camera.zoom
    }
    return p
  }

  func screen(_ w: CGPoint) -> CGPoint { CGPoint(x: w.x * camera.zoom + camera.x, y: w.y * camera.zoom + camera.y) }
  func world(_ s: CGPoint) -> CGPoint { CGPoint(x: (s.x - camera.x) / camera.zoom, y: (s.y - camera.y) / camera.zoom) }

  private func portWorld(_ k: CardKey, _ port: String, output: Bool) -> CGPoint {
    let o = origin(k), p = session.geometry(k).port(port, output: output)
    return CGPoint(x: o.x + p.x, y: o.y + p.y)
  }

  private func wireEnds(_ c: JSONValue) -> (CGPoint, CGPoint, CardKey)? {
    guard let fc = c["from"]?["card"]?.string, let fp = c["from"]?["port"]?.string,
          let tc = c["to"]?["card"]?.string, let tp = c["to"]?["port"]?.string else { return nil }
    let fk = key(tab.scope + [fc]), tk = key(tab.scope + [tc])
    return (screen(portWorld(fk, fp, output: true)), screen(portWorld(tk, tp, output: false)), fk)
  }

  private func curve(_ a: CGPoint, _ b: CGPoint) -> Path {
    var path = Path()
    path.move(to: a)
    let dx = max(40, abs(b.x - a.x) / 2)
    path.addCurve(to: b, control1: CGPoint(x: a.x + dx, y: a.y), control2: CGPoint(x: b.x - dx, y: b.y))
    return path
  }

  private func connectionMid(_ i: Int) -> CGPoint? {
    let conns = session.connections(in: tab.scope)
    guard i < conns.count, let (a, b, _) = wireEnds(conns[i]) else { return nil }
    return curve(a, b).trimmedPath(from: 0.49, to: 0.51).boundingRect.center
  }

  // MARK: - Layers

  private var background: some View {
    Canvas { ctx, sz in
      guard prefs.showGrid else { return }
      let step = prefs.gridSize * camera.zoom
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
    .contentShape(Rectangle())
    .gesture(backgroundDrag)
    .onTapGesture(count: 2, coordinateSpace: .named("canvas")) { _ in fitAll() }
    .onTapGesture(coordinateSpace: .named("canvas")) { loc in backgroundTap(loc) }
    .contextMenu {
      Button("付箋を追加") { addNote() }
      Button("全体表示") { fitAll() }
      Button("整列") { session.arrange(tab.scope) }
      if !tab.selection.isEmpty { Button("選択をグループ化") { groupSelection() }.disabled(!session.connected) }
    }
  }

  @ViewBuilder private var wires: some View {
    let conns = session.connections(in: tab.scope)
    let animate = prefs.animateLines && session.runActive
    SwiftUI.TimelineView(.animation(minimumInterval: 1 / 30, paused: !animate)) { t in
      Canvas { ctx, _ in
        let phase = animate ? -CGFloat(t.date.timeIntervalSinceReferenceDate * 30).truncatingRemainder(dividingBy: 20) : 0
        for (i, c) in conns.enumerated() {
          guard let (a, b, from) = wireEnds(c) else { continue }
          let path = curve(a, b)
          let selected = i == tab.selectedConnection
          let active = session.cardStates[from]?.state == .running || session.cardStates[from]?.state == .succeeded && session.runActive
          let color: Color = selected ? .accentColor : (active ? .orange : .secondary.opacity(0.8))
          ctx.stroke(path, with: .color(color),
                     style: StrokeStyle(lineWidth: selected ? 3 : 1.7, lineCap: .round, dash: active && animate ? [8, 6] : [], dashPhase: phase))
        }
      }
    }
    .allowsHitTesting(false)
  }

  private func cardView(_ k: CardKey) -> some View {
    let g = session.geometry(k)
    return CardNode(session: session, cardKey: k, selected: tab.selection.contains(k),
                    onPortDrag: { port, pt in if session.isEditable { pending = ((k, port), pt) } },
                    onPortDrop: { port, pt in finishConnect(from: (k, port), at: pt) })
      .scaleEffect(camera.zoom, anchor: .topLeading)
      .place(at: screen(origin(k)), size: g.size, zoom: camera.zoom)
      .gesture(cardDrag(k))
      .onTapGesture(count: 2) { open(k) }
      .onTapGesture { select(k) }
      .contextMenu { CardMenu(workspace: workspace, session: session, tab: tab, cardKey: k, onGroup: groupSelection) }
  }

  private func groupFrame(_ id: String) -> some View {
    let g = session.groups(in: tab.scope).first { $0["id"]?.string == id } ?? .object([])
    let members = (g["cards"]?.array ?? []).compactMap { $0.string }.map { key(tab.scope + [$0]) }.filter { session.instance($0) != nil }
    var rect = CGRect.null
    for m in members { rect = rect.union(CGRect(origin: origin(m), size: session.geometry(m).size)) }
    if rect.isNull { rect = CGRect(x: 0, y: 0, width: 200, height: 100) }
    rect = rect.insetBy(dx: -18, dy: -18)
    rect.origin.y -= 22
    rect.size.height += 22
    return GroupFrameView(session: session, group: g, scope: tab.scope)
      .frame(width: rect.width, height: rect.height)
      .scaleEffect(camera.zoom, anchor: .topLeading)
      .place(at: screen(rect.origin), size: rect.size, zoom: camera.zoom)
      .onTapGesture(count: 2) { editingGroup = id }
      .onTapGesture { tab.selection = Set(members) }
      .contextMenu {
        Button("グループを編集…") { editingGroup = id }
        Button("メンバーを選択") { tab.selection = Set(members) }
        Divider()
        Button("グループを解除（Card は残ります）") { session.ungroup(id) }
      }
  }

  private func noteView(_ id: String) -> some View {
    let n = (session.layout["notes"]?.array ?? []).first { $0["id"]?.string == id } ?? .object([])
    let at = CGPoint(x: n["x"]?.double ?? 0, y: n["y"]?.double ?? 0)
    let sz = CGSize(width: n["width"]?.double ?? 240, height: n["height"]?.double ?? 120)
    return StickyNoteView(note: n, onResize: { d in
      session.updateNote(id) { v in
        v["width"] = .real(max(120, sz.width + d.width / camera.zoom).rounded())
        v["height"] = .real(max(60, sz.height + d.height / camera.zoom).rounded())
      }
    })
    .frame(width: sz.width, height: sz.height)
    .scaleEffect(camera.zoom, anchor: .topLeading)
    .place(at: screen(at), size: sz, zoom: camera.zoom)
    .gesture(DragGesture(minimumDistance: 3).onEnded { v in
      session.updateNote(id) { n in
        n["x"] = .real((at.x + v.translation.width / camera.zoom).rounded())
        n["y"] = .real((at.y + v.translation.height / camera.zoom).rounded())
      }
    })
    .onTapGesture(count: 2) { editingNote = id }
    .contextMenu {
      Button("付箋を編集…") { editingNote = id }
      Menu("色") {
        ForEach(NoteColor.all, id: \.id) { c in Button(c.label) { session.updateNote(id) { $0["color"] = .string(c.id) } } }
      }
      Divider()
      Button("付箋を削除", role: .destructive) { session.removeNote(id) }
    }
  }

  private func outputFrame(_ k: CardKey) -> some View {
    let o = origin(k)
    let f = session.displayFrame(k)
    let sz = CGSize(width: f?["width"]?.double ?? 320, height: f?["height"]?.double ?? 170)
    let at = CGPoint(x: f?["x"]?.double ?? o.x, y: f?["y"]?.double ?? (o.y + session.geometry(k).height + 18))
    return OutputFrameView(session: session, cardKey: k,
                           onExpand: { openWindow(id: "visual", value: CardRef(path: session.identity.url.path, card: k)) },
                           onClose: { session.updateDisplayFrame(k) { $0["hidden"] = .bool(true) } },
                           onResize: { d in
                             session.updateDisplayFrame(k) { v in
                               v["x"] = .real(at.x.rounded())
                               v["y"] = .real(at.y.rounded())
                               v["width"] = .real(max(180, sz.width + d.width / camera.zoom).rounded())
                               v["height"] = .real(max(100, sz.height + d.height / camera.zoom).rounded())
                             }
                           })
      .frame(width: sz.width, height: sz.height)
      .scaleEffect(camera.zoom, anchor: .topLeading)
      .place(at: screen(at), size: sz, zoom: camera.zoom)
      .gesture(DragGesture(minimumDistance: 3).onEnded { v in
        session.updateDisplayFrame(k) { d in
          d["x"] = .real((at.x + v.translation.width / camera.zoom).rounded())
          d["y"] = .real((at.y + v.translation.height / camera.zoom).rounded())
          d["width"] = .real(sz.width)
          d["height"] = .real(sz.height)
        }
      })
      .contextMenu {
        Button("拡大して表示") { openWindow(id: "visual", value: CardRef(path: session.identity.url.path, card: k)) }
        Button("チャットに参照を追加") { AppModel.shared.chat(for: session).addReference(.display(k)) }
        Divider()
        Button("枠を削除（Card は残ります）") { session.updateDisplayFrame(k) { $0["hidden"] = .bool(true) } }
      }
  }

  // MARK: - Gestures

  private var backgroundDrag: some Gesture {
    DragGesture(minimumDistance: 2, coordinateSpace: .named("canvas"))
      .onChanged { v in
        if marquee != nil || (panStart == nil && NSEvent.modifierFlags.contains(.shift)) {
          marquee = (v.startLocation, v.location)
          return
        }
        if panStart == nil { panStart = (camera.x, camera.y) }
        var c = camera
        c.x = panStart!.x + v.translation.width
        c.y = panStart!.y + v.translation.height
        session.cameras[scopeKey] = c
      }
      .onEnded { v in
        if let m = marquee {
          let a = world(m.start), b = world(v.location)
          let r = CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(b.x - a.x), height: abs(b.y - a.y))
          tab.selection = Set(cardKeys.filter { r.intersects(CGRect(origin: session.position($0), size: session.geometry($0).size)) })
          marquee = nil
        }
        panStart = nil
        saveView()
      }
  }

  private func cardDrag(_ k: CardKey) -> some Gesture {
    DragGesture(minimumDistance: 3, coordinateSpace: .named("canvas"))
      .onChanged { v in
        if !tab.selection.contains(k) { tab.selection = [k] }
        drag = (k, v.translation)
      }
      .onEnded { v in
        let moved = tab.selection.contains(k) ? tab.selection : [k]
        let dx = v.translation.width / camera.zoom, dy = v.translation.height / camera.zoom
        drag = nil
        // Layout only: no computation is re-resolved for a move.
        session.edit { s in
          for m in moved {
            let p = s.position(m)
            var x = p.x + dx, y = p.y + dy
            if prefs.showGrid {
              x = (x / prefs.gridSize).rounded() * prefs.gridSize
              y = (y / prefs.gridSize).rounded() * prefs.gridSize
            }
            s.setPosition(m, CGPoint(x: x, y: y))
          }
        }
      }
  }

  private func backgroundTap(_ loc: CGPoint) {
    // A click near a wire selects it; elsewhere clears the selection.
    let conns = session.connections(in: tab.scope)
    for (i, c) in conns.enumerated() {
      guard let (a, b, _) = wireEnds(c) else { continue }
      let hit = curve(a, b).strokedPath(StrokeStyle(lineWidth: 10)).contains(loc)
      if hit {
        tab.selectedConnection = i
        tab.selection = []
        return
      }
    }
    tab.selection.removeAll()
    tab.selectedConnection = nil
  }

  private func select(_ k: CardKey) {
    if NSEvent.modifierFlags.contains(.shift) || NSEvent.modifierFlags.contains(.command) {
      if tab.selection.contains(k) { tab.selection.remove(k) } else { tab.selection.insert(k) }
    } else {
      tab.selection = [k]
    }
    tab.selectedConnection = nil
  }

  private func open(_ k: CardKey) {
    switch session.kind(k) {
    case .subgraph where session.instance(k)?["canvas"] != nil: tab.enter(path(k))
    case .draft: openWindow(id: "draft", value: CardRef(path: session.identity.url.path, card: k))
    default: openWindow(id: "card", value: CardRef(path: session.identity.url.path, card: k))
    }
  }

  private func finishConnect(from: (CardKey, String), at pt: CGPoint) {
    pending = nil
    guard session.isEditable else { return }
    let w = world(pt)
    for k in cardKeys where k != from.0 {
      for port in session.geometry(k).inputs {
        let p = portWorld(k, port, output: false)
        if hypot(p.x - w.x, p.y - w.y) < 16 / camera.zoom + 6 {
          session.connect(from: from, to: (k, port), in: tab.scope)
          return
        }
      }
    }
  }

  private func deleteSelection() {
    guard session.isEditable else {
      session.notice = "実行中は Card を削除できません。"
      return
    }
    if let i = tab.selectedConnection {
      session.removeConnection(at: i, in: tab.scope)
      tab.selectedConnection = nil
      return
    }
    session.deleteCards(tab.selection, in: tab.scope)
    tab.selection.removeAll()
  }

  private func groupSelection() {
    guard !tab.selection.isEmpty else { return }
    editingGroup = session.addGroup(tab.selection, in: tab.scope)
  }

  private func addNote() {
    let center = world(CGPoint(x: size.width / 2 - 120, y: size.height / 2 - 60))
    editingNote = session.addNote(in: tab.scope, at: center)
  }

  // MARK: - Camera

  private func saveView() { session.saveViewSoon(scope: tab.scope, selection: tab.selection) }

  private func setZoom(_ z: Double, around p: CGPoint) {
    var c = camera
    let nz = max(0.2, min(3, z))
    // Keep the point under the cursor fixed.
    c.x = p.x - (p.x - c.x) * nz / c.zoom
    c.y = p.y - (p.y - c.y) * nz / c.zoom
    c.zoom = nz
    session.cameras[scopeKey] = c
  }

  private func zoom(by f: Double, around p: CGPoint?) {
    setZoom(camera.zoom * f, around: p ?? CGPoint(x: size.width / 2, y: size.height / 2))
  }

  func fitAll() {
    var rect = CGRect.null
    for k in cardKeys { rect = rect.union(CGRect(origin: session.position(k), size: session.geometry(k).size)) }
    for id in noteIds {
      if let n = (session.layout["notes"]?.array ?? []).first(where: { $0["id"]?.string == id }) {
        rect = rect.union(CGRect(x: n["x"]?.double ?? 0, y: n["y"]?.double ?? 0, width: n["width"]?.double ?? 240, height: n["height"]?.double ?? 120))
      }
    }
    guard !rect.isNull, size.width > 0 else { return }
    rect = rect.insetBy(dx: -60, dy: -80)
    let z = max(0.2, min(1.5, min(size.width / rect.width, size.height / rect.height)))
    session.cameras[scopeKey] = (Double(size.width / 2 - rect.midX * z), Double(size.height / 2 - rect.midY * z), Double(z))
    saveView()
  }
}

struct IdBox: Identifiable { let id: String }

extension CGRect {
  var center: CGPoint { CGPoint(x: midX, y: midY) }
}

extension View {
  /// Places a view by its top-left corner in canvas coordinates.
  func place(at topLeft: CGPoint, size: CGSize, zoom: Double) -> some View {
    position(x: topLeft.x + size.width * zoom / 2, y: topLeft.y + size.height * zoom / 2)
      .frame(width: size.width, height: size.height, alignment: .topLeading)
  }
}

/// Two-finger scroll pans the canvas; ⌘-scroll zooms around the pointer.
struct ScrollCatcher: NSViewRepresentable {
  let onScroll: (Double, Double, Double, CGPoint?) -> Void

  func makeNSView(context: Context) -> NSView { CatcherView(onScroll: onScroll) }
  func updateNSView(_ nsView: NSView, context: Context) { (nsView as? CatcherView)?.onScroll = onScroll }

  final class CatcherView: NSView {
    var onScroll: (Double, Double, Double, CGPoint?) -> Void
    init(onScroll: @escaping (Double, Double, Double, CGPoint?) -> Void) {
      self.onScroll = onScroll
      super.init(frame: .zero)
    }
    required init?(coder: NSCoder) { fatalError() }
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? {
      // Only take scroll events; clicks fall through to SwiftUI.
      NSApp.currentEvent?.type == .scrollWheel ? self : nil
    }
    override func scrollWheel(with e: NSEvent) {
      let p = convert(e.locationInWindow, from: nil)
      if e.modifierFlags.contains(.command) { onScroll(0, 0, Double(e.scrollingDeltaY) * 0.01, p) }
      else { onScroll(Double(e.scrollingDeltaX), Double(e.scrollingDeltaY), 0, nil) }
    }
  }
}
