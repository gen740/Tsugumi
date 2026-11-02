import AppKit
import SwiftUI
import TsugumiRPC

private let cardWidth: CGFloat = 190
private let headerHeight: CGFloat = 40
private let portSpacing: CGFloat = 18

/// Ports of a Card for drawing: resolved ports when available, else declared inputs.
struct CardPorts {
  var inputs: [String]
  var outputs: [String]
}

struct CanvasView: View {
  @Bindable var session: ProjectSession
  @State private var drag: (key: CardKey, offset: CGSize)?
  @State private var pending: (from: (CardKey, String), point: CGPoint)?
  @State private var panStart: (x: Double, y: Double)?
  @State private var selectedConnection: Int?
  @State private var zoomStart: Double?

  private var scopeKey: CardKey { key(session.scope) }
  private var camera: (x: Double, y: Double, zoom: Double) { session.cameras[scopeKey] ?? (40, 40, 1) }

  var body: some View {
    GeometryReader { _ in
      ZStack(alignment: .topLeading) {
        Color(nsColor: .textBackgroundColor)
          .contentShape(Rectangle())
          .gesture(panGesture)
          .onTapGesture {
            session.selection.removeAll()
            selectedConnection = nil
            session.saveViewSoon()
          }
        Canvas { ctx, _ in drawConnections(&ctx) }
          .allowsHitTesting(false)
        ForEach(cards, id: \.key) { item in
          CardNode(session: session, key: item.key, card: item.card, ports: ports(item.key),
                   selected: session.selection.contains(item.key),
                   onPortDrag: { port, pt in pending = ((item.key, port), pt) },
                   onPortDrop: { port, pt in finishConnect(from: (item.key, port), at: pt) })
            .scaleEffect(camera.zoom, anchor: .topLeading)
            .position(at: screen(origin(item.key)), size: nodeSize(item.key), zoom: camera.zoom)
            .gesture(cardDrag(item.key))
            .onTapGesture(count: 2) { enter(item) }
            .onTapGesture { select(item.key) }
        }
        displayPanels
        if let p = pending {
          Path { path in
            path.move(to: screen(portPoint(p.from.0, p.from.1, output: true)))
            path.addLine(to: p.point)
          }
          .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 2, dash: [6, 4]))
          .allowsHitTesting(false)
        }
        breadcrumbs.padding(8)
      }
      .coordinateSpace(name: "canvas")
      .clipped()
      .overlay(ScrollCatcher { dx, dy, zoomDelta in
        var c = camera
        if zoomDelta != 0 {
          c.zoom = max(0.2, min(3, c.zoom * (1 + zoomDelta)))
        } else {
          c.x += dx
          c.y += dy
        }
        session.cameras[scopeKey] = c
        session.saveViewSoon()
      })
      .gesture(MagnifyGesture().onChanged { v in
        if zoomStart == nil { zoomStart = camera.zoom }
        var c = camera
        c.zoom = max(0.2, min(3, (zoomStart ?? 1) * v.magnification))
        session.cameras[scopeKey] = c
      }.onEnded { _ in
        zoomStart = nil
        session.saveViewSoon()
      })
      .onDeleteCommand { deleteSelection() }
    }
  }

  // MARK: - Data

  private var cards: [(key: CardKey, card: JSONValue)] {
    session.cards(in: session.scope).compactMap { c in
      guard let id = c["id"]?.string else { return nil }
      return (key(session.scope + [id]), c)
    }
  }

  func ports(_ k: CardKey) -> CardPorts {
    if let r = session.resolution[k] {
      return CardPorts(inputs: r.inputs.map(\.name), outputs: r.outputs.map(\.name))
    }
    // Unsaved Card: declared inputs only.
    let def = session.instance(k)?["definition"]?.string
    let ins = def.flatMap { session.definitions[$0]?["inputs"]?.array }?.compactMap { $0["name"]?.string } ?? []
    return CardPorts(inputs: ins, outputs: [])
  }

  private func nodeSize(_ k: CardKey) -> CGSize {
    let p = ports(k)
    return CGSize(width: cardWidth, height: headerHeight + CGFloat(max(p.inputs.count, p.outputs.count, 1)) * portSpacing + 8)
  }

  private func origin(_ k: CardKey) -> CGPoint {
    var p = session.position(k)
    if let d = drag, d.key == k || (session.selection.contains(k) && session.selection.contains(d.key)) {
      p.x += d.offset.width / camera.zoom
      p.y += d.offset.height / camera.zoom
    }
    return p
  }

  private func screen(_ world: CGPoint) -> CGPoint {
    CGPoint(x: world.x * camera.zoom + camera.x, y: world.y * camera.zoom + camera.y)
  }

  private func world(_ screenPoint: CGPoint) -> CGPoint {
    CGPoint(x: (screenPoint.x - camera.x) / camera.zoom, y: (screenPoint.y - camera.y) / camera.zoom)
  }

  private func portPoint(_ k: CardKey, _ port: String, output: Bool) -> CGPoint {
    let o = origin(k)
    let list = output ? ports(k).outputs : ports(k).inputs
    let i = CGFloat(list.firstIndex(of: port) ?? 0)
    return CGPoint(x: o.x + (output ? cardWidth : 0), y: o.y + headerHeight + i * portSpacing + portSpacing / 2)
  }

  // MARK: - Drawing

  private func drawConnections(_ ctx: inout GraphicsContext) {
    for (i, c) in session.connections(in: session.scope).enumerated() {
      guard let fc = c["from"]?["card"]?.string, let fp = c["from"]?["port"]?.string,
            let tc = c["to"]?["card"]?.string, let tp = c["to"]?["port"]?.string else { continue }
      let a = screen(portPoint(key(session.scope + [fc]), fp, output: true))
      let b = screen(portPoint(key(session.scope + [tc]), tp, output: false))
      var path = Path()
      path.move(to: a)
      let dx = max(40, abs(b.x - a.x) / 2)
      path.addCurve(to: b, control1: CGPoint(x: a.x + dx, y: a.y), control2: CGPoint(x: b.x - dx, y: b.y))
      let active = session.cardStates[key(session.scope + [fc])]?.state == .running
      ctx.stroke(path, with: .color(i == selectedConnection ? .accentColor : (active ? .orange : .secondary)),
                 lineWidth: i == selectedConnection ? 3 : 1.6)
    }
  }

  @ViewBuilder private var displayPanels: some View {
    ForEach(cards.filter { session.cardSetting($0.key, "canvasEnabled")?.bool == true }, id: \.key) { item in
      let o = origin(item.key)
      let panel = session.layout["displays"]?[item.key]
      let size = CGSize(width: panel?["width"]?.double ?? 320, height: panel?["height"]?.double ?? 160)
      let at = CGPoint(x: panel?["x"]?.double ?? o.x, y: panel?["y"]?.double ?? (o.y + nodeSize(item.key).height + 16))
      DisplayPanel(display: session.displays[item.key], title: item.card["name"]?.string ?? item.card["id"]?.string ?? "")
        .frame(width: size.width, height: size.height)
        .scaleEffect(camera.zoom, anchor: .topLeading)
        .position(at: screen(at), size: size, zoom: camera.zoom)
    }
  }

  private var breadcrumbs: some View {
    HStack(spacing: 4) {
      Button(session.project["name"]?.string ?? "Project") { setScope([]) }
        .buttonStyle(.link)
      ForEach(Array(session.scope.enumerated()), id: \.offset) { i, id in
        Image(systemName: "chevron.right").font(.caption)
        Button(id) { setScope(Array(session.scope.prefix(i + 1))) }.buttonStyle(.link)
      }
    }
    .padding(6)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 6))
  }

  // MARK: - Gestures

  private var panGesture: some Gesture {
    DragGesture(minimumDistance: 2)
      .onChanged { v in
        if panStart == nil { panStart = (camera.x, camera.y) }
        var c = camera
        c.x = panStart!.x + v.translation.width
        c.y = panStart!.y + v.translation.height
        session.cameras[scopeKey] = c
      }
      .onEnded { _ in
        panStart = nil
        session.saveViewSoon()
      }
  }

  private func cardDrag(_ k: CardKey) -> some Gesture {
    DragGesture(minimumDistance: 3, coordinateSpace: .named("canvas"))
      .onChanged { v in
        if !session.selection.contains(k) { session.selection = [k] }
        drag = (k, v.translation)
      }
      .onEnded { v in
        let moved = session.selection.contains(k) ? session.selection : [k]
        let dx = v.translation.width / camera.zoom, dy = v.translation.height / camera.zoom
        drag = nil
        // Layout only: no computation is re-resolved for a move.
        session.edit { s in
          for m in moved {
            let p = s.position(m)
            s.setPosition(m, CGPoint(x: p.x + dx, y: p.y + dy))
          }
        }
      }
  }

  private func select(_ k: CardKey) {
    if NSEvent.modifierFlags.contains(.shift) {
      if session.selection.contains(k) { session.selection.remove(k) } else { session.selection.insert(k) }
    } else {
      session.selection = [k]
    }
    selectedConnection = nil
    session.saveViewSoon()
  }

  private func enter(_ item: (key: CardKey, card: JSONValue)) {
    guard item.card["canvas"] != nil else { return }
    setScope(path(item.key))
  }

  private func setScope(_ s: [String]) {
    session.scope = s
    session.selection.removeAll()
    session.saveViewSoon()
  }

  private func finishConnect(from: (CardKey, String), at pt: CGPoint) {
    pending = nil
    let w = world(pt)
    for item in cards where item.key != from.0 {
      for port in ports(item.key).inputs {
        let p = portPoint(item.key, port, output: false)
        if hypot(p.x - w.x, p.y - w.y) < 14 {
          let fromId = path(from.0).last!, toId = path(item.key).last!
          session.edit { s in
            s.updateGraph(s.scope) { g in
              var conns = g["connections"]?.array ?? []
              let c: JSONValue = .object([
                ("from", .object([("card", .string(fromId)), ("port", .string(from.1))])),
                ("to", .object([("card", .string(toId)), ("port", .string(port))])),
              ])
              if !conns.contains(c) { conns.append(c) }
              g["connections"] = .array(conns)
            }
          }
          return
        }
      }
    }
  }

  private func deleteSelection() {
    let ids = Set(session.selection.map { path($0).last! })
    guard !ids.isEmpty else { return }
    session.edit { s in
      s.updateGraph(s.scope) { g in
        g["cards"] = .array((g["cards"]?.array ?? []).filter { !ids.contains($0["id"]?.string ?? "") })
        g["connections"] = .array((g["connections"]?.array ?? []).filter {
          !ids.contains($0["from"]?["card"]?.string ?? "") && !ids.contains($0["to"]?["card"]?.string ?? "")
        })
      }
      for k in s.selection { s.layout["cards"]?[k] = nil }
    }
    session.selection.removeAll()
  }
}

private extension View {
  /// Places a view by its top-left corner in canvas coordinates.
  func position(at topLeft: CGPoint, size: CGSize, zoom: Double) -> some View {
    position(x: topLeft.x + size.width * zoom / 2, y: topLeft.y + size.height * zoom / 2)
      .frame(width: size.width, height: size.height, alignment: .topLeading)
  }
}

struct CardNode: View {
  let session: ProjectSession
  let key: CardKey
  let card: JSONValue
  let ports: CardPorts
  let selected: Bool
  let onPortDrag: (String, CGPoint) -> Void
  let onPortDrop: (String, CGPoint) -> Void

  var body: some View {
    let state = session.cardStates[key]
    let diags = session.resolution[key]?.diagnostics.filter(\.error) ?? []
    VStack(alignment: .leading, spacing: 0) {
      HStack(spacing: 6) {
        Circle().fill(color(state?.state)).frame(width: 9, height: 9)
        VStack(alignment: .leading, spacing: 1) {
          Text(card["name"]?.string ?? definitionName ?? card["id"]?.string ?? "?")
            .font(.system(size: 12, weight: .semibold)).lineLimit(1)
          Text(subtitle).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
        }
        Spacer(minLength: 0)
        if !diags.isEmpty {
          Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow).help(diags.map(\.message).joined(separator: "\n"))
        }
      }
      .padding(.horizontal, 8)
      .frame(height: headerHeight)
      HStack(alignment: .top, spacing: 0) {
        VStack(alignment: .leading, spacing: 0) {
          ForEach(ports.inputs, id: \.self) { p in
            HStack(spacing: 4) {
              Circle().strokeBorder(Color.secondary).frame(width: 9, height: 9).offset(x: -4.5)
              Text(p).font(.system(size: 10)).foregroundStyle(.secondary)
            }
            .frame(height: portSpacing)
          }
        }
        Spacer(minLength: 0)
        VStack(alignment: .trailing, spacing: 0) {
          ForEach(ports.outputs, id: \.self) { p in
            HStack(spacing: 4) {
              Text(p).font(.system(size: 10)).foregroundStyle(.secondary)
              Circle().fill(Color.accentColor).frame(width: 9, height: 9).offset(x: 4.5)
                .gesture(DragGesture(coordinateSpace: .named("canvas"))
                  .onChanged { onPortDrag(p, $0.location) }
                  .onEnded { onPortDrop(p, $0.location) })
            }
            .frame(height: portSpacing)
          }
        }
      }
      .padding(.bottom, 8)
    }
    .frame(width: cardWidth, alignment: .topLeading)
    .background(RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .controlBackgroundColor)))
    .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(selected ? Color.accentColor : Color.secondary.opacity(0.4),
                                                            lineWidth: selected ? 2 : 1))
    .shadow(radius: selected ? 4 : 1)
    .accessibilityElement(children: .combine)
    .accessibilityLabel("Card \(card["id"]?.string ?? ""), \(subtitle)")
  }

  private var definitionName: String? {
    card["definition"]?.string.flatMap { session.definitions[$0]?["name"]?.string }
  }

  private var subtitle: String {
    var parts: [String] = []
    if card["canvas"] != nil { parts.append("subgraph · double-click to open") }
    if let d = card["definition"]?.string {
      let def = session.definitions[d]
      parts.append(def?["template"]?.string ?? def?["language"]?.string ?? d)
    }
    if let s = session.cardStates[key] {
      parts.append("\(s.state) #\(s.invocation)")
    }
    return parts.joined(separator: " · ")
  }

  private func color(_ s: Tsugumi_Data_CardState?) -> Color {
    switch s {
    case .running?, .preparing?: return .orange
    case .succeeded?: return .green
    case .failed?: return .red
    case .blocked?: return .purple
    case .skipped?: return .gray
    case .cancelled?: return .brown
    default: return .secondary.opacity(0.5)
    }
  }
}

/// Two-finger scroll pans the canvas; ⌘-scroll zooms.
struct ScrollCatcher: NSViewRepresentable {
  let onScroll: (Double, Double, Double) -> Void

  func makeNSView(context: Context) -> NSView { CatcherView(onScroll: onScroll) }
  func updateNSView(_ nsView: NSView, context: Context) { (nsView as? CatcherView)?.onScroll = onScroll }

  final class CatcherView: NSView {
    var onScroll: (Double, Double, Double) -> Void
    init(onScroll: @escaping (Double, Double, Double) -> Void) {
      self.onScroll = onScroll
      super.init(frame: .zero)
    }
    required init?(coder: NSCoder) { fatalError() }
    override func hitTest(_ point: NSPoint) -> NSView? {
      // Only take scroll events; clicks fall through to SwiftUI.
      NSApp.currentEvent?.type == .scrollWheel ? self : nil
    }
    override func scrollWheel(with e: NSEvent) {
      if e.modifierFlags.contains(.command) { onScroll(0, 0, Double(e.scrollingDeltaY) * 0.01) }
      else { onScroll(Double(e.scrollingDeltaX), Double(e.scrollingDeltaY), 0) }
    }
  }
}
