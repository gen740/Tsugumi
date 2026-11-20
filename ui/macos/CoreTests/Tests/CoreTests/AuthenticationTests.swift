import Darwin
import Foundation
import Testing
import TsugumiAgentClient
import TsugumiRPC
import TsugumiTestSupport

/// Native browser opening and polling use the Host contract. OAuth validation,
/// credential storage and rotation are exercised in tests/core.
@Suite(.serialized) struct AuthenticationTests {
  @Test func registrationValidatesTheCallbackAndKeepsTheSelectedAccount() async throws {
    let fixture = try AuthenticationFixture()
    defer { fixture.stop() }
    let issuer = "http://127.0.0.1:\(fixture.port)"
    let store = "Tsugumi test registration \(UUID().uuidString)"
    let auth = ChatGPTAuth.shared(
      endpoints: ChatGPTEndpoints(issuer: issuer, resource: "https://api.openai.com/v1"), store: store)
    let browser = URLSession(configuration: .ephemeral)
    let signedIn = try await auth.signIn { url in _ = try await browser.data(from: url) }
    #expect(signedIn.welcome && signedIn.account.planEnabled)
    #expect(signedIn.account.email == "fixture@example.com")
    #expect(signedIn.account.clientId == "oaiapp_fixture_1")
    try await auth.markWelcomed(clientId: signedIn.account.clientId)
    #expect(await auth.accountsWithoutAsking()?.map(\.signedIn) == [true])

    func authorizations() async throws -> [[String: Any]] {
      let data = try await browser.data(from: URL(string: "\(issuer)/fixture/chatgpt")!).0
      let stats = try #require(try JSONSerialization.jsonObject(with: data) as? [String: Any])
      return try #require(stats["authorize"] as? [[String: Any]])
    }
    let first = try #require(try await authorizations().first)
    #expect(first["client_id"] as? String == "dynamic_agent_client")
    #expect(first["agent_name_hint"] as? String == "Tsugumi")
    #expect((first["ext_agent_host_id"] as? String)?.hasPrefix("urn:uuid:") == true)
    let again = try await auth.signIn(clientId: signedIn.account.clientId) { url in
      _ = try await browser.data(from: url)
    }
    #expect(!again.welcome)
    let latest = try #require(try await authorizations().last)
    #expect(latest["client_id"] as? String == signedIn.account.clientId)
    #expect(latest["agent_name_hint"] == nil)
    #expect(latest["ext_agent_host_id"] as? String == first["ext_agent_host_id"] as? String)
    #expect(latest["id_token_hint"] as? Bool == false)
    #expect(latest["login_hint"] as? String == signedIn.account.email)
    #expect(try await auth.signOut(clientId: signedIn.account.clientId))
    #expect(await auth.accounts().map(\.signedIn) == [false])
  }

  @Test func providerInputReturnsOnlyStatus() async throws {
    let fixture = try AuthenticationFixture()
    defer { fixture.stop() }
    let client = CoreHostClient()
    let p = Tsugumi_Host_ProviderInputT()
    p.apiKey = "NATIVE_SECRET_SENTINEL"
    let reply = try await client.call("ProviderSetRequest", ["profile": "native-test", "id": "anthropic",
      "provider": p])
    #expect(reply["configured"] as? Bool == true)
    #expect(reply["source"] as? String == "hostd")
    #expect(!String(describing: reply).contains(p.apiKey!))
    let removed = try await client.call("ProviderSetRequest", ["profile": "native-test", "id": "anthropic",
      "remove": true])
    #expect(removed["configured"] as? Bool == false)
  }
}

private final class AuthenticationFixture {
  let directory = URL(fileURLWithPath: ProcessInfo.processInfo.environment["TSUGUMI_SOCKET_DIR"] ?? "/tmp").appendingPathComponent("auth-\(UUID().uuidString.prefix(8))")
  private(set) var port = 0
  private let provider = Process()
  private let daemon = Process()
  private let previousState: String?

  init() throws {
    previousState = ProcessInfo.processInfo.environment["TSUGUMI_HOST_STATE"]
    let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
      .appendingPathComponent("../../../../..").standardizedFileURL
    let build = ProcessInfo.processInfo.environment["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) }
      ?? root.appendingPathComponent("build")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let state = directory.appendingPathComponent("state")
    setenv("TSUGUMI_HOST_STATE", state.path, 1)
    daemon.executableURL = ProcessInfo.processInfo.environment["TSUGUMI_HOSTD"].map { URL(fileURLWithPath: $0) }
      ?? build.appendingPathComponent("hostd/tsugumi-hostd")
    // Run scratch refuses a path through a symbolic link, and
    // `resolvingSymlinksInPath` removes `/private`, which puts `/tmp` back.
    let canonical = realpath(directory.path, nil).map { pointer -> String in
      defer { free(pointer) }
      return String(cString: pointer)
    } ?? directory.path
    daemon.arguments = ["--state-dir", state.path,
                        "--socket-dir", directory.appendingPathComponent("b").path,
                        "--usage-scratch-dir", URL(fileURLWithPath: canonical).appendingPathComponent("scratch").path,
                        "--unix", directory.appendingPathComponent("agent.sock").path,
                        "--listen", "127.0.0.1:0", "--demos-dir", root.appendingPathComponent("demos").path]
    daemon.standardOutput = FileHandle.nullDevice
    daemon.standardError = FileHandle.nullDevice
    provider.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    provider.arguments = ["python3", root.appendingPathComponent("tests/fixtures/model_fixture.py").path, "--port", "0"]
    let output = Pipe()
    provider.standardOutput = output
    provider.standardError = FileHandle.nullDevice
    do {
      try daemon.run()
      try provider.run()
      let line = String(decoding: output.fileHandleForReading.availableData, as: UTF8.self)
      port = try #require(line.split(separator: ":").last.flatMap {
        Int($0.trimmingCharacters(in: .whitespacesAndNewlines))
      })
      try #require(try fixtureReadiness(state.appendingPathComponent("host.ready"), pid: daemon.processIdentifier) {
        FileManager.default.fileExists(atPath: state.appendingPathComponent("host.ready").path) ? true : nil
      } == true)
    } catch { stop(); throw error }
  }

  func stop() {
    for process in [provider, daemon] where process.isRunning {
      try? finishFixtureProcess(process.processIdentifier, signal: SIGTERM)
      process.waitUntilExit()
    }
    if let previousState { setenv("TSUGUMI_HOST_STATE", previousState, 1) }
    else { unsetenv("TSUGUMI_HOST_STATE") }
    try? FileManager.default.removeItem(at: directory)
  }

}
