import Darwin
import FlatBuffers
import Foundation
import Testing
import TsugumiRPC
@testable import TsugumiLauncher

private let daemonToken = String(repeating: "a", count: 64)
private let testServerId = String(repeating: "d", count: 64)
private let caps = ExecutionServiceCapabilities(serverId: testServerId, nativeSystem: "x86_64-linux", systems: ["x86_64-linux"],
                                                isolation: "pid_namespace", resourceLimits: true, maxConcurrentJobs: 4,
                                                workerContract: ExecutionServiceRegistry.workerContract)

private func capabilitiesMessage() -> Tsugumi_RemoteWorker_MessageT {
  let c = Tsugumi_RemoteWorker_CapabilitiesT()
  c.serverId = testServerId
  c.nativeSystem = "x86_64-linux"
  c.systems = ["x86_64-linux"]
  c.resourceLimits = true
  c.maxJobs = 4
  c.workerContract = UInt32(ExecutionServiceRegistry.workerContract)
  let reply = Tsugumi_RemoteWorker_ReplyT()
  reply.body = Tsugumi_RemoteWorker_ReplyBodyUnion(c, type: .capabilities)
  let message = Tsugumi_RemoteWorker_MessageT()
  message.payload = Tsugumi_RemoteWorker_PayloadUnion(reply, type: .reply)
  return message
}

@Test func executionBrokerRequiresSafeEndpointsAndVerifiedCapabilities() throws {
  for endpoint in ["http://127.0.0.1:1234", "tcp://localhost:1234", "tcp://192.0.2.1:1234", "tcp://127.0.0.1:0", "unix:relative"] {
    #expect(throws: ExecutionServiceRegistry.Invalid.self) { try ExecutionServiceRegistry.endpointURL(endpoint) }
  }
  #expect(try ExecutionServiceRegistry.endpointURL("tcp://[::1]:8781").absoluteString == "tcp://[::1]:8781")
  #expect(try ExecutionServiceRegistry.endpointURL("unix:/tmp/worker.sock").path == "/tmp/worker.sock")
  let message = capabilitiesMessage()
  #expect(try ExecutionServiceBroker.checkedProbe(message) == caps)
  let reply = try #require(message.payload?.value as? Tsugumi_RemoteWorker_ReplyT)
  let c = try #require(reply.body?.value as? Tsugumi_RemoteWorker_CapabilitiesT)
  c.workerContract = 99
  #expect(throws: (any Error).self) { try ExecutionServiceBroker.checkedProbe(message) }
  c.workerContract = UInt32(ExecutionServiceRegistry.workerContract)
  c.serverId = "invalid"
  #expect(throws: (any Error).self) { try ExecutionServiceBroker.checkedProbe(message) }
}

@Test func executionBrokerFramesRoundtripAndBoundsLengths() throws {
  let data = try ExecutionServiceBroker.Frame.encode(capabilitiesMessage())
  let length = data.prefix(4).enumerated().reduce(0) { $0 | (Int($1.element) << (8 * $1.offset)) }
  #expect(length == data.count - 4)
  var offset = 0
  let restored = try ExecutionServiceBroker.Frame.read { count in
    defer { offset += count }
    return data.subdata(in: offset..<(offset + count))
  }
  #expect(try ExecutionServiceBroker.checkedProbe(restored) == caps)
  #expect(throws: (any Error).self) {
    try ExecutionServiceBroker.Frame.read { _ in Data([255, 255, 255, 255]) }
  }
}

@Test func executionBrokerUsesOnlyAuthenticatedProjectScopeAndServerPin() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-broker-\(UUID().uuidString)")
  defer { try? FileManager.default.removeItem(at: root) }
  let project = root.appendingPathComponent("project")
  let control = root.appendingPathComponent("control")
  try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
  try FileManager.default.createDirectory(at: control, withIntermediateDirectories: true)
  try Data("{}".utf8).write(to: project.appendingPathComponent("tsugumi.json"))
  let entry = StoredExecutionService(id: "worker", name: "Worker", endpoint: "tcp://127.0.0.1:8781", token: daemonToken, capabilities: caps)
  try ExecutionServiceRegistry.saveStored([entry], support: root)
  #expect(try ExecutionServiceRegistry.load(support: root) == [entry.connection])
  #expect(!String(decoding: try JSONEncoder().encode(entry.connection), as: UTF8.self).contains(daemonToken))
  let identity = try ProjectIdentity(url: project)
  let broker = ExecutionServiceBroker(support: root)
  try broker.authorize(identity, control: control)
  let token = try String(contentsOf: control.appendingPathComponent("execution-broker-token"), encoding: .utf8)

  func send(_ auth: String, pin: String = testServerId) throws -> (String?, Tsugumi_RemoteWorker_ErrorCode?) {
    var sockets: [Int32] = [-1, -1]
    try #require(socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0)
    defer { Darwin.close(sockets[0]); Darwin.close(sockets[1]) }
    let q = Tsugumi_RemoteWorker_RequestT()
    q.token = auth; q.service = "worker"; q.serverId = pin
    q.projectScope = String(repeating: "0", count: 64)
    q.body = Tsugumi_RemoteWorker_RequestBodyUnion(Tsugumi_RemoteWorker_StatusRequestT(), type: .statusrequest)
    let m = Tsugumi_RemoteWorker_MessageT()
    m.payload = Tsugumi_RemoteWorker_PayloadUnion(q, type: .request)
    try ExecutionServiceBroker.Frame.write(m, to: sockets[0])
    var scope: String?
    broker.forward(sockets[1]) { destination, request, _ in
      #expect(destination == entry)
      scope = request.projectScope
      #expect(request.serverId == testServerId)
      #expect(request.body?.type == .statusrequest)
    }
    if let scope { return (scope, nil) }
    let response = try ExecutionServiceBroker.Frame.read(from: sockets[0])
    let reply = try #require(response.payload?.value as? Tsugumi_RemoteWorker_ReplyT)
    let error = try #require(reply.body?.value as? Tsugumi_RemoteWorker_FailureT)
    return (nil, error.code)
  }
  let (scope, error) = try send(token)
  #expect(scope?.count == 64 && scope != String(repeating: "0", count: 64) && error == nil)
  #expect(try send("forged").1 == .invalid)
  #expect(try send(token, pin: String(repeating: "e", count: 64)).1 == .registrationchanged)
  broker.revoke(identity.id)
  #expect(try send(token).1 == .invalid)
}
