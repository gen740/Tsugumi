import AppKit
import Foundation
import Testing
@testable import TsugumiApp

/// The menu asks hostd for each connection without blocking other menus.
/// Stored-versus-refreshed catalog behavior is covered by models_rpc_test.cc.
@MainActor
struct ModelMenuTests {
  @Test func openingRefreshesSettingsBeforePresentingTheMenu() async {
    let menu = ModelMenu()
    let models = ["gpt-6.1-sol", "gpt-6.1-terra"]
    let connections = [ChatPanel.Connection(id: "chatgpt", name: "ChatGPT", models: models)]
    menu.setContents(connections: connections, provider: "chatgpt", model: models[0], names: [:])
    await withCheckedContinuation { continuation in
      menu.beforeOpen = {
        menu.setContents(connections: connections, provider: "chatgpt", model: models[1], names: [:])
        continuation.resume()
      }
      // No anchor: exercise the opening path without showing a window or menu.
      menu.show()
    }
    #expect(menu.menu.items.filter { $0.state == .on && models.contains($0.title) }.map(\.title) == [models[1]])
    menu.beforeOpen = nil
  }

  /// Counts the fetches of each connection.
  final class Stub: @unchecked Sendable {
    private let lock = NSLock()
    private var calls: [String: Int] = [:]
    func called(_ id: String) -> Int {
      lock.lock()
      defer { lock.unlock() }
      return calls[id] ?? 0
    }
    func count(_ id: String) {
      lock.lock()
      calls[id, default: 0] += 1
      lock.unlock()
    }
  }

  @Test func eachOpeningRequestsTheCoreCatalogAgain() async {
    let stub = Stub()
    let r = ModelListRefresher { id in
      stub.count(id)
      return [(id: "\(id)-\(stub.called(id))", name: "Name \(id)")]
    }
    var finished: [String] = []
    for opening in 1...3 {
      await withCheckedContinuation { continuation in
        r.refresh(["anthropic", "chatgpt"]) { id, _ in
          finished.append(id)
          if finished.count == opening * 2 { continuation.resume() }
        }
      }
    }
    #expect(stub.called("anthropic") == 3 && stub.called("chatgpt") == 3)
  }

  @Test func aFailedOrEmptyAnswerReportsTheCoreOutcome() async {
    struct Down: Error {}
    let r = ModelListRefresher { id in
      if id == "anthropic" { throw Down() }
      return []
    }
    var outcomes: [String: ModelListRefresher.Outcome] = [:]
    await withCheckedContinuation { continuation in
      r.refresh(["anthropic", "openai"]) { id, o in
        outcomes[id] = o
        if outcomes.count == 2 { continuation.resume() }
      }
    }
    #expect(outcomes == ["anthropic": .failed, "openai": .empty])
  }

  @Test func aSlowConnectionHoldsNothingAndIsNotAskedTwice() async {
    let stub = Stub()
    let gate = AsyncStream<Void>.makeStream()
    let r = ModelListRefresher { id in
      stub.count(id)
      if id == "openai" { for await _ in gate.stream { break } }
      return [(id: "\(id)-new", name: id)]
    }
    var finished: [String] = []
    var complete: CheckedContinuation<Void, Never>?
    await withCheckedContinuation { continuation in
      r.refresh(["anthropic", "openai"]) { id, _ in
        finished.append(id)
        if finished.count == 1 { continuation.resume() }
        if finished.count == 3 { complete?.resume() }
      }
    }
    #expect(finished == ["anthropic"])
    // Opened again meanwhile: the fast one again, the slow one still on its way.
    await withCheckedContinuation { continuation in
      r.refresh(["anthropic", "openai"]) { id, _ in finished.append(id); continuation.resume() }
    }
    #expect(stub.called("anthropic") == 2 && stub.called("openai") == 1)
    await withCheckedContinuation { complete = $0; gate.continuation.yield() }
    #expect(r.running.isEmpty)
  }

  @Test func theMenuShowsEachConnectionAndTheCurrentChoice() {
    let m = ModelMenu()
    var picked: [String] = []
    m.onChoose = { picked = [$0, $1] }
    #expect(m.menu.items.count == 1 && !m.menu.items[0].isEnabled)  // nothing set up yet: it says so
    m.setContents(connections: [.init(id: "anthropic", name: "Anthropic API", models: ["a1", "a2"]),
                                .init(id: "chatgpt", name: "ChatGPT", models: ["g1"])],
                  provider: "chatgpt", model: "g1", names: ["g1": "GPT One"])
    // Below the effort and Fast mode of the model in use (offersWhatTheModelTakesAndSaysWhyNot).
    var models: [NSMenuItem] { Array(m.menu.items.drop { $0.title != "Anthropic API" }.filter { !$0.isSeparatorItem }) }
    #expect(models.map(\.title) == ["Anthropic API", "a1", "a2", "ChatGPT", "GPT One"])
    #expect(m.menu.items.filter { $0.state == .on }.map(\.title) == ["GPT One"])
    #expect(models[0].isSectionHeader)
    // The same contents leave the open menu's items alone.
    let before = m.menu.items
    m.setContents(connections: [.init(id: "anthropic", name: "Anthropic API", models: ["a1", "a2"]),
                                .init(id: "chatgpt", name: "ChatGPT", models: ["g1"])],
                  provider: "chatgpt", model: "g1", names: ["g1": "GPT One"])
    #expect(zip(before, m.menu.items).allSatisfy { $0 === $1 })
    let item = models[1]
    _ = (item.target as? NSObject)?.perform(item.action, with: item)
    #expect(picked == ["anthropic", "a1"])
    m.setContents(connections: [.init(id: "anthropic", name: "Anthropic API", models: ["a1", "a2"]), .init(id: "chatgpt", name: "ChatGPT", models: ["g1"])], provider: "anthropic", model: "a1", names: ["g1": "GPT One"])
    #expect(m.menu.items.filter { $0.state == .on }.map(\.title) == ["a1"])
  }

  @Test func openingTheMenuIsReported() {
    let m = ModelMenu()
    var opened = 0
    m.onOpen = { opened += 1 }
    m.menuWillOpen(m.menu)
    m.menuWillOpen(m.menu)
    #expect(opened == 2)
  }

  /// The effort and Fast mode of the model in use, above the models: only what
  /// the model takes, and what cannot be chosen says why.
  @Test func offersWhatTheModelTakesAndSaysWhyNot() {
    let m = ModelMenu()
    let connections = [ChatPanel.Connection(id: "anthropic", name: "Anthropic API", models: ["claude-haiku-4-5", "claude-opus-5-5", "claude-sonnet-4-6"]),
                       ChatPanel.Connection(id: "chatgpt", name: "ChatGPT プラン", models: ["gpt-6.1-sol"])]
    func effortItems() -> [NSMenuItem] { m.menu.items.filter { ($0.representedObject as? [String])?.first == "effort" } }
    func fastItem() -> NSMenuItem? { m.menu.items.first { ($0.representedObject as? [String]) == ["fast"] } }
    func press(_ item: NSMenuItem?) {
      guard let item else { Issue.record("Missing menu action"); return }
      _ = (item.target as? NSObject)?.perform(item.action, with: item)
    }
    m.setContents(connections: connections, provider: "anthropic", model: "claude-opus-5-5", names: [:])
    #expect(effortItems().compactMap { ($0.representedObject as? [String])?.last } == ["", "low", "medium", "high", "xhigh", "max"])
    #expect(effortItems().first?.state == .on)
    var effort: String?, fast: Bool?
    m.onEffort = { effort = $0 }; m.onFast = { fast = $0 }
    press(effortItems().first { ($0.representedObject as? [String])?.last == "xhigh" })
    press(fastItem())
    #expect(effort == "xhigh" && fast == true)
    m.setChoices(effort: "xhigh", fast: true)
    #expect(effortItems().first { ($0.representedObject as? [String])?.last == "xhigh" }?.state == .on)
    #expect(fastItem()?.state == .on)
    press(fastItem()); #expect(fast == false)
    m.setContents(connections: connections, provider: "anthropic", model: "claude-sonnet-4-6", names: [:])
    #expect(effortItems().compactMap { ($0.representedObject as? [String])?.last } == ["", "low", "medium", "high", "max"])
    #expect(fastItem()?.isEnabled == false)
    m.setContents(connections: connections, provider: "anthropic", model: "claude-haiku-4-5", names: [:])
    #expect(effortItems().isEmpty && fastItem()?.isEnabled == false)
    m.setContents(connections: connections, provider: "chatgpt", model: "gpt-6.1-sol", names: [:])
    #expect(fastItem()?.isEnabled == false)
  }
}
