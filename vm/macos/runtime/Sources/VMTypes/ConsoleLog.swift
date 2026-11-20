import Foundation

/// The serial attachment writes `console.log` and the helper records its own
/// notes and supervisor events in `launcher.log`. Both feed the console view.
public final class ConsoleLog: @unchecked Sendable {
  private let lock = NSLock()
  private var lines: [String] = []
  private var partial = Data()
  private var observers: [UUID: @Sendable (String) -> Void] = [:]
  private var file: FileHandle?
  private let limit = 5000

  public init() {}

  /// Starts a new boot: the guest's console file and the app's note file.
  public func open(console url: URL) {
    lock.lock()
    defer { lock.unlock() }
    let dir = url.deletingLastPathComponent()
    let prev = dir.appendingPathComponent("console.prev.log")
    try? FileManager.default.removeItem(at: prev)
    try? FileManager.default.moveItem(at: url, to: prev)
    FileManager.default.createFile(atPath: url.path, contents: nil)
    let notes = dir.appendingPathComponent("launcher.log")
    if !FileManager.default.fileExists(atPath: notes.path) { FileManager.default.createFile(atPath: notes.path, contents: nil) }
    file = try? FileHandle(forWritingTo: notes)
    _ = try? file?.seekToEnd()
    lines.removeAll()
    partial = Data()
  }

  /// Adds a line written by the app (not the guest), marked as such.
  public func note(_ text: String) {
    let line = "[tsugumi] " + text
    lock.lock()
    file?.write(Data((ISO8601DateFormatter().string(from: Date()) + " " + text + "\n").utf8))
    lock.unlock()
    append(line: line)
  }

  public func feed(_ data: Data) -> [String] {
    // Split on bytes: the guest ends lines with "\r\n", which a Swift String
    // holds as one Character (searching it for "\n" finds nothing).
    var complete: [String] = []
    lock.lock()
    partial.append(data)
    while let nl = partial.firstIndex(of: 10) {
      var line = partial[partial.startIndex..<nl]
      if line.last == 13 { line = line.dropLast() }
      complete.append(String(decoding: line, as: UTF8.self))
      partial.removeSubrange(partial.startIndex...nl)
    }
    lock.unlock()
    for l in complete { append(line: l) }
    return complete
  }

  private func append(line: String) {
    lock.lock()
    lines.append(line)
    if lines.count > limit { lines.removeFirst(lines.count - limit) }
    let obs = Array(observers.values)
    lock.unlock()
    for o in obs { o(line) }
  }

  public var snapshot: [String] {
    lock.lock()
    defer { lock.unlock() }
    return lines
  }

  public func observe(_ handler: @escaping @Sendable (String) -> Void) -> UUID {
    let id = UUID()
    lock.lock()
    observers[id] = handler
    lock.unlock()
    return id
  }

  public func stopObserving(_ id: UUID) {
    lock.lock()
    observers[id] = nil
    lock.unlock()
  }
}

