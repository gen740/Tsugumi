import AppKit
import Foundation
import Testing
@testable import TsugumiApp

/// The chat's model menu lists every connection again when it opens: new lists
/// go where Settings > API keeps them, and a connection that fails, answers
/// nothing or is slow never takes away what the menu can show.
@MainActor
struct ModelMenuTests {
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

  private func defaults() -> UserDefaults {
    let name = "tsugumi-test-\(UUID().uuidString)"
    let d = UserDefaults(suiteName: name)!
    d.removePersistentDomain(forName: name)
    return d
  }

  private func settle(_ done: @escaping () -> Bool) async {
    for _ in 0..<400 where !done() { try? await Task.sleep(for: .milliseconds(5)) }
  }

  @Test func eachOpeningListsAgainAndSavesWhereSettingsDoes() async {
    let d = defaults()
    let stub = Stub()
    let r = ModelListRefresher(defaults: d) { id in
      stub.count(id)
      return [(id: "\(id)-\(stub.called(id))", label: "Name \(id)")]
    }
    var finished: [String] = []
    for opening in 1...3 {
      r.refresh(["anthropic", "chatgpt"]) { id, _ in finished.append(id) }
      await settle { finished.count == opening * 2 }
    }
    #expect(stub.called("anthropic") == 3 && stub.called("chatgpt") == 3)
    #expect(d.stringArray(forKey: AgentSettings.modelsKey("anthropic")) == ["anthropic-3"])
    #expect(d.stringArray(forKey: AgentSettings.modelsKey("chatgpt")) == ["chatgpt-3"])
    #expect(d.dictionary(forKey: "agent.modelNames") as? [String: String] == ["chatgpt-3": "Name chatgpt"])
  }

  @Test func aFailedOrEmptyAnswerKeepsTheSavedList() async {
    struct Down: Error {}
    let d = defaults()
    d.set(["saved-a"], forKey: AgentSettings.modelsKey("anthropic"))
    d.set(["saved-o"], forKey: AgentSettings.modelsKey("openai"))
    let r = ModelListRefresher(defaults: d) { id in
      if id == "anthropic" { throw Down() }
      return []
    }
    var outcomes: [String: ModelListRefresher.Outcome] = [:]
    r.refresh(["anthropic", "openai"]) { id, o in outcomes[id] = o }
    await settle { outcomes.count == 2 }
    #expect(outcomes == ["anthropic": .failed, "openai": .empty])
    #expect(d.stringArray(forKey: AgentSettings.modelsKey("anthropic")) == ["saved-a"])
    #expect(d.stringArray(forKey: AgentSettings.modelsKey("openai")) == ["saved-o"])
  }

  @Test func aSlowConnectionHoldsNothingAndIsNotAskedTwice() async {
    let d = defaults()
    let stub = Stub()
    let gate = AsyncStream<Void>.makeStream()
    let r = ModelListRefresher(defaults: d) { id in
      stub.count(id)
      if id == "openai" { for await _ in gate.stream { break } }
      return [(id: "\(id)-new", label: id)]
    }
    var finished: [String] = []
    r.refresh(["anthropic", "openai"]) { id, _ in finished.append(id) }
    await settle { finished == ["anthropic"] }
    #expect(finished == ["anthropic"])
    #expect(d.stringArray(forKey: AgentSettings.modelsKey("anthropic")) == ["anthropic-new"])
    // Opened again meanwhile: the fast one again, the slow one still on its way.
    r.refresh(["anthropic", "openai"]) { id, _ in finished.append(id) }
    await settle { finished == ["anthropic", "anthropic"] }
    #expect(stub.called("anthropic") == 2 && stub.called("openai") == 1)
    gate.continuation.yield()
    await settle { finished.count == 3 }
    #expect(r.running.isEmpty)
    #expect(d.stringArray(forKey: AgentSettings.modelsKey("openai")) == ["openai-new"])
  }

  @Test func theMenuShowsEachConnectionAndTheCurrentChoice() {
    let m = ModelMenu()
    var picked: [String] = []
    m.onChoose = { picked = [$0, $1] }
    #expect(m.menu.items.count == 1 && !m.menu.items[0].isEnabled)  // nothing set up yet: it says so
    m.setContents(connections: [.init(id: "anthropic", name: "Anthropic API", models: ["a1", "a2"]),
                                .init(id: "chatgpt", name: "ChatGPT", models: ["g1"])],
                  provider: "chatgpt", model: "g1", names: ["g1": "GPT One"])
    #expect(m.menu.items.map(\.title) == ["Anthropic API", "a1", "a2", "ChatGPT", "GPT One"])
    #expect(m.menu.items.filter { $0.state == .on }.map(\.title) == ["GPT One"])
    #expect(m.menu.items[0].isSectionHeader)
    // The same contents leave the open menu's items alone.
    let before = m.menu.items
    m.setContents(connections: [.init(id: "anthropic", name: "Anthropic API", models: ["a1", "a2"]),
                                .init(id: "chatgpt", name: "ChatGPT", models: ["g1"])],
                  provider: "chatgpt", model: "g1", names: ["g1": "GPT One"])
    #expect(zip(before, m.menu.items).allSatisfy { $0 === $1 })
    let item = m.menu.items[1]
    _ = (item.target as? NSObject)?.perform(item.action, with: item)
    #expect(picked == ["anthropic", "a1"])
    m.setCurrent(provider: "anthropic", model: "a1")
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
}
