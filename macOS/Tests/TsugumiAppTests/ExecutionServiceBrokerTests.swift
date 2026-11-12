import Darwin
import Foundation
import Testing
@testable import TsugumiLauncher

private func worker() -> ExecutionServiceConnection {
  var entry = ExecutionServiceConnection()
  entry.id = "worker"
  entry.name = "Worker"
  entry.sshHost = "trusted"
  entry.gateway = "/tmp/gateway/bin/tsugumi-remote-gateway"
  return entry
}

@Test func executionBrokerSSHArgumentsDisableAgentAndOtherForwarding() {
  let args = ExecutionServiceBroker.sshArguments(worker())
  #expect(args.contains("ForwardAgent=no"))
  #expect(args.contains("ClearAllForwardings=yes"))
  #expect(args.contains("StrictHostKeyChecking=yes"))
  #expect(args.contains("BatchMode=yes"))
  #expect(Array(args.suffix(3)) == ["--", "trusted", "'/tmp/gateway/bin/tsugumi-remote-gateway'"])
  var invalid = worker()
  invalid.gateway = "/tmp/gateway'; touch bad"
  #expect(throws: ExecutionServiceRegistry.Invalid.self) { try ExecutionServiceRegistry.validate([invalid]) }
}

@Test func executionBrokerProbeRequiresMatchingWorkerContract() throws {
  let entry = worker()
  var reply: [String: Any] = ["ok": true, "native_system": "x86_64-linux", "systems": ["x86_64-linux"], "worker_contract": 1]
  let verified = try ExecutionServiceBroker.checkedProbe(entry, reply: reply)
  #expect(verified.nativeSystem == "x86_64-linux")
  for bad: Any? in [nil, 99, true, "1"] {
    reply["worker_contract"] = bad
    do {
      _ = try ExecutionServiceBroker.checkedProbe(entry, reply: reply)
      Issue.record("Expected a worker contract error")
    } catch {
      #expect(error.localizedDescription.contains("Rebuild the gateway"))
    }
  }
}

@Test func executionBrokerUsesOnlyAuthenticatedProjectScope() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-broker-\(UUID().uuidString)")
  defer { try? FileManager.default.removeItem(at: root) }
  let project = root.appendingPathComponent("project")
  let control = root.appendingPathComponent("control")
  try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
  try FileManager.default.createDirectory(at: control, withIntermediateDirectories: true)
  try Data("{}".utf8).write(to: project.appendingPathComponent("tsugumi.json"))
  let entry = worker()
  try ExecutionServiceRegistry.save([entry], support: root)
  #expect(try ExecutionServiceRegistry.load(support: root) == [entry])
  let registry = root.appendingPathComponent("execution-services.json")
  #expect(try FileManager.default.attributesOfItem(atPath: registry.path)[.posixPermissions] as? Int == 0o600)

  let identity = try ProjectIdentity(url: project)
  #expect(identity.supportsExternalExecution)
  let broker = ExecutionServiceBroker(support: root)
  try broker.authorize(identity, control: control)
  let tokenPath = control.appendingPathComponent("execution-broker-token")
  let token = try String(contentsOf: tokenPath, encoding: .utf8)
  let forged = String(repeating: "0", count: 64)
  let key = root.appendingPathComponent(".execution-broker-key")
  #expect(try FileManager.default.attributesOfItem(atPath: key.path)[.posixPermissions] as? Int == 0o600)

  func send(_ token: String, using broker: ExecutionServiceBroker) throws -> (scope: String?, error: Bool) {
    var sockets: [Int32] = [-1, -1]
    try #require(socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0)
    defer { Darwin.close(sockets[0]); Darwin.close(sockets[1]) }
    let request: [String: Any] = [
      "version": 1, "operation": "status", "service": "worker",
      "project_scope": forged, "broker_token": token,
    ]
    let json = try JSONSerialization.data(withJSONObject: request)
    var length = UInt32(json.count).bigEndian
    try writeAll(sockets[0], withUnsafeBytes(of: &length) { Data($0) } + json)
    var scope: String?
    broker.forward(sockets[1]) { destination, forwarded, _ in
      #expect(destination == entry)
      #expect(forwarded["broker_token"] == nil)
      scope = forwarded["project_scope"] as? String
    }
    Darwin.shutdown(sockets[1], SHUT_WR)
    let response = try FileHandle(fileDescriptor: sockets[0]).readToEnd() ?? Data()
    if response.isEmpty { return (scope, false) }
    let header = try JSONSerialization.jsonObject(with: response.dropFirst(4)) as? [String: Any]
    return (scope, header?["ok"] as? Bool == false)
  }

  let invalid = try send("invalid", using: broker)
  #expect(invalid.error)
  #expect(invalid.scope == nil)
  let authorized = try send(token, using: broker)
  let scope = try #require(authorized.scope)
  #expect(!authorized.error)
  #expect(scope.count == 64)
  #expect(scope != forged)

  let renamed = root.appendingPathComponent("renamed-project")
  try FileManager.default.moveItem(at: project, to: renamed)
  let restarted = ExecutionServiceBroker(support: root)
  try restarted.authorize(ProjectIdentity(url: renamed), control: control)
  let newToken = try String(contentsOf: tokenPath, encoding: .utf8)
  #expect(newToken != token)
  #expect(try send(token, using: restarted).error)
  #expect(try send(newToken, using: restarted).scope == scope)
  restarted.revoke(identity.id)
  #expect(try send(newToken, using: restarted).error)
}
