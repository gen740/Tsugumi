import AppKit
import Carbon.HIToolbox
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
  // The generated initializer creates a FailureT even for an optional field.
  u.failure = nil
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

@MainActor private func sent(_ card: CardKey, _ view: String, _ bytes: [UInt8]) -> Tsugumi_RPC_NotificationPayloadUnion {
  let d = Tsugumi_Data_DisplayT()
  d.card = cardPath(card); d.view = view; d.bytes = bytes
  let u = Tsugumi_RPC_DisplayUpdatedT()
  u.display = d
  return payload(u, .displayupdated)
}

@Test func pendingDataKeepsItsOrder() {
  var box = CardViewOutbox()
  box.put(.data(Data([1])))
  box.put(.data(Data([2])))
  box.put(.data(Data([3])))
  #expect(box.take()?.0 == .data(Data([1])))
  #expect(box.pending == [.data(Data([2])), .data(Data([3]))])
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

  var chart: [CardViewMessage] = [], notes: [CardViewMessage] = []
  let a = s.cardViews.attach("plot", view: "chart") { message, done in chart.append(message); done() }
  let b = s.cardViews.attach("plot", view: "notes") { message, done in notes.append(message); done() }
  // Arbitrary bytes, NUL and non-UTF-8 included, reach only the named view.
  s.applyViewNotification(sent("plot", "chart", [0, 0xff, 0xfe, 0]))
  s.applyViewNotification(sent("plot", "missing", [5]))
  // A Card thread may still send after its call returned, with no active run.
  s.applyViewNotification(sent("plot", "chart", [9]))
  #expect(chart == [.data(Data([0, 0xff, 0xfe, 0])), .data(Data([9]))])
  #expect(notes.isEmpty)
  #expect(s.cardViews.undeclared("plot") == ["missing"])

  // A window opened later receives only new messages.
  var late: [CardViewMessage] = []
  let c = s.cardViews.attach("plot", view: "chart") { message, done in late.append(message); done() }
  #expect(late.isEmpty)

  // Declaring the name later clears the error; an empty list clears the declarations.
  s.applyViewNotification(declared("plot", [html("chart"), html("missing")]))
  #expect(s.cardViews.undeclared("plot").isEmpty)
  #expect(s.cardViews.ambiguous("plot").isEmpty)

  // A name two declarations share is reported, and data sent to it reaches neither.
  s.applyViewNotification(declared("plot", [html("chart"), html("chart")]))
  #expect(s.cardViews.ambiguous("plot") == ["chart"])
  s.applyViewNotification(sent("plot", "chart", [4]))
  #expect(chart.last == .data(Data([9])))
  #expect(s.cardViews.undeclared("plot") == ["missing"])
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

  let id = relay.attach("plot", view: "chart") { [weak host] message, done in
    if let host { host.send(message, complete: done) }
    else { done() }
  }
  relay.receive("plot", view: "other", bytes: Data([7]))
  relay.receive("plot", view: "chart", bytes: Data([0, 255, 0, 10]))
  // After the Card's call returned.
  relay.receive("plot", view: "chart", bytes: Data([9]))
  try await waitForModel("the view received the run") { probe.log.last == "data:9" }
  #expect(probe.log == ["port", "data:0,255,0,10", "data:9"])

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

@MainActor @Test func theWindowRoutesDawnToItsNativeHost() async throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-views-\(UUID().uuidString)")
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  let s = ProjectSession(identity: try ProjectIdentity(url: dir), environments: EnvironmentManager(image: nil))
  var requested = false
  s.viewFiles = CardViewFiles { card, index, name, offset in
    #expect(card == ["plot"] && index == 0 && name == "gpu" && offset == 0)
    requested = true
    // Reaching the Dawn loader is enough to exercise routing without a GPU.
    throw CardViewFileError.unavailable
  }
  let window = UUID()
  s.openDisplayViewer(window, card: "plot")
  defer { s.closeDisplayViewer(window) }
  s.applyViewNotification(declared("plot", [CardViewDeclaration(name: "gpu", type: "dawn", blob: "/nix/store/x-gpu.wasm")]))
  let view = NSHostingView(rootView: CardViewsPanel(session: s, cardKey: "plot"))
  let offscreen = NSWindow(contentRect: CGRect(x: 0, y: 0, width: 500, height: 400),
                           styleMask: .borderless, backing: .buffered, defer: true)
  offscreen.contentView = view
  defer { offscreen.contentView = nil }
  view.layoutSubtreeIfNeeded()
  let deadline = ContinuousClock.now + .seconds(2)
  while !requested && ContinuousClock.now < deadline {
    try await Task.sleep(for: .milliseconds(10))
  }
  #expect(requested, "the Dawn host requested its Wasm file")
  func hasWebView(_ view: NSView) -> Bool {
    view is WKWebView || view.subviews.contains(where: hasWebView)
  }
  #expect(!hasWebView(view))
}

// Hand-encoded Wasm. One page of memory and four exports, each returning i32.const 0.
@MainActor @Test func dawnModuleChecksAllowedImportsWithoutAGPU() throws {
  let valid: [UInt8] = [
    0, 0x61, 0x73, 0x6d, 1, 0, 0, 0,
    // Types: abi, start, resize, draw.
    1, 0x1b, 4,
    0x60, 0, 1, 0x7f,
    0x60, 6, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7d, 1, 0x7f,
    0x60, 3, 0x7f, 0x7f, 0x7d, 1, 0x7f,
    0x60, 1, 0x7f, 1, 0x7f,
    3, 5, 4, 0, 1, 2, 3,
    5, 3, 1, 0, 1,
    // Exports: memory, tsugumi_view_abi/start/resize/draw.
    7, 0x5c, 5,
    6, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 2, 0,
    16, 0x74, 0x73, 0x75, 0x67, 0x75, 0x6d, 0x69, 0x5f, 0x76, 0x69, 0x65, 0x77, 0x5f, 0x61, 0x62, 0x69, 0, 0,
    18, 0x74, 0x73, 0x75, 0x67, 0x75, 0x6d, 0x69, 0x5f, 0x76, 0x69, 0x65, 0x77, 0x5f, 0x73, 0x74, 0x61, 0x72, 0x74, 0, 1,
    19, 0x74, 0x73, 0x75, 0x67, 0x75, 0x6d, 0x69, 0x5f, 0x76, 0x69, 0x65, 0x77, 0x5f, 0x72, 0x65, 0x73, 0x69, 0x7a, 0x65, 0, 2,
    17, 0x74, 0x73, 0x75, 0x67, 0x75, 0x6d, 0x69, 0x5f, 0x76, 0x69, 0x65, 0x77, 0x5f, 0x64, 0x72, 0x61, 0x77, 0, 3,
    10, 21, 4, 4, 0, 0x41, 0, 0x0b, 4, 0, 0x41, 0, 0x0b,
               4, 0, 0x41, 0, 0x0b, 4, 0, 0x41, 0, 0x0b,
  ]
  #expect(dawnModuleError(valid) == nil)

  // Insert env.x as an imported function and shift the function export indices by one.
  let forbiddenImport: [UInt8] = [2, 9, 1, 3, 0x65, 0x6e, 0x76, 1, 0x78, 0, 0]
  var forbidden = valid
  for index in [79, 100, 122, 142] { forbidden[index] += 1 }
  forbidden.insert(contentsOf: forbiddenImport, at: 37)
  let diagnostic = try #require(dawnModuleError(forbidden))
  #expect(diagnostic.contains("env"))
}

@Test func dawnMapsPhysicalKeysToUSBHID() {
  #expect(DawnKeys.usage(UInt16(kVK_ANSI_A)) == 0x04)
  #expect(DawnKeys.usage(UInt16(kVK_Return)) == 0x28)
  #expect(DawnKeys.usage(UInt16(kVK_LeftArrow)) == 0x50)
  #expect(DawnKeys.usage(UInt16(kVK_RightShift)) == 0xe5)
  #expect(DawnKeys.usage(UInt16(kVK_ANSI_KeypadEnter)) == 0x58)
  #expect(DawnKeys.usage(.max) == nil)
}

@Test func dawnTakesPointerButtonsFromTheEvent() {
  let b = DawnPointer.buttons
  #expect(b(.leftMouseDown, 0, 0) == 1)
  #expect(b(.rightMouseDown, 1, 1) == 3)
  #expect(b(.otherMouseDown, 2, 0) == 4)
  #expect(b(.otherMouseDown, 3, 0) == 0, "buttons past middle have no bit")
  #expect(b(.leftMouseUp, 0, 3) == 2)
  #expect(b(.rightMouseUp, 1, 2) == 0)
  #expect(b(.otherMouseUp, 2, 5) == 1)
  // A drag holds its button even when the press was not seen (a first click elsewhere).
  #expect(b(.leftMouseDragged, 0, 0) == 1)
  #expect(b(.rightMouseDragged, 1, 1) == 3)
  #expect(b(.mouseMoved, 0, 7) == 0, "a plain move holds no button")
  #expect(b(.scrollWheel, 0, 2) == 2)
  #expect(b(.mouseExited, 0, 1) == 1)
}

@MainActor @Test func uiScriptKeysReachDawnUsages() {
  let usage = { (name: String) in UIScript.keyCodes[name].flatMap { DawnKeys.usage($0) } }
  #expect(usage("p") == 0x13)
  #expect(usage("a") == 0x04 && usage("z") == 0x1d)
  #expect(usage("1") == 0x1e && usage("0") == 0x27)
  #expect(usage("space") == 0x2c && usage("return") == 0x28 && usage("escape") == 0x29)
  #expect(usage("left") == 0x50 && usage("right") == 0x4f && usage("up") == 0x52 && usage("down") == 0x51)
  #expect(UIScript.keyCodes["f13"] == nil)
}
