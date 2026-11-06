import Foundation
import Testing
import TsugumiAgent
@testable import TsugumiApp
@testable import TsugumiLauncher

/// Back in a project whose answer was stopped by leaving it: the chat resumes
/// the answer as any answer goes, and what is sent then gets its reply. Real
/// tsugumid and the local model fixture (Requires `build/core/tsugumid`, python3).
@MainActor
@Suite(.serialized) struct ChatResumeTests {
  /// The model fixture; each streamed chunk takes `slow` seconds.
  final class Model {
    let process = Process()
    let port: Int

    init(slow: Double) throws {
      process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
      process.arguments = ["python3", SyncTests.root.appendingPathComponent("tests/fixtures/model_fixture.py").path,
                           "--port", "0", "--slow", "\(slow)"]
      let out = Pipe()
      process.standardOutput = out
      process.standardError = FileHandle.nullDevice
      try process.run()
      // "model fixture on http://127.0.0.1:<port>"
      let line = String(decoding: out.fileHandleForReading.availableData, as: UTF8.self)
      port = line.split(separator: ":").last.flatMap { Int($0.trimmingCharacters(in: .whitespacesAndNewlines)) } ?? 0
    }

    func stop() {
      process.terminate()
      process.waitUntilExit()
    }
  }

  struct Setup {
    let session: ProjectSession
    let chat: ChatController
    let stop: () -> Void
  }

  /// A chat opened on a conversation whose last request was never answered,
  /// as it is after leaving the project during the answer.
  func chatWithAnUnansweredRequest(slow: Double) throws -> Setup {
    let project = try SyncTests.project()
    let daemon = try SyncTests.Daemon(project: project)
    let model = try Model(slow: slow)
    let storage = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("chat-\(UUID().uuidString.prefix(8))")
    var conversation = Conversation()
    conversation.messages = [ChatMessage(role: "user", content: [.text("hello")])]
    try conversation.save(storage.appendingPathComponent("sessions/unanswered.json"))
    let session = ProjectSession(identity: try ProjectIdentity(url: project), environments: EnvironmentManager(image: nil))
    session.endpointOverride = daemon.endpoint
    let port = model.port
    let chat = ChatController(session: session, directory: storage, connection: { .localFixture(port: port) })
    return Setup(session: session, chat: chat, stop: {
      model.stop()
      daemon.stop()
      try? FileManager.default.removeItem(at: project)
      try? FileManager.default.removeItem(at: storage)
    })
  }

  func until(_ what: String, seconds: Double = 30, _ ok: () -> Bool) async throws {
    let deadline = Date().addingTimeInterval(seconds)
    while !ok() && Date() < deadline { try await Task.sleep(for: .milliseconds(20)) }
    #expect(ok(), "\(what)")
    try #require(ok())
  }

  func reply(_ chat: ChatController, after n: Int) -> Bool {
    chat.items.dropFirst(n).contains { $0.role == "assistant" }
  }

  /// The resumed answer waits for the project, shows as working (it can be
  /// stopped) and ends like any answer; then a message gets its reply.
  @Test func aResumedAnswerShowsAsWorkingAndTheChatGoesOn() async throws {
    let t = try chatWithAnUnansweredRequest(slow: 0.05)
    defer { t.stop() }
    let chat = t.chat
    try await until("the conversation was read") { !chat.items.isEmpty }
    try await Task.sleep(for: .milliseconds(300))
    #expect(!chat.busy && chat.items.count == 1, "not before the project is connected: \(chat.items.map(\.text))")

    t.session.start()
    try await SyncTests.connected(t.session)
    try await until("the answer resumed") { reply(chat, after: 1) || chat.status != nil }
    #expect(chat.busy, "the answer is under way: \(chat.status ?? "-")")
    try await until("the answer ended") { !chat.busy }
    #expect(chat.status == nil, "\(chat.status ?? "-")")

    let before = chat.items.count
    chat.send("続けて")
    #expect(chat.busy)
    try await until("the message got its reply") { !chat.busy && reply(chat, after: before) }
    #expect(chat.status == nil, "\(chat.status ?? "-")")
  }

  /// Stopping the resumed answer and sending at once: the stopped turn ends
  /// after the new one began, and says nothing about it.
  @Test func aMessageAfterStoppingIsNotShownAsStopped() async throws {
    let t = try chatWithAnUnansweredRequest(slow: 0.2)
    defer { t.stop() }
    let chat = t.chat
    t.session.start()
    try await SyncTests.connected(t.session)
    try await until("the answer resumed") { chat.busy && reply(chat, after: 1) }

    chat.cancel()
    #expect(!chat.busy)
    let before = chat.items.count
    chat.send("続けて")
    try await until("the stopped turn was replaced and the new reply began") { reply(chat, after: before) }
    #expect(chat.busy && chat.status == nil, "busy \(chat.busy): \(chat.status ?? "-")")
    try await until("the reply ended") { !chat.busy }
    #expect(chat.status == nil, "\(chat.status ?? "-")")
  }

  /// Back in the project: its chat is on the session open now, not on the
  /// closed one, where its tools have no connection.
  @Test func theChatFollowsTheOpenSession() throws {
    let project = try SyncTests.project()
    defer { try? FileManager.default.removeItem(at: project) }
    let environments = EnvironmentManager(image: nil)
    let left = ProjectSession(identity: try ProjectIdentity(url: project), environments: environments)
    let back = ProjectSession(identity: try ProjectIdentity(url: project), environments: environments)
    let model = AppModel()
    let chat = model.chat(for: left)
    #expect(model.chat(for: left) === chat)
    #expect(model.chat(for: back).session === back)
    #expect(model.chat(for: back) === model.chat(for: back))
  }

  /// Stopped while waiting for the project, it does not start afterwards.
  @Test func stoppingBeforeTheProjectConnectsKeepsItStopped() async throws {
    let t = try chatWithAnUnansweredRequest(slow: 0.05)
    defer { t.stop() }
    let chat = t.chat
    try await until("the conversation was read") { !chat.items.isEmpty }
    chat.cancel()
    t.session.start()
    try await SyncTests.connected(t.session)
    try await Task.sleep(for: .milliseconds(500))
    #expect(!chat.busy && !reply(chat, after: 1), "\(chat.items.map(\.text))")
  }
}
