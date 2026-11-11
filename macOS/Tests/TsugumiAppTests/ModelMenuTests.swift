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

  /// The effort and Fast mode of the model in use, above the models: only what
  /// the model takes, and what cannot be chosen says why.
  @Test func offersWhatTheModelTakesAndSaysWhyNot() {
    let m = ModelMenu()
    let connections = [ChatPanel.Connection(id: "anthropic", name: "Anthropic API", models: ["claude-haiku-4-5", "claude-opus-5-5", "claude-sonnet-4-6"]),
                       ChatPanel.Connection(id: "chatgpt", name: "ChatGPT プラン", models: ["gpt-6.1-sol"])]
    func titles() -> [String] { Array(m.menu.items.map(\.title).prefix { $0 != "Anthropic API" }).filter { !$0.isEmpty } }
    func press(_ title: String) {
      let item = m.menu.items.first { $0.title == title }!
      _ = (item.target as? NSObject)?.perform(item.action, with: item)
    }

    m.setContents(connections: connections, provider: "anthropic", model: "claude-opus-5-5", names: [:])
    #expect(titles() == ["Effort", "既定（medium）", "low", "medium", "high", "xhigh", "max", "Fast mode（料金は通常の 2 倍）"])
    #expect(m.menu.item(withTitle: "既定（medium）")?.state == .on)

    var effort: String?, fast: Bool?
    m.onEffort = { effort = $0 }
    m.onFast = { fast = $0 }
    press("xhigh")
    press("Fast mode（料金は通常の 2 倍）")
    #expect(effort == "xhigh" && fast == true)
    m.setChoices(effort: "xhigh", fast: true)
    #expect(m.menu.item(withTitle: "xhigh")?.state == .on && m.menu.item(withTitle: "Fast mode（料金は通常の 2 倍）")?.state == .on)
    press("Fast mode（料金は通常の 2 倍）")
    #expect(fast == false, "pressed again: off")

    m.setChoices(effort: nil, fast: false)
    m.setCurrent(provider: "anthropic", model: "claude-sonnet-4-6")
    #expect(titles() == ["Effort", "既定（high）", "low", "medium", "high", "max", "Fast mode: Claude Opus 5.5 でだけ選べます"])
    #expect(m.menu.items.first { $0.title.hasPrefix("Fast mode") }?.isEnabled == false)

    m.setCurrent(provider: "anthropic", model: "claude-haiku-4-5")
    #expect(titles() == ["Effort", "このモデルでは選べません（公式の文書で対応を確かめたモデルだけ選べます）", "Fast mode: Claude Opus 5.5 でだけ選べます"])

    m.setCurrent(provider: "chatgpt", model: "gpt-6.1-sol")
    #expect(titles().last == "Fast mode: ChatGPT プランでは選べません（指定が拒否されることがあるため）")
  }
}
