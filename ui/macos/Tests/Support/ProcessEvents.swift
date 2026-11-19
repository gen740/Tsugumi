import Darwin
import Foundation

/// Follow an atomically published readiness file or the child's exit.
public func fixtureReadiness<T>(_ file: URL, pid: pid_t?, read: () throws -> T?) throws -> T? {
  while true {
    let parent = file.deletingLastPathComponent()
    var watched = parent
    while !FileManager.default.fileExists(atPath: watched.path) {
      watched.deleteLastPathComponent()
    }
    let directory = open(watched.path, O_EVTONLY | O_CLOEXEC)
    if directory < 0, errno == ENOENT { continue }
    guard directory >= 0 else { throw systemError() }
    defer { close(directory) }
    let queue = kqueue()
    guard queue >= 0 else { throw systemError() }
    defer { close(queue) }
    var events = [kevent(ident: UInt(directory), filter: Int16(EVFILT_VNODE),
                        flags: UInt16(EV_ADD | EV_CLEAR),
                        fflags: UInt32(NOTE_WRITE | NOTE_RENAME | NOTE_DELETE), data: 0, udata: nil)]
    if let pid { events.append(exitEvent(pid)) }
    if kevent(queue, &events, Int32(events.count), nil, 0, nil) < 0 {
      if errno == ESRCH { return try read() }
      throw systemError()
    }
    if let value = try read() { return value }
    if watched != parent, FileManager.default.fileExists(atPath: parent.path) { continue }
    // If an intermediate directory appeared while registering, follow it next.
    if watched != parent {
      let next = watched.appendingPathComponent(parent.pathComponents[watched.pathComponents.count])
      if FileManager.default.fileExists(atPath: next.path) { continue }
    }
    var event = kevent()
    let count = kevent(queue, nil, 0, &event, 1, nil)
    if count < 0, errno != EINTR { throw systemError() }
    if count > 0, event.filter == Int16(EVFILT_PROC) { return try read() }
  }
}

/// Signal the fixture process only after subscribing to its exit.
public func finishFixtureProcess(_ pid: pid_t, signal: Int32) throws {
  let queue = kqueue()
  guard queue >= 0 else { throw systemError() }
  defer { close(queue) }
  var change = exitEvent(pid)
  if kevent(queue, &change, 1, nil, 0, nil) < 0 {
    if errno == ESRCH { return }
    throw systemError()
  }
  if kill(pid, signal) < 0, errno != ESRCH { throw systemError() }
  // A fixture that ignores TERM should still release its sockets and files.
  // The grace period exists solely to escalate cleanup to KILL.
  var grace = timespec(tv_sec: 5, tv_nsec: 0)
  var event = kevent()
  if signal != SIGKILL, kevent(queue, nil, 0, &event, 1, &grace) > 0 { return }
  if signal != SIGKILL, kill(pid, SIGKILL) < 0, errno != ESRCH { throw systemError() }
  while kevent(queue, nil, 0, &event, 1, nil) < 0 {
    if errno != EINTR { throw systemError() }
  }
}

private func exitEvent(_ pid: pid_t) -> kevent {
  kevent(ident: UInt(pid), filter: Int16(EVFILT_PROC), flags: UInt16(EV_ADD | EV_ONESHOT),
         fflags: UInt32(NOTE_EXIT), data: 0, udata: nil)
}

private func systemError() -> POSIXError { POSIXError(.init(rawValue: errno) ?? .EIO) }
