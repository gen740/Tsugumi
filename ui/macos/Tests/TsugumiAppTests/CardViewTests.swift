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

@Test func cardViewLinkEndsFollowTheCardAndThePanel() {
  let id = UUID()
  let card = CGRect(x: 0, y: 0, width: 200, height: 100)
  let panel = CGRect(x: 400, y: 20, width: 300, height: 200)
  let link = CardViewLink(panel: id, card: "plot", cardRect: card, panelRect: panel)
  // Across, on the facing edges, in the middle of the stretch they share.
  #expect(link.horizontal)
  #expect(link.a == CGPoint(x: 200, y: 60))
  #expect(link.b == CGPoint(x: 400, y: 60))

  // The Card moves down: both ends follow to the middle of what they now share.
  let moved = CardViewLink(panel: id, card: "plot", cardRect: card.offsetBy(dx: 0, dy: 80), panelRect: panel)
  #expect(moved.a == CGPoint(x: 200, y: 130))
  #expect(moved.b == CGPoint(x: 400, y: 130))

  // The panel moves below the Card: the link runs down between them.
  let below = CardViewLink(panel: id, card: "plot", cardRect: card,
                           panelRect: CGRect(x: 50, y: 300, width: 300, height: 200))
  #expect(!below.horizontal)
  #expect(below.a == CGPoint(x: 125, y: 100))
  #expect(below.b == CGPoint(x: 125, y: 300))
}

@MainActor @Test func cardViewLinksGoAwayWithTheirPanels() {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let presentation = CardViewPresentation()
  // One panel per view, however often the Card is called: one link each.
  let chart = opening(), table = opening()
  let elsewhere = CardViewOpening(id: UUID(), card: ["outer", "inner"], name: "log", type: "html",
                                  mode: .panel, width: 0, height: 0)
  for view in [chart, table, elsewhere] {
    presentation.open(view, files: .unavailable, send: { _, _ in }, close: {})
  }
  defer { presentation.closeAll() }
  let rects = [chart.id: CGRect(x: 300, y: 0, width: 200, height: 100),
               table.id: CGRect(x: 300, y: 200, width: 200, height: 100),
               elsewhere.id: CGRect(x: -300, y: 0, width: 200, height: 100)]
  var folded: Set<UUID> = []
  func links() -> [CardViewLink] {
    let shown = CardViewPanelPlacer.shown(presentation.panelOpenings, folded: folded)
    return CardViewLink.links(shown.map { (id: $0.id, card: key($0.card), rect: rects[$0.id]!) }) {
      $0 == "plot" ? CGRect(x: 0, y: 0, width: 200, height: 100) : nil
    }
  }
  // Only a Card on this canvas gets a line.
  #expect(links().map(\.panel) == [chart.id, table.id])
  #expect(links().allSatisfy { $0.card == "plot" })

  folded.insert(chart.id)
  #expect(links().map(\.panel) == [table.id])
  presentation.detachPanel(table.id)
  #expect(links().isEmpty)
  folded.remove(chart.id)
  #expect(links().map(\.panel) == [chart.id])
}

// Folding hides a panel and its line; it stays open, keeps its place and
// still takes the Card's data. Unfolding brings it back where it was, or
// beside its Card when it never had a place.
@MainActor @Test func foldingAPanelHidesItAndUnfoldingRestoresIt() async throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let presentation = CardViewPresentation()
  let placed = CardViewOpening(id: UUID(), card: ["collect"], name: "markdown", type: "html",
                               mode: .panel, width: 0, height: 0)
  let never = CardViewOpening(id: UUID(), card: ["collect"], name: "plot", type: "html",
                              mode: .panel, width: 0, height: 0)
  // The view's file arrives only when the test lets it: until then its data waits for it.
  var release: CheckedContinuation<Void, Never>?
  let files = CardViewFiles { _, _ in
    await withCheckedContinuation { release = $0 }
    return CardViewFilePage(bytes: Data("<p>ok</p>".utf8), eof: true, nextOffset: nil)
  }
  var closes = 0
  presentation.open(placed, files: files, send: { _, _ in }, close: { closes += 1 })
  presentation.open(never, files: .unavailable, send: { _, _ in }, close: { closes += 1 })
  defer { release?.resume(); presentation.closeAll() }
  let card = CGRect(x: 0, y: 0, width: 200, height: 100)
  let none = CardViewPanelPlacer.Obstacles(rects: [card], wires: [])
  var folded: Set<UUID> = []
  func place(_ placements: [UUID: CanvasCardViewPlacement]) -> [UUID: CanvasCardViewPlacement] {
    CardViewPanelPlacer.placeNew(CardViewPanelPlacer.shown(presentation.panelOpenings, folded: folded),
                                 in: "", placements: CardViewPanelPlacer.kept(placements, for: presentation.panelOpenings),
                                 cardRect: { $0 == "collect" ? card : nil }, obstacles: none)
  }
  func links(_ placements: [UUID: CanvasCardViewPlacement]) -> [UUID] {
    let shown = CardViewPanelPlacer.shown(presentation.panelOpenings, folded: folded)
    return CardViewLink.links(shown.compactMap { o in placements[o.id].map { (id: o.id, card: key(o.card), rect: $0.rect) } }) {
      $0 == "collect" ? card : nil
    }.map(\.panel)
  }

  folded = [never.id]
  var placements = place([:])
  placements[placed.id]?.rect.origin = CGPoint(x: 900, y: -400)
  let moved = try #require(placements[placed.id]?.rect)
  #expect(placements[never.id] == nil, "a folded panel is not placed")

  folded.insert(placed.id)
  placements = place(placements)
  #expect(links(placements).isEmpty, "folded: no panel and no line")
  #expect(placements[placed.id]?.rect == moved, "its place is kept")
  #expect(Set(presentation.openings.map(\.id)) == [placed.id, never.id] && closes == 0, "folding closes nothing")

  // A folded view still takes the Card's data: it is queued for the view, not dropped.
  var delivered = false
  presentation.receive(placed.id, bytes: Data("next".utf8)) { delivered = true }
  #expect(!delivered)
  var dropped = false
  presentation.receive(UUID(), bytes: Data("x".utf8)) { dropped = true }
  #expect(dropped, "data for a view that is not open is completed at once")

  folded = []
  placements = place(placements)
  #expect(placements[placed.id]?.rect == moved, "back where it was")
  let fresh = try #require(placements[never.id]?.rect)
  #expect(!fresh.intersects(card) && !fresh.intersects(moved), "beside its Card")
  #expect(Set(links(placements)) == [placed.id, never.id])
}

// A panel belongs to the canvas that shows its Card: a Card inside a Card in
// Card has its panel on the inner canvas, and on no other.
@MainActor @Test func cardViewPanelsGoOnTheCanvasOfTheirCard() {
  let inner = CardViewOpening(id: UUID(), card: ["outer", "inner", "collect"], name: "markdown", type: "html",
                              mode: .panel, width: 0, height: 0)
  let top = CardViewOpening(id: UUID(), card: ["report"], name: "markdown", type: "html",
                            mode: .panel, width: 0, height: 0)
  #expect(CardViewPanelPlacer.scope(of: inner.card) == "outer/inner")
  #expect(CardViewPanelPlacer.scope(of: top.card) == "")
  let card = CGRect(x: 0, y: 0, width: 200, height: 100)
  let cards: [CardKey: CGRect] = ["outer": card, "report": card.offsetBy(dx: 0, dy: 300),
                                  "outer/inner": card, "outer/inner/collect": card]
  let none = CardViewPanelPlacer.Obstacles(rects: [], wires: [])
  func place(_ scope: CardKey, _ placements: [UUID: CanvasCardViewPlacement] = [:]) -> [UUID: CanvasCardViewPlacement] {
    CardViewPanelPlacer.placeNew([inner, top], in: scope, placements: placements,
                                 cardRect: { cards[$0] }, obstacles: none)
  }
  let atTop = place("")
  #expect(Set(atTop.keys) == [top.id])
  let atOuter = place("outer", atTop)
  #expect(Set(atOuter.keys) == [top.id], "the outer canvas does not take the inner Card's panel")
  let atInner = place("outer/inner", atOuter)
  #expect(atInner[inner.id]?.scope == "outer/inner")
  #expect(atInner[top.id]?.scope == "")
}

@MainActor @Test func aNewCardViewPanelCoversNeitherItsCardNorAnotherPanel() throws {
  let card = CGRect(x: 0, y: 0, width: 200, height: 100)
  let a = CardViewOpening(id: UUID(), card: ["s", "collect"], name: "markdown", type: "html",
                          mode: .panel, width: 0, height: 0)
  let b = CardViewOpening(id: UUID(), card: ["s", "collect"], name: "plot", type: "html",
                          mode: .panel, width: 900, height: 700)
  // Another Card to its right, and a wire leaving it downward.
  let other = CGRect(x: 300, y: -40, width: 200, height: 160)
  var wire = Path()
  wire.move(to: CGPoint(x: 100, y: 100))
  wire.addLine(to: CGPoint(x: 100, y: 700))
  let obstacles = CardViewPanelPlacer.Obstacles(rects: [card, other], wires: [wire])
  let placed = CardViewPanelPlacer.placeNew([a, b], in: "s", placements: [:],
                                            cardRect: { $0 == "s/collect" ? card : nil }, obstacles: obstacles)
  let ra = try #require(placed[a.id]?.rect), rb = try #require(placed[b.id]?.rect)
  // A modest size, also when the Card asks for more.
  #expect(ra.size == CGSize(width: 400, height: 280))
  #expect(rb.size == CGSize(width: 480, height: 360))
  for r in [ra, rb] {
    #expect(!r.intersects(card) && !r.intersects(other) && !obstacles.blocks(r))
    // Next to its Card, with room for the line.
    let gap = max(r.minX - card.maxX, card.minX - r.maxX, r.minY - card.maxY, card.minY - r.maxY)
    #expect(gap >= CardViewPanelPlacer.cardGap && gap < 200)
  }
  #expect(!ra.intersects(rb))
}

// Closing the window of a detached panel docks it back: the same session is a
// panel on its Card's canvas again, beside its Card, with its line; nothing is closed.
@MainActor @Test func closingADetachedPanelsWindowDocksItBesideItsCard() throws {
  NSApplication.shared.setActivationPolicy(.prohibited)
  let presentation = CardViewPresentation()
  let view = CardViewOpening(id: UUID(), card: ["outer", "collect"], name: UUID().uuidString, type: "html",
                             mode: .panel, width: 0, height: 0)
  var closes = 0
  presentation.open(view, files: .unavailable, send: { _, _ in }, close: { closes += 1 })
  defer { presentation.closeAll() }
  let card = CGRect(x: 0, y: 0, width: 200, height: 100)
  let none = CardViewPanelPlacer.Obstacles(rects: [card], wires: [])
  func place(_ placements: [UUID: CanvasCardViewPlacement]) -> [UUID: CanvasCardViewPlacement] {
    CardViewPanelPlacer.placeNew(presentation.panelOpenings, in: "outer",
                                 placements: CardViewPanelPlacer.kept(placements, for: presentation.panelOpenings),
                                 cardRect: { $0 == "outer/collect" ? card : nil }, obstacles: none)
  }
  var placements = place([:])
  placements[view.id]?.rect.origin = CGPoint(x: 2000, y: 2000)

  presentation.detachPanel(view.id)
  placements = place(placements)
  #expect(placements.isEmpty, "a panel shown in a window is not on the canvas")
  let window = try #require(NSApp.windows.first { $0.title == view.name })
  window.close()

  #expect(presentation.panelOpenings.map(\.id) == [view.id])
  #expect(presentation.openings.map(\.id) == [view.id], "the session stays open")
  #expect(window.contentView == nil)
  placements = place(placements)
  let rect = try #require(placements[view.id]?.rect)
  #expect(placements[view.id]?.scope == "outer")
  #expect(!rect.intersects(card) && abs(rect.minX - card.maxX) < 200, "beside its Card again")
  let links = CardViewLink.links([(id: view.id, card: key(view.card), rect: rect)]) { $0 == "outer/collect" ? card : nil }
  #expect(links.map(\.panel) == [view.id])

  #expect(closes == 0, "the core is not told to close it")
  #expect(NSApp.windows.filter { $0.title == view.name && $0.isVisible }.isEmpty)
}

@MainActor @Test func aMovedCardViewPanelKeepsItsPlace() {
  let view = CardViewOpening(id: UUID(), card: ["collect"], name: "markdown", type: "html",
                             mode: .panel, width: 0, height: 0)
  let moved = CGRect(x: -900, y: 800, width: 520, height: 300)
  let placed = CardViewPanelPlacer.placeNew(
    [view], in: "", placements: [view.id: CanvasCardViewPlacement(scope: "", rect: moved)],
    cardRect: { _ in CGRect(x: 0, y: 0, width: 200, height: 100) },
    obstacles: CardViewPanelPlacer.Obstacles(rects: [], wires: []))
  #expect(placed[view.id]?.rect == moved)
}
