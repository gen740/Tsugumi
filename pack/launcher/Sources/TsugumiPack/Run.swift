import Darwin
import Foundation

struct PackError: LocalizedError {
  let errorDescription: String?

  init(_ message: String) { errorDescription = message }
}

struct AppRun {
  let resources: URL
  let manifest: Manifest
  var start: String?
  var debugSocketPath: URL?
  var testClock = false

  struct Manifest: Decodable {
    let name: String
    let project: String
    let triggers: [String]
    let mode: PackedExecutionMode
    let debug: Bool
  }

  static func parse(_ arguments: [String], resources: URL) throws -> AppRun? {
    let bytes = try Data(contentsOf: resources.appendingPathComponent("pack.json"))
    let manifest = try JSONDecoder().decode(Manifest.self, from: bytes)
    var run = AppRun(resources: resources, manifest: manifest)
    var options = arguments.makeIterator()
    while let option = options.next() {
      switch option {
      case "--start":
        guard let value = options.next(), manifest.triggers.contains(value) else {
          throw PackError("--start requires an exported trigger")
        }
        run.start = value
      case "--debug-socket":
        guard manifest.debug, let path = options.next(), path.hasPrefix("/") else {
          throw PackError("--debug-socket requires a debug export and an absolute path")
        }
        run.debugSocketPath = URL(fileURLWithPath: path)
      case "--test-clock":
        guard manifest.debug else { throw PackError("--test-clock requires a debug export") }
        run.testClock = true
      case "--describe":
        print(String(decoding: bytes, as: UTF8.self))
        return nil
      case "--help", "-h":
        print("Usage: \(manifest.name) [--start TRIGGER] [--describe]"
              + (manifest.debug ? " [--debug-socket PATH] [--test-clock]" : ""))
        return nil
      default:
        throw PackError("Unknown packed app argument \(option)")
      }
    }
    guard let start = run.start ?? manifest.triggers.first else {
      throw PackError("The exported Project has no trigger Card")
    }
    run.start = start
    return run
  }

  @MainActor func run() throws -> Int32 {
    let fm = FileManager.default
    var temporary = Array("/private/tmp/tsugumi-pack.XXXXXX".utf8CString)
    guard let path = mkdtemp(&temporary) else {
      throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }
    let workspace = URL(fileURLWithPath: String(validatingCString: path) ?? "", isDirectory: true)
    defer { try? fm.removeItem(at: workspace) }
    let project = workspace.appendingPathComponent("project", isDirectory: true)
    try fm.copyItem(at: resources.appendingPathComponent("project", isDirectory: true), to: project)
    let identifier = Bundle.main.bundleIdentifier ?? "org.tsugumi.pack"
    let support = try fm.url(for: .applicationSupportDirectory, in: .userDomainMask,
                             appropriateFor: nil, create: true).appendingPathComponent(identifier, isDirectory: true)
    try fm.createDirectory(at: support, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])

    let helpers = resources.deletingLastPathComponent().appendingPathComponent("Helpers", isDirectory: true)
    let socket = workspace.appendingPathComponent("core.sock")
    let daemon = Process()
    daemon.executableURL = helpers.appendingPathComponent("tsugumid-pack")
    daemon.arguments = [
      "--project", project.path, "--unix", socket.path,
      "--packed-builds", resources.appendingPathComponent("native-builds.json").path,
      "--application-support", support.path,
      "--worker", helpers.appendingPathComponent("tsugumi-worker").path,
      "--workerd", helpers.appendingPathComponent("tsugumi-workerd").path,
      "--scratch-dir", workspace.appendingPathComponent("scratch").path,
      "--run-scratch-dir", workspace.appendingPathComponent("run").path,
      "--cpus", String(min(max(ProcessInfo.processInfo.activeProcessorCount, 1), 64)),
    ]
    let log = support.appendingPathComponent("last-launch.log")
    try Data().write(to: log, options: .atomic)
    let logHandle = try FileHandle(forWritingTo: log)
    defer { try? logHandle.close() }
    daemon.standardOutput = logHandle
    daemon.standardError = logHandle
    try daemon.run()
    defer { stopChild(daemon) }

    signal(SIGPIPE, SIG_IGN)
    let relay = PackViewRelay(title: manifest.name, debugClock: testClock)
    defer { relay.disconnect() }
    var done = false
    var requestedStop = false
    var result: Result<Int32, Error> = .success(0)
    let signals = [SIGTERM, SIGINT].map { number in
      signal(number, SIG_IGN)
      let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
      source.setEventHandler { requestedStop = true }
      source.activate()
      return source
    }
    defer {
      for source in signals { source.cancel() }
    }
    let viewDebug = DebugViews(presentation: relay.presentation)
    var debug: DebugSocket?
    defer { debug?.stop() }
    let runTask = Task { @MainActor in
      do {
        for _ in 0..<200 {
          if fm.fileExists(atPath: socket.path) { break }
          guard daemon.isRunning else { throw PackError("The packed core stopped before startup") }
          try await Task.sleep(for: .milliseconds(50))
        }
        try await relay.connect(adapter: helpers.appendingPathComponent("tsugumi-rpc"),
                                schema: resources.appendingPathComponent("tsugumi.bfbs"), socket: socket)
        // The connected presentation pipe is the sole client for this run.
        try fm.removeItem(at: socket)
        let project = try await relay.call("ProjectInspectRequest")
        guard let revision = project["rev"] as? NSNumber, let start else {
          throw PackError("The packed core did not return a project revision")
        }
        if manifest.debug {
          let path = debugSocketPath ?? workspace.appendingPathComponent("debug.sock")
          let endpoint = DebugSocket(path: path) { method, body in
            if let value = try await viewDebug.handle(method, body) { return value }
            switch method {
            case "PackRunRequest":
              var run = body
              run["base_rev"] = revision
              run["op_id"] = UUID().uuidString.lowercased()
              run["origin"] = "pack-debug"
              return try await relay.call("RunRequest", run)
            case "RunStatusRequest", "RunInvocationsRequest", "RunLatestInvocationsRequest",
                 "RunRecordRequest", "RunValueReadRequest", "ArtifactListRequest", "RunCancelRequest":
              return try await relay.call(method, body)
            case "PackQuitRequest":
              requestedStop = true
              return [:]
            default:
              throw PackError("Unknown packed debug request \(method)")
            }
          }
          try endpoint.start()
          debug = endpoint
          print("Debug socket \(path.path)")
        }
        let run = try await relay.call("RunRequest", [
          "base_rev": revision, "op_id": UUID().uuidString.lowercased(), "origin": "pack",
          "card": ["segments": [start]],
        ])
        guard let id = (run["run"] as? [String: Any])?["run_id"] as? String else {
          throw PackError("The packed core did not return a run ID")
        }
        print("Run \(id)")
        if !manifest.debug {
          let terminal = ["succeeded", "failed", "cancelled"]
          while !Task.isCancelled {
            let status = try await relay.call("RunStatusRequest", ["run_id": id])
            let state = status["state"] as? String ?? ""
            if terminal.contains(state) {
              if state == "succeeded" {
                result = .success(0)
              } else {
                let failure = status["failure"] as? [String: Any]
                result = .failure(PackError(failure?["diagnostic"] as? String ?? "Run \(id) \(state)"))
              }
              done = true
              break
            }
            try await Task.sleep(for: .milliseconds(100))
          }
        }
      } catch {
        result = .failure(error)
        done = true
      }
    }
    defer { runTask.cancel() }
    while !requestedStop {
      if !daemon.isRunning {
        let diagnostic = try? String(contentsOf: log, encoding: .utf8)
        throw PackError("The packed core exited (\(daemon.terminationStatus)). "
                        + (diagnostic.flatMap { $0.split(separator: "\n").last.map(String.init) } ?? "See \(log.path)."))
      }
      if done && relay.presentation.isEmpty { break }
      _ = RunLoop.main.run(mode: .default, before: Date(timeIntervalSinceNow: 0.016))
      relay.pollWindows()
    }
    return requestedStop ? 0 : try result.get()
  }
}

/// Ends only the child we launched and lets it reap its own worker processes.
func stopChild(_ process: Process) {
  guard process.isRunning else { return }
  process.terminate()
  for _ in 0..<100 {
    guard process.isRunning else { return }
    usleep(20_000)
  }
  kill(process.processIdentifier, SIGKILL)
  process.waitUntilExit()
}
