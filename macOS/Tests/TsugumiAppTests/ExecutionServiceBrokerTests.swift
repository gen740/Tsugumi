import Darwin
import Foundation
import Testing
@testable import TsugumiLauncher

private let daemonToken = String(repeating: "a", count: 64)
private let testServerId = String(repeating: "d", count: 64)
private let caps = ExecutionServiceCapabilities(serverId: testServerId, nativeSystem: "x86_64-linux", systems: ["x86_64-linux"],
                                                isolation: "pid_namespace", resourceLimits: true, maxConcurrentJobs: 4,
                                                workerContract: ExecutionServiceRegistry.workerContract)

private func framed(_ header: [String: Any]) throws -> Data {
  let json = try JSONSerialization.data(withJSONObject: header)
  var length = UInt32(json.count).bigEndian
  return withUnsafeBytes(of: &length) { Data($0) } + json
}

@Test func executionBrokerRequiresSafeEndpointsAndVerifiedCapabilities() throws {
  for endpoint in ["http://localhost:1234", "http://192.0.2.1:1234", "https://user:password@example.org", "https://example.org/path", "https://local%0d%0a.test"] {
    #expect(throws: ExecutionServiceRegistry.Invalid.self) { try ExecutionServiceRegistry.endpointURL(endpoint) }
  }
  #expect(try ExecutionServiceRegistry.endpointURL("https://EXAMPLE.org:443/").absoluteString == "https://example.org")
  #expect(try ExecutionServiceRegistry.endpointURL("http://[::1]:8781").absoluteString == "http://[::1]:8781")
  var reply: [String: Any] = ["ok": true, "native_system": caps.nativeSystem, "systems": caps.systems,
                              "worker_contract": caps.workerContract, "server_id": caps.serverId,
                              "isolation": caps.isolation, "resource_limits": caps.resourceLimits, "max_jobs": caps.maxConcurrentJobs]
  for executor: String? in [nil, "direct"] {
    reply["executor"] = executor
    #expect(throws: (any Error).self) { try ExecutionServiceBroker.checkedProbe(reply) }
  }
  reply["executor"] = "daemon"
  #expect(try ExecutionServiceBroker.checkedProbe(reply) == caps)
  for bad: Any? in [nil, 99, true, "1"] {
    reply["worker_contract"] = bad
    do {
      _ = try ExecutionServiceBroker.checkedProbe(reply)
      Issue.record("Expected a worker contract error")
    } catch {
      #expect(error.localizedDescription.contains("Rebuild the execution daemon"))
    }
  }
  reply["worker_contract"] = caps.workerContract
  reply["server_id"] = "bad\r\nheader"
  #expect(throws: (any Error).self) { try ExecutionServiceBroker.checkedProbe(reply) }
}

@Test func executionBrokerUsesOnlyAuthenticatedProjectScopeAndServerPin() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-broker-\(UUID().uuidString)")
  defer { try? FileManager.default.removeItem(at: root) }
  let project = root.appendingPathComponent("project")
  let control = root.appendingPathComponent("control")
  try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
  try FileManager.default.createDirectory(at: control, withIntermediateDirectories: true)
  try Data("{}".utf8).write(to: project.appendingPathComponent("tsugumi.json"))
  let entry = StoredExecutionService(id: "worker", name: "Worker", endpoint: "http://127.0.0.1:8781",
                                     token: daemonToken, capabilities: caps)
  try ExecutionServiceRegistry.saveStored([entry], support: root)
  let registry = root.appendingPathComponent("execution-services.json")
  let visible = try ExecutionServiceRegistry.load(support: root)
  #expect(visible == [entry.connection])
  #expect(!String(decoding: try JSONEncoder().encode(visible), as: UTF8.self).contains(daemonToken))
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

  func send(_ token: String, using broker: ExecutionServiceBroker, pin: String? = testServerId) throws -> (scope: String?, error: Bool, code: String?) {
    var sockets: [Int32] = [-1, -1]
    try #require(socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0)
    defer { Darwin.close(sockets[0]); Darwin.close(sockets[1]) }
    var request: [String: Any] = [
      "version": 1, "operation": "status", "service": "worker", "project_scope": forged, "broker_token": token,
    ]
    request["server_id"] = pin
    try writeAll(sockets[0], framed(request))
    var scope: String?
    broker.forward(sockets[1]) { destination, forwarded, _ in
      #expect(destination == entry)
      #expect(forwarded["broker_token"] == nil)
      #expect(forwarded["server_id"] as? String == testServerId)
      scope = forwarded["project_scope"] as? String
    }
    Darwin.shutdown(sockets[1], SHUT_WR)
    let response = try FileHandle(fileDescriptor: sockets[0]).readToEnd() ?? Data()
    if response.isEmpty { return (scope, false, nil) }
    let header = try JSONSerialization.jsonObject(with: response.dropFirst(4)) as? [String: Any]
    return (scope, header?["ok"] as? Bool == false, header?["code"] as? String)
  }

  let invalid = try send("invalid", using: broker)
  #expect(invalid.error)
  #expect(invalid.scope == nil)
  for pin: String? in [nil, "old-daemon"] {
    let replaced = try send(token, using: broker, pin: pin)
    #expect(replaced.scope == nil)
    #expect(replaced.code == "registration_changed")
  }
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

@Test func executionBrokerProbesOverHTTPAndWaitsBeforeSendingJobBytes() async throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-broker-http-\(UUID().uuidString)")
  defer { try? FileManager.default.removeItem(at: root) }
  let control = root.appendingPathComponent("control")
  try FileManager.default.createDirectory(at: control, withIntermediateDirectories: true)
  try Data("{}".utf8).write(to: root.appendingPathComponent("tsugumi.json"))
  let listener = socket(AF_INET, SOCK_STREAM, 0)
  try #require(listener >= 0)
  defer { Darwin.close(listener) }
  var address = sockaddr_in()
  address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
  address.sin_family = sa_family_t(AF_INET)
  address.sin_addr = in_addr(s_addr: inet_addr("127.0.0.1"))
  var length = socklen_t(MemoryLayout<sockaddr_in>.size)
  let bound = withUnsafePointer(to: &address) {
    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(listener, $0, length) }
  }
  try #require(bound == 0 && listen(listener, 2) == 0)
  let inspected = withUnsafeMutablePointer(to: &address) {
    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(listener, $0, &length) }
  }
  try #require(inspected == 0)
  let endpoint = "http://127.0.0.1:\(UInt16(bigEndian: address.sin_port))"

  let server = Task.detached {
    for request in 0..<3 {
      let fd = accept(listener, nil, nil)
      try #require(fd >= 0)
      defer { Darwin.close(fd) }
      var timeout = timeval(tv_sec: 5, tv_usec: 0)
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
      var requestBytes = Data()
      while !requestBytes.suffix(4).elementsEqual([13, 10, 13, 10]) && requestBytes.count < 16384 {
        var byte: UInt8 = 0
        try #require(Darwin.read(fd, &byte, 1) == 1)
        requestBytes.append(byte)
      }
      let headers = String(decoding: requestBytes, as: UTF8.self)
      #expect(headers.hasPrefix("POST /rpc HTTP/1.1\r\n"))
      #expect(headers.contains("Authorization: Bearer \(daemonToken)\r\n"))
      if request == 0 || request == 2 {
        if request == 0 {
          #expect(!headers.contains("X-Tsugumi-Server"))
        } else {
          #expect(headers.contains("X-Tsugumi-Server: \(testServerId)\r\n"))
          #expect(headers.contains("Expect: 100-continue\r\n"))
          try writeAll(fd, Data("HTTP/1.1 100 Continue\r\n\r\n".utf8))
        }
        let field = try #require(headers.components(separatedBy: "\r\n").first { $0.hasPrefix("Content-Length: ") })
        let count = try #require(Int(field.dropFirst("Content-Length: ".count)))
        let handle = FileHandle(fileDescriptor: fd)
        var requestBody = Data()
        while requestBody.count < count {
          let chunk = try #require(try handle.read(upToCount: count - requestBody.count))
          try #require(!chunk.isEmpty)
          requestBody.append(chunk)
        }
        let frameLength = requestBody.prefix(4).reduce(0) { ($0 << 8) | Int($1) }
        let bodyOffset = 4 + frameLength
        try #require(bodyOffset <= requestBody.count)
        let decoded = try JSONSerialization.jsonObject(with: requestBody[4..<bodyOffset]) as? [String: Any]
        let body: Data
        if request == 0 {
          #expect(decoded?["operation"] as? String == "probe")
          #expect(decoded?["project_scope"] == nil)
          body = try framed(["version": 1, "ok": true, "executor": "daemon", "server_id": testServerId,
                             "native_system": caps.nativeSystem, "systems": caps.systems, "isolation": caps.isolation,
                             "resource_limits": caps.resourceLimits, "max_jobs": caps.maxConcurrentJobs, "worker_contract": caps.workerContract])
        } else {
          #expect(decoded?["operation"] as? String == "submit")
          #expect(decoded?["server_id"] as? String == testServerId)
          #expect(decoded?["broker_token"] == nil)
          #expect((decoded?["project_scope"] as? String)?.count == 64)
          let payload = Data(requestBody.dropFirst(bodyOffset))
          #expect(payload == Data("job bytes".utf8))
          body = try framed(["version": 1, "ok": true, "size": payload.count]) + payload
        }
        let response = "HTTP/1.1 200 OK\r\nContent-Length: \(body.count)\r\nConnection: close\r\n\r\n"
        try writeAll(fd, Data(response.utf8) + body)
      } else {
        #expect(headers.contains("X-Tsugumi-Server: \(testServerId)\r\n"))
        #expect(headers.contains("Expect: 100-continue\r\n"))
        var quiet = timeval(tv_sec: 0, tv_usec: 150_000)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &quiet, socklen_t(MemoryLayout<timeval>.size))
        var byte: UInt8 = 0
        #expect(Darwin.read(fd, &byte, 1) < 0 && errno == EAGAIN, "A silent endpoint must not receive frame bytes")
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        try writeAll(fd, Data("HTTP/1.1 409 Conflict\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".utf8))
        let read = Darwin.read(fd, &byte, 1)
        #expect(read == 0 || (read < 0 && errno == ECONNRESET), "The broker sent no frame bytes to a rejected endpoint")
      }
    }
  }

  var input = ExecutionServiceRegistration()
  input.id = "worker"
  input.name = "Worker"
  input.endpoint = endpoint
  input.token = daemonToken
  let connections = try await ExecutionServiceRegistry.register(input, support: root)
  #expect(connections.first?.capabilities == caps)
  input.endpoint = "http://127.0.0.1:1"
  input.token = nil
  await #expect(throws: ExecutionServiceRegistry.Invalid.self) { try await ExecutionServiceRegistry.register(input, support: root) }

  let broker = ExecutionServiceBroker(support: root)
  try broker.authorize(ProjectIdentity(url: root), control: control)
  let token = try String(contentsOf: control.appendingPathComponent("execution-broker-token"), encoding: .utf8)
  let body = Data("job bytes".utf8)
  func send() throws -> Data {
    var sockets: [Int32] = [-1, -1]
    try #require(socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets) == 0)
    defer { Darwin.close(sockets[0]); Darwin.close(sockets[1]) }
    try writeAll(sockets[0], framed(["version": 1, "operation": "submit", "size": body.count,
                                     "service": "worker", "broker_token": token, "server_id": testServerId]) + body)
    broker.forward(sockets[1])
    Darwin.shutdown(sockets[1], SHUT_WR)
    return try FileHandle(fileDescriptor: sockets[0]).readToEnd() ?? Data()
  }
  let refused = try send()
  let refusal = try JSONSerialization.jsonObject(with: refused.dropFirst(4)) as? [String: Any]
  #expect(refusal?["code"] as? String == "registration_changed")
  let received = try send()
  let replyLength = received.prefix(4).reduce(0) { ($0 << 8) | Int($1) }
  #expect(received.dropFirst(4 + replyLength) == body)
  try await server.value
}
