import AppKit
import Foundation
import Observation
import SwiftUI
import Testing
import FlatBuffers
import TsugumiRPC
import TsugumiLauncher
import WebKit
@testable import TsugumiApp

private func html(_ name: String) -> CardViewDeclaration {
  CardViewDeclaration(name: name, type: "html", blob: "/nix/store/00000000000000000000000000000000-\(name).html")
}

private func cardPath(_ key: CardKey) -> Tsugumi_Data_CardPathT {
  let p = Tsugumi_Data_CardPathT()
  p.segments = path(key)
  return p
}

@MainActor private func payload(_ value: NativeObject, _ type: Tsugumi_RPC_NotificationPayload) -> Tsugumi_RPC_NotificationPayloadUnion {
  Tsugumi_RPC_NotificationPayloadUnion(value, type: type)
}

@MainActor private func declared(_ card: CardKey, _ views: [CardViewDeclaration], failure: String? = nil) -> Tsugumi_RPC_NotificationPayloadUnion {
  let u = Tsugumi_RPC_CardViewsUpdatedT()
  u.card = cardPath(card)
  if let failure {
    let f = Tsugumi_Data_FailureT()
    f.kind = .nixFailed
    f.diagnostic = failure
    u.failure = f
  }
  u.views = views.map { v in
    let w = Tsugumi_Data_CardViewT()
    w.name = v.name; w.type = v.type; w.blob = v.blob
    return w
  }
  return payload(u, .cardviewsupdated)
}

@MainActor private func sent(_ card: CardKey, _ view: String, _ bytes: [UInt8], run: String? = nil) -> Tsugumi_RPC_NotificationPayloadUnion {
  let d = Tsugumi_Data_DisplayT()
  d.card = cardPath(card); d.view = view; d.bytes = bytes; d.runId = run
  let u = Tsugumi_RPC_DisplayUpdatedT()
  u.display = d
  return payload(u, .displayupdated)
}

@MainActor private func runState(_ card: CardKey, _ run: String, _ state: Tsugumi_Data_DisplayRunState) -> Tsugumi_RPC_NotificationPayloadUnion {
  let r = Tsugumi_RPC_DisplayRunStateChangedT()
  r.card = cardPath(card); r.runId = run; r.state = state
  return payload(r, .displayrunstatechanged)
}

@Test func pendingDataIsReplacedButRunStateKeepsItsOrder() {
  var box = CardViewOutbox()
  box.put(.run(started: true))
  box.put(.data(Data([1])))
  box.put(.data(Data([2])))
  box.put(.run(started: false))
  box.put(.data(Data([3])))
  #expect(box.pending == [.run(started: true), .data(Data([2])), .run(started: false), .data(Data([3]))])
  #expect(box.take() == .run(started: true))
  box.put(.data(Data([4])))
  #expect(box.pending == [.data(Data([2])), .run(started: false), .data(Data([4]))])
}

@MainActor @Test func theSessionRoutesViewTrafficByNameForOpenWindowsOnly() throws {
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-views-\(UUID().uuidString)")
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))

  // A Card without an open window keeps nothing.
  s.applyViewNotification(declared("plot", [html("chart")]))
  s.applyViewNotification(sent("plot", "chart", [1]))
  #expect(s.cardViews.cards.isEmpty)

  let window = UUID()
  s.openDisplayViewer(window, card: "plot")
  // Declarations arrive before any run; there is no data yet.
  s.applyViewNotification(declared("plot", [html("chart"), html("notes"), CardViewDeclaration(name: "gpu", type: "dawn", blob: "/nix/store/x-gpu.wasm")]))
  #expect(s.cardViews.declarations["plot"]?.map(\.name) == ["chart", "notes", "gpu"])
  #expect(s.cardViews.runs["plot"] == nil)

  var chart: [CardViewMessage] = [], notes: [CardViewMessage] = []
  let a = s.cardViews.attach("plot", view: "chart") { chart.append($0) }
  let b = s.cardViews.attach("plot", view: "notes") { notes.append($0) }
  s.applyViewNotification(runState("plot", "7", .started))
  // Arbitrary bytes, NUL and non-UTF-8 included, reach only the named view.
  s.applyViewNotification(sent("plot", "chart", [0, 0xff, 0xfe, 0], run: "7"))
  s.applyViewNotification(sent("plot", "missing", [5], run: "7"))
  s.applyViewNotification(runState("plot", "7", .ended))
  // A Card thread may still send after its call returned, with no active run.
  s.applyViewNotification(sent("plot", "chart", [9]))
  #expect(chart == [.run(started: true), .data(Data([0, 0xff, 0xfe, 0])), .run(started: false), .data(Data([9]))])
  #expect(notes == [.run(started: true), .run(started: false)])
  #expect(s.cardViews.undeclared("plot") == ["missing"])

  // A window opened later starts from the state so far.
  var late: [CardViewMessage] = []
  let c = s.cardViews.attach("plot", view: "chart") { late.append($0) }
  #expect(late == [.run(started: true), .data(Data([9])), .run(started: false)])

  // Declaring the name later clears the error; an empty list clears the declarations.
  s.applyViewNotification(declared("plot", [html("chart"), html("missing")]))
  #expect(s.cardViews.undeclared("plot").isEmpty)
  #expect(s.cardViews.ambiguous("plot").isEmpty)

  // A name two declarations share is reported, and data sent to it reaches neither.
  s.applyViewNotification(declared("plot", [html("chart"), html("chart")]))
  #expect(s.cardViews.ambiguous("plot") == ["chart"])
  s.applyViewNotification(sent("plot", "chart", [4]))
  #expect(chart.last == .data(Data([9])))
  #expect(s.cardViews.undeclared("plot").isEmpty)
  s.applyViewNotification(declared("plot", [], failure: "views are broken"))
  #expect(s.cardViews.failures["plot"] == "views are broken")
  #expect(s.cardViews.undeclared("plot").isEmpty, "a failed evaluation is shown instead of undeclared names")
  s.applyViewNotification(declared("plot", []))
  #expect(s.cardViews.failures["plot"] == nil, "an evaluated empty list has no failure")
  #expect(s.cardViews.declarations["plot"] == [])

  for id in [a, b, c] { s.cardViews.detach(id) }
  #expect(s.cardViews.attachedCount == 0)
  s.closeDisplayViewer(window)
  #expect(s.cardViews.cards.isEmpty, "nothing stays after the last window closes")
}

@MainActor @Test func viewFilesAreReadPageByPageWithinTheLimit() async throws {
  var asked: [UInt64] = []
  let files = CardViewFiles { card, index, name, offset in
    #expect(card == ["plot"] && index == 1 && name == "chart")
    asked.append(offset)
    return offset == 0 ? CardViewFilePage(bytes: Data("<p>".utf8), eof: false, nextOffset: 3)
      : CardViewFilePage(bytes: Data("x</p>".utf8), eof: true, nextOffset: nil)
  }
  #expect(try await files.read(card: ["plot"], index: 1, name: "chart") == Data("<p>x</p>".utf8))
  #expect(asked == [0, 3])
  let stuck = CardViewFiles { _, _, _, _ in CardViewFilePage(bytes: Data(), eof: false, nextOffset: 0) }
  await #expect(throws: CardViewFileError.self) { try await stuck.read(card: [], index: 0, name: "a") }
  let huge = CardViewFiles { _, _, _, _ in CardViewFilePage(bytes: Data(count: CardViewFiles.limit + 1), eof: true, nextOffset: nil) }
  await #expect(throws: CardViewFileError.self) { try await huge.read(card: [], index: 0, name: "a") }
  await #expect(throws: CardViewFileError.self) { try await CardViewFiles.unavailable.read(card: [], index: 0, name: "a") }
}

/// Watches the view's own document from an isolated world, so the test sees
/// what the view received without giving the view any channel back.
@MainActor @Observable private final class Probe: NSObject, WKScriptMessageHandler {
  var log: [String] = []
  static let world = WKContentWorld.world(name: "tsugumi-test-probe")

  func install(_ configuration: WKWebViewConfiguration) {
    let script = """
      if (location.href === 'about:srcdoc') {
        const report = () => window.webkit.messageHandlers.probe.postMessage(document.body.dataset.log || '[]')
        new MutationObserver(report).observe(document.body, { attributes: true, attributeFilter: ['data-log'] })
        report()
      }
      """
    configuration.userContentController.addUserScript(WKUserScript(
      source: script, injectionTime: .atDocumentEnd, forMainFrameOnly: false, in: Self.world))
    configuration.userContentController.add(self, contentWorld: Self.world, name: "probe")
  }

  func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
    guard let text = message.body as? String, let data = text.data(using: .utf8),
          let entries = try? JSONSerialization.jsonObject(with: data) as? [String] else { return }
    log = entries
  }
}

private func viewDocument(_ out: Outside) -> String {
  """
  <!doctype html><html><head><link rel="stylesheet" href="\(out.address("sheet.css"))"></head><body>
  <img src="\(out.address("tag.png"))">
  <script>
  const log = []
  const note = entry => { log.push(entry); document.body.dataset.log = JSON.stringify(log) }
  addEventListener('message', event => {
    if (event.data?.tsugumi !== 1 || !(event.data.port instanceof MessagePort)) return
    note('port')
    event.data.port.onmessage = ({ data }) => {
      if (data.type === 'data') note('data:' + Array.from(new Uint8Array(data.bytes)).join(','))
      else note('run:' + data.state)
    }
  })
  fetch('\(out.address("fetch"))').catch(() => {})
  new Image().src = '\(out.address("image"))'
  try { new WebSocket('ws://127.0.0.1:\(out.port)/socket') } catch {}
  try { navigator.sendBeacon('\(out.address("beacon"))') } catch {}
  try { window.open('\(out.address("popup"))') } catch {}
  setTimeout(() => { try { location.href = '\(out.address("leave"))' } catch {} }, 20)
  </script></body></html>
  """
}

@MainActor @Test func anHTMLViewGetsItsPortAndOrderedMessagesWithoutNetwork() async throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let out = try Outside()
  defer { out.stop() }
  let probe = Probe()
  let relay = CardViewRelay()
  relay.declare("plot", [html("chart"), html("other")])
  let host = HTMLViewHost { probe.install($0) }
  host.web.frame = CGRect(x: 0, y: 0, width: 400, height: 300)
  #expect(host.web.configuration.websiteDataStore.isPersistent == false)

  try await host.mount(viewDocument(out))
  try await waitForModel("the view took its port") { probe.log == ["port"] }

  let id = relay.attach("plot", view: "chart") { [weak host] in host?.send($0) }
  relay.runState("plot", runId: "1", started: true)
  relay.receive("plot", view: "other", bytes: Data([7]))
  relay.receive("plot", view: "chart", bytes: Data([0, 255, 0, 10]))
  relay.runState("plot", runId: "1", started: false)
  // After the Card's call returned.
  relay.receive("plot", view: "chart", bytes: Data([9]))
  try await waitForModel("the view received the run") { probe.log.last == "data:9" }
  #expect(probe.log == ["port", "run:started", "data:0,255,0,10", "run:ended", "data:9"])

  // Teardown: nothing more reaches the view.
  relay.detach(id)
  host.close()
  relay.receive("plot", view: "chart", bytes: Data([1]))
  host.send(.data(Data([2])))
  try await Task.sleep(for: .milliseconds(300))
  #expect(probe.log.last == "data:9")
  #expect(relay.attachedCount == 0)
  #expect(out.requests == [], "the view reached the network: \(out.requests)")
}

@MainActor @Test func theWindowHostsEachDeclaredTypeAndShowsDawnAsAPlaceholder() throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-views-\(UUID().uuidString)")
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  let window = UUID()
  s.openDisplayViewer(window, card: "plot")
  defer { s.closeDisplayViewer(window) }
  s.applyViewNotification(declared("plot", [CardViewDeclaration(name: "gpu", type: "dawn", blob: "/nix/store/x-gpu.wasm")]))
  let view = NSHostingView(rootView: CardViewsPanel(session: s, cardKey: "plot"))
  view.frame = CGRect(x: 0, y: 0, width: 500, height: 400)
  view.layoutSubtreeIfNeeded()
  func webViews(_ v: NSView) -> Int { (v is WKWebView ? 1 : 0) + v.subviews.map(webViews).reduce(0, +) }
  #expect(webViews(view) == 0, "a dawn view does not get a WebKit host")
}
