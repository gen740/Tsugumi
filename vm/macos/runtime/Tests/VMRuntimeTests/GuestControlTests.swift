import Darwin
import FlatBuffers
import Foundation
import TsugumiRPC
import XCTest
@testable import TsugumiVMRuntime

final class GuestControlTests: XCTestCase {
  private func sockets() throws -> (GuestControl, Int32) {
    var pair: [Int32] = [-1, -1]
    guard socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0 else {
      throw POSIXError(.EIO)
    }
    return (GuestControl(fd: pair[0]), pair[1])
  }

  func testDeliversAnEventAndMatchesConcurrentRepliesByID() async throws {
    let (channel, peer) = try sockets()
    defer { shutdown(peer, SHUT_RDWR); Darwin.close(peer) }
    let ready = expectation(description: "project readiness event")
    channel.start(event: { message in
      guard let status = message.payload?.value as? Tsugumi_VMGuest_ProjectStatusT else {
        XCTFail("expected a project event")
        return
      }
      XCTAssertEqual(status.key, "abcd")
      XCTAssertEqual(status.phase, .ready)
      ready.fulfill()
    }, closed: { _ in })
    let clock = Tsugumi_VMGuest_ClockT()
    clock.unixMs = 5
    let maintenance = Tsugumi_VMGuest_MaintenanceT()
    maintenance.action = .verifystore
    async let clockReply = channel.request(clock, type: .clock)
    async let storeReply = channel.request(maintenance, type: .maintenance)
    let first = try XCTUnwrap(GuestControl.read(peer))
    let second = try XCTUnwrap(GuestControl.read(peer))
    XCTAssertNotEqual(first.requestId, second.requestId)
    let status = Tsugumi_VMGuest_ProjectStatusT()
    status.key = "abcd"
    status.phase = .ready
    try emit(status, type: .projectstatus, to: peer)
    for request in [second, first] {
      let value = try XCTUnwrap(request.payload?.value as? Tsugumi_VMGuest_RequestT)
      let reply = Tsugumi_VMGuest_ReplyT()
      reply.message = value.command?.value is Tsugumi_VMGuest_ClockT ? "clock" : "store"
      try emit(reply, type: .reply, id: request.requestId, to: peer)
    }
    let (clockValue, storeValue) = try await (clockReply, storeReply)
    XCTAssertEqual(clockValue.message, "clock")
    XCTAssertEqual(storeValue.message, "store")
    await fulfillment(of: [ready], timeout: 5)
  }

  func testDisconnectFailsAPendingRequest() async throws {
    let (channel, peer) = try sockets()
    defer { Darwin.close(peer) }
    channel.start(event: { _ in XCTFail("unexpected event") }, closed: { _ in })
    async let pending = channel.request(Tsugumi_VMGuest_ClockT(), type: .clock)
    _ = try XCTUnwrap(GuestControl.read(peer))
    shutdown(peer, SHUT_RDWR)
    do {
      _ = try await pending
      XCTFail("expected a transport failure")
    } catch is LauncherError {
    }
  }

  func testReadsMemoryOnTheControlStream() async throws {
    let (channel, peer) = try sockets()
    defer { shutdown(peer, SHUT_RDWR); Darwin.close(peer) }
    channel.start(event: { _ in XCTFail("unexpected event") }, closed: { _ in })
    async let pending = channel.memorySnapshot()
    let request = try XCTUnwrap(GuestControl.read(peer))
    let payload = try XCTUnwrap(request.payload?.value as? Tsugumi_VMGuest_RequestT)
    XCTAssertTrue(payload.command?.value is Tsugumi_VMGuest_ReadMemoryT)
    let memory = Tsugumi_VMGuest_MemoryT()
    memory.totalKib = 4 << 20
    memory.availableKib = 3 << 20
    memory.cachedKib = 1 << 20
    let reply = Tsugumi_VMGuest_ReplyT()
    reply.memory = memory
    try emit(reply, type: .reply, id: request.requestId, to: peer)
    let snapshot = try await pending
    XCTAssertEqual(snapshot.totalKib, memory.totalKib)
    XCTAssertEqual(snapshot.availableKib, memory.availableKib)
    XCTAssertEqual(snapshot.cachedKib, memory.cachedKib)
  }

  private func emit(_ value: NativeObject, type: Tsugumi_VMGuest_Payload, id: UInt64 = 0, to peer: Int32) throws {
    var message: Tsugumi_VMGuest_MessageT? = Tsugumi_VMGuest_MessageT()
    message?.requestId = id
    message?.payload = Tsugumi_VMGuest_PayloadUnion(value, type: type)
    var builder = FlatBufferBuilder(initialSize: 256)
    let offset = Tsugumi_VMGuest_Message.pack(&builder, obj: &message)
    Tsugumi_VMGuest_Message.finish(&builder, end: offset)
    try VMHostIPC.write(builder.data, to: peer)
  }
}
