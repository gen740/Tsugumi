import CryptoKit
import Darwin
import Foundation
import Testing
import TsugumiAgentClient
@testable import TsugumiApp
@testable import TsugumiLauncher

/// UI subscriptions follow core-owned conversations through the production
/// client. Only the HTTP model response is replaced by a local fixture.
@MainActor
@Suite(.serialized) struct ChatResumeTests {
  @MainActor final class Fixture {
    let directory = URL(fileURLWithPath: "/tmp/tsugumi-chat-\(UUID().uuidString.prefix(8))")
    let conversationID = UUID().uuidString.lowercased()
    let core = Process()
    let model = Process()
    private let previousState = ProcessInfo.processInfo.environment["TSUGUMI_AGENT_STATE"]
    private let previousSupport = ProcessInfo.processInfo.environment["TSUGUMI_SUPPORT_DIR"]
    private var project: URL?
    private var daemon: SyncTests.Daemon?
    private var sessions: [ProjectSession] = []
    private var chats: [ChatController] = []
    private var port = 0

    var session: ProjectSession { sessions[0] }

    func start(slow: Double, options: [String]) async throws {
      try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
      let state = directory.appendingPathComponent("state")
      setenv("TSUGUMI_AGENT_STATE", state.path, 1)
      setenv("TSUGUMI_SUPPORT_DIR", directory.appendingPathComponent("support").path, 1)
      let build = ProcessInfo.processInfo.environment["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) }
        ?? SyncTests.root.appendingPathComponent("build")
      core.executableURL = ProcessInfo.processInfo.environment["TSUGUMI_AGENTD"].map { URL(fileURLWithPath: $0) }
        ?? build.appendingPathComponent("core/tsugumi-agentd")
      core.arguments = ["--state-dir", state.path, "--unix", directory.appendingPathComponent("agent.sock").path,
                        "--listen", "127.0.0.1:0", "--demos-dir", SyncTests.root.appendingPathComponent("demos").path]
      core.standardOutput = FileHandle.nullDevice
      core.standardError = FileHandle.nullDevice
      try core.run()
      let deadline = Date().addingTimeInterval(15)
      while !FileManager.default.fileExists(atPath: state.appendingPathComponent("agent.ready").path),
            core.isRunning, Date() < deadline { try await Task.sleep(for: .milliseconds(20)) }
      try #require(core.isRunning && FileManager.default.fileExists(atPath: state.appendingPathComponent("agent.ready").path))

      model.executableURL = URL(fileURLWithPath: "/usr/bin/env")
      model.arguments = ["python3", SyncTests.root.appendingPathComponent("tests/fixtures/model_fixture.py").path,
                         "--port", "0", "--slow", "\(slow)"] + options
      let output = Pipe()
      model.standardOutput = output
      model.standardError = FileHandle.nullDevice
      try model.run()
      let line = String(decoding: output.fileHandleForReading.availableData, as: UTF8.self)
      port = try #require(line.split(separator: ":").last.flatMap { Int($0.trimmingCharacters(in: .whitespacesAndNewlines)) })

      let project = try SyncTests.project()
      self.project = project
      let daemon = try SyncTests.Daemon(project: project)
      self.daemon = daemon
      _ = try makeSession()
      let endpoint = daemon.endpoint
      _ = try await CoreAgentClient.shared.call("RegisterProjectRequest", ["project_id": endpoint.projectId,
        "endpoint": endpoint.url.absoluteString, "token": endpoint.token, "title": "Chat UI fixture"])
      _ = try await CoreAgentClient.shared.call("ConversationCreateRequest", ["conversation_id": conversationID,
        "project_id": endpoint.projectId, "role": "worker"])
      try await CoreAgentClient.shared.configure(.localFixture(port: port), conversationID: conversationID)
    }

    func makeSession() throws -> ProjectSession {
      let project = try #require(project)
      let daemon = try #require(daemon)
      let session = ProjectSession(identity: try ProjectIdentity(url: project),
        environments: EnvironmentManager(support: directory.appendingPathComponent("support"), image: nil))
      session.endpointOverride = daemon.endpoint
      sessions.append(session)
      return session
    }

    /// The VM launcher creates read-only shares; this host fixture uses the
    /// daemon's equivalent allowlist, separate from the Agent's access grants.
    func shareFolders(_ folders: [URL]) throws {
      let daemon = try #require(daemon)
      let data = try JSONSerialization.data(withJSONObject: ["folders": folders.map { $0.standardizedFileURL.path }])
      try data.write(to: daemon.dir.appendingPathComponent("control/attached.json"))
    }

    func makeChat() -> ChatController {
      let port = port
      let chat = ChatController(session: session, directory: directory.appendingPathComponent("ui"),
                                connection: { .localFixture(port: port) })
      chats.append(chat)
      return chat
    }

    func keep(_ chat: ChatController) { chats.append(chat) }

    func send(_ text: String = "hello") async throws {
      _ = try await CoreAgentClient.shared.call("ConversationSendRequest", ["conversation_id": conversationID,
        "turn_id": UUID().uuidString, "text": text])
    }

    func snapshot() async throws -> [String: Any] {
      try await CoreAgentClient.shared.call("ConversationGetRequest", ["conversation_id": conversationID])
    }

    func finished() async throws -> [String: Any] {
      let deadline = Date().addingTimeInterval(30)
      var result = try await snapshot()
      while result["busy"] as? Bool == true, Date() < deadline {
        try await Task.sleep(for: .milliseconds(20))
        result = try await snapshot()
      }
      try #require(result["busy"] as? Bool == false)
      return result
    }

    func stop() async {
      for chat in chats { chat.detach() }
      for session in sessions { await session.close() }
      // Let the UI's detach tasks release their socket observations before
      // terminating the isolated daemon and restoring the environment.
      try? await Task.sleep(for: .milliseconds(100))
      for process in [core, model] where process.isRunning {
        process.terminate()
        let deadline = Date().addingTimeInterval(5)
        while process.isRunning, Date() < deadline { try? await Task.sleep(for: .milliseconds(20)) }
        if process.isRunning { kill(process.processIdentifier, SIGKILL) }
        process.waitUntilExit()
      }
      daemon?.stop()
      try? await Task.sleep(for: .milliseconds(100))
      if let previousState { setenv("TSUGUMI_AGENT_STATE", previousState, 1) } else { unsetenv("TSUGUMI_AGENT_STATE") }
      if let previousSupport { setenv("TSUGUMI_SUPPORT_DIR", previousSupport, 1) } else { unsetenv("TSUGUMI_SUPPORT_DIR") }
      if let project { try? FileManager.default.removeItem(at: project) }
      try? FileManager.default.removeItem(at: directory)
    }
  }

  func withFixture(slow: Double = 0.05, options: [String] = [],
                   _ body: (Fixture) async throws -> Void) async throws {
    let fixture = Fixture()
    do {
      try await fixture.start(slow: slow, options: options)
      try await body(fixture)
      await fixture.stop()
    } catch {
      await fixture.stop()
      throw error
    }
  }

  func until(_ what: String, seconds: Double = 30, _ ok: () -> Bool) async throws {
    let deadline = Date().addingTimeInterval(seconds)
    while !ok(), Date() < deadline { try await Task.sleep(for: .milliseconds(20)) }
    try #require(ok(), "\(what)")
  }

  func reply(_ chat: ChatController, after n: Int) -> Bool {
    chat.items.dropFirst(n).contains { $0.role == "assistant" }
  }

  @Test func multipleAttachedFoldersRemainDistinctAndReadableAfterSending() async throws {
    try await withFixture { t in
      let first = t.directory.appendingPathComponent("first/data")
      let second = t.directory.appendingPathComponent("second/data")
      for (folder, text) in [(first, "first folder"), (second, "second folder")] {
        try FileManager.default.createDirectory(at: folder.appendingPathComponent("nested"), withIntermediateDirectories: true)
        try Data(text.utf8).write(to: folder.appendingPathComponent("nested/note.txt"))
      }
      t.session.start()
      try await SyncTests.connected(t.session)
      let chat = t.makeChat()
      try await until("the chat is ready") { chat.currentId == t.conversationID }
      // Invalid batches do not appear attached, and can be retried.
      await chat.attach(folders: [first, t.directory.appendingPathComponent("missing")])
      #expect(chat.folders.isEmpty && chat.status != nil && !chat.attaching)
      await chat.attach(folders: [first, second, first])
      let paths = [first, second].map { $0.standardizedFileURL.path }
      #expect(chat.folders == paths && chat.status == nil && !chat.attaching)
      #expect(await t.session.environments.extraFolders[t.session.identity.id] == paths)
      await chat.attach(folders: [first, second])
      #expect(chat.folders == paths, "Repeated attachment does not duplicate either folder")
      try t.shareFolders([first, second])
      let project = try #require(t.session.endpointOverride?.projectId)
      let resources = paths.map { path in
        "folder:\(project):" + SHA256.hash(data: Data(path.utf8)).map { String(format: "%02x", $0) }.joined()
      }
      func grant(_ resource: String) async throws -> String {
        let request = try await CoreAgentClient.shared.call("AccessRequest", ["project_id": resource,
          "conversation_id": t.conversationID, "access": "Read", "scope": "Conversation", "reason": "Fixture-selected folder"])
        let id = try #require(request["id"] as? String)
        _ = try await CoreAgentClient.shared.call("AccessResolveRequest", ["request_id": id, "allow": true])
        return id
      }
      func read(_ folder: String, _ path: String) async throws -> String {
        let args = try JSONSerialization.data(withJSONObject: ["folder": folder, "path": path])
        let result = try await CoreAgentClient.shared.call("ToolCallRequest", ["conversation_id": t.conversationID,
          "project_id": project, "name": "attached_folder_read", "arguments_json": String(decoding: args, as: UTF8.self), "op_id": UUID().uuidString])
        #expect(result["isError"] as? Bool == false)
        let text = try #require(result["text"] as? String)
        let envelope = try #require(JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any])
        let value = try #require(envelope["value"] as? [String: Any])
        return try #require(value["bytes"] as? String)
      }
      _ = try await grant(project)
      let firstGrant = try await grant(resources[0])
      do {
        _ = try await read(paths[1], "nested/note.txt")
        Issue.record("A folder needs its own grant")
      } catch let failure as CoreAgentFailure { #expect(failure.code == "grant_required") }
      _ = try await grant(resources[1])
      chat.send("hello from both attached folders")
      try await until("the message with both attachments ended") { !chat.busy && reply(chat, after: 0) }
      let snapshot = try await t.snapshot()
      let conversation = try #require(snapshot["conversation"] as? [String: Any])
      let messages = try #require(conversation["messages"] as? [[String: Any]])
      let user = try #require(messages.first { $0["role"] as? String == "user" })
      let content = try #require(user["content"] as? [[String: Any]])
      let text = content.compactMap { $0["text"] as? String }.joined()
      for path in paths { #expect(text.contains(path)) }
      let attached = content.flatMap { $0["resource_ids"] as? [String] ?? [] }
      for resource in resources { #expect(attached.contains(resource)) }
      #expect(try await read(paths[0], "") == "nested/\n")
      #expect(try await read(paths[0], "nested/note.txt") == "first folder")
      #expect(try await read(paths[1], "nested/note.txt") == "second folder")
      _ = try await CoreAgentClient.shared.call("AccessRevokeRequest", ["grant_id": firstGrant])
      do {
        _ = try await read(paths[0], "nested/note.txt")
        Issue.record("Revoked folder access must not remain readable")
      } catch let failure as CoreAgentFailure { #expect(failure.code == "grant_required") }
      #expect(try await read(paths[1], "nested/note.txt") == "second folder")
    }
  }

  @Test func aRunningCoreAnswerShowsAsWorkingAndTheChatGoesOn() async throws {
    try await withFixture(slow: 0.1) { t in
      try await t.send()
      let chat = t.makeChat()
      try await until("the persisted request is visible") { chat.currentId == t.conversationID && !chat.items.isEmpty }
      #expect(!chat.busy, "The UI waits for its project connection; core keeps working.")
      t.session.start()
      try await SyncTests.connected(t.session)
      try await until("the running answer is observed") { chat.busy }
      try await until("the answer ended") { !chat.busy && reply(chat, after: 1) }
      #expect(chat.status == nil && !chat.failed)
      let before = chat.items.count
      chat.send("hello again")
      #expect(chat.busy)
      try await until("the next message got its reply") { !chat.busy && reply(chat, after: before) }
      #expect(chat.status == nil && !chat.failed)
    }
  }

  @Test func aMessageAfterStoppingIsNotShownAsStopped() async throws {
    try await withFixture(slow: 0.1) { t in
      try await t.send()
      let chat = t.makeChat()
      t.session.start()
      try await SyncTests.connected(t.session)
      try await until("the answer is streaming") { chat.busy && reply(chat, after: 1) }
      chat.cancel()
      #expect(!chat.busy)
      let before = chat.items.count
      chat.send("hello again")
      try await until("the new reply ended") { !chat.busy && reply(chat, after: before) }
      #expect(chat.status == nil && !chat.failed, "\(chat.status ?? "-")")
    }
  }

  @Test func detachingTheUIKeepsTheAnswerAndReopeningReadsIt() async throws {
    try await withFixture(slow: 0.1) { t in
      try await t.send()
      let left = t.makeChat()
      t.session.start()
      try await SyncTests.connected(t.session)
      try await until("the answer is streaming") { left.busy && reply(left, after: 1) }
      left.detach()
      let complete = try await t.finished()
      let conversation = try #require(complete["conversation"] as? [String: Any])
      let messages = try #require(conversation["messages"] as? [[String: Any]])
      #expect(messages.map { $0["role"] as? String } == ["user", "assistant"])
      let back = t.makeChat()
      try await until("the completed answer is restored") { back.currentId == t.conversationID && reply(back, after: 1) }
      #expect(!back.busy && !back.failed)
      #expect(back.items.count == 2)
      #expect(try await t.snapshot()["busy"] as? Bool == false)
    }
  }

  @Test func compactionShowsProgressAndKeepsTheVisibleHistory() async throws {
    try await withFixture { t in
      for n in 0..<4 { try await t.send("hello \(n)"); _ = try await t.finished() }
      let chat = t.makeChat()
      t.session.start()
      try await SyncTests.connected(t.session)
      try await until("the history is visible") { chat.items.count == 8 }
      chat.compact()
      #expect(chat.busy && chat.status == String(localized: "会話を圧縮しています…"))
      try await until("compaction ended") { !chat.busy }
      let trace = try await CoreAgentClient.shared.call("EventsRequest", ["conversation_id": t.conversationID, "after_seq": 0])
      let errors = (trace["events"] as? [[String: Any]] ?? []).compactMap { $0["event"] as? [String: Any] }
        .filter { $0["kind"] as? String == "error" }.compactMap { $0["message"] as? String }
      #expect(chat.status == nil && !chat.failed && !chat.contextFull, "\(chat.status ?? "-"); core errors: \(errors)")
      try await until("the compacted history is restored") { chat.items.contains { $0.archived } }
      #expect(chat.items.count == 8, "Only the original messages belong in the transcript: \(chat.items.map(\.text))")
    }
  }

  @Test func aConversationThatCannotFitSaysToStartANewOne() async throws {
    try await withFixture(slow: 0, options: ["--context-limit", "6000"]) { t in
      let chat = t.makeChat()
      t.session.start()
      try await SyncTests.connected(t.session)
      try await until("the conversation is open") { chat.currentId == t.conversationID }
      chat.send(String(repeating: "y", count: 100_000))
      try await until("the oversized request ended") { !chat.busy }
      #expect(chat.contextFull && chat.failed)
      #expect(chat.status == String(localized: "この会話はモデルの文脈に収まらなくなりました。新しい会話を始めてください（この会話は一覧に残ります）。"), "\(chat.status ?? "-")")
      chat.newSession()
      try await until("a new conversation is open") { chat.currentId != t.conversationID && chat.items.isEmpty }
      chat.send("hello")
      try await until("the new conversation got its reply") { !chat.busy && reply(chat, after: 0) }
      #expect(!chat.contextFull && !chat.failed, "\(chat.status ?? "-")")
    }
  }

  @Test func theChatFollowsTheOpenSession() async throws {
    try await withFixture(slow: 0) { t in
      let back = try t.makeSession()
      let model = AppModel()
      // Its home refresh talks to this fixture's core; it must not outlive the
      // fixture and start the shared core in whatever state directory comes next.
      await model.initialRefresh?.value
      let first = model.chat(for: t.session)
      t.keep(first)
      try await until("the first UI is attached") { first.currentId == t.conversationID }
      #expect(model.chat(for: t.session) === first)
      let replacement = model.chat(for: back)
      t.keep(replacement)
      try await until("the replacement UI is attached") { replacement.currentId == t.conversationID }
      #expect(replacement.session === back)
      #expect(replacement !== first)
      #expect(model.chat(for: back) === replacement)
    }
  }

  @Test func stoppingBeforeTheProjectConnectsKeepsItStopped() async throws {
    try await withFixture(slow: 0.1) { t in
      try await t.send()
      let chat = t.makeChat()
      try await until("the persisted request is visible") { chat.currentId == t.conversationID && !chat.items.isEmpty }
      chat.cancel()
      let stopped = try await t.finished()
      #expect(stopped["resumable"] as? Bool == false)
      #expect(stopped["continuable"] as? Bool == true)
      t.session.start()
      try await SyncTests.connected(t.session)
      try await Task.sleep(for: .milliseconds(250))
      #expect(!chat.busy && !reply(chat, after: 1))
      #expect(chat.stoppedAtLimit && !chat.failed)
    }
  }
}
