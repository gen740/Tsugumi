import CryptoKit
import Darwin
import Foundation
import Testing
@testable import TsugumiAgentClient
import TsugumiRPC
@testable import TsugumiApp
@testable import TsugumiLauncher

/// UI subscriptions follow core-owned conversations through the production
/// client. Only the HTTP model response is replaced by a local fixture.
@MainActor
@Suite(.serialized, .hostService) struct ChatResumeTests {
  @MainActor final class Fixture {
    let directory = URL(fileURLWithPath: ProcessInfo.processInfo.environment["TSUGUMI_SOCKET_DIR"] ?? "/tmp").appendingPathComponent("chat-\(UUID().uuidString.prefix(8))")
    let conversationID = UUID().uuidString.lowercased()
    var core = Process()
    let model = Process()
    private let environment = HostServiceEnvironment()
    private var project: URL?
    private var daemon: SyncTests.Daemon?
    private var sessions: [ProjectSession] = []
    private var chats: [ChatController] = []
    private var streamRelease: URL { directory.appendingPathComponent("stream.release") }
    private var port = 0
    private var servicePID: Int32?
    private var helperPID: URL { directory.appendingPathComponent("vmhost.pid") }

    var session: ProjectSession { sessions[0] }

    func start(slow: Double, options: [String], managed: Bool) async throws {
      try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
      let state = directory.appendingPathComponent("state")
      await environment.install(state: state, support: directory.appendingPathComponent("support"))
      let build = ProcessInfo.processInfo.environment["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) }
        ?? SyncTests.root.appendingPathComponent("build")
      core.executableURL = ProcessInfo.processInfo.environment["TSUGUMI_HOSTD"].map { URL(fileURLWithPath: $0) }
        ?? build.appendingPathComponent("hostd/tsugumi-hostd")
      core.arguments = ["--state-dir", state.path,
                        "--socket-dir", directory.appendingPathComponent("b").path,
                        "--usage-scratch-dir", canonicalDirectory(directory).appendingPathComponent("scratch").path,
                        "--unix", directory.appendingPathComponent("agent.sock").path,
                        "--listen", "127.0.0.1:0", "--demos-dir", SyncTests.root.appendingPathComponent("demos").path]
      if managed { try useHelperDouble() }
      core.standardOutput = FileHandle.nullDevice
      core.standardError = FileHandle.nullDevice
      try core.run()
      try await waitForAgent()
      try releaseStream()
      model.executableURL = URL(fileURLWithPath: "/usr/bin/env")
      model.arguments = ["python3", SyncTests.root.appendingPathComponent("tests/fixtures/model_fixture.py").path,
                         "--port", "0", "--slow", "\(slow)", "--stream-release", streamRelease.path] + options
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
      // Match the host folder registered by EnvironmentManager's Ready state.
      _ = try await CoreHostClient.shared.call("RegisterProjectRequest", ["project_id": endpoint.projectId,
        "endpoint": endpoint.url.absoluteString, "token": endpoint.token, "title": "Chat UI fixture",
        "path": project.standardizedFileURL.path])
      let _: Tsugumi_Agent_ConversationResultT = try await CoreHostClient.shared.agentCall("ConversationCreateRequest", ["conversation_id": conversationID,
        "project_id": endpoint.projectId, "role": "worker"])
      let provider = Tsugumi_Host_ProviderInputT()
      provider.endpoint = "http://127.0.0.1:\(port)"
      provider.model = "fixture"
      // Use the model budget: 6K cannot hold the real system prompt and tools.
      _ = try await CoreHostClient.shared.call("ProviderSetRequest", ["profile": "desktop", "id": "fixture", "provider": provider])
      try await CoreHostClient.shared.configure(.fixture, conversationID: conversationID)
    }

    /// The existing local OAuth fixture replaces the browser and OpenAI.
    func signInChatGPT(clientId: String? = nil, consent: Bool = false) async throws -> ChatGPTAuth.SignInResult {
      let auth = ChatGPTAuth.shared(endpoints: .init(issuer: "http://127.0.0.1:\(port)", resource: "https://api.openai.com/v1"))
      let browser = URLSession(configuration: .ephemeral)
      return try await auth.signIn(clientId: clientId, consent: consent) { url in _ = try await browser.data(from: url) }
    }

    func restartAgent() async throws {
      let binary = core.executableURL
      let arguments = core.arguments
      let oldPID = servicePID ?? core.processIdentifier
      kill(oldPID, SIGKILL)
      if oldPID == core.processIdentifier { core.waitUntilExit() }
      else {
        let deadline = Date().addingTimeInterval(5)
        while kill(oldPID, 0) == 0, Date() < deadline { try await Task.sleep(for: .milliseconds(20)) }
        try #require(kill(oldPID, 0) != 0, "The previous fixture service did not exit")
      }
      // Leave stale readiness in place: the observer may already have replaced
      // it with the winner's file. Validate the new PID instead of unlinking it.
      core = Process()
      core.executableURL = binary
      core.arguments = arguments
      core.standardOutput = FileHandle.nullDevice
      core.standardError = FileHandle.nullDevice
      try core.run()
      try await waitForAgent(replacing: oldPID)
      kill(model.processIdentifier, SIGCONT)
    }

    private func waitForAgent(replacing oldPID: Int32? = nil) async throws {
      let readyFile = directory.appendingPathComponent("state/host.ready")
      // An observing client can win the startup lock. Status 4 means wait for
      // that service, not that the replacement service failed to start.
      let replacement = core
      let deadline = Date().addingTimeInterval(15)
      var ready: [String: Any]?
      while ready == nil, Date() < deadline {
        if let bytes = try? Data(contentsOf: readyFile),
           let value = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any],
           let pid = value["pid"] as? Int32, pid != oldPID, kill(pid, 0) == 0,
           value["unix_socket"] is String,
           value["host_socket"] is String, value["user_token"] is String { ready = value; break }
        if !replacement.isRunning, replacement.terminationStatus != 4 { break }
        try await Task.sleep(for: .milliseconds(50))
      }
      let value = try #require(ready, "The host service did not become ready")
      servicePID = try #require(value["pid"] as? Int32)
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

    /// The Mac app's host service starts projects through its VM helper, whose
    /// `tsugumid` accepts the app's connection (a native client sends no
    /// Origin); one started by the host service itself is for browsers and
    /// refuses it. The helper double keeps that contract without booting a VM.
    private func useHelperDouble() throws {
      let environment = ProcessInfo.processInfo.environment
      let python = try #require((environment["PATH"] ?? "").split(separator: ":")
        .map { URL(fileURLWithPath: String($0)).appendingPathComponent("python3").path }
        .first { FileManager.default.isExecutableFile(atPath: $0) }, "python3 runs the VM helper double")
      let source = try String(contentsOf: SyncTests.root.appendingPathComponent("tests/python/vm_helper_double.py"), encoding: .utf8)
      let helper = directory.appendingPathComponent("vmhost")
      try Data("#!\(python)\n\(source)".utf8).write(to: helper)
      try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: helper.path)
      let support = directory.appendingPathComponent("support")
      core.arguments = (core.arguments ?? []) + ["--vmhost", helper.path, "--vm-support", support.path,
                                                 "--vm-image", "/image"]
      core.environment = environment.merging(["HOST_LOG": directory.appendingPathComponent("vmhost.log").path,
        "HOST_BUSY": directory.appendingPathComponent("vmhost.busy").path, "HOST_PID": helperPID.path]) { _, value in value }
    }

    /// Transfer this test's project to the production host environment manager.
    func useManagedEnvironment() async throws {
      await session.close()
      daemon?.stop()
      let managed = try ProjectIdentity(url: #require(project))
      let session = ProjectSession(identity: managed, environments: self.session.environments)
      sessions.insert(session, at: 0)
      session.start()
    }

    func makeChat() -> ChatController {
      let chat = ChatController(session: session, directory: directory.appendingPathComponent("ui"),
                                connection: { .fixture })
      chats.append(chat)
      return chat
    }

    func keep(_ chat: ChatController) { chats.append(chat) }

    func send(_ text: String = "hello", holdingModel: Bool = false) async throws {
      // Keep the response pending until restart, regardless of CI scheduling.
      if holdingModel { try #require(kill(model.processIdentifier, SIGSTOP) == 0) }
      _ = try await CoreHostClient.shared.call("ConversationSendRequest", ["conversation_id": conversationID,
        "turn_id": UUID().uuidString, "text": text])
    }

    func holdStream() throws { try FileManager.default.removeItem(at: streamRelease) }
    func releaseStream() throws { try Data().write(to: streamRelease) }

    func snapshot() async throws -> Tsugumi_Agent_ConversationResultT {
      try await CoreHostClient.shared.agentCall("ConversationGetRequest", ["conversation_id": conversationID])
    }

    func finished() async throws -> Tsugumi_Agent_ConversationResultT {
      let deadline = Date().addingTimeInterval(30)
      var result = try await snapshot()
      while result.busy, Date() < deadline {
        try await Task.sleep(for: .milliseconds(20))
        result = try await snapshot()
      }
      try #require(!result.busy)
      return result
    }

    func stop() async {
      try? releaseStream()
      for chat in chats { await chat.detach().value }
      for session in sessions { await session.close() }
      for session in sessions where session.endpointOverride == nil { try? await session.environments.stop(session.identity) }
      if model.isRunning { kill(model.processIdentifier, SIGCONT) }
      if let pid = servicePID, pid != core.processIdentifier {
        // The lock winner belongs to this fixture even when the client launched it.
        kill(pid, SIGTERM)
        let deadline = Date().addingTimeInterval(5)
        while kill(pid, 0) == 0, Date() < deadline { try? await Task.sleep(for: .milliseconds(20)) }
        if kill(pid, 0) == 0 { kill(pid, SIGKILL) }
      }
      for process in [core, model] where process.isRunning {
        process.terminate()
        let deadline = Date().addingTimeInterval(5)
        while process.isRunning, Date() < deadline { try? await Task.sleep(for: .milliseconds(20)) }
        if process.isRunning { kill(process.processIdentifier, SIGKILL) }
        process.waitUntilExit()
      }
      daemon?.stop()
      // The helper double outlives the host service, as the VM helper does.
      if let text = try? String(contentsOf: helperPID, encoding: .utf8), let pid = Int32(text), pid > 1 {
        kill(pid, SIGTERM)
        let deadline = Date().addingTimeInterval(5)
        while kill(pid, 0) == 0, Date() < deadline { try? await Task.sleep(for: .milliseconds(20)) }
        if kill(pid, 0) == 0 { kill(pid, SIGKILL) }
      }
      await environment.restore()
      if let project { try? FileManager.default.removeItem(at: project) }
      try? FileManager.default.removeItem(at: directory)
    }
  }

  func withFixture(slow: Double = 0.05, options: [String] = [], managed: Bool = false,
                   _ body: (Fixture) async throws -> Void) async throws {
    let fixture = Fixture()
    do {
      try await fixture.start(slow: slow, options: options, managed: managed)
      try await body(fixture)
      await fixture.stop()
    } catch {
      await fixture.stop()
      throw error
    }
  }

  func until(_ what: String, seconds: Double = 30, _ ok: @escaping @MainActor () -> Bool) async throws {
    try await waitForModel(what, ok)
  }

  func reply(_ chat: ChatController, after n: Int) -> Bool {
    chat.items.dropFirst(n).contains { $0.role == "assistant" }
  }

  @Test func openingAndRefreshingAnInterruptedConversationOnlyObserve() async throws {
    try await withFixture(slow: 0.3) { t in
      try await t.send("hello interrupted", holdingModel: true)
      // The core records preparing/model before sending the RPC acknowledgement.
      try await t.restartAgent()
      let before = try await t.snapshot()
      try #require(before.resumable)
      let operation = try #require(before.interruptedOperation)
      #expect(["preparing", "model"].contains(operation.kind))
      let chat = t.makeChat()
      try await until("the interruption is visible") { chat.interrupted }
      #expect(!chat.busy && !chat.canContinue)
      let agent = Agent(id: t.conversationID, transport: RPCToolTransport(session: t.session), provider: .fixture)
      try await agent.open()
      try await agent.refresh()
      let after = try await t.snapshot()
      #expect(!after.busy)
      #expect(after.resumable)
      #expect(after.eventSeq == before.eventSeq)
      #expect(after.conversation.messages.count == 1)
      #expect(chat.interruptedOperation?.kind == operation.kind)
      t.session.start()
      try await SyncTests.connected(t.session)
      chat.resume()
      try await until("explicit resume finished") { !chat.busy && reply(chat, after: 0) }
      #expect(!chat.interrupted)
    }
  }

  @Test func aRestartWhileObservedShowsInterruptionWithoutResuming() async throws {
    try await withFixture(slow: 2) { t in
      t.session.start()
      try await SyncTests.connected(t.session)
      try await t.send("hello interrupted", holdingModel: true)
      let chat = t.makeChat()
      try await until("the running conversation is observed") { chat.busy }
      try await t.restartAgent()
      try await until("the restarted conversation is interrupted") { chat.interrupted }
      #expect(!chat.busy && !chat.failed)
      let snapshot = try await t.snapshot()
      #expect(snapshot.resumable)
      #expect(try await t.snapshot().eventSeq == snapshot.eventSeq)
    }
  }

  @Test func anInterruptedConversationAcceptsANewMessageWithoutResume() async throws {
    try await withFixture(slow: 0.3) { t in
      try await t.send("hello interrupted", holdingModel: true)
      try await t.restartAgent()
      let chat = t.makeChat()
      try await until("the interruption is visible") { chat.interrupted }
      t.session.start()
      try await SyncTests.connected(t.session)
      chat.send("hello instead")
      try await until("the new message finished") { !chat.busy && reply(chat, after: 1) }
      #expect(!chat.interrupted)
      #expect(chat.items.contains { $0.role == "user" && $0.text == "hello instead" })
    }
  }

  @Test func multipleAttachedFoldersRemainDistinctAndReadableAfterSending() async throws {
    try await withFixture(managed: true) { t in
      let first = t.directory.appendingPathComponent("first/data")
      let second = t.directory.appendingPathComponent("second/data")
      for (folder, text) in [(first, "first folder"), (second, "second folder")] {
        try FileManager.default.createDirectory(at: folder.appendingPathComponent("nested"), withIntermediateDirectories: true)
        try Data(text.utf8).write(to: folder.appendingPathComponent("nested/note.txt"))
      }
      try await t.useManagedEnvironment()
      try await SyncTests.connected(t.session)
      let chat = t.makeChat()
      try await until("the chat is ready") { chat.currentId == t.conversationID }
      // Invalid batches do not appear attached, and can be retried.
      await chat.attach(folders: [first, t.directory.appendingPathComponent("missing")])
      #expect(chat.folders.isEmpty && chat.status != nil && !chat.attaching)
      await chat.attach(folders: [first, second, first])
      let paths = [first, second].map { $0.standardizedFileURL.path }
      #expect(chat.folders == paths && chat.status == nil && !chat.attaching)
      let attached = try await CoreHostClient(support: t.session.environments.support).call("EnvironmentRequest", ["project": t.session.identity.url.path, "action": Tsugumi_Host_EnvironmentAction.state])
      #expect(attached["folders"] as? [String] == paths)
      await chat.attach(folders: [first, second])
      #expect(chat.folders == paths, "Repeated attachment does not duplicate either folder")
      #expect(t.session.connected)
      let project = try #require(t.session.projectId)
      let resources = paths.map { path in
        "folder:\(project):" + SHA256.hash(data: Data(path.utf8)).map { String(format: "%02x", $0) }.joined()
      }
      func grant(_ resource: String) async throws -> String {
        let request = try await CoreHostClient.shared.call("AccessRequest", ["project_id": resource,
          "conversation_id": t.conversationID, "access": "Read", "reason": "Fixture-selected folder"])
        let id = try #require(request["id"] as? String)
        _ = try await CoreHostClient.shared.call("AccessResolveRequest", ["request_id": id, "allow": true])
        return id
      }
      func read(_ folder: String, _ path: String) async throws -> String {
        let args = try JSONSerialization.data(withJSONObject: ["folder": folder, "path": path])
        let result = try await CoreHostClient.shared.call("ToolCallRequest", ["conversation_id": t.conversationID,
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
      } catch let failure as CoreHostFailure { #expect(failure.code == "grant_required") }
      _ = try await grant(resources[1])
      chat.send("hello from both attached folders")
      try await until("the message with both attachments ended") { !chat.busy && reply(chat, after: 0) }
      let snapshot = try await t.snapshot()
      let conversation = snapshot.conversation
      let messages = conversation.messages.compactMap { $0 }
      let user = try #require(messages.first { $0.role == "user" })
      let content = try #require(JSONSerialization.jsonObject(with: Data((user.contentJson ?? "[]").utf8)) as? [[String: Any]])
      let text = content.compactMap { $0["text"] as? String }.joined()
      for path in paths { #expect(text.contains(path)) }
      let sentResources = content.flatMap { $0["resource_ids"] as? [String] ?? [] }
      for resource in resources { #expect(sentResources.contains(resource)) }
      #expect(try await read(paths[0], "") == "nested/\n")
      #expect(try await read(paths[0], "nested/note.txt") == "first folder")
      #expect(try await read(paths[1], "nested/note.txt") == "second folder")
      _ = try await CoreHostClient.shared.call("AccessRevokeRequest", ["grant_id": firstGrant])
      do {
        _ = try await read(paths[0], "nested/note.txt")
        Issue.record("Revoked folder access must not remain readable")
      } catch let failure as CoreHostFailure { #expect(failure.code == "grant_required") }
      #expect(try await read(paths[1], "nested/note.txt") == "second folder")
    }
  }

  @Test func aRunningCoreAnswerShowsAsWorkingAndTheChatGoesOn() async throws {
    try await withFixture(slow: 0) { t in
      try t.holdStream()
      try await t.send()
      let chat = t.makeChat()
      try await until("the running answer is observed before the project connects") {
        chat.currentId == t.conversationID && !chat.items.isEmpty && chat.busy
      }
      #expect(!t.session.connected, "Observing core work does not need a project connection")
      t.session.start()
      try await SyncTests.connected(t.session)
      try await until("the running answer is observed") { chat.busy }
      try t.releaseStream()
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
    try await withFixture(slow: 0) { t in
      try t.holdStream()
      try await t.send()
      let chat = t.makeChat()
      t.session.start()
      try await SyncTests.connected(t.session)
      try await until("the answer is streaming") { chat.busy && reply(chat, after: 1) }
      chat.cancel()
      #expect(!chat.busy)
      try t.releaseStream()
      let before = chat.items.count
      chat.send("hello again")
      try await until("the new reply ended") { !chat.busy && reply(chat, after: before) }
      #expect(chat.status == nil && !chat.failed, "\(chat.status ?? "-")")
    }
  }

  @Test func detachingTheUIKeepsTheAnswerAndReopeningReadsIt() async throws {
    try await withFixture(slow: 0) { t in
      try t.holdStream()
      try await t.send()
      let left = t.makeChat()
      t.session.start()
      try await SyncTests.connected(t.session)
      try await until("the answer is streaming") { left.busy && reply(left, after: 1) }
      await left.detach().value
      try t.releaseStream()
      let complete = try await t.finished()
      let conversation = complete.conversation
      let messages = conversation.messages.compactMap { $0 }
      #expect(messages.map { $0.role } == ["user", "assistant"])
      let back = t.makeChat()
      try await until("the completed answer is restored") { back.currentId == t.conversationID && reply(back, after: 1) }
      #expect(!back.busy && !back.failed)
      #expect(back.items.count == 2)
      #expect(try await t.snapshot().busy == false)
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
      let trace: Tsugumi_Agent_EventsPageT = try await CoreHostClient.shared.agentCall("EventsRequest", ["conversation_id": t.conversationID, "after_seq": 0])
      let errors = trace.events.compactMap { $0?.event }.filter { $0.kind == "error" }.compactMap(\.message)
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

  @Test func copiedProjectsNeverRestoreEachOthersChatOrPermission() async throws {
    try await withFixture(slow: 0) { t in
      let original = t.makeChat()
      try await until("copy A is open") { original.currentId == t.conversationID }
      try await t.send("Only copy A")
      let project = try #require(t.session.endpointOverride)
      _ = try await CoreHostClient.shared.call("AccessRequest", ["project_id": project.projectId,
        "conversation_id": t.conversationID, "access": "Read", "reason": "Read project content"])
      let copied = t.directory.appendingPathComponent("copy-b")
      // A distinct project copies definitions, without the original project's conversation store.
      try FileManager.default.createDirectory(at: copied, withIntermediateDirectories: true)
      for source in try FileManager.default.contentsOfDirectory(at: t.session.identity.url, includingPropertiesForKeys: nil)
        where source.lastPathComponent != ".tsugumi" {
        try FileManager.default.copyItem(at: source, to: copied.appendingPathComponent(source.lastPathComponent))
      }
      let identity = try ProjectIdentity(url: copied)
      #expect(identity.id != t.session.identity.id)
      let second = ProjectSession(identity: identity, environments: t.session.environments)
      let projectID = UUID().uuidString.lowercased()
      second.endpointOverride = Endpoint(url: project.url, token: project.token, projectId: projectID, serverInstanceId: project.serverInstanceId)
      // A folder is registered under the identity it holds.
      try FileManager.default.createDirectory(at: copied.appendingPathComponent(".tsugumi"), withIntermediateDirectories: true)
      try Data(projectID.utf8).write(to: copied.appendingPathComponent(".tsugumi/project-id"))
      _ = try await CoreHostClient.shared.call("RegisterProjectRequest", ["project_id": projectID,
        "endpoint": project.url.absoluteString, "token": project.token, "title": "copy-b", "path": copied.path])
      let chat = ChatController(session: second, connection: { .fixture })
      t.keep(chat)
      try await until("copy B has its own conversation") { !chat.currentId.isEmpty }
      #expect(chat.currentId != t.conversationID)
      #expect(chat.sessions.isEmpty && chat.items.isEmpty && chat.permissionPrompt == nil)
      #expect(await chat.search("Only copy A").isEmpty)
      chat.select(t.conversationID)
      try await until("an explicit cross-project selection is refused") { chat.status != nil && !chat.loading }
      #expect(chat.items.isEmpty && chat.permissionPrompt == nil)
      let saved = try await t.snapshot()
      #expect(saved.projectId == project.projectId)
    }
  }

  @Test func theChatModelFollowsSettingsEditedOutsideItsWindow() async throws {
    try await withFixture(slow: 0) { t in
      let chat = ChatController(session: t.session)
      t.keep(chat)
      let settings = try await CoreHostClient.shared.call("SettingsGetRequest", ["profile": "desktop"])
      _ = try await CoreHostClient.shared.call("SettingsUpdateRequest", ["profile": "desktop", "revision": settings["revision"]!,
        "changes": [["key": "agent.provider", "value_json": "\"chatgpt\""],
                    ["key": "agent.model.chatgpt", "value_json": "\"gpt-6.1-terra\""]]])
      // Opening the panel/menu or activating its window reads once, with no timer.
      await chat.refreshModelSettings()
      #expect(chat.displayedProvider.model == "gpt-6.1-terra")
      #expect(chat.displayedProvider.auth == .chatGPT)
      let menu = ModelMenu()
      menu.setContents(connections: [.init(id: "chatgpt", name: String(localized: "ChatGPT プラン"), models: ["gpt-6.1-sol", "gpt-6.1-terra"])],
        provider: "chatgpt", model: chat.displayedProvider.model, names: [:])
      #expect(menu.menu.items.filter { $0.state == .on && $0.title.hasPrefix("gpt-") }.map(\.title) == ["gpt-6.1-terra"])
    }
  }

  @Test func compactionDoesNotReplaceStreamedRowsWithTheAgentsOlderCache() async throws {
    try await withFixture(slow: 0) { t in
      try await t.send("Initial request")
      _ = try await t.finished()
      let chat = t.makeChat()
      try await until("the older snapshot is loaded") { chat.ready }
      let host = try await t.snapshot()
      let conversation = host.conversation
      let row = try #require(conversation.rows.compactMap { $0 }.last)
      row.text = "New streamed progress"
      let snapshot = Tsugumi_Agent_ConversationT(); snapshot.id = t.conversationID; snapshot.rows = [row]
      let projected = try ServiceWire.conversation(snapshot)
      chat.handle(.presentation(projected.rows), of: chat.answer)
      chat.handle(.compacted, of: chat.answer)
      await Task.yield()
      await Task.yield()
      #expect(chat.items.last?.text == "New streamed progress")
    }
  }

  @Test func failedAndCancelledSelectionsKeepTheVisibleConversation() async throws {
    try await withFixture(slow: 0) { t in
      try await t.send("Keep this conversation")
      _ = try await t.finished()
      let chat = t.makeChat()
      try await until("the conversation is visible") { chat.items.count == 2 }
      let text = chat.items.map(\.text)
      chat.select(String(repeating: "x", count: 257))
      #expect(chat.items.map(\.text) == text, "A pending selection keeps the previous panel")
      try await until("the failed load reports its error") { !chat.loading && chat.status != nil }
      #expect(chat.items.map(\.text) == text)
      #expect(!chat.ready, "Old rows cannot receive a message for the failed selection")
      chat.select(String(repeating: "x", count: 257))
      chat.select(t.conversationID)
      try await until("the cancelled selection is replaced") { chat.ready }
      #expect(chat.items.map(\.text) == text)
      #expect(chat.status == nil)
    }
  }

  @Test func theChatFollowsTheOpenSession() async throws {
    try await withFixture(slow: 0) { t in
      try await t.send("Keep the selected conversation")
      _ = try await t.finished()
      let back = try t.makeSession()
      let model = AppModel()
      // Its home refresh talks to this fixture's core; it must not outlive the
      // fixture and start the shared core in whatever state directory comes next.
      await model.initialRefresh?.value
      let first = model.chat(for: t.session)
      t.keep(first)
      try await until("the first UI is attached") { first.currentId == t.conversationID }
      try await until("the selected transcript is visible") { first.ready && first.items.count == 2 }
      let visible = first.items.map(\.text)
      let project = try #require(t.session.endpointOverride)
      let _: Tsugumi_Agent_ConversationResultT = try await CoreHostClient.shared.agentCall("ConversationCreateRequest", ["conversation_id": UUID().uuidString.lowercased(), "project_id": project.projectId])
      #expect(model.chat(for: t.session) === first)
      let replacement = model.chat(for: back)
      t.keep(replacement)
      #expect(replacement.currentId == t.conversationID, "Replacing a project session keeps the selected conversation, even when a newer one exists")
      #expect(replacement.items.map(\.text) == visible, "The panel keeps its content while reconnecting")
      try await until("the replacement UI is attached") { replacement.currentId == t.conversationID }
      #expect(replacement.session === back)
      #expect(replacement !== first)
      #expect(model.chat(for: back) === replacement)
    }
  }

  @Test func stoppingBeforeTheProjectConnectsKeepsItStopped() async throws {
    try await withFixture(slow: 0) { t in
      try await t.send(holdingModel: true)
      let chat = t.makeChat()
      try await until("the persisted request is visible") { chat.currentId == t.conversationID && !chat.items.isEmpty }
      chat.cancel()
      let stopped = try await t.finished()
      try #require(kill(t.model.processIdentifier, SIGCONT) == 0)
      #expect(!stopped.resumable)
      #expect(stopped.continuable)
      t.session.start()
      try await SyncTests.connected(t.session)
      try await until("the cancelled conversation is observed") { !chat.busy && chat.canContinue }
      #expect(!chat.busy && !reply(chat, after: 1))
      #expect(chat.canContinue && !chat.failed)
    }
  }
}
