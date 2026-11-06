import Foundation
import TsugumiAgent
import TsugumiRPC

/// "版の要約": a small model writes what a version changed from its diff, only
/// while switched on (the version tree asks first): code and settings go to
/// the model's provider, never data files. It costs tokens of its own and is
/// not made on the ChatGPT plan, whose allowance is for the Agent.
@MainActor
enum VersionSummarizer {
  static let defaultsKey = "versions.summaries"
  static var enabled: Bool { UserDefaults.standard.bool(forKey: defaultsKey) }

  /// The small model of the Agent's provider (none for the local fixture or the plan).
  static func config() -> ProviderConfig? {
    guard var c = ThoughtSummarizer.config(), c.auth != .chatGPT else { return nil }
    c.maxTokens = 1500  // room for its own reasoning on a large diff; the answer is cut to one line
    return c
  }

  /// What a summary is written from: the version's message and what it changed.
  nonisolated static func input(message: String, changed: [String], diff: String, truncated: Bool) -> String {
    let cards = changed.map { $0.isEmpty ? "the Workflow itself" : $0 }.joined(separator: ", ")
    return "Commit message: \(message.trimmingCharacters(in: .whitespacesAndNewlines))\nChanged Cards: \(cards)\n\n"
      + "Diff\(truncated ? " (cut short)" : ""):\n\(diff)"
  }

  /// One line, however the model answers.
  nonisolated static func clean(_ text: String) -> String {
    String(text.split(whereSeparator: \.isWhitespace).joined(separator: " ").prefix(300))
  }

  static func summarize(_ input: String, config: ProviderConfig) async throws -> String {
    let english = Bundle.main.preferredLocalizations.first?.hasPrefix("en") == true
    let system = Agent.versionSummaryPrompt.replacingOccurrences(of: "{language}", with: english ? "English" : "Japanese (plain form, 常体)")
    var text = ""
    for try await e in try MessagesProvider(config: config).stream(system: system, messages: [ChatMessage(role: "user", content: [.text(input)])], tools: []) {
      if case .text(let t) = e { text += t }
    }
    return clean(text)
  }
}

/// The project's versions (core/src/versions.cc): a git repository whose
/// commits are what runs ran, with their results in notes and named
/// versions as tags.
extension ProjectSession {
  func refreshVersions() async {
    guard let rpc else { return }
    versionGraph = try? await rpc.versionGraph()
  }

  func loadVersionCanvas(_ commit: String) async {
    guard let rpc, versionCanvases[commit] == nil else { return }
    if let canvas = try? await rpc.versionCanvas(commit: commit) { versionCanvases[commit] = canvas }
  }

  /// Has a small model write what `commit` changed, once, while switched on:
  /// as a run of it ends and when it is hovered without a summary.
  func summarizeVersion(_ commit: String) async {
    guard VersionSummarizer.enabled, let rpc, !versionSummaryTried.contains(commit) else { return }
    versionSummaryTried.insert(commit)
    versionSummaryBusy.insert(commit)
    defer { versionSummaryBusy.remove(commit) }
    guard let config = VersionSummarizer.config() else {
      versionSummaryErrors[commit] = String(localized: "この接続では版の要約を作りません。")
      return
    }
    do {
      await refreshVersions()
      guard let c = versionGraph?.commitList.first(where: { $0.id == commit }), (c.changeSummary ?? "").isEmpty else { return }
      let diff = try await rpc.versionDiff(commit: commit)
      // Nothing a summary may read changed (the layout, data files).
      guard !diff.text.isEmpty else { return }
      let input = VersionSummarizer.input(message: c.message ?? "", changed: c.changedList, diff: diff.text, truncated: diff.truncated)
      let text = try await VersionSummarizer.summarize(input, config: config)
      if text.isEmpty {
        versionSummaryErrors[commit] = String(localized: "モデルが文を返しませんでした。")
      } else {
        _ = try await rpc.versionSummary(opId: UUID(), origin: "ui", commit: commit, summary: text)
        await refreshVersions()
      }
    } catch {
      versionSummaryErrors[commit] = error.localizedDescription
    }
  }

  /// Makes the project a repository (a first version of what is saved).
  func makeRepository() async throws {
    guard let rpc else { throw RPCError.disconnected }
    _ = try await rpc.versionInit(opId: UUID(), origin: "ui")
    await refreshVersions()
  }

  /// Names what is saved now as a version (a commit and an annotated tag).
  func markVersion(name: String, message: String) async throws {
    guard let rpc else { throw RPCError.disconnected }
    if dirty { await save() }
    if dirty { throw VersionError.unsaved }
    _ = try await rpc.versionMark(opId: UUID(), origin: "ui", name: name, message: message)
    await refreshVersions()
  }

  /// Starts from `commit`: the documents become that version's; a change and
  /// run from there opens a branch, and nothing later is lost.
  func startFrom(_ commit: String) async throws {
    guard let rpc else { throw RPCError.disconnected }
    if runActive { throw VersionError.running }
    if dirty { throw VersionError.unsaved }
    _ = try await rpc.versionCheckout(baseRev: rev, opId: UUID(), origin: "ui", commit: commit)
    await refreshVersions()
  }
}

enum VersionError: LocalizedError {
  case unsaved, running
  var errorDescription: String? {
    switch self {
    case .unsaved: return String(localized: "未保存の変更があります。保存してから選んでください。")
    case .running: return String(localized: "実行中は別の版に移れません。")
    }
  }
}
