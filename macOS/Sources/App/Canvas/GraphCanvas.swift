import AppKit
import SwiftUI
import TsugumiRPC

/// The canvas of one scope: grid, groups, connections, Cards, sticky notes
/// and output frames, with pan/zoom, selection, wiring and drag & drop.
/// Inside a Card in Card, the outer Card's inputs and outputs are drawn as
/// bars above and below the inner graph.
struct GraphCanvas: View {
  @Environment(\.openWindow) private var openWindow
  @Environment(\.displayScale) private var displayScale
  @Environment(AppPreferences.self) private var prefs
  @Bindable var workspace: Workspace
  let session: ProjectSession
  @Bindable var tab: WorkflowTab
  @State private var drag: (key: CardKey, offset: CGSize)?
  @State private var boundaryDrag: (input: Bool, offset: CGSize)?
  /// A wire being drawn from a port: from an output (to an input), or from
  /// an input back to an output (`toInput` false).
  @State private var pending: (from: (CardKey, String), point: CGPoint, fromOutput: Bool)?
  @State private var panStart: (x: Double, y: Double)?
  @State private var marquee: (start: CGPoint, end: CGPoint)?
  @State private var zoomStart: Double?
  @State private var size: CGSize = .zero
  @State private var editingNote: String?
  @State private var editingGroup: String?
  @State private var hover: HoverTarget?
  @State private var hoverAt: CGPoint = .zero
  /// Where the pointer last was over the canvas (a menu adds there).
  @State private var pointer: CGPoint = .zero
  @State private var bubbleSize = CGSize(width: 300, height: 120)
  @State private var hoverTask: Task<Void, Never>?
  @State private var findText: String?
  @FocusState private var findFocused: Bool
  /// The canvas has the keyboard focus: its single-key shortcuts work.
  @FocusState private var canvasFocused: Bool

  private var scopeKey: CardKey { key(tab.scope) }
  /// An output frame being dragged (screen translation).
  @State private var frameDrag: (card: CardKey, by: CGSize)?
  /// A sticky note being dragged (screen translation); saved once, on release.
  @State private var noteDrag: (id: String, by: CGSize)?
  /// The selection being moved from one of its notes or frames (screen translation); saved once, on release.
  @State private var selectionDrag: CGSize?
  /// Laid out again when this changes (the zoom has stopped, or a pan went far: CanvasPanHost).
  @State private var redrawn = 0
  /// Counts the passes of the body (CanvasPanHost takes the content of a new pass only,
  /// and the camera of the latest one, for what its views report in its terms: `live`).
  private final class Passes {
    var count = 0
    var base = CanvasCamera()
    func next(_ base: CanvasCamera) -> Int {
      self.base = base
      count += 1
      return count
    }
  }
  /// The wires' paths as last routed. Routing is by far the costliest part of
  /// a pass (seconds, with a couple of hundred wires), and most passes change
  /// nothing it depends on (the Cards', frames' and ports' places). When it is
  /// quick it is done in the pass, so the wires of a Card being dragged follow
  /// it; when it is not, it is done off the main thread: until it is, the
  /// wires whose ends have not moved keep their paths and the others are
  /// plain curves, and `onRouted` then has the canvas drawn with the new ones.
  private final class Routes {
    private struct Inputs: Equatable {
      struct Ends: Equatable {
        let a: CGPoint
        let b: CGPoint
      }
      let flow: FlowDirection
      let obstacles: [CGRect]
      let ends: [Ends]

      func route() -> [Path] {
        var router = WireRouter(flow: flow, obstacles: obstacles)
        return router.routeAll(ends.map { ($0.a, $0.b) })
      }
    }
    /// What `routed` was made from, and how long that took.
    private var made: Inputs?
    private var routed: [Path] = []
    private var seconds = 0.0
    private var wanted: Inputs?
    private var working = false
    private var onRouted: (() -> Void)?
    /// Within this, routing is done in the pass.
    private static let quick = 0.006

    func paths(flow: FlowDirection, obstacles: [CGRect], ends: [(a: CGPoint, b: CGPoint)], onRouted: @escaping () -> Void) -> [Path] {
      let inputs = Inputs(flow: flow, obstacles: obstacles, ends: ends.map { Inputs.Ends(a: $0.a, b: $0.b) })
      if inputs == made { return routed }
      // Not routed yet, a few wires are taken to be quick; after that, what the last routing took says.
      if made == nil ? ends.count <= 40 : seconds < Self.quick {
        let start = ContinuousClock.now
        routed = inputs.route()
        seconds = Self.seconds(ContinuousClock.now - start)
        made = inputs
        wanted = nil
        return routed
      }
      wanted = inputs
      self.onRouted = onRouted
      work()
      let plain = WireRouter(flow: flow, obstacles: [])
      return inputs.ends.indices.map { i in
        if let made, i < made.ends.count, i < routed.count, made.ends[i] == inputs.ends[i] { return routed[i] }
        return plain.curve(inputs.ends[i].a, inputs.ends[i].b)
      }
    }

    /// One routing at a time; what is wanted by the time it ends, if it changed meanwhile, is routed next.
    private func work() {
      guard !working, let inputs = wanted, inputs != made else { return }
      working = true
      Task.detached(priority: .userInitiated) {
        let start = ContinuousClock.now
        let paths = inputs.route()
        let took = Self.seconds(ContinuousClock.now - start)
        await MainActor.run {
          self.routed = paths
          self.made = inputs
          self.seconds = took
          self.working = false
          self.work()
          self.onRouted?()
        }
      }
    }

    private nonisolated static func seconds(_ d: Duration) -> Double {
      Double(d.components.seconds) + Double(d.components.attoseconds) / 1e18
    }
  }
  @State private var routes = Routes()
  /// How far beyond the canvas, on every side, the content is laid out (CanvasPanHost).
  private static let overscan: CGFloat = 300
  @State private var passes = Passes()
  /// The camera as it is now, read without observing it: the content is laid
  /// out for the camera of when the body last ran, and CanvasPanHost shows it
  /// through the camera of the moment.
  var camera: (x: Double, y: Double, zoom: Double) { session.cameraUnobserved(scopeKey) ?? (40, 40, 1) }
  private var cam: CanvasCamera { CanvasCamera(x: camera.x, y: camera.y, zoom: camera.zoom) }

  var body: some View {
    let routed = routedWires()
    let _ = redrawn
    GeometryReader { geo in
      ZStack(alignment: .topLeading) {
        background
        CanvasPanHost(session: session, scope: scopeKey, base: cam, pass: passes.next(cam), margin: Self.overscan,
                      solid: solid(routed), onSettled: { redrawn += 1 }) {
          // The wires are drawn over the whole of it; the rest is placed in the canvas's own part.
          let m = Self.overscan
          let toLayer = cam.transform.concatenating(CGAffineTransform(translationX: m, y: m))
          ZStack(alignment: .topLeading) {
            ZStack(alignment: .topLeading) {
              ForEach(session.groups(in: tab.scope).compactMap { $0["id"]?.string }, id: \.self) { id in
                groupFrame(id)
              }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .padding(m)
            WireLayer(wires: routed.map { WireDraw(path: $0.path.applying(toLayer), style: $0.style, dotted: $0.dotted, thread: $0.thread) },
                      animate: prefs.animateLines && session.runActive, zoom: camera.zoom, presences: session.presences)
            if tab.showNixReferences { nixLayer(transform: toLayer) }
            ZStack(alignment: .topLeading) { panned(routed) }
              .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
              .coordinateSpace(name: "canvas")
              .padding(m)
          }
        }
        // In the view's own space (where the pointer is), not the canvas's.
        if let p = pending {
          Path { path in
            path.move(to: screen(portWorld(p.from.0, p.from.1, output: p.fromOutput)))
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
        if let hover, !interacting {
          HoverBubble(content: hoverContent(hover))
            .fixedSize()
            .onGeometryChange(for: CGSize.self) { $0.size } action: { bubbleSize = $0 }
            .position(bubblePosition(in: geo.size))
            .allowsHitTesting(false)
            .transition(.opacity)
        }
        if findText != nil { findBox.padding(.top, 52).frame(maxWidth: .infinity, alignment: .top) }
        // ⌘I: details of the selected Card without the pointer.
        Button("") { showDetailsForSelection() }
          .shortcut(.showDetails)
          .opacity(0).frame(width: 0, height: 0).accessibilityHidden(true)
      }
      .coordinateSpace(name: "canvas")
      .clipped()
      .onContinuousHover(coordinateSpace: .named("canvas")) { phase in
        switch phase {
        case .active(let p):
          pointer = p
          trackHover(p, routed: routed)
        case .ended: clearHover()
        }
      }
      .overlay(ScrollCatcher { dx, dy, zoomDelta, at in
        clearHover()
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
        // Pinch zooms around where the fingers are.
        setZoom((zoomStart ?? 1) * v.magnification, around: v.startLocation)
      }.onEnded { _ in
        zoomStart = nil
        saveView()
      })
      .onDeleteCommand { deleteSelection() }
      // Keyboard focus for the single-key shortcuts (S, A, L, …): taken by a
      // click on the canvas, given up to any text field.
      .focusable()
      .focusEffectDisabled()
      .focused($canvasFocused)
      .focusedSceneValue(\.canvasFocused, canvasFocused)
      .simultaneousGesture(TapGesture().onEnded { canvasFocused = true })
      .onChange(of: canvasFocused, initial: true) { _, f in UIScript.shared.canvasFocused = f }
      .onKeyPress(keys: [.delete, .deleteForward]) { _ in
        deleteSelection()
        return .handled
      }
      .dropDestination(for: String.self) { items, loc in
        guard session.isEditable else { return false }
        var placed: [CardKey] = []
        var accepted = false
        for (i, item) in items.enumerated() {
          let w = world(loc)
          let at = CGPoint(x: w.x + Double(i) * 24, y: w.y + Double(i) * 24)
          if let t = DefinitionDrag.template(item) {
            placed.append(session.addCard(template: t, in: tab.scope, at: at))
          } else if let id = DefinitionDrag.id(item) {
            placed.append(session.placeDefinition(id, at: at))
          } else if item.hasPrefix(DefinitionDrag.externalDefinitionPrefix) {
            let parts = item.dropFirst(DefinitionDrag.externalDefinitionPrefix.count).split(separator: "\n").map(String.init)
            guard parts.count == 2 else { continue }
            Task { if let k = await session.placeDefinitionCopy(prefix: parts[0], id: parts[1], in: tab.scope, at: at) { tab.selection = [k] } }
            accepted = true
          } else if item.hasPrefix(DefinitionDrag.externalPrefix) {
            let from = String(item.dropFirst(DefinitionDrag.externalPrefix.count))
            Task { if let k = await session.placeExternalCopy(from: from, in: tab.scope, at: at) { tab.selection = [k] } }
            accepted = true
          }
        }
        if !placed.isEmpty { tab.selection = Set(placed) }
        return !placed.isEmpty || accepted
      }
      .onAppear {
        size = geo.size
        fitIfNew()
      }
      // A scope shown for the first time (no saved camera) shows its whole flow
      // as soon as its Cards are there.
      .onChange(of: cardKeys.count) { fitIfNew() }
      .onChange(of: tab.revealNixCard) { _, card in
        if let card { reveal(card); tab.revealNixCard = nil }
      }
      .onChange(of: geo.size) { size = geo.size }
      .onReceive(NotificationCenter.default.publisher(for: .canvasFitAll)) { n in
        if (n.object as? UUID) == tab.id || n.object == nil { fitAll() }
      }
      .onReceive(NotificationCenter.default.publisher(for: .canvasAddNote)) { n in
        if (n.object as? UUID) == tab.id { addNote() }
      }
      .onReceive(NotificationCenter.default.publisher(for: .canvasAddBuiltin)) { n in
        guard (n.object as? UUID) == tab.id, let raw = n.userInfo?["template"] as? String, let t = DefinitionTemplate(rawValue: raw) else { return }
        addBuiltin(t, at: world(CGPoint(x: size.width / 2 - 120, y: size.height / 2 - 60)))
      }
      .modifier(CommandHandlers(canvas: self))
      .sheet(item: Binding(get: { editingNote.map(IdBox.init) }, set: { editingNote = $0?.id })) { box in
        NoteEditor(session: session, noteId: box.id)
      }
      .sheet(item: Binding(get: { editingGroup.map(IdBox.init) }, set: { editingGroup = $0?.id })) { box in
        GroupEditor(session: session, scope: tab.scope, groupId: box.id)
      }
    }
  }

  /// What moves with the canvas, above the wires: laid out for the camera of this pass (CanvasPanHost).
  @ViewBuilder private func panned(_ routed: [RoutedWire]) -> some View {
    if let b = boundary { boundaryBars(b) }
    ForEach(cardKeys, id: \.self) { k in cardView(k) }
    ForEach(outputKeys, id: \.self) { k in outputFrame(k) }
    // The link's ports, where the link meets the Card and the frame.
    ForEach(outputKeys, id: \.self) { k in
      let ends = displayLinkEnds(card: outlineRect(k), frame: frameRect(k))
      ForEach([ends.a, ends.b], id: \.x) { p in
        Circle().fill(Color.purple.opacity(0.75))
          .frame(width: 7, height: 7)
          .position(screen(p))
          .allowsHitTesting(false)
      }
    }
    ForEach(noteIds, id: \.self) { id in noteView(id) }
    // The Agent at work: threads being drawn and the tsugumi.
    // (one bird per helper while the Agent has work delegated)
    ForEach(Array(session.presences.enumerated()), id: \.offset) { _, p in
      // One Tsugumi: while the guide's bird is out here (demo mode), the Agent's bird is not drawn too.
      PresenceLayer(presence: p, geometry: presenceGeometry(routed),
                    avatar: GuideController.shared.birdAway(session.identity.url.path) ? .none : prefs.agentAvatar,
                    lead: p === session.presence && !session.helperPresences.isEmpty)
    }
    if let i = tab.selectedConnection, let mid = routed.first(where: { $0.connection == i })?.path.applying(cam.transform).midPoint {
      Button { if session.isEditable { session.removeConnection(at: i, in: tab.scope); tab.selectedConnection = nil } } label: {
        Label("切断", systemImage: "scissors").font(.caption)
      }
      .buttonStyle(.borderedProminent).controlSize(.small)
      .disabled(!session.isEditable)
      .position(mid)
    }
  }

  /// A point the content reports (in the terms of the pass that laid it out) as the canvas shows it now.
  private func live(_ p: CGPoint) -> CGPoint {
    let base = passes.base, now = camera
    let scale = now.zoom / base.zoom
    return CGPoint(x: p.x * scale + now.x - base.x * scale, y: p.y * scale + now.y - base.y * scale)
  }

  /// What of the content takes the mouse (world units, with room for ports and handles): CanvasPanHost.
  private func solid(_ routed: [RoutedWire]) -> [CGRect] {
    var rects = cardKeys.map { cardRect($0).insetBy(dx: -14, dy: -14) }
    rects += outputKeys.map { frameRect($0).insetBy(dx: -12, dy: -12) }
    rects += session.notes(in: tab.scope).map { noteRect($0).insetBy(dx: -12, dy: -12) }
    for g in session.groups(in: tab.scope) {
      let members = (g["cards"]?.array ?? []).compactMap { $0.string }.map { key(tab.scope + [$0]) }.filter { session.instance($0) != nil }
      var rect = members.reduce(CGRect.null) { $0.union(cardRect($1)) }
      if rect.isNull { rect = CGRect(x: 0, y: 0, width: 200, height: 100) }
      rects.append(rect.insetBy(dx: -18, dy: -18).insetBy(dx: 0, dy: -11).offsetBy(dx: 0, dy: -11))
    }
    if let b = boundary { rects += [b.top, b.bottom] }
    // The button that cuts the selected wire, at its middle.
    if let i = tab.selectedConnection, let mid = routed.first(where: { $0.connection == i })?.path.midPoint {
      rects.append(CGRect(x: mid.x, y: mid.y, width: 0, height: 0).insetBy(dx: -60 / camera.zoom, dy: -20 / camera.zoom))
    }
    return rects
  }

  private var interacting: Bool { drag != nil || pending != nil || marquee != nil || panStart != nil || zoomStart != nil || noteDrag != nil || selectionDrag != nil }

  // MARK: - Data

  private var cardKeys: [CardKey] { session.cards(in: tab.scope).compactMap { $0["id"]?.string }.map { key(tab.scope + [$0]) } }

  private var outputKeys: [CardKey] {
    session.framedCards(in: tab.scope)
  }

  private var noteIds: [String] {
    session.notes(in: tab.scope).compactMap { $0["id"]?.string }
  }

  func origin(_ k: CardKey) -> CGPoint {
    var p = session.placedPosition(k)
    if let d = drag, d.key == k || (tab.selection.contains(k) && tab.selection.contains(d.key)) {
      p.x += d.offset.width / camera.zoom
      p.y += d.offset.height / camera.zoom
    }
    if let by = selectionDrag, tab.selection.contains(k) {
      p.x += by.width / camera.zoom
      p.y += by.height / camera.zoom
    }
    return p
  }

  func screen(_ w: CGPoint) -> CGPoint { cam.screen(w) }
  func world(_ s: CGPoint) -> CGPoint { cam.world(s) }

  private func cardRect(_ k: CardKey) -> CGRect { CGRect(origin: origin(k), size: session.geometry(k).size) }

  private func portWorld(_ k: CardKey, _ port: String, output: Bool) -> CGPoint {
    let o = origin(k), p = session.geometry(k).port(port, output: output)
    return CGPoint(x: o.x + p.x, y: o.y + p.y)
  }

  /// A Card's outline in world units (routing Cards are smaller than their box).
  private func outlineRect(_ k: CardKey) -> CGRect {
    let o = origin(k), r = session.geometry(k).outlineRect
    return r.offsetBy(dx: o.x, dy: o.y)
  }

  /// Output frame of a Card (world units): saved place, or beside the Card.
  private func frameRect(_ k: CardKey) -> CGRect {
    let r = session.displayRect(k, origin: origin(k))
    guard let by = selectionMove, tab.selectedFrames.contains(k), !session.frameFollowsCard(k, moving: tab.selection) else { return r }
    return r.offsetBy(dx: by.width / camera.zoom, dy: by.height / camera.zoom)
  }

  private func noteRect(_ n: JSONValue) -> CGRect { session.noteRect(n) }

  /// The selection's move in progress (screen translation), from whichever of its items is dragged.
  private var selectionMove: CGSize? { selectionDrag ?? drag?.offset }

  private var nixLinks: [NixLink] {
    guard tab.showNixReferences else { return [] }
    return visibleNixLinks(nixReferences(session), scope: tab.scope, visible: Set(cardKeys))
  }

  private func nixLayer(transform: CGAffineTransform) -> some View {
    let links = nixLinks
    let active = activeNixLinks(links, hovered: tab.hoveredNixCard, selected: tab.selectedNixReference)
    let highlighted = Set(active.flatMap { [$0.source, $0.target] })
    return Canvas { context, _ in
      for link in links {
        let a = cardRect(link.source), b = cardRect(link.target)
        let selected = active.contains { $0.reference.id == link.reference.id }
        let color = (link.reference.valid ? Color.purple : Color.orange)
          .opacity(tab.hoveredNixCard != nil && !selected ? 0.2 : 0.9)
        context.stroke(nixCurve(a, b).applying(transform), with: .color(color),
                       style: StrokeStyle(lineWidth: selected ? 3 : 1.5, dash: [7, 4]))
        let right = b.midX >= a.midX
        let tip = CGPoint(x: right ? b.minX : b.maxX, y: b.midY).applying(transform)
        let dx: CGFloat = right ? -9 : 9
        let arrow = Path { p in
          p.move(to: tip); p.addLine(to: CGPoint(x: tip.x + dx, y: tip.y - 4))
          p.addLine(to: CGPoint(x: tip.x + dx, y: tip.y + 4)); p.closeSubpath()
        }
        context.fill(arrow, with: .color(color))
      }
      for card in highlighted {
        let rect = cardRect(card).applying(transform).insetBy(dx: -3, dy: -3)
        context.stroke(Path(roundedRect: rect, cornerRadius: 10), with: .color(.purple), lineWidth: 3)
      }
    }
    .allowsHitTesting(false)
  }

  // MARK: - Wires

  struct RoutedWire {
    var connection: Int?   // index in the scope's connections (nil: boundary wire or display link)
    var path: Path         // world units
    var style: WireDraw.Style
    var dotted = false     // an end of it is a Draft
    var thread: AgentPresence.Wire? = nil  // a connection, as the Agent's thread knows it
  }

  /// Where Cards and wires are, for the Agent's presence on this canvas.
  private func presenceGeometry(_ routed: [RoutedWire]) -> PresenceGeometry {
    let shown = Set(cardKeys)
    return PresenceGeometry(card: { k in shown.contains(k) ? outlineRect(k) : nil },
                            wire: { w in routed.first { $0.thread == w }?.path },
                            dotted: { w in session.isDraftWire(from: w.from, to: w.to) },
                            screen: { screen($0) }, zoom: camera.zoom,
                            obstacles: { cardKeys.map(cardRect) + outputKeys.map(frameRect) })
  }

  /// Every wire of the scope, routed once per update and used for drawing,
  /// hover and clicks alike.
  private func routedWires() -> [RoutedWire] {
    var obstacles = cardKeys.map(cardRect)
    obstacles += outputKeys.map(frameRect)
    let b = boundary
    if let b { obstacles += [b.top, b.bottom] }
    let flow = session.flow
    // Routed together: a wire's path must not depend on the order of the connections.
    var ends: [(a: CGPoint, b: CGPoint)] = []
    var out: [RoutedWire] = []
    for (i, c) in session.connections(in: tab.scope).enumerated() {
      guard let fc = c["from"]?["card"]?.string, let fp = c["from"]?["port"]?.string,
            let tc = c["to"]?["card"]?.string, let tp = c["to"]?["port"]?.string else { continue }
      let fk = key(tab.scope + [fc]), tk = key(tab.scope + [tc])
      ends.append((portWorld(fk, fp, output: true), portWorld(tk, tp, output: false)))
      out.append(RoutedWire(connection: i, path: Path(),
                            style: i == tab.selectedConnection ? .selected : style(from: fk, port: fp, to: tk),
                            dotted: session.isDraftWire(from: fk, to: tk),
                            thread: AgentPresence.Wire(from: fk, fromPort: fp, to: tk, toPort: tp)))
    }
    if let b {
      for p in b.inputs {
        guard let inner = p.inner, session.instance(inner.0) != nil else { continue }
        ends.append((p.point, portWorld(inner.0, inner.1, output: false)))
        out.append(RoutedWire(connection: nil, path: Path(), style: .normal, dotted: session.isDraftWire(from: inner.0, to: nil)))
      }
      for p in b.outputs {
        guard let inner = p.inner, session.instance(inner.0) != nil else { continue }
        ends.append((portWorld(inner.0, inner.1, output: true), p.point))
        out.append(RoutedWire(connection: nil, path: Path(), style: style(from: inner.0, port: inner.1, to: nil),
                              dotted: session.isDraftWire(from: inner.0, to: nil)))
      }
    }
    for (i, path) in routes.paths(flow: flow, obstacles: obstacles, ends: ends, onRouted: { redrawn += 1 }).enumerated() { out[i].path = path }
    // Display links: which Card a result frame belongs to (not a computation).
    for k in outputKeys {
      out.append(RoutedWire(connection: nil, path: displayLink(card: outlineRect(k), frame: frameRect(k)), style: .display))
    }
    return out
  }

  /// A wire is colored only when a value really went through it in this run
  /// (a branch colors only the side it chose), and animated while the value
  /// is on its way to a Card that has not finished with it yet.
  private func style(from: CardKey, port: String, to: CardKey?) -> WireDraw.Style {
    // A Card in Card sends what the inner Card behind that output sent.
    if session.kind(from) == .subgraph, let (inner, p) = session.boundarySource(from, output: port) {
      return style(from: inner, port: p, to: to)
    }
    guard let st = session.cardStates[from] else { return .normal }
    if session.runActive, st.sent.contains(port) {
      if let dest = to.flatMap({ session.cardStates[$0] }) {
        if [.waiting, .ready, .preparing, .running].contains(dest.state) { return .active }
      } else if to != nil {
        return .active
      }
    }
    return st.sentInRun.contains(port) ? .carried : .normal
  }

  // MARK: - Card in Card boundary

  struct BoundaryPort {
    var name: String
    var point: CGPoint          // world units, on the bar
    var inner: (CardKey, String)?
    var type: String?
  }

  struct Boundary {
    var top: CGRect
    var bottom: CGRect
    var inputs: [BoundaryPort]
    var outputs: [BoundaryPort]
  }

  /// The outer Card's inputs (above the inner graph) and outputs (below it).
  private var boundary: Boundary? {
    guard !tab.scope.isEmpty, let b = session.boundary(scopeKey) else { return nil }
    let ins = b["inputs"]?.object ?? [], outs = b["outputs"]?.object ?? []
    guard !ins.isEmpty || !outs.isEmpty else { return nil }
    var box = cardKeys.reduce(CGRect.null) { $0.union(CGRect(origin: session.placedPosition($1), size: session.geometry($1).size)) }
    if box.isNull { box = CGRect(x: 0, y: 0, width: 240, height: 120) }
    let width = max(box.width, CGFloat(max(ins.count, outs.count)) * 110 + 40)
    let x = box.midX - width / 2
    // Placed above / below the inner Cards unless moved (layout.json "boundaries").
    func placed(_ side: String, _ auto: CGPoint) -> CGPoint {
      var p = auto
      if let v = session.boundaryPosition(tab.scope, side) { p = v }
      if let d = boundaryDrag, d.input == (side == "inputs") {
        p.x += d.offset.width / camera.zoom
        p.y += d.offset.height / camera.zoom
      }
      return p
    }
    let top = CGRect(origin: placed("inputs", CGPoint(x: x, y: box.minY - 96)), size: CGSize(width: width, height: 40))
    let bottom = CGRect(origin: placed("outputs", CGPoint(x: x, y: box.maxY + 56)), size: CGSize(width: width, height: 40))
    let resolved = session.resolution[scopeKey]
    func ports(_ list: [(String, JSONValue)], bar: CGRect, output: Bool) -> [BoundaryPort] {
      list.enumerated().map { i, e in
        let px = bar.minX + bar.width * CGFloat(i + 1) / CGFloat(list.count + 1)
        let inner = e.1["card"]?.string.flatMap { c in e.1["port"]?.string.map { (key(tab.scope + [c]), $0) } }
        let t = (output ? resolved?.outputs : resolved?.inputs)?.first { $0.name == e.0 }?.rootType
        return BoundaryPort(name: e.0, point: CGPoint(x: px, y: output ? bar.minY : bar.maxY), inner: inner, type: t)
      }
    }
    return Boundary(top: top, bottom: bottom, inputs: ports(ins, bar: top, output: false), outputs: ports(outs, bar: bottom, output: true))
  }

  private func boundaryBars(_ b: Boundary) -> some View {
    let z = CGFloat(camera.zoom)
    return ZStack(alignment: .topLeading) {
      BoundaryBarView(title: String(localized: "この Card の入力"), ports: b.inputs, input: true, zoom: z, rect: b.top)
        .placeScaled(at: screen(b.top.origin), size: b.top.size, zoom: camera.zoom, pixel: displayScale)
        .gesture(boundaryMove(input: true, from: undragged(b.top.origin, input: true)))
        .help("ドラッグで移動できます")
      BoundaryBarView(title: String(localized: "この Card の出力"), ports: b.outputs, input: false, zoom: z, rect: b.bottom)
        .placeScaled(at: screen(b.bottom.origin), size: b.bottom.size, zoom: camera.zoom, pixel: displayScale)
        .gesture(boundaryMove(input: false, from: undragged(b.bottom.origin, input: false)))
        .help("ドラッグで移動できます")
    }
  }

  /// A bar's position without the drag in progress.
  private func undragged(_ p: CGPoint, input: Bool) -> CGPoint {
    guard let d = boundaryDrag, d.input == input else { return p }
    return CGPoint(x: p.x - d.offset.width / camera.zoom, y: p.y - d.offset.height / camera.zoom)
  }

  /// Moves a boundary bar; the wires to its ports follow while dragging.
  private func boundaryMove(input: Bool, from origin: CGPoint) -> some Gesture {
    DragGesture(minimumDistance: 3, coordinateSpace: .named("canvas"))
      .onChanged { v in
        clearHover()
        boundaryDrag = (input, v.translation)
      }
      .onEnded { v in
        boundaryDrag = nil
        var x = origin.x + v.translation.width / camera.zoom, y = origin.y + v.translation.height / camera.zoom
        if prefs.showGrid {
          x = snapToGrid(x, prefs.gridSize)
          y = snapToGrid(y, prefs.gridSize)
        }
        // Layout only (the external's layout.json).
        session.setBoundaryPosition(tab.scope, input ? "inputs" : "outputs", CGPoint(x: x.rounded(), y: y.rounded()))
      }
  }

  // MARK: - Layers

  private var background: some View {
    CanvasDots(session: session, scope: scopeKey, shown: prefs.showGrid, spacing: prefs.gridSize)
    .background(Color(nsColor: .textBackgroundColor))
    .contentShape(Rectangle())
    .gesture(backgroundDrag)
    .onTapGesture(count: 2, coordinateSpace: .named("canvas")) { _ in fitAll() }
    .onTapGesture(coordinateSpace: .named("canvas")) { loc in backgroundTap(loc) }
    .contextMenu {
      Menu("Card を追加") {
        BuiltinCardItems { t in addBuiltin(t, at: world(pointer)) }
      }
      .disabled(!session.isEditable)
      Button("付箋を追加") { addNote(at: world(pointer)) }
      Divider()
      Button("全体表示") { fitAll() }
      Button("整列") { session.arrange(tab.scope) }
      Button("配線の交差を減らす") { session.untangle(tab.scope) }
      if !tab.selection.isEmpty { Button("選択をグループ化") { groupSelection() }.disabled(!session.connected) }
    }
  }

  /// What a Card on the canvas is drawn from (Settled): moving the canvas changes none of it.
  private struct CardSlot: Equatable {
    let session: ObjectIdentifier
    let tab: ObjectIdentifier
    let card: CardKey
    let data: CardNodeData
    let flow: FlowDirection
    let zoom: Double
    let selected: Bool
    let editable: Bool
    let canRun: Bool
    let presence: ObjectIdentifier
  }

  /// What a display frame is drawn from besides the session's own state, which it reads itself.
  private struct FrameSlot: Equatable {
    let session: ObjectIdentifier
    let tab: ObjectIdentifier
    let card: CardKey
    let selected: Bool
    let rect: CGRect
  }

  private func cardView(_ k: CardKey) -> some View {
    let data = session.nodeData(k)
    let g = CardGeometry(data, flow: session.flow)
    let flow = session.flow, zoom = camera.zoom, selected = tab.selection.contains(k)
    let presence = session.presence(forCard: k)
    let canRun = data.kind == .trigger && session.connected && !session.runActive
    // A Draft being written is drawn every time: what is typed is read from the session as it changes.
    let key = data.draftEditing ? nil : CardSlot(
      session: ObjectIdentifier(session), tab: ObjectIdentifier(tab), card: k, data: data, flow: flow, zoom: zoom,
      selected: selected, editable: session.isEditable, canRun: canRun, presence: ObjectIdentifier(presence))
    return Settled(key: key) {
      CardNodeView(data: data, flow: flow, zoom: zoom, selected: selected,
                   onParamCommit: data.kind.editsValueOnCanvas ? { name, text in commitParam(k, name, text) } : nil,
                   paramsEditable: session.isEditable,
                   onPortDrag: { port, pt in if session.isEditable { clearHover(); pending = ((k, port), live(pt), true) } },
                   onPortDrop: { port, pt in finishConnect(from: (k, port), at: live(pt)) },
                   onInputDrag: { port, pt in if session.isEditable { clearHover(); pending = ((k, port), live(pt), false) } },
                   onInputDrop: { port, pt in finishConnect(toInput: (k, port), at: live(pt)) },
                   onStageTap: { st in workspace.showLog(for: k, stage: st, invocation: session.cardStates[k]?.invocation) },
                   onToggleCollapse: { session.setCollapsed([k], !session.collapsed(k)) },
                   onComments: { openWindow(id: "card", value: CardRef(path: session.identity.url.path, card: k, category: .comments)) },
                   onRun: canRun
                     ? { RunActions.run(session, card: k, propagate: true) } : nil,
                   draftEdit: draftEdit(k))
        .modifier(CardWriting(presence: presence, card: k, title: data.title, zoom: zoom))
        .guideTarget("card:\(k)", radius: 12, zoom: zoom)
    }
      .equatable()
      .placeScaled(at: screen(origin(k)), size: g.size, zoom: camera.zoom, pixel: displayScale)
      .zIndex(data.draftEditing ? 1 : 0)
      .gesture(cardDrag(k))
      .onTapGesture(count: 2) { open(k) }
      .onTapGesture { select(k) }
      .contextMenu { CardMenu(workspace: workspace, session: session, tab: tab, cardKey: k, onGroup: groupSelection) }
  }

  /// Writing this Card's Draft, when it is the one being written.
  private func draftEdit(_ k: CardKey) -> DraftEdit? {
    guard let e = session.inlineDraft, e.card == k else { return nil }
    return DraftEdit(name: Binding(get: { session.inlineDraft?.name ?? "" }, set: { session.inlineDraft?.name = $0 }),
                     content: Binding(get: { session.inlineDraft?.content ?? "" }, set: { session.inlineDraft?.content = $0 }),
                     nameProblem: session.draftNameProblem(k, e.name),
                     onDone: { byKey in
                       finishDraftEdit()
                       if byKey { canvasFocused = true }
                     })
  }

  /// Starts writing a Draft on its Card, at a zoom where the text can be read.
  func writeDraft(_ k: CardKey) {
    session.startDraftEdit(k)
    if camera.zoom < 0.8 { setZoom(1, around: screen(cardRect(k).center)) }
    reveal(k)
  }

  /// Keeps what was written on a Draft's Card; a new name moves what points at the Card.
  private func finishDraftEdit() {
    guard let from = session.inlineDraft?.card else { return }
    Task {
      guard let to = await session.finishDraftEdit() else { return }
      NotificationCenter.default.post(name: .cardRenamed, object: nil,
                                      userInfo: ["from": from, "to": to, "project": session.identity.url.path])
      if tab.selection.remove(from) != nil { tab.selection.insert(to) }
    }
  }

  private func groupFrame(_ id: String) -> some View {
    let g = session.groups(in: tab.scope).first { $0["id"]?.string == id } ?? .object([])
    let members = (g["cards"]?.array ?? []).compactMap { $0.string }.map { key(tab.scope + [$0]) }.filter { session.instance($0) != nil }
    var rect = CGRect.null
    for m in members { rect = rect.union(cardRect(m)) }
    if rect.isNull { rect = CGRect(x: 0, y: 0, width: 200, height: 100) }
    rect = rect.insetBy(dx: -18, dy: -18)
    rect.origin.y -= 22
    rect.size.height += 22
    return GroupFrameView(session: session, group: g, scope: tab.scope)
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
    let n = session.note(id) ?? .object([])
    let r = noteRect(n)
    // Follows the pointer while dragged (screen units, so any zoom); the layout changes once, on release.
    let by = noteDrag?.id == id ? noteDrag!.by : tab.selectedNotes.contains(id) ? selectionMove ?? .zero : .zero
    return StickyNoteView(note: n, selected: tab.selectedNotes.contains(id), onResize: { d in
      session.updateNote(id) { v in
        v["width"] = .real(max(120, r.width + d.width / camera.zoom).rounded())
        v["height"] = .real(max(60, r.height + d.height / camera.zoom).rounded())
      }
    })
    .place(at: CGPoint(x: screen(r.origin).x + by.width, y: screen(r.origin).y + by.height), size: r.size, zoom: camera.zoom)
    // In the canvas space: the note moves under the pointer, so its own space would shrink the translation.
    .gesture(DragGesture(minimumDistance: 3, coordinateSpace: .named("canvas")).onChanged { v in
      clearHover()
      if tab.selectedNotes.contains(id) {
        selectionDrag = v.translation
        return
      }
      if noteDrag == nil { tab.selection = [] }
      noteDrag = (id, v.translation)
    }.onEnded { v in
      if selectionDrag != nil { return moveSelection(by: v.translation) }
      noteDrag = nil
      let to = Self.droppedNoteOrigin(from: r.origin, by: v.translation, zoom: camera.zoom)
      session.updateNote(id) { n in
        n["x"] = .real(to.x)
        n["y"] = .real(to.y)
      }
    })
    .onTapGesture(count: 2) { editingNote = id }
    .contextMenu {
      Button("付箋を編集…") { editingNote = id }
      Menu("色") {
        ForEach(NoteColor.all, id: \.id) { c in Button(c.label) { session.updateNote(id) { $0["color"] = .string(c.id) } } }
      }
      Menu("文字の大きさ") {
        let current = StickyNoteView.fontSize(n)
        Button("大きく") { setNoteFont(id, current + 2) }
        Button("小さく") { setNoteFont(id, current - 2) }
        Divider()
        ForEach([11.0, 13, 16, 20, 28], id: \.self) { v in
          Button("\(Int(v))\(v == 13 ? "（デフォルト）" : "")") { setNoteFont(id, v) }
        }
      }
      Divider()
      Button("付箋を削除", role: .destructive) { session.removeNote(id) }
    }
  }

  /// Where a note dragged by `translation` screen points lands (world units, whole numbers; no grid snap).
  static func droppedNoteOrigin(from origin: CGPoint, by translation: CGSize, zoom: Double) -> CGPoint {
    CGPoint(x: (origin.x + translation.width / zoom).rounded(), y: (origin.y + translation.height / zoom).rounded())
  }

  private func setNoteFont(_ id: String, _ size: CGFloat) {
    let v = max(8, min(48, size))
    session.updateNote(id) { $0["fontSize"] = v == 13 ? nil : .real(Double(v)) }
  }

  private func outputFrame(_ k: CardKey) -> some View {
    let r = frameRect(k)
    let selected = tab.selectedFrames.contains(k)
    return Settled(key: FrameSlot(session: ObjectIdentifier(session), tab: ObjectIdentifier(tab), card: k, selected: selected, rect: r)) {
      OutputFrameView(session: session, cardKey: k, selected: selected,
                      onExpand: { openWindow(id: "visual", value: CardRef(path: session.identity.url.path, card: k)) },
                      onClose: { session.closeDisplayFrame(k) },
                      onResize: { d in
                        session.updateDisplayFrame(k) { v in
                          v["x"] = .real(r.minX.rounded())
                          v["y"] = .real(r.minY.rounded())
                          v["width"] = .real(max(180, r.width + d.width / camera.zoom).rounded())
                          v["height"] = .real(max(100, r.height + d.height / camera.zoom).rounded())
                        }
                      })
        .environment(\.canvasPlace, r)
    }
      .equatable()
      .place(at: screen(r.origin), size: r.size, zoom: camera.zoom)
      // Follows the pointer while dragged; the layout changes once, on release.
      .offset(frameDrag?.card == k ? frameDrag!.by : .zero)
      .gesture(DragGesture(minimumDistance: 3, coordinateSpace: .named("canvas")).onChanged { v in
        if tab.selectedFrames.contains(k) {
          selectionDrag = v.translation
          return
        }
        if frameDrag == nil { tab.selection = [] }
        frameDrag = (k, v.translation)
      }.onEnded { v in
        if selectionDrag != nil { return moveSelection(by: v.translation) }
        frameDrag = nil
        session.updateDisplayFrame(k) { d in
          d["x"] = .real((r.minX + v.translation.width / camera.zoom).rounded())
          d["y"] = .real((r.minY + v.translation.height / camera.zoom).rounded())
          d["width"] = .real(r.width)
          d["height"] = .real(r.height)
        }
      })
      .contextMenu {
        Button("拡大して表示") { openWindow(id: "visual", value: CardRef(path: session.identity.url.path, card: k)) }
        Button("チャットに参照を追加") { AppModel.shared.chat(for: session).addReference(.display(k)) }
        Divider()
        Button("枠を削除（Card は残ります）") { session.closeDisplayFrame(k) }
      }
  }

  // MARK: - Commands

  func handleZoom(_ n: Notification) {
    guard (n.object as? UUID) == tab.id, let f = n.userInfo?["zoom"] as? Double else { return }
    let center = CGPoint(x: size.width / 2, y: size.height / 2)
    if f == 0 { setZoom(1, around: center) } else { zoom(by: f, around: center) }
    saveView()
  }

  func handleFind(_ n: Notification) {
    if (n.object as? UUID) == tab.id { findText = findText ?? ""; findFocused = true }
  }

  /// Esc: cancel what is going on, then clear the selection.
  func escape() {
    if pending != nil { pending = nil }
    else if findText != nil { findText = nil }
    else if hover != nil { clearHover() }
    else { tab.selection = []; tab.selectedConnection = nil }
  }

  func handleDetails(_ n: Notification) {
    if (n.object as? UUID) == tab.id, let k = n.userInfo?["card"] as? CardKey, session.instance(k) != nil {
      tab.selection = [k]
      showDetailsForSelection()
    }
  }

  func handleDelete(_ n: Notification) {
    if (n.object as? UUID) == tab.id { deleteSelection() }
  }

  /// G: the selected Cards become a group; a selection that is one group's
  /// Cards is ungrouped.
  func handleGroup(_ n: Notification) {
    guard (n.object as? UUID) == tab.id, !tab.selection.isEmpty else { return }
    let ids = Set(tab.selection.compactMap { path($0).last })
    if let g = session.groups(in: tab.scope).first(where: { Set(($0["cards"]?.array ?? []).compactMap(\.string)) == ids }),
       let gid = g["id"]?.string {
      session.ungroup(gid)
    } else {
      _ = session.addGroup(tab.selection, in: tab.scope)
    }
  }

  /// E: a new Draft Card in the middle of the canvas, written on the Card.
  func handleAddDraft(_ n: Notification) {
    guard (n.object as? UUID) == tab.id, session.isEditable else { return }
    let k = session.addCard(template: .draft, in: tab.scope, at: world(CGPoint(x: size.width / 2, y: size.height / 2)), clearOfOthers: true)
    tab.selection = [k]
    writeDraft(k)
  }

  /// The Card menu's "Write name and content".
  func handleWriteDraft(_ n: Notification) {
    if (n.object as? UUID) == tab.id, let k = n.userInfo?["card"] as? CardKey, session.isEditable { writeDraft(k) }
  }

  func handleReveal(_ n: Notification) {
    if (n.object as? UUID) == tab.id, let k = n.userInfo?["card"] as? CardKey { reveal(k) }
  }

  // MARK: - Card search (⌘F)

  private var findMatches: [CardKey] {
    let q = (findText ?? "").trimmingCharacters(in: .whitespaces)
    guard !q.isEmpty else { return [] }
    return cardKeys.filter { k in
      session.displayName(k).localizedCaseInsensitiveContains(q) || k.localizedCaseInsensitiveContains(q) ||
        (session.cardDescription(k) ?? "").localizedCaseInsensitiveContains(q)
    }
  }

  private var findBox: some View {
    VStack(alignment: .leading, spacing: 4) {
      HStack(spacing: 6) {
        Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
        TextField("Card を検索（名前・説明）", text: Binding(get: { findText ?? "" }, set: { findText = $0 }))
          .textFieldStyle(.plain)
          .focused($findFocused)
          .onSubmit { if let k = findMatches.first { pick(k) } }
          .onExitCommand { findText = nil }
        Button { findText = nil } label: { Image(systemName: "xmark.circle.fill") }.buttonStyle(.borderless).foregroundStyle(.secondary)
      }
      ForEach(findMatches.prefix(8), id: \.self) { k in
        Button { pick(k) } label: {
          HStack {
            Image(systemName: session.kind(k).symbol).frame(width: 16)
            Text(session.displayName(k))
            Spacer()
            Text(session.kind(k).label).font(.caption).foregroundStyle(.secondary)
          }
          .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
      }
      if !(findText ?? "").isEmpty && findMatches.isEmpty { Text("見つかりません").font(.caption).foregroundStyle(.secondary) }
    }
    .padding(8)
    .frame(width: 320)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 9))
    .shadow(color: .black.opacity(0.12), radius: 6, y: 2)
  }

  private func pick(_ k: CardKey) {
    tab.selection = [k]
    findText = nil
    reveal(k)
  }

  // MARK: - Hover details

  enum HoverTarget: Equatable {
    case card(CardKey)
    case wire(Int)
  }

  private func trackHover(_ p: CGPoint, routed: [RoutedWire]) {
    hoverAt = p
    guard !interacting else { clearHover(); return }
    let w = world(p)
    let over = cardKeys.reversed().first(where: { cardRect($0).contains(w) })
    if tab.showNixReferences { tab.hoveredNixCard = over }
    var target: HoverTarget?
    if let k = cardKeys.reversed().first(where: { cardRect($0).contains(w) }) {
      // A Draft being written has its details in front of it.
      target = k == session.inlineDraft?.card ? nil : .card(k)
    } else {
      // About 8 points on screen either side of the line, at any zoom.
    let tolerance = 8 / camera.zoom
      target = routed.first { r in
        r.connection != nil && r.path.strokedPath(StrokeStyle(lineWidth: tolerance * 2)).contains(w)
      }?.connection.map(HoverTarget.wire)
    }
    if target == hover { return }
    hoverTask?.cancel()
    if hover != nil { hover = nil }
    guard let target else { return }
    // A short wait, so moving across the canvas does not flash details.
    hoverTask = Task { @MainActor in
      try? await Task.sleep(for: .milliseconds(450))
      guard !Task.isCancelled, !interacting else { return }
      withAnimation(.easeOut(duration: 0.12)) { hover = target }
    }
  }

  private func clearHover() {
    tab.hoveredNixCard = nil
    hoverTask?.cancel()
    hoverTask = nil
    if hover != nil { hover = nil }
  }

  private func showDetailsForSelection() {
    guard tab.selection.count == 1, let k = tab.selection.first else { return }
    let r = cardRect(k)
    hoverAt = screen(CGPoint(x: r.maxX, y: r.minY))
    hover = .card(k)
  }

  /// Right below the pointer (its top-left corner just past it), flipped to
  /// the other side where the canvas ends, and kept inside the canvas.
  private func bubblePosition(in size: CGSize) -> CGPoint {
    let (w, h) = (bubbleSize.width, bubbleSize.height)
    let gap: CGFloat = 14
    var left = hoverAt.x + gap
    if left + w > size.width - 6 { left = hoverAt.x - gap - w }
    var top = hoverAt.y + gap
    if top + h > size.height - 6 { top = hoverAt.y - gap - h }
    left = min(max(6, left), max(6, size.width - w - 6))
    top = min(max(6, top), max(6, size.height - h - 6))
    return CGPoint(x: left + w / 2, y: top + h / 2)
  }

  private func hoverContent(_ t: HoverTarget) -> HoverBubble.Content {
    switch t {
    case .card(let k):
      let d = session.nodeData(k)
      let lines = session.cardDetails(k)
      var status = d.runText ?? d.idleText
      if let id = session.currentRunId, d.invocation > 0 {
        status += String(localized: "（Run \(String(id.prefix(8))) の中でこの Card の \(d.invocation) 回目の実行）")
      }
      return .init(title: d.title, subtitle: d.kind.label, symbol: d.symbol ?? d.kind.symbol, lines: lines,
                   status: status, problems: d.errors + (d.failure.map { [$0] } ?? []), warnings: d.warnings)
    case .wire(let i):
      let conns = session.connections(in: tab.scope)
      let symbol = "point.topleft.down.to.point.bottomright.curvepath"
      guard i < conns.count, let fc = conns[i]["from"]?["card"]?.string, let fp = conns[i]["from"]?["port"]?.string,
            let tc = conns[i]["to"]?["card"]?.string, let tp = conns[i]["to"]?["port"]?.string else {
        return .init(title: String(localized: "接続"), subtitle: "", symbol: symbol)
      }
      let fk = key(tab.scope + [fc]), tk = key(tab.scope + [tc])
      let out = session.resolution[fk]?.outputs.first { $0.name == fp }
      let inp = session.resolution[tk]?.inputs.first { $0.name == tp }
      var lines = ["\(session.displayName(fk)).\(fp)  →  \(session.displayName(tk)).\(tp)"]
      lines.append(String(localized: "型: ") + (out?.rootType ?? inp?.rootType ?? String(localized: "未解決")))
      lines.append((inp?.optional ?? false) ? String(localized: "受け側の入力: 省略可") : String(localized: "受け側の入力: 必須"))
      var status = String(localized: "この Run ではまだ値が通っていません")
      if let st = session.cardStates[fk], st.sentInRun.contains(fp), let id = session.currentRunId {
        status = String(localized: "Run \(String(id.prefix(8))) で値が通りました（送信元の最新は実行 #\(st.invocation)）")
      }
      return .init(title: String(localized: "接続"), subtitle: "", symbol: symbol, lines: lines, status: status)
    }
  }

  // MARK: - Gestures

  private var backgroundDrag: some Gesture {
    DragGesture(minimumDistance: 2, coordinateSpace: .named("canvas"))
      .onChanged { v in
        clearHover()
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
          let items = session.marqueeItems(r, in: tab.scope)
          tab.selection = items.cards
          tab.selectedNotes = items.notes
          tab.selectedFrames = items.frames
          tab.selectedConnection = nil
          marquee = nil
        }
        panStart = nil
        saveView()
      }
  }

  private func cardDrag(_ k: CardKey) -> some Gesture {
    DragGesture(minimumDistance: 3, coordinateSpace: .named("canvas"))
      .onChanged { v in
        clearHover()
        if !tab.selection.contains(k) { tab.selection = [k] }
        drag = (k, v.translation)
      }
      .onEnded { v in
        if !tab.selection.contains(k) { tab.selection = [k] }
        moveSelection(by: v.translation)
      }
  }

  /// The end of a drag of the selection (screen translation), from any of its items.
  private func moveSelection(by t: CGSize) {
    drag = nil
    selectionDrag = nil
    session.moveSelection(cards: tab.selection, notes: tab.selectedNotes, frames: tab.selectedFrames,
                          by: CGSize(width: t.width / camera.zoom, height: t.height / camera.zoom),
                          snap: prefs.showGrid ? prefs.gridSize : nil)
  }

  private func backgroundTap(_ loc: CGPoint) {
    // A click near a wire selects it (the path as drawn); elsewhere clears the selection.
    let w = world(loc)
    let tolerance = max(5, 7 / camera.zoom)
    if tab.showNixReferences, let ref = nixLinks.first(where: {
      nixCurve(cardRect($0.source), cardRect($0.target)).strokedPath(StrokeStyle(lineWidth: tolerance * 2)).contains(w)
    }) {
      tab.selectedNixReference = ref.reference.id
      tab.showNixList = true
      return
    }
    tab.selectedNixReference = nil
    if let i = routedWires().first(where: { r in
      r.connection != nil && r.path.strokedPath(StrokeStyle(lineWidth: tolerance * 2)).contains(w)
    })?.connection {
      tab.selectedConnection = i
      tab.selection = []
      return
    }
    tab.selection.removeAll()
    tab.selectedConnection = nil
  }

  private func select(_ k: CardKey) {
    if NSEvent.modifierFlags.contains(.shift) || NSEvent.modifierFlags.contains(.command) {
      // Adding to the selection keeps what a marquee caught.
      let notes = tab.selectedNotes, frames = tab.selectedFrames
      if tab.selection.contains(k) { tab.selection.remove(k) } else { tab.selection.insert(k) }
      tab.selectedNotes = notes
      tab.selectedFrames = frames
    } else {
      tab.selection = [k]
    }
    tab.selectedConnection = nil
  }

  private func open(_ k: CardKey) {
    switch session.kind(k) {
    case .subgraph: tab.enter(path(k))
    case .draft where session.isEditable:
      if session.inlineDraft?.card != k { writeDraft(k) }
    default: openWindow(id: "card", value: CardRef(path: session.identity.url.path, card: k))
    }
  }

  private func finishConnect(from: (CardKey, String), at pt: CGPoint) {
    pending = nil
    guard session.isEditable, let to = wireTarget(at: pt, output: false, except: from.0) else { return }
    session.connect(from: wireStart(from, output: true), to: to, in: tab.scope)
  }

  private func finishConnect(toInput to: (CardKey, String), at pt: CGPoint) {
    pending = nil
    guard session.isEditable, let from = wireTarget(at: pt, output: true, except: to.0) else { return }
    session.connect(from: from, to: wireStart(to, output: false), in: tab.scope)
  }

  /// Another wire from a Draft's port is another port.
  private func wireStart(_ end: (CardKey, String), output: Bool) -> (CardKey, String) {
    session.kind(end.0) == .draft && session.portWired(end.0, end.1, output: output, in: tab.scope) ? (end.0, CardGeometry.newPort) : end
  }

  /// Where a dropped wire ends. A Draft takes it anywhere on the Card: a port
  /// of its own that has no wire, or a new one (never replacing another wire).
  private func wireTarget(at pt: CGPoint, output: Bool, except: CardKey) -> (CardKey, String)? {
    if let near = nearestPort(at: pt, output: output, except: except) {
      let taken = session.kind(near.0) == .draft && session.portWired(near.0, near.1, output: output, in: tab.scope)
      return taken ? (near.0, CardGeometry.newPort) : near
    }
    let w = world(pt)
    let margin = 18 / camera.zoom + 6
    return cardKeys.reversed().first {
      $0 != except && session.kind($0) == .draft && cardRect($0).insetBy(dx: -margin, dy: -margin).contains(w)
    }.map { ($0, CardGeometry.newPort) }
  }

  /// The port under a point (screen), within a finger's width.
  private func nearestPort(at pt: CGPoint, output: Bool, except: CardKey) -> (CardKey, String)? {
    let w = world(pt)
    var best: ((CardKey, String), Double)?
    for k in cardKeys where k != except {
      let g = session.geometry(k)
      for port in output ? g.outputs : g.inputs {
        let p = portWorld(k, port, output: output)
        let d = Double(hypot(p.x - w.x, p.y - w.y))
        if d < 18 / camera.zoom + 6, d < (best?.1 ?? .infinity) { best = ((k, port), d) }
      }
    }
    return best?.0
  }

  private func commitParam(_ k: CardKey, _ name: String, _ text: String) {
    guard session.isEditable else { return }
    let type = session.kind(k) == .switchCard && name == "value" ? "bool"
      : session.cardDefinition(k)?["params"]?.array?.first(where: { $0["name"]?.string == name })?["type"]?.string
    guard let type else { return }
    session.setParam(k, name, parseParam(text, type))
  }

  private func deleteSelection() {
    // Notes and frames are layout: they can go during a run, as from their own menus.
    guard session.isEditable || (tab.selection.isEmpty && tab.selectedConnection == nil) else {
      session.notice = String(localized: "実行中は Card を削除できません。")
      return
    }
    if let i = tab.selectedConnection {
      session.removeConnection(at: i, in: tab.scope)
      tab.selectedConnection = nil
      return
    }
    session.deleteSelection(cards: tab.selection, notes: tab.selectedNotes, frames: tab.selectedFrames, in: tab.scope)
    tab.selection.removeAll()
  }

  private func groupSelection() {
    guard !tab.selection.isEmpty else { return }
    editingGroup = session.addGroup(tab.selection, in: tab.scope)
  }

  /// A built-in Card (from the + button or the canvas menu); a Draft is
  /// written on its Card.
  func addBuiltin(_ t: DefinitionTemplate, at w: CGPoint) {
    guard session.isEditable else { return }
    let k = session.addCard(template: t, in: tab.scope, at: w, clearOfOthers: true)
    tab.selection = [k]
    if t == .draft { writeDraft(k) }
  }

  private func addNote(at w: CGPoint? = nil) {
    let center = w ?? world(CGPoint(x: size.width / 2 - 120, y: size.height / 2 - 60))
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

  /// Centres a Card in the view when it is not fully visible (zoom kept).
  private func reveal(_ k: CardKey) {
    guard session.instance(k) != nil, size.width > 0 else { return }
    let r = cardRect(k)
    let s = CGRect(origin: screen(r.origin), size: CGSize(width: r.width * camera.zoom, height: r.height * camera.zoom))
    if CGRect(origin: .zero, size: size).insetBy(dx: 20, dy: 50).contains(s) { return }
    var c = camera
    c.x = size.width / 2 - r.midX * c.zoom
    c.y = size.height / 2 - r.midY * c.zoom
    withAnimation(.easeInOut(duration: 0.2)) { session.cameras[scopeKey] = c }
    saveView()
  }

  private func fitIfNew() {
    guard session.cameras[scopeKey] == nil, !cardKeys.isEmpty, size.width > 0 else { return }
    DispatchQueue.main.async { fitAll() }
  }

  /// Frames everything shown in the scope: Cards, output frames, notes,
  /// groups, the Card in Card bars and the wires (feedback lanes run outside
  /// the Cards), in the part of the canvas its floating controls leave free.
  func fitAll() {
    var rect = cardKeys.reduce(CGRect.null) { $0.union(cardRect($1)) }
    for k in outputKeys { rect = rect.union(frameRect(k)) }
    for n in session.notes(in: tab.scope) { rect = rect.union(noteRect(n)) }
    for g in session.groups(in: tab.scope) {
      let members = (g["cards"]?.array ?? []).compactMap { $0.string }.map { key(tab.scope + [$0]) }.filter { session.instance($0) != nil }
      for m in members { rect = rect.union(cardRect(m).insetBy(dx: -18, dy: -40)) }
    }
    if let b = boundary { rect = rect.union(b.top).union(b.bottom) }
    for w in routedWires() where w.style != .display { rect = rect.union(w.path.boundingRect) }
    guard !rect.isNull, size.width > 0 else { return }
    var c = CanvasCamera()
    c.fit(rect, in: size, insets: EdgeInsets(top: 52, leading: 8, bottom: 48, trailing: 8), maxZoom: 1.2, margin: 24)
    session.cameras[scopeKey] = (c.x, c.y, c.zoom)
    saveView()
  }
}

/// Menu and keyboard commands of a canvas (zoom, search, Esc, details).
private struct CommandHandlers: ViewModifier {
  let canvas: GraphCanvas

  func body(content: Content) -> some View {
    content
      .onReceive(NotificationCenter.default.publisher(for: .canvasZoom)) { n in canvas.handleZoom(n) }
      .onReceive(NotificationCenter.default.publisher(for: .canvasFind)) { n in canvas.handleFind(n) }
      .onExitCommand { canvas.escape() }
      .onReceive(NotificationCenter.default.publisher(for: .canvasShowDetails)) { n in canvas.handleDetails(n) }
      .onReceive(NotificationCenter.default.publisher(for: .canvasReveal)) { n in canvas.handleReveal(n) }
      .onReceive(NotificationCenter.default.publisher(for: .canvasDelete)) { n in canvas.handleDelete(n) }
      .onReceive(NotificationCenter.default.publisher(for: .canvasGroup)) { n in canvas.handleGroup(n) }
      .onReceive(NotificationCenter.default.publisher(for: .canvasAddDraft)) { n in canvas.handleAddDraft(n) }
      .onReceive(NotificationCenter.default.publisher(for: .canvasWriteDraft)) { n in canvas.handleWriteDraft(n) }
  }
}

struct IdBox: Identifiable { let id: String }

extension CGRect {
  var center: CGPoint { CGPoint(x: midX, y: midY) }
}

extension Path {
  /// The point halfway along the path.
  var midPoint: CGPoint? {
    let r = trimmedPath(from: 0.495, to: 0.505).boundingRect
    return r.isNull ? nil : r.center
  }
}

/// A bar standing for the outer Card's inputs (above) or outputs (below)
/// inside a Card in Card: public name, type and the inner port it maps to.
struct BoundaryBarView: View {
  let title: String
  let ports: [GraphCanvas.BoundaryPort]
  let input: Bool
  let zoom: CGFloat
  let rect: CGRect

  var body: some View {
    ZStack(alignment: .topLeading) {
      RoundedRectangle(cornerRadius: 8 * zoom).fill(Color.accentColor.opacity(0.08))
      RoundedRectangle(cornerRadius: 8 * zoom).strokeBorder(Color.accentColor.opacity(0.5), style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
      Label(title, systemImage: input ? "arrow.down.to.line" : "arrow.down.to.line.compact")
        .font(.system(size: max(6, 9.5 * zoom), weight: .semibold)).foregroundStyle(Color.accentColor)
        .padding(.leading, 8 * zoom).padding(.top, 3 * zoom)
      ForEach(Array(ports.enumerated()), id: \.offset) { _, p in
        let x = (p.point.x - rect.minX) * zoom
        VStack(spacing: 0) {
          Text(p.name).font(.system(size: max(6, 10 * zoom), weight: .medium)).lineLimit(1)
          if let t = p.type { Text(t).font(.system(size: max(5, 8 * zoom), design: .monospaced)).foregroundStyle(.secondary).lineLimit(1) }
        }
        .frame(width: 104 * zoom)
        .position(x: x, y: rect.height * zoom / 2 + 5 * zoom)
        .help(p.inner.map { String(localized: "内部の接続先: \(String($0.0.split(separator: "/").last ?? "")).\($0.1)") }
              ?? String(localized: "内部の接続先が未設定です"))
        Group {
          if input { Circle().fill(Color.accentColor) } else {
            RoundedRectangle(cornerRadius: 1.5).fill(Color(nsColor: .controlBackgroundColor))
              .overlay(RoundedRectangle(cornerRadius: 1.5).strokeBorder(Color.secondary, lineWidth: 1.4))
          }
        }
        .frame(width: max(6, 8 * zoom), height: max(6, 8 * zoom))
        .position(x: x, y: input ? rect.height * zoom : 0)
      }
    }
    .accessibilityElement(children: .combine)
    .accessibilityLabel(title + ": " + ports.map(\.name).joined(separator: ", "))
  }
}

/// Details shown near the pointer over a Card or a wire.
struct HoverBubble: View {
  struct Content {
    var title: String
    var subtitle: String
    var symbol: String
    var lines: [String] = []
    var status: String?
    var problems: [String] = []
    var warnings: [String] = []
  }

  let content: Content

  private var wraps: Bool {
    (content.lines + content.problems + content.warnings + [content.status ?? ""]).contains { $0.count > 28 }
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 5) {
      HStack(spacing: 6) {
        Image(systemName: content.symbol).foregroundStyle(.secondary)
        Text(content.title).font(.callout.weight(.semibold)).lineLimit(1)
        if !content.subtitle.isEmpty { Text(content.subtitle).font(.caption).foregroundStyle(.secondary) }
      }
      ForEach(Array(content.lines.enumerated()), id: \.offset) { _, l in
        Text(l).font(.caption).lineLimit(8).fixedSize(horizontal: false, vertical: true)
      }
      if let s = content.status { Text(s).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
      ForEach(Array(content.problems.enumerated()), id: \.offset) { _, p in
        Label(p, systemImage: "xmark.octagon").font(.caption).foregroundStyle(.red).lineLimit(4)
          .fixedSize(horizontal: false, vertical: true)
      }
      ForEach(Array(content.warnings.enumerated()), id: \.offset) { _, p in
        Label(p, systemImage: "exclamationmark.triangle").font(.caption).foregroundStyle(.orange).lineLimit(3)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
    // A fixed width for text that wraps: the bubble takes its ideal size, and
    // a text's ideal is one line, however long.
    .frame(width: wraps ? 340 : nil, alignment: .leading)
    .padding(10)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 9))
    .overlay(RoundedRectangle(cornerRadius: 9).strokeBorder(.secondary.opacity(0.25)))
    .shadow(color: .black.opacity(0.12), radius: 6, y: 2)
  }
}

/// Pointer input of a canvas that SwiftUI gestures do not give:
/// - mouse wheel: zoom around the pointer; ⌘-scroll as well
/// - trackpad two-finger scroll: pan (pinch is a SwiftUI gesture)
/// - middle-button drag, or Space + drag: pan with a mouse
/// Clicks, and everything while a text field or editor has focus, go on
/// to SwiftUI and AppKit as usual.
struct ScrollCatcher: NSViewRepresentable {
  let onScroll: (Double, Double, Double, CGPoint?) -> Void

  func makeNSView(context: Context) -> NSView { CatcherView(onScroll: onScroll) }
  func updateNSView(_ nsView: NSView, context: Context) { (nsView as? CatcherView)?.onScroll = onScroll }

  final class CatcherView: NSView {
    var onScroll: (Double, Double, Double, CGPoint?) -> Void
    private var monitor: Any?
    private var spaceHeld = false
    private var panning = false

    init(onScroll: @escaping (Double, Double, Double, CGPoint?) -> Void) {
      self.onScroll = onScroll
      super.init(frame: .zero)
      monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp]) { [weak self] e in
        guard let self, e.keyCode == 49, self.window?.isKeyWindow == true else { return e }  // space
        // Space is text in an editor or a field: leave it alone there.
        if self.window?.firstResponder is NSText { return e }
        if e.type == .keyDown {
          if !self.spaceHeld { self.spaceHeld = true; NSCursor.openHand.push() }
        } else if self.spaceHeld {
          self.spaceHeld = false
          NSCursor.pop()
        }
        return nil
      }
    }
    required init?(coder: NSCoder) { fatalError() }
    deinit { if let monitor { NSEvent.removeMonitor(monitor) } }

    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? {
      switch NSApp.currentEvent?.type {
      case .scrollWheel?, .otherMouseDown?, .otherMouseDragged?, .otherMouseUp?: return self
      case .leftMouseDown?, .leftMouseDragged?, .leftMouseUp?: return spaceHeld || panning ? self : nil
      default: return nil
      }
    }

    override func scrollWheel(with e: NSEvent) {
      let p = convert(e.locationInWindow, from: nil)
      if e.modifierFlags.contains(.command) {
        onScroll(0, 0, Double(e.scrollingDeltaY) * (e.hasPreciseScrollingDeltas ? 0.01 : 0.08), p)
      } else if !e.hasPreciseScrollingDeltas {
        // A mouse wheel (line steps): zoom around the pointer.
        onScroll(0, 0, Double(e.scrollingDeltaY) * 0.08, p)
      } else {
        onScroll(Double(e.scrollingDeltaX), Double(e.scrollingDeltaY), 0, nil)
      }
    }

    override func otherMouseDown(with e: NSEvent) { panning = true; NSCursor.closedHand.push() }
    override func otherMouseDragged(with e: NSEvent) { onScroll(Double(e.deltaX), Double(e.deltaY), 0, nil) }
    override func otherMouseUp(with e: NSEvent) { panning = false; NSCursor.pop() }
    override func mouseDown(with e: NSEvent) { panning = true }
    override func mouseDragged(with e: NSEvent) { onScroll(Double(e.deltaX), Double(e.deltaY), 0, nil) }
    override func mouseUp(with e: NSEvent) { panning = false }
  }
}

/// The built-in Cards, for the canvas menu and the + button.
struct BuiltinCardItems: View {
  let add: (DefinitionTemplate) -> Void

  /// The Cards a person writes, then the ones with a fixed role.
  static let written: [DefinitionTemplate] = [.python, .cpp, .go, .rust, .ruby, .csharp, .draft, .marimo]
  static let fixed: [DefinitionTemplate] = [.trigger, .input, .switchCard, .fork, .branch, .merge, .logger]

  var body: some View {
    Menu("レポート（HTML・PDF 未対応）") {
      ForEach([DefinitionTemplate.report_rmarkdown, .report_quarto], id: \.self) { t in
        Button { add(t) } label: { Label(t.label, systemImage: t.symbol) }
      }
    }
    ForEach(Self.written, id: \.self) { t in
      Button { add(t) } label: { Label(t.label, systemImage: t.symbol) }
    }
    Menu("可視化（図を描く Card）") {
      ForEach([DefinitionTemplate.matplotlib, .plotly, .altair], id: \.self) { t in
        Button(t.label) { add(t) }
      }
    }
    Divider()
    ForEach(Self.fixed, id: \.self) { t in
      Button { add(t) } label: { Label(t.label, systemImage: t.symbol) }
    }
  }
}
