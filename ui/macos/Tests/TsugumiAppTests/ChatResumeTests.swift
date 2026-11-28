import CryptoKit
import Darwin
import Foundation
import Testing
@testable import TsugumiAgentClient
import TsugumiRPC
import TsugumiTestSupport
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

    func start(managed: Bool) async throws {
      try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
      let state = directory.appendingPathComponent("state")
      await environment.install(state: state, support: directory.appendingPathComponent("support"))
      let build = ProcessInfo.processInfo.environment["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) }
        ?? SyncTests.root.appendingPathComponent("build")
      core.executableURL = ProcessInfo.processInfo.environment["TSUGUMI_HOSTD"].map { URL(fileURLWithPath: $0) }
        ?? build.appendingPathComponent("hostd/tsugumi-hostd")
      core.arguments = ["--state-dir", state.path,
                        "--socket-dir", directory.appendingPathComponent("b").path,
                        "--run-scratch-root", canonicalDirectory(directory).appendingPathComponent("scratch").path,
                        "--unix", directory.appendingPathComponent("agent.sock").path,
                        "--listen", "127.0.0.1:0"]
      if managed { try useHelperDouble() }
      core.standardOutput = FileHandle.nullDevice
      core.standardError = FileHandle.nullDevice
      try core.run()
      try await waitForAgent()
      try releaseStream()
      model.executableURL = URL(fileURLWithPath: "/usr/bin/env")
      model.arguments = ["python3", SyncTests.root.appendingPathComponent("tests/fixtures/model_fixture.py").path,
                         "--port", "0", "--stream-release", streamRelease.path]
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
      try finishFixtureProcess(oldPID, signal: SIGKILL)
      if oldPID == core.processIdentifier { core.waitUntilExit() }
      // Leave stale readiness in place: the observer may already have replaced
      // it with the winner's file. Validate the new PID instead of unlinking it.
      core = Process()
      core.executableURL = binary
      core.arguments = arguments
      core.standardOutput = FileHandle.nullDevice
      core.standardError = FileHandle.nullDevice
      try core.run()
      try await waitForAgent(replacing: oldPID)
    }

    private func waitForAgent(replacing oldPID: Int32? = nil) async throws {
      let readyFile = directory.appendingPathComponent("state/host.ready")
      // An observing client can win the startup lock. Status 4 means wait for
      // that service, not that the replacement service failed to start.
      let replacement = core
      func read() -> [String: Any]? {
        guard let bytes = try? Data(contentsOf: readyFile),
              let value = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any],
              let pid = value["pid"] as? Int32, pid != oldPID, kill(pid, 0) == 0,
              value["unix_socket"] is String, value["host_socket"] is String,
              value["user_token"] is String else { return nil }
        return value
      }
      var ready = try fixtureReadiness(readyFile, pid: replacement.processIdentifier, read: read)
      if ready == nil {
        replacement.waitUntilExit()
        if replacement.terminationStatus == 4 {
          ready = try fixtureReadiness(readyFile, pid: nil, read: read)
        }
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

    func send(_ text: String = "hello") async throws {
      _ = try await CoreHostClient.shared.call("ConversationSendRequest", ["conversation_id": conversationID,
        "turn_id": UUID().uuidString, "text": text])
    }

    func holdStream() throws { try FileManager.default.removeItem(at: streamRelease) }
    func releaseStream() throws { try Data().write(to: streamRelease) }

    func snapshot() async throws -> Tsugumi_Agent_ConversationResultT {
      try await CoreHostClient.shared.agentCall("ConversationGetRequest", ["conversation_id": conversationID])
    }

    func stop() async {
      try? releaseStream()
      for chat in chats { await chat.detach().value }
      for session in sessions { await session.close() }
      for session in sessions where session.endpointOverride == nil { try? await session.environments.stop(session.identity) }
      if let pid = servicePID, pid != core.processIdentifier {
        // The lock winner belongs to this fixture even when the client launched it.
        try? finishFixtureProcess(pid, signal: SIGTERM)
      }
      for process in [core, model] where process.isRunning {
        try? finishFixtureProcess(process.processIdentifier, signal: SIGTERM)
        process.waitUntilExit()
      }
      daemon?.stop()
      // The helper double outlives the host service, as the VM helper does.
      if let text = try? String(contentsOf: helperPID, encoding: .utf8), let pid = Int32(text), pid > 1 {
        try? finishFixtureProcess(pid, signal: SIGTERM)
      }
      await environment.restore()
      if let project { try? FileManager.default.removeItem(at: project) }
      try? FileManager.default.removeItem(at: directory)
    }
  }

  func withFixture(managed: Bool = false,
                   _ body: (Fixture) async throws -> Void) async throws {
    let fixture = Fixture()
    do {
      try await fixture.start(managed: managed)
      try await body(fixture)
      await fixture.stop()
    } catch {
      await fixture.stop()
      throw error
    }
  }

  func until(_ what: String, _ ok: @escaping @MainActor () -> Bool) async throws {
    try await waitForModel(what, ok)
  }

  func reply(_ chat: ChatController, after n: Int) -> Bool {
    chat.items.dropFirst(n).contains { $0.role == "assistant" }
  }

  @Test func hostPreferencesReloadAfterReconnectIncludingAFreshStore() async throws {
    let shared = CoreHostClient.shared
    let preferences = CorePreferences.shared
    func readRevision(_ client: CoreHostClient) async throws -> String {
      let before = try await client.call("SettingsGetRequest", ["profile": "desktop"])
      return try #require(before["revision"] as? String)
    }
    func setSearch(_ enabled: Bool, on client: CoreHostClient, expected: String? = nil) async throws {
      let revision: String
      if let expected { revision = expected } else { revision = try await readRevision(client) }
      _ = try await client.call("SettingsUpdateRequest", [
        "profile": "desktop", "revision": revision,
        "changes": [["key": "agent.webSearch", "value_json": enabled ? "true" : "false"]],
      ])
    }
    try await withFixture { _ in
      // Web search is on by default, so each edit turns it off first.
      try await setSearch(false, on: shared)
      try await until("preferences display the edited setting") { !preferences.bool(forKey: "agent.webSearch") }
      await shared.disconnect()
      let editor = CoreHostClient()
      try await setSearch(true, on: editor)
      await editor.disconnect()
      _ = try await shared.call("SettingsGetRequest", ["profile": "desktop"])
      try await until("preferences display the edit made while disconnected") { preferences.bool(forKey: "agent.webSearch") }
      try await setSearch(false, on: shared)
      try await until("preferences display the previous host's final revision") { !preferences.bool(forKey: "agent.webSearch") }
      let stale = try await readRevision(shared)
      try await withFixture { _ in
        try await until("preferences display the fresh host's lower revision") { preferences.bool(forKey: "agent.webSearch") }
        for enabled in [true, false, true] { try await setSearch(enabled, on: shared) }
        do {
          try await setSearch(false, on: shared, expected: stale)
          Issue.record("Accepted an edit from the previous store")
        } catch let failure as CoreHostFailure {
          #expect(failure.code == "conflict")
        }
        let current = try await shared.call("SettingsGetRequest", ["profile": "desktop"])
        let values = try #require(current["values"] as? [[String: String]])
        #expect(values.first { $0["key"] == "agent.webSearch" }?["value_json"] == "true")
      }
    }
  }

  @Test func openingAndRefreshingAnInterruptedConversationOnlyObserve() async throws {
    try await withFixture { t in
      try t.holdStream()
      try await t.send("hello interrupted")
      let observing = t.makeChat()
      // Send acknowledges a prepared turn before the provider is reached.
      // Seeing streamed text establishes that the model is actually waiting
      // at the fixture's release gate and that this UI has subscribed.
      try await until("the held model is streaming") { observing.failed || (observing.busy && reply(observing, after: 1)) }
      try #require(observing.busy, "\(observing.status?.message ?? "")")
      try await t.restartAgent()
      try await until("the running observer sees the interruption") { observing.interrupted || observing.failed }
      try #require(observing.interrupted, "\(observing.status?.message ?? "")")
      let before = try await t.snapshot()
      try #require(before.resumable)
      #expect(before.interruptedOperation?.kind == "model")

      let reopened = t.makeChat()
      try await until("reopening shows the interruption") { (reopened.ready && reopened.interrupted) || reopened.failed }
      try #require(reopened.interrupted, "\(reopened.status?.message ?? "")")
      let agent = Agent(id: t.conversationID, transport: RPCToolTransport(session: t.session), provider: .fixture)
      try await agent.open()
      try await agent.refresh()
      let after = try await t.snapshot()
      #expect(!after.busy && after.resumable && after.eventSeq == before.eventSeq)
      #expect(!reopened.busy && !reopened.canContinue)
      #expect(reopened.interruptedOperation?.kind == "model")

      t.session.start()
      try await SyncTests.connected(t.session)
      try t.releaseStream()
      reopened.resume()
      try await until("explicit resume finished") { reopened.failed || (!reopened.busy && !reopened.interrupted && reply(reopened, after: 1)) }
      #expect(!reopened.failed)
    }
  }

  @Test func aMessageCarriesEachAttachedFolderOnce() async throws {
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
      try await until("the chat is ready") { chat.failed || (chat.ready && chat.currentId == t.conversationID) }
      try #require(!chat.failed, "\(chat.status?.message ?? "")")
      // Invalid batches do not appear attached, and can be retried.
      await chat.attach(folders: [first, t.directory.appendingPathComponent("missing")])
      #expect(chat.folders.isEmpty && chat.status != nil && !chat.attaching)
      // While hostd restarts the environment for the share, the window keeps
      // the project and the chat: no loading card, no problem page.
      let session = t.session
      let seen = SeenDuringRestart()
      let watching = Task { @MainActor in
        while !Task.isCancelled {
          if session.restartingEnvironment {
            seen.restart = true
            if EnvironmentOverlay.loading(session) != nil || EnvironmentOverlay.hasProblem(session) { seen.cover = true }
          }
          await Task.yield()
        }
      }
      await chat.attach(folders: [first, second, first])
      watching.cancel()
      _ = await watching.value
      #expect(seen.restart && !seen.cover && !session.restartingEnvironment)
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
      // Attaching is the person's decision: the message needs nothing more.
      chat.send("hello from both attached folders")
      try await until("the message with both attachments ended") { chat.failed || (!chat.busy && reply(chat, after: 0)) }
      try #require(!chat.failed, "\(chat.status?.message ?? "")")
      let snapshot = try await t.snapshot()
      let conversation = snapshot.conversation
      let messages = conversation.messages.compactMap { $0 }
      let user = try #require(messages.first { $0.role == "user" })
      let content = try #require(JSONSerialization.jsonObject(with: Data((user.contentJson ?? "[]").utf8)) as? [[String: Any]])
      let text = content.compactMap { $0["text"] as? String }.joined()
      for path in paths { #expect(text.contains(path)) }
      let sentResources = content.flatMap { $0["resource_ids"] as? [String] ?? [] }
      #expect(Set(sentResources) == Set(resources) && sentResources.count == resources.count)
    }
  }

  @Test func stoppingBeforeConnectingStillAllowsAnotherMessageAndReopening() async throws {
    try await withFixture { t in
      try t.holdStream()
      try await t.send()
      let chat = t.makeChat()
      try await until("the model is streaming before project connection") { chat.failed || (chat.busy && reply(chat, after: 1)) }
      try #require(!chat.failed, "\(chat.status?.message ?? "")")
      #expect(!t.session.connected)
      chat.cancel()
      try await until("cancellation allows continuation") { chat.failed || (!chat.busy && chat.canContinue) }
      try #require(!chat.failed, "\(chat.status?.message ?? "")")
      t.session.start()
      try await SyncTests.connected(t.session)

      chat.send("hello again")
      try await until("the next message is streaming") {
        chat.failed || chat.busy && chat.items.last?.role == "assistant"
          && chat.items.contains { $0.role == "user" && $0.text == "hello again" }
      }
      try #require(!chat.failed, "\(chat.status?.message ?? "")")
      await chat.detach().value
      let reopened = t.makeChat()
      try await until("the reopened chat is observing the held turn") { reopened.busy || reopened.failed }
      try #require(!reopened.failed, "\(reopened.status?.message ?? "")")
      try t.releaseStream()
      try await until("the detached turn is complete") {
        reopened.failed || reopened.ready && !reopened.busy && reopened.items.last?.role == "assistant"
      }
      let complete = try await t.snapshot()
      #expect(!complete.busy && !reopened.failed && reopened.status == nil)
      #expect(complete.conversation.messages.compactMap { $0 }.last?.role == "assistant")
    }
  }
}

@MainActor private final class SeenDuringRestart {
  var restart = false
  var cover = false
}
