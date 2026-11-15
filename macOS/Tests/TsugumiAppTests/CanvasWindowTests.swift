import AppKit
import Observation
import SwiftUI
import Testing
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

/// Real SwiftUI hit testing and gesture arbitration, without launching the app
/// or putting a window on the user's screen.
@Suite(.serialized) @MainActor struct CanvasWindowTests {
  @Observable fileprivate final class Interaction {
    var position = CGPoint(x: 60, y: 60)
    var moved = CGSize.zero
    var wires = 0
    var runs = 0
    var edits = 0
  }

  private struct RoutingCanvas: View {
    let data: CardNodeData
    let flow: FlowDirection
    let zoom: Double
    let interaction: Interaction

    var body: some View {
      CardNodeView(data: data, flow: flow, zoom: zoom,
        onParamCommit: { _, _, _, _ in interaction.edits += 1; return true },
        onPortDrag: { _, _ in interaction.wires += 1 }, onInputDrag: { _, _ in interaction.wires += 1 },
        onRun: { interaction.runs += 1 })
        .placeScaled(at: interaction.position, size: CardGeometry(data, flow: flow).size, zoom: zoom)
        .gesture(DragGesture(minimumDistance: 3, coordinateSpace: .named("canvas")).onEnded {
          interaction.moved = $0.translation
          interaction.position.x += $0.translation.width
          interaction.position.y += $0.translation.height
        })
        .frame(width: 360, height: 240)
        .coordinateSpace(name: "canvas")
    }
  }

  private func send(_ type: NSEvent.EventType, at point: CGPoint, to window: NSWindow) async throws {
    let event = try #require(NSEvent.mouseEvent(with: type,
      location: CGPoint(x: point.x, y: window.frame.height - point.y), modifierFlags: [],
      timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window.windowNumber,
      context: nil, eventNumber: 0, clickCount: 1, pressure: 1))
    window.sendEvent(event)
    try await Task.sleep(for: .milliseconds(25))
  }

  @Test func routingBodiesMoveWithoutStartingAWire() async throws {
    for flow in [FlowDirection.topToBottom, .leftToRight] {
      for zoom in [1.0, 0.2] {
        for kind in [CardKind.trigger, .branch, .fork, .merge, .collect, .switchCard] {
          let ports = kind == .branch ? [PortData(name: "value"), PortData(name: "condition")] : [PortData(name: "in")]
          let outputs = kind == .branch || kind == .fork ? [PortData(name: "a"), PortData(name: "b")] : [PortData(name: "out")]
          let data = CardNodeData(title: "start", kind: kind, inputs: kind == .trigger || kind == .switchCard ? [] : ports, outputs: outputs)
          let g = CardGeometry(data, flow: flow)
          let c = Interaction()
          let host = NSHostingView(rootView: RoutingCanvas(data: data, flow: flow, zoom: zoom, interaction: c))
          let window = NSWindow(contentRect: CGRect(x: -10000, y: -10000, width: 360, height: 240),
                                styleMask: [.borderless], backing: .buffered, defer: false)
          window.contentView = host
          window.orderBack(nil)
          defer { window.orderOut(nil); window.contentView = nil }
          host.layoutSubtreeIfNeeded()
          try await Task.sleep(for: .milliseconds(50))
          // The start's title, or the middle of the routing body.
          let body = kind == .trigger ? CGPoint(x: 55, y: 17) : g.iconCenter(in: g.outlineRect)
          // Mouse events land on screen pixels; keep the translation exact.
          let from = CGPoint(x: (60 + body.x * zoom).rounded(), y: (60 + body.y * zoom).rounded())
          try await send(.leftMouseDown, at: from, to: window)
          for step in 1...4 {
            try await send(.leftMouseDragged, at: CGPoint(x: from.x + Double(step) * 10, y: from.y + Double(step) * 5), to: window)
          }
          try await send(.leftMouseUp, at: CGPoint(x: from.x + 40, y: from.y + 20), to: window)
          #expect(c.moved == CGSize(width: 40, height: 20), "\(kind), \(flow), \(zoom)")
          #expect(c.position == CGPoint(x: 100, y: 80))
          #expect(c.wires == 0 && c.runs == 0 && c.edits == 0, "\(kind), \(flow), \(zoom)")
          // Input and output ports still start a wire, exclusively, at either zoom.
          for output in [false, true] {
            guard let p = (output ? data.outputs : data.inputs).first else { continue }
            c.wires = 0
            c.moved = .zero
            let port = g.port(p.name, output: output)
            let fromPort = CGPoint(x: c.position.x + port.x * zoom, y: c.position.y + port.y * zoom)
            try await send(.leftMouseDown, at: fromPort, to: window)
            try await send(.leftMouseDragged, at: CGPoint(x: fromPort.x + 20, y: fromPort.y + 20), to: window)
            try await send(.leftMouseUp, at: CGPoint(x: fromPort.x + 20, y: fromPort.y + 20), to: window)
            #expect(c.wires > 0 && c.moved == .zero, "\(kind), \(flow), \(zoom), output=\(output)")
          }
          if kind == .trigger {
            let button = CGPoint(x: c.position.x + 17 * zoom, y: c.position.y + 17 * zoom)
            try await send(.leftMouseDown, at: button, to: window)
            try await send(.leftMouseUp, at: button, to: window)
            #expect(c.runs == 1)
          }
        }
      }
    }
  }
}

extension CanvasWindowTests {
  @Test(.hostService) func statusBarKeepsOneLineInANarrowCanvas() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("canvas-status-\(UUID())")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    try Data(#"{"name":"status","canvas":{"cards":[],"connections":[]}}"#.utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
    let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
    let workspace = Workspace(), tab = WorkflowTab(scope: [])
    // Offline and failed save states both used to wrap in the last layout.
    for dirty in [false, true] {
      if dirty { s.project = try .parse(#"{"name":"edited","canvas":{"cards":[],"connections":[]}}"#) }
      for width in [900.0, 350, 180, 80, 1] {
        let bar = StatusBar(workspace: workspace, session: s, tab: tab)
        let view = NSHostingView(rootView: bar.frame(width: width))
        let size = view.fittingSize
        #expect(size.height > 20 && size.height <= 36, "\(width), dirty=\(dirty): \(size)")
        // The bottom controls can move to a second row; the status itself cannot grow.
        let bottom = NSHostingView(rootView: CanvasBottomControls {
          Color.clear.frame(width: 320, height: 30)
        } status: { bar }.frame(width: width))
        #expect(bottom.fittingSize.height <= 80)
      }
    }
  }

  @Test func languageRestartTracksLocalAndRemoteSelections() {
    for running in AppPreferences.Language.allCases {
      let next: AppPreferences.Language = running == .ja ? .en : .ja
      var state = LanguageRestart(running: running)
      #expect(!state.pending)
      let changed = state.select(next)
      #expect(changed)
      #expect(state.pending)
      // A host echo of the local edit does not show a second notice.
      let echo = state.select(next)
      #expect(!echo)
      #expect(state.pending) // Choosing Later leaves the setting pending.
      let reverted = state.select(running)
      #expect(reverted)
      #expect(!state.pending)
      // A subsequent Web selection takes exactly the same path.
      let remote = state.select(next)
      #expect(remote && state.pending)
    }
  }

  @Test func languageRestartNoticeUsesTheSelectedLanguage() throws {
    let resources = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../Resources").standardizedFileURL
    let bundle = try #require(Bundle(path: resources.path))
    let english = LanguageRestartText(language: .en, resources: bundle)
    #expect(english.now == "Restart now" && english.later == "Later")
    #expect(english.message.contains("already use the new language"))
    let japanese = LanguageRestartText(language: .ja, resources: bundle)
    #expect(japanese.now == "今すぐ再起動" && japanese.later == "あとで")
    #expect(japanese.message.contains("すでに新しい言語"))
  }
}

extension CanvasWindowTests {
  @Test func languageRestartReopensOnlyAfterAnAcceptedQuit() {
    let updater = Updater(channel: .dev)
    var askedToQuit = 0
    var reopened: [URL] = []
    updater.restart { askedToQuit += 1 }
    #expect(askedToQuit == 1 && updater.relaunch)
    #expect(reopened.isEmpty)
    // The normal unsaved-edit confirmation can cancel the restart.
    updater.cancelRelaunch()
    updater.installIfReady { reopened.append($0) }
    #expect(reopened.isEmpty)
    updater.restart { askedToQuit += 1 }
    // Only applicationWillTerminate calls this after edits are settled.
    updater.installIfReady { reopened.append($0) }
    #expect(askedToQuit == 2 && reopened.count == 1)
  }
}
