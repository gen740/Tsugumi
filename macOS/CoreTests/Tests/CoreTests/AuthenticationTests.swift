import Darwin
import Foundation
import Testing
import TsugumiAgentClient
import TsugumiRPC

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
    let client = CoreAgentClient()
    let p = Tsugumi_Service_ProviderInputT()
    p.apiKey = "NATIVE_SECRET_SENTINEL"
    let reply = try await client.call("ProviderSetRequest", ["profile": "native-test", "id": "anthropic", "provider": p])
    let status = try #require(reply["value"] as? [String: Any])
    #expect(status["configured"] as? Bool == true)
    #expect(!String(describing: reply).contains(p.apiKey!))
    let removed = try await client.call("ProviderSetRequest", ["profile": "native-test", "id": "anthropic", "remove": true])
    #expect((removed["value"] as? [String: Any])?["configured"] as? Bool == false)
  }
}

private final class AuthenticationFixture {
  let directory = URL(fileURLWithPath: "/tmp/tsugumi-auth-\(UUID().uuidString.prefix(8))")
  let port: Int
  private let provider = Process()
  private let daemon = Process()
  private let previousState: String?

  init() throws {
    previousState = ProcessInfo.processInfo.environment["TSUGUMI_AGENT_STATE"]
    let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
      .appendingPathComponent("../../../..").standardizedFileURL
    let build = ProcessInfo.processInfo.environment["TSUGUMI_BUILD_DIR"].map { URL(fileURLWithPath: $0) }
      ?? root.appendingPathComponent("build")
    port = Self.freePort()
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let state = directory.appendingPathComponent("state")
    setenv("TSUGUMI_AGENT_STATE", state.path, 1)
    daemon.executableURL = ProcessInfo.processInfo.environment["TSUGUMI_AGENTD"].map { URL(fileURLWithPath: $0) }
      ?? build.appendingPathComponent("core/tsugumi-agentd")
    daemon.arguments = ["--state-dir", state.path, "--unix", directory.appendingPathComponent("agent.sock").path,
                        "--listen", "127.0.0.1:0", "--demos-dir", root.appendingPathComponent("demos").path]
    daemon.standardOutput = FileHandle.nullDevice
    daemon.standardError = FileHandle.nullDevice
    provider.executableURL = URL(fileURLWithPath: "/usr/bin/env")
    provider.arguments = ["python3", root.appendingPathComponent("tests/fixtures/model_fixture.py").path, "--port", "\(port)"]
    provider.standardOutput = FileHandle.nullDevice
    provider.standardError = FileHandle.nullDevice
    do {
      try daemon.run()
      try provider.run()
      let deadline = Date().addingTimeInterval(15)
      while Date() < deadline {
        if FileManager.default.fileExists(atPath: state.appendingPathComponent("agent.ready").path),
           Self.accepts(port), provider.isRunning, daemon.isRunning { return }
        usleep(20_000)
      }
      throw FixtureError.startup
    } catch { stop(); throw error }
  }

  func stop() {
    for process in [provider, daemon] where process.isRunning {
      process.terminate()
      let deadline = Date().addingTimeInterval(5)
      while process.isRunning && Date() < deadline { usleep(20_000) }
      if process.isRunning { kill(process.processIdentifier, SIGKILL) }
    }
    if let previousState { setenv("TSUGUMI_AGENT_STATE", previousState, 1) }
    else { unsetenv("TSUGUMI_AGENT_STATE") }
    try? FileManager.default.removeItem(at: directory)
  }

  private enum FixtureError: Error { case startup }

  private static func freePort() -> Int {
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    defer { close(fd) }
    var address = sockaddr_in()
    address.sin_family = sa_family_t(AF_INET)
    address.sin_addr.s_addr = inet_addr("127.0.0.1")
    var length = socklen_t(MemoryLayout<sockaddr_in>.size)
    _ = withUnsafeMutablePointer(to: &address) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, length) }
    }
    _ = withUnsafeMutablePointer(to: &address) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &length) }
    }
    return Int(UInt16(bigEndian: address.sin_port))
  }

  private static func accepts(_ port: Int) -> Bool {
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    defer { close(fd) }
    var address = sockaddr_in()
    address.sin_family = sa_family_t(AF_INET)
    address.sin_addr.s_addr = inet_addr("127.0.0.1")
    address.sin_port = UInt16(port).bigEndian
    return withUnsafePointer(to: &address) {
      $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
        connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
      }
    } == 0
  }
}
