// tsugumi-vmhost: the shared VM for the web server of the `tsugumi` command
// (web/server/vm.ts), through the same EnvironmentManager as the app, so the
// server finds the app's VM, store and control folders in the same places.
//
//   tsugumi-vmhost --support DIR --image DIR --source DIR
//
// JSON, one object per line. Requests on stdin: {"id": n, "op": ..., ...};
// answers on stdout: {"id": n, "result": ...} or {"id": n, "error": ...,
// "recovery": ..., "log": ...}, in any order, and events without an id. The
// VM stops when stdin ends, so it never outlives the server.
//
// Needs the com.apple.security.virtualization entitlement (codesign).
import Darwin
import Foundation
import TsugumiLauncher

signal(SIGPIPE, SIG_IGN)  // a closed socket is an error, never the end of the process

var support: URL?
var imageDir: URL?
var sourceDir: URL?
var args = CommandLine.arguments.dropFirst()
while let a = args.popFirst() {
  let value = args.popFirst().map { URL(fileURLWithPath: $0, isDirectory: true) }
  switch a {
  case "--support": support = value
  case "--image": imageDir = value
  case "--source": sourceDir = value
  default:
    FileHandle.standardError.write(Data("usage: tsugumi-vmhost --support DIR --image DIR --source DIR\n".utf8))
    exit(2)
  }
}
guard let support, let imageDir, let sourceDir else {
  FileHandle.standardError.write(Data("--support, --image and --source are required\n".utf8))
  exit(2)
}

let manager = EnvironmentManager(support: support, image: VMImage(directory: imageDir, source: sourceDir))
// The app's settings: its bundle id unless a check asks for another domain.
let defaults = UserDefaults(suiteName: ProcessInfo.processInfo.environment["TSUGUMI_DEFAULTS_DOMAIN"] ?? "app.tsugumi.Tsugumi")!

let outputLock = NSLock()

@Sendable func emit(_ object: [String: Any]) {
  guard let data = try? JSONSerialization.data(withJSONObject: object) else { return }
  outputLock.lock()
  defer { outputLock.unlock() }
  FileHandle.standardOutput.write(data + Data("\n".utf8))
}

/// The open terminals: their sockets to the VM, by the session id it gave them.
final class Terminals: @unchecked Sendable {
  private let lock = NSLock()
  private var sockets: [UInt64: Int32] = [:]

  func add(_ fd: Int32, _ session: UInt64) {
    lock.withLock { sockets[session] = fd }
    Thread.detachNewThread { [self] in
      var buffer = [UInt8](repeating: 0, count: 16384)
      while true {
        let n = read(fd, &buffer, buffer.count)
        if n <= 0 { break }
        emit(["event": "terminal", "session": session, "data": Data(buffer[0..<n]).base64EncodedString()])
      }
      lock.withLock { sockets[session] = nil }
      Darwin.close(fd)
      emit(["event": "terminalExit", "session": session])
    }
  }

  func write(_ session: UInt64, _ data: Data) {
    guard let fd = lock.withLock({ sockets[session] }) else { return }
    data.withUnsafeBytes { (buffer: UnsafeRawBufferPointer) in
      var at = 0
      while at < buffer.count {
        let n = Darwin.write(fd, buffer.baseAddress! + at, buffer.count - at)
        if n <= 0 { return }
        at += n
      }
    }
  }

  func close(session: UInt64) {
    if let fd = lock.withLock({ sockets[session] }) { shutdown(fd, SHUT_RDWR) }
  }
}
let terminals = Terminals()

/// Projects whose progress is already reported.
actor Observed {
  private var ids: Set<String> = []
  func first(_ id: String) -> Bool { ids.insert(id).inserted }
}
let observed = Observed()

@Sendable func project(_ request: [String: Any]) throws -> ProjectIdentity {
  try ProjectIdentity(url: URL(fileURLWithPath: request["path"] as? String ?? ""))
}

@Sendable func answer(_ request: [String: Any]) async throws -> Any {
  switch request["op"] as? String {
  case "start":
    let p = try project(request)
    await manager.setAllowedOrigins(request["origins"] as? [String] ?? [])
    if await observed.first(p.id) {
      _ = await manager.observe(p) { state in
        switch state {
        case .preparing(let stage), .starting(let stage): emit(["event": "progress", "key": p.id, "stage": stage])
        default: break
        }
      }
    }
    let endpoint = try await manager.start(p)
    return ["endpoint": endpoint.url.absoluteString, "token": endpoint.token, "projectId": endpoint.projectId,
            "readyFile": manager.controlDir(p).appendingPathComponent("server.ready").path]
  case "stop":
    try await manager.stop(try project(request))
    return [:]
  case "folders":
    await manager.setExtraFolders(try project(request), request["folders"] as? [String] ?? [])
    return [:]
  case "terminal":
    let (fd, session) = try await manager.openTerminal(try project(request), rows: request["rows"] as? Int ?? 24, cols: request["cols"] as? Int ?? 80)
    terminals.add(fd, session)
    return ["session": session]
  case "input":
    guard let session = request["session"] as? UInt64, let data = Data(base64Encoded: request["data"] as? String ?? "") else { return [:] }
    terminals.write(session, data)
    return [:]
  case "resize":
    guard let session = request["session"] as? UInt64 else { return [:] }
    await manager.resizeTerminal(session: session, rows: request["rows"] as? Int ?? 24, cols: request["cols"] as? Int ?? 80)
    return [:]
  case "closeTerminal":
    if let session = request["session"] as? UInt64 { terminals.close(session: session) }
    return [:]
  case "prefs":
    var values: [String: Any] = [:]
    for key in request["keys"] as? [String] ?? [] {
      if let value = defaults.object(forKey: key), JSONSerialization.isValidJSONObject([value]) { values[key] = value }
    }
    return values
  case "setPrefs":
    for (key, value) in request["values"] as? [String: Any] ?? [:] { defaults.set(value, forKey: key) }
    return [:]
  default:
    throw LauncherError.notReady("unknown request \(request["op"] ?? "")")
  }
}

Thread.detachNewThread {
  while let line = readLine() {
    guard let request = (try? JSONSerialization.jsonObject(with: Data(line.utf8))) as? [String: Any], let id = request["id"] else { continue }
    Task {
      do {
        emit(["id": id, "result": try await answer(request)])
      } catch {
        var failure: [String: Any] = ["id": id, "error": "\(error)"]
        if let e = error as? LauncherError {
          failure["recovery"] = e.recovery
          if case .startFailed(_, let log) = e { failure["log"] = log }
        }
        emit(failure)
      }
    }
  }
  // The server has gone: tsugumid records interrupted runs, and the VM stops.
  Task {
    await manager.shutdown()
    exit(0)
  }
}
dispatchMain()
