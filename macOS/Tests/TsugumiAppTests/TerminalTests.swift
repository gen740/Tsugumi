import AppKit
import Foundation
import Testing
import TsugumiLauncher
@testable import TsugumiApp

/// A project folder that exists only for the test.
private func temporaryProject() throws -> (identity: ProjectIdentity, remove: () -> Void) {
  let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  try Data("{}".utf8).write(to: dir.appendingPathComponent("tsugumi.json"))
  return (try ProjectIdentity(url: dir), { try? FileManager.default.removeItem(at: dir) })
}

/// Stands in for the VM: every opened shell is one end of a socket pair, and
/// the test holds the other end (what vmd would see).
@MainActor
private final class Shells {
  /// The VM's end of each shell, in the order they were opened.
  private(set) var remote: [Int32] = []

  func open() -> (fd: Int32, session: UInt64) {
    var pair: [Int32] = [-1, -1]
    precondition(socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0)
    _ = fcntl(pair[1], F_SETFL, O_NONBLOCK)
    remote.append(pair[1])
    return (pair[0], UInt64(remote.count))
  }

  func terminals(_ identity: ProjectIdentity) -> ProjectTerminals {
    ProjectTerminals(identity: identity) { [self] _, _, _ in open() }
  }

  /// What the VM's end of shell `index` sees: `.open` (nothing to read, still
  /// connected), `.closed` (the app hung up), or the bytes the app wrote.
  enum Seen: Equatable { case open, closed, bytes([UInt8]) }
  func seen(_ index: Int) -> Seen {
    var buffer = [UInt8](repeating: 0, count: 64)
    let count = Darwin.read(remote[index], &buffer, buffer.count)
    if count > 0 { return .bytes(Array(buffer[0..<count])) }
    return count == 0 ? .closed : .open
  }

  deinit { for fd in remote { Darwin.close(fd) } }
}

@MainActor private func settle(_ terminals: ProjectTerminals) async {
  for tab in terminals.tabs { await tab.connection.opening?.value }
}

/// Waits for what the main queue was already asked to do (a reader's end).
@MainActor private func drainMain() async {
  await withCheckedContinuation { done in DispatchQueue.main.async { done.resume() } }
}

@MainActor @Test func aTerminalClosedBeforeConnectingDisposesTheLateSocket() async throws {
  let project = try temporaryProject()
  defer { project.remove() }
  var pair: [Int32] = [-1, -1]
  #expect(socketpair(AF_UNIX, SOCK_STREAM, 0, &pair) == 0)
  defer { Darwin.close(pair[1]) }
  _ = fcntl(pair[1], F_SETFL, O_NONBLOCK)
  var continuation: CheckedContinuation<(fd: Int32, session: UInt64), Never>?
  var ended = false
  let connection = TerminalConnection(identity: project.identity, onEnd: { _ in ended = true },
    openTerminal: { _, _, _ in await withCheckedContinuation { continuation = $0 } })
  connection.open()
  while continuation == nil { await Task.yield() }
  connection.close()
  continuation!.resume(returning: (pair[0], 1))
  await connection.opening?.value
  var byte: UInt8 = 0
  #expect(Darwin.read(pair[1], &byte, 1) == 0, "Closing before connection must shut down the subsequently returned shell socket")
  #expect(!ended, "A removed tab must not publish a stale completion")
}

@MainActor @Test func closingOneTerminalLeavesAnotherSocketOpen() async throws {
  let project = try temporaryProject()
  defer { project.remove() }
  let shells = Shells()
  var connections: [TerminalConnection] = []
  for _ in 0..<2 {
    let connection = TerminalConnection(identity: project.identity, onEnd: { _ in },
      openTerminal: { _, _, _ in shells.open() })
    connections.append(connection)
    connection.open()
    await connection.opening?.value
  }
  defer { for connection in connections { connection.close() } }
  connections[0].close()
  #expect(shells.seen(0) == .closed)
  #expect(shells.seen(1) == .open, "The second terminal remains connected")
}

@MainActor @Test func aClosedTerminalsQueuedWriteAndReaderLeaveAReusedDescriptorAlone() async throws {
  let project = try temporaryProject()
  defer { project.remove() }
  var old: [Int32] = [-1, -1]
  #expect(socketpair(AF_UNIX, SOCK_STREAM, 0, &old) == 0)
  defer { Darwin.close(old[1]) }
  let number = old[0]
  let connection = TerminalConnection(identity: project.identity, onEnd: { _ in },
    openTerminal: { _, _, _ in (old[0], 1) })
  connection.open()
  await connection.opening?.value
  // A write waits in the queue while the terminal closes and another socket
  // takes the descriptor number the terminal had.
  connection.writeQueue.suspend()
  connection.send(Array("stale".utf8))
  connection.close()
  var fresh: [Int32] = [-1, -1]
  #expect(socketpair(AF_UNIX, SOCK_STREAM, 0, &fresh) == 0)
  defer { Darwin.close(fresh[0]); Darwin.close(fresh[1]) }
  // The lowest free number is normally the one just closed; the checks below
  // hold for the new pair either way.
  let local = fresh.contains(number) ? number : fresh[0]
  let other = fresh[0] == local ? fresh[1] : fresh[0]
  _ = fcntl(local, F_SETFL, O_NONBLOCK)
  _ = fcntl(other, F_SETFL, O_NONBLOCK)
  connection.writeQueue.resume()
  connection.writeQueue.sync {}
  var buffer = [UInt8](repeating: 0, count: 16)
  #expect(Darwin.read(other, &buffer, buffer.count) == -1 && errno == EAGAIN, "The queued write went to its own socket, not the new one")
  // The old reader must not consume what arrives for the new owner.
  #expect(Darwin.write(other, "new", 3) == 3)
  await drainMain()
  #expect(Darwin.read(local, &buffer, buffer.count) == 3)
  #expect(Array(buffer[0..<3]) == Array("new".utf8))
  connection.send(Array("late".utf8))
  connection.writeQueue.sync {}
  #expect(Darwin.read(other, &buffer, buffer.count) == -1 && errno == EAGAIN, "A closed terminal writes nothing")
}

@MainActor @Test func terminalsStartWithThePanelAndWaitForTheEnvironment() async throws {
  let project = try temporaryProject()
  defer { project.remove() }
  let shells = Shells()
  let terminals = shells.terminals(project.identity)
  terminals.environment(connected: true)
  #expect(terminals.tabs.isEmpty && shells.remote.isEmpty, "A project whose terminal is never shown starts no shell")
  terminals.environment(connected: false)
  terminals.show()
  terminals.show()
  #expect(terminals.tabs.map(\.number) == [1] && terminals.selection == 1)
  #expect(shells.remote.isEmpty, "The tab waits for the environment")
  terminals.environment(connected: true)
  await settle(terminals)
  #expect(shells.remote.count == 1 && terminals.tabs[0].live)
}

@MainActor @Test func switchingTabsKeepsEveryShellAndOnlyTheSelectedTabTakesInput() async throws {
  let project = try temporaryProject()
  defer { project.remove() }
  let shells = Shells()
  let terminals = shells.terminals(project.identity)
  terminals.environment(connected: true)
  terminals.show()
  terminals.open()
  await settle(terminals)
  #expect(terminals.selection == 2)
  let first = terminals.tabs[0].connection, second = terminals.tabs[1].connection
  first.send(Array("no".utf8))
  second.send(Array("two".utf8))
  first.writeQueue.sync {}
  second.writeQueue.sync {}
  #expect(shells.seen(0) == .open, "An unselected tab takes no input, and its shell stays open")
  #expect(shells.seen(1) == .bytes(Array("two".utf8)))
  terminals.select(1)
  terminals.select(99)
  #expect(terminals.selection == 1, "Selecting a tab that does not exist changes nothing")
  first.send(Array("one".utf8))
  second.send(Array("no".utf8))
  first.writeQueue.sync {}
  second.writeQueue.sync {}
  #expect(shells.seen(0) == .bytes(Array("one".utf8)))
  #expect(shells.seen(1) == .open, "Selecting another tab leaves the earlier shell running")
}

@MainActor @Test func closingTabsMovesTheSelectionAndNeverReusesANumber() async throws {
  let project = try temporaryProject()
  defer { project.remove() }
  let shells = Shells()
  let terminals = shells.terminals(project.identity)
  terminals.environment(connected: true)
  terminals.show()
  terminals.open()
  terminals.open()
  await settle(terminals)
  #expect(terminals.tabs.map(\.number) == [1, 2, 3] && terminals.selection == 3)
  // Closing an unselected tab keeps the selection and ends only that shell.
  terminals.close(1)
  #expect(terminals.selection == 3)
  #expect(shells.seen(0) == .closed && shells.seen(1) == .open && shells.seen(2) == .open)
  // Closing the selected last tab selects the one before it.
  terminals.close(3)
  #expect(terminals.selection == 2 && shells.seen(2) == .closed)
  terminals.open()
  #expect(terminals.tabs.map(\.number) == [2, 4], "A closed tab's number is not given to another shell")
  // Closing the selected tab in the middle selects the one that takes its place.
  terminals.select(2)
  terminals.close(2)
  #expect(terminals.selection == 4)
  // Closing the only tab leaves none selected; the next one still gets a new number.
  terminals.close(4)
  #expect(terminals.tabs.isEmpty && terminals.selection == 0)
  terminals.open()
  #expect(terminals.tabs.map(\.number) == [5] && terminals.selection == 5)
}

@MainActor @Test func reconnectingReopensEndedTabsAndClearsTheirLabels() async throws {
  let project = try temporaryProject()
  defer { project.remove() }
  let shells = Shells()
  let terminals = shells.terminals(project.identity)
  terminals.environment(connected: true)
  terminals.show()
  terminals.open()
  await settle(terminals)
  // The first shell ends (the environment went away, or the user typed exit).
  Darwin.shutdown(shells.remote[0], SHUT_RDWR)
  for _ in 0..<2000 where terminals.tabs[0].ended == nil { try await Task.sleep(for: .milliseconds(5)) }
  #expect(terminals.tabs[0].ended != nil && !terminals.tabs[0].live)
  #expect(terminals.tabs[1].ended == nil && terminals.tabs[1].live)
  terminals.environment(connected: false)
  #expect(terminals.tabs.map(\.number) == [1, 2], "Losing the connection keeps the tabs")
  terminals.environment(connected: true)
  await settle(terminals)
  #expect(terminals.tabs[0].ended == nil && terminals.tabs[0].live, "The ended tab has a shell again and no stale label")
  #expect(shells.remote.count == 3, "Only the tab without a shell was reopened")
  #expect(shells.seen(1) == .open, "A shell that survived the reconnection is kept")
  #expect(shells.seen(2) == .open)
}

@MainActor @Test func terminalsAreCappedAndAllEndWithTheProject() async throws {
  let project = try temporaryProject()
  defer { project.remove() }
  let shells = Shells()
  let terminals = shells.terminals(project.identity)
  terminals.environment(connected: true)
  terminals.show()
  for _ in 1..<ProjectTerminals.limit { #expect(terminals.open()) }
  #expect(!terminals.canOpen)
  #expect(!terminals.open(), "The ninth terminal is refused")
  #expect(terminals.tabs.count == ProjectTerminals.limit && terminals.selection == ProjectTerminals.limit)
  await settle(terminals)
  terminals.close(1)
  #expect(terminals.open(), "Closing a tab makes room for another")
  await settle(terminals)
  #expect(shells.remote.count == ProjectTerminals.limit + 1)
  terminals.closeAll()
  #expect(terminals.tabs.isEmpty && terminals.selection == 0)
  for index in shells.remote.indices { #expect(shells.seen(index) == .closed) }
}
