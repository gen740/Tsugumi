import AppKit
import Carbon.HIToolbox
import Foundation
import Observation
import SwiftUI
import Testing
import WebKit
@testable import TsugumiApp
@testable import TsugumiViewPresentation

private func opening(_ id: UUID = UUID(), type: String = "html") -> CardViewOpening {
  CardViewOpening(id: id, card: ["plot"], name: "chart", type: type, mode: .panel, width: 0, height: 0)
}

@Test func pendingDataKeepsItsOrder() {
  var box = CardViewOutbox()
  box.put(.data(Data([1])))
  box.put(.data(Data([2])))
  box.put(.data(Data([3])))
  #expect(box.take()?.0 == .data(Data([1])))
  #expect(box.pending == [.data(Data([2])), .data(Data([3]))])
}

@MainActor @Test func sameNameSessionsKeepIndependentQueuesUntilClosed() async throws {
  let presentation = CardViewPresentation()
  let first = opening(), second = opening()
  var credits: [Int] = []
  var closed: [UUID] = []
  for view in [first, second] {
    presentation.open(view, files: CardViewFiles { _, _ in
      try await Task.sleep(for: .seconds(300))
      throw CardViewFileError.unavailable
    }, send: { _, _ in }, close: { closed.append(view.id) })
  }

  presentation.receive(first.id, bytes: Data([1])) { credits.append(1) }
  presentation.receive(second.id, bytes: Data([2])) { credits.append(2) }
  presentation.receive(first.id, bytes: Data([3])) { credits.append(3) }
  #expect(credits.isEmpty)

  presentation.close(first.id)
  presentation.close(first.id)
  #expect(credits == [1, 3])
  #expect(presentation.openings.map(\.id) == [second.id])
  presentation.receive(first.id, bytes: Data([4])) { credits.append(4) }
  #expect(presentation.sessionClosed(second.id)?.id == second.id)
  #expect(presentation.sessionClosed(second.id) == nil)
  #expect(credits == [1, 3, 4, 2])
  try await waitForModel("the user close reached the relay") { presentation.isIdle }
  #expect(closed == [first.id])
}

@MainActor @Test func viewFilesAreReadPageByPageWithinTheLimit() async throws {
  let id = UUID()
  var asked: [UInt64] = []
  let files = CardViewFiles { session, offset in
    #expect(session == id)
    asked.append(offset)
    return offset == 0 ? CardViewFilePage(bytes: Data("<p>".utf8), eof: false, nextOffset: 3)
      : CardViewFilePage(bytes: Data("x</p>".utf8), eof: true, nextOffset: nil)
  }
  #expect(try await files.read(session: id) == Data("<p>x</p>".utf8))
  #expect(asked == [0, 3])
  let stuck = CardViewFiles { _, _ in CardViewFilePage(bytes: Data(), eof: false, nextOffset: 0) }
  await #expect(throws: CardViewFileError.self) { try await stuck.read(session: id) }
  let huge = CardViewFiles { _, _ in CardViewFilePage(bytes: Data(count: CardViewFiles.limit + 1), eof: true, nextOffset: nil) }
  await #expect(throws: CardViewFileError.self) { try await huge.read(session: id) }
  await #expect(throws: CardViewFileError.self) { try await CardViewFiles.unavailable.read(session: id) }
}

/// Watches the view's own document from an isolated world, so the test sees
/// what the view received without giving the view any channel back.
@MainActor @Observable private final class Probe: NSObject, WKScriptMessageHandler {
  var log: [String] = []
  var sent: [(String, Data)] = []
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
  note('ready')
  tsugumi.onmessage = bytes => note('data:' + Array.from(bytes).join(','))
  tsugumi.send(new Uint8Array([0, 255]), '*').then(() => note('send-ok'))
  tsugumi.send(new Uint8Array([8]), 'screens/a')
  tsugumi.send(new Uint8Array([9]), 'unregistered').catch(() => note('send-rejected'))
  fetch('\(out.address("fetch"))').catch(() => {})
  new Image().src = '\(out.address("image"))'
  try { new WebSocket('ws://127.0.0.1:\(out.port)/socket') } catch {}
  try { navigator.sendBeacon('\(out.address("beacon"))') } catch {}
  try { window.open('\(out.address("popup"))') } catch {}
  setTimeout(() => { try { location.href = '\(out.address("leave"))' } catch {} }, 20)
  </script></body></html>
  """
}

@MainActor @Test func htmlMessagesUseOrderedBytesAndCardPathsWithoutNetworkAccess() async throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let out = try Outside()
  defer { out.stop() }
  let probe = Probe()
  let host = HTMLViewHost(configure: { probe.install($0) }, sendToCard: { to, bytes in
    if to == "unregistered" { throw CardViewFileError.unavailable }
    probe.sent.append((to, bytes))
  })
  defer { host.close() }
  host.web.frame = CGRect(x: 0, y: 0, width: 400, height: 300)
  #expect(host.web.configuration.websiteDataStore.isPersistent == false)

  host.send(.data(Data([0, 255, 0, 10])))
  try await host.mount(viewDocument(out))
  host.send(.data(Data([9])))
  try await waitForModel("the view exchanged messages") { probe.log.contains("data:9") && probe.log.contains("send-rejected") && probe.sent.count == 2 }
  #expect(probe.log.filter { $0 == "ready" || $0.hasPrefix("data:") } == ["ready", "data:0,255,0,10", "data:9"])
  #expect(probe.log.contains("send-ok"))
  #expect(probe.sent[0].0 == "*")
  #expect(probe.sent[0].1 == Data([0, 255]))
  #expect(probe.sent[1].0 == "screens/a")
  #expect(probe.sent[1].1 == Data([8]))

  let closedLog = probe.log
  host.close()
  host.send(.data(Data([2])))
  try await Task.sleep(for: .milliseconds(300))
  #expect(probe.log == closedLog)
  #expect(out.requests.isEmpty)
}

@MainActor @Test func panelReadsDawnWasmBySession() async throws {
  let presentation = CardViewPresentation()
  let view = opening(type: "dawn")
  var requested = false
  let files = CardViewFiles { id, offset in
    #expect(id == view.id && offset == 0)
    requested = true
    throw CardViewFileError.unavailable
  }
  presentation.open(view, files: files, send: { _, _ in }, close: {})
  defer { presentation.closeAll() }
  let deadline = ContinuousClock.now + .seconds(2)
  while !requested && ContinuousClock.now < deadline {
    try await Task.sleep(for: .milliseconds(10))
  }
  #expect(requested)
  #expect(presentation.hasPanels)
}

@MainActor @Test func detachingOnePanelKeepsItsSessionAndTheOtherPanel() throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let presentation = CardViewPresentation()
  let markdown = CardViewOpening(id: UUID(), card: ["report"], name: UUID().uuidString,
                                 type: "html", mode: .panel, width: 400, height: 300)
  let dawn = opening(type: "dawn")
  for panel in [markdown, dawn] {
    presentation.open(panel, files: .unavailable, send: { _, _ in }, close: {})
  }
  defer { presentation.closeAll() }
  #expect(presentation.panelOpenings.map(\.id) == [markdown.id, dawn.id])

  presentation.detachPanel(markdown.id)
  #expect(presentation.panelOpenings.map(\.id) == [dawn.id])
  #expect(Set(presentation.openings.map(\.id)) == [markdown.id, dawn.id])
  let window = try #require(NSApp.windows.first { $0.title == markdown.name })
  #expect(window.contentView != nil)

  presentation.detachPanel(markdown.id)
  #expect(NSApp.windows.filter { $0.title == markdown.name }.count == 1)
  #expect(presentation.sessionClosed(markdown.id)?.id == markdown.id)
  #expect(window.contentView == nil)
  #expect(presentation.panelOpenings.map(\.id) == [dawn.id])
}

// A small Wasm module with one page of memory and the required view exports.
@MainActor @Test func dawnModuleChecksAllowedImportsWithoutAGPU() throws {
  func section(_ id: UInt8, _ data: [UInt8]) -> [UInt8] {
    [id, UInt8(data.count)] + data
  }
  func name(_ text: String) -> [UInt8] { [UInt8(text.utf8.count)] + Array(text.utf8) }
  let parameters: [[UInt8]] = [
    [],
    [0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7d],
    [0x7f, 0x7f, 0x7d, 0x7e],
    [0x7f, 0x7e],
  ]
  let types: [UInt8] = [4] + parameters.flatMap { [0x60, UInt8($0.count)] + $0 + [1, 0x7f] }
  func module(forbiddenImport: Bool) -> [UInt8] {
    var bytes: [UInt8] = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]
    bytes += section(1, types)
    if forbiddenImport { bytes += section(2, [1] + name("env") + name("x") + [0, 0]) }
    bytes += section(3, [4, 0, 1, 2, 3])
    bytes += section(5, [1, 0, 1])
    var exports: [UInt8] = [5] + name("memory") + [2, 0]
    for (index, member) in ["abi", "start", "resize", "draw"].enumerated() {
      exports += name("tsugumi_view_" + member) + [0, UInt8(index + (forbiddenImport ? 1 : 0))]
    }
    bytes += section(7, exports)
    bytes += section(10, [4] + Array(repeating: [4, 0, 0x41, 0, 0x0b], count: 4).flatMap { $0 })
    return bytes
  }
  #expect(dawnModuleError(module(forbiddenImport: false)) == nil)
  let diagnostic = try #require(dawnModuleError(module(forbiddenImport: true)))
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
