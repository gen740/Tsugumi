import AppKit
import Foundation
import Testing
import TsugumiLauncher
@testable import TsugumiApp

@MainActor @Test func aTerminalClosedBeforeConnectingDisposesTheLateSocket() async throws {
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  try Data("{}".utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  var pair: [Int32] = [-1, -1]
  #expect(socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0)
  defer { Darwin.close(pair[1]) }
  _ = fcntl(pair[1], F_SETFL, O_NONBLOCK)
  var continuation: CheckedContinuation<(fd: Int32, session: UInt64), Never>?
  var ended = false
  let coordinator = TerminalHost.Coordinator(identity: try ProjectIdentity(url: dir), onEnd: { _ in ended = true },
    openTerminal: { _, _, _ in await withCheckedContinuation { continuation = $0 } })
  coordinator.open()
  while continuation == nil { await Task.yield() }
  coordinator.close()
  continuation!.resume(returning: (pair[0], 1))
  await coordinator.opening?.value
  var byte: UInt8 = 0
  #expect(Darwin.read(pair[1], &byte, 1) == 0, "Closing before connection must shut down the subsequently returned shell socket")
  #expect(!ended, "A removed tab must not publish a stale completion")
}

@MainActor @Test func closingOneTerminalLeavesAnotherSocketOpen() async throws {
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: dir) }
  try Data("{}".utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  let identity = try ProjectIdentity(url: dir)
  var pairs = [[Int32](), [Int32]()]
  var terminals: [TerminalHost.Coordinator] = []
  for index in 0..<2 {
    var pair: [Int32] = [-1, -1]
    #expect(socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0)
    pairs[index] = pair
    _ = fcntl(pair[1], F_SETFL, O_NONBLOCK)
    let terminal = TerminalHost.Coordinator(identity: identity, onEnd: { _ in },
      openTerminal: { _, _, _ in (pair[0], UInt64(index + 1)) })
    terminals.append(terminal)
    terminal.open()
    await terminal.opening?.value
  }
  defer { for terminal in terminals { terminal.close() }; for pair in pairs { Darwin.close(pair[1]) } }
  terminals[0].close()
  var byte: UInt8 = 0
  #expect(Darwin.read(pairs[0][1], &byte, 1) == 0)
  #expect(Darwin.read(pairs[1][1], &byte, 1) == -1)
  #expect(errno == EAGAIN, "The second terminal remains connected")
}
