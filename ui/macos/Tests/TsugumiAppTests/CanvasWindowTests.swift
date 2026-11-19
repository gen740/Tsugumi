import AppKit
import Observation
import SwiftUI
import Testing
import TsugumiTestSupport
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

/// Geometry used by the production hit shapes, without synthetic mouse events
/// whose delivery depends on an off-screen WindowServer window.
@Suite(.serialized) @MainActor struct CanvasWindowTests {
  @Test func routingHitRegionsLeaveTheBodyForMoving() {
    for flow in [FlowDirection.topToBottom, .leftToRight] {
      for zoom in [1.0, 0.2] {
        for kind in [CardKind.trigger, .branch, .fork, .merge, .collect, .switchCard] {
          let inputs = kind == .trigger || kind == .switchCard ? [] :
            (kind == .branch ? [PortData(name: "value"), PortData(name: "condition")] : [PortData(name: "in")])
          let outputs = kind == .branch || kind == .fork ? [PortData(name: "a"), PortData(name: "b")] : [PortData(name: "out")]
          let data = CardNodeData(title: "start", kind: kind, inputs: inputs, outputs: outputs)
          let g = CardGeometry(data, flow: flow)
          let hit = CardNodeView.hitShape(data, flow: flow, zoom: zoom, newPorts: false)
          let body = kind == .trigger ? CGPoint(x: 55, y: 17) : g.iconCenter(in: g.outlineRect)
          let point = CGPoint(x: body.x * zoom, y: body.y * zoom)
          #expect(hit.contains(point), "\(kind), \(flow), \(zoom)")
          for (ports, output) in [(inputs, false), (outputs, true)] {
            for port in ports {
              let pt = g.port(port.name, output: output)
              let rect = CGRect(x: pt.x * zoom - g.portHitSize(zoom: zoom) / 2,
                                y: pt.y * zoom - g.portHitSize(zoom: zoom) / 2,
                                width: g.portHitSize(zoom: zoom), height: g.portHitSize(zoom: zoom))
              #expect(!rect.contains(point), "The wire target must leave the body free")
              #expect(hit.contains(CGPoint(x: pt.x * zoom, y: pt.y * zoom)))
            }
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
    let source = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
      .appendingPathComponent("../../Resources").standardizedFileURL
    let resources = testTemporaryDirectory.appendingPathComponent("language-restart-\(UUID())")
    let fm = FileManager.default
    defer { try? fm.removeItem(at: resources) }
    let englishFolder = resources.appendingPathComponent("en.lproj")
    try fm.createDirectory(at: englishFolder, withIntermediateDirectories: true)
    try fm.copyItem(at: source.appendingPathComponent("ja.lproj"), to: resources.appendingPathComponent("ja.lproj"))

    let strings = englishFolder.appendingPathComponent("Localizable.strings")
    try Data().write(to: strings)
    let output = try FileHandle(forWritingTo: strings)
    let generate = Process()
    generate.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    generate.arguments = ["python3", source.deletingLastPathComponent().appendingPathComponent("../shared/i18n/strings.py").path]
    generate.standardOutput = output
    try generate.run()
    generate.waitUntilExit()
    try output.close()
    try #require(generate.terminationStatus == 0)

    let bundle = try #require(Bundle(path: resources.path))
    let english = LanguageRestartText(language: .en, resources: bundle)
    let japanese = LanguageRestartText(language: .ja, resources: bundle)
    #expect(!english.now.isEmpty && !english.later.isEmpty && !english.message.isEmpty)
    #expect(english.now != japanese.now && english.message != japanese.message)
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
