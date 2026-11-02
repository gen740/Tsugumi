import Foundation

/// Runs the Apple `container` CLI. The host only starts, inspects and stops
/// environments; it never runs Card code or shells.
public struct ContainerCLI: Sendable {
  public static let pinnedVersion = "0.6.0"
  public let executable: URL

  public init(executable: URL = URL(fileURLWithPath: "/usr/local/bin/container")) {
    self.executable = executable
  }

  public struct Output: Sendable {
    public let status: Int32
    public let stdout: String
    public let stderr: String
    public var ok: Bool { status == 0 }
  }

  public func run(_ args: [String], timeout: TimeInterval = 600) async throws -> Output {
    guard FileManager.default.isExecutableFile(atPath: executable.path) else {
      throw LauncherError.containerMissing
    }
    return try await withCheckedThrowingContinuation { cont in
      let p = Process()
      p.executableURL = executable
      p.arguments = args
      let out = Pipe(), err = Pipe()
      p.standardOutput = out
      p.standardError = err
      p.standardInput = FileHandle.nullDevice
      // Read concurrently so a chatty command cannot fill the pipe and block.
      let outData = DataBox(), errData = DataBox()
      out.fileHandleForReading.readabilityHandler = { outData.append($0.availableData) }
      err.fileHandleForReading.readabilityHandler = { errData.append($0.availableData) }
      p.terminationHandler = { proc in
        out.fileHandleForReading.readabilityHandler = nil
        err.fileHandleForReading.readabilityHandler = nil
        outData.append(out.fileHandleForReading.readDataToEndOfFile())
        errData.append(err.fileHandleForReading.readDataToEndOfFile())
        cont.resume(returning: Output(status: proc.terminationStatus,
                                      stdout: String(decoding: outData.data, as: UTF8.self),
                                      stderr: String(decoding: errData.data, as: UTF8.self)))
      }
      do {
        try p.run()
      } catch {
        cont.resume(throwing: error)
        return
      }
      DispatchQueue.global().asyncAfter(deadline: .now() + timeout) { if p.isRunning { p.terminate() } }
    }
  }

  public func version() async throws -> String {
    let o = try await run(["--version"], timeout: 20)
    // "container CLI version 0.6.0-16-g14f1df5 (build: release, ...)"
    let words = o.stdout.split(separator: " ")
    guard let i = words.firstIndex(of: "version"), i + 1 < words.count else { return o.stdout }
    return String(words[i + 1])
  }

  public func serviceRunning() async -> Bool {
    (try? await run(["system", "status"], timeout: 20).ok) ?? false
  }
}

final class DataBox: @unchecked Sendable {
  private let lock = NSLock()
  private(set) var data = Data()
  func append(_ d: Data) {
    lock.lock()
    data.append(d)
    lock.unlock()
  }
}

public enum LauncherError: Error, CustomStringConvertible, Sendable {
  case containerMissing
  case unsupportedVersion(String)
  case serviceStopped
  case imageMissing
  case startFailed(reason: String, log: String)
  case notReady(String)
  case locked
  case invalidProject(String)

  public var description: String {
    switch self {
    case .containerMissing:
      return "Apple container is not installed (/usr/local/bin/container)."
    case .unsupportedVersion(let v):
      return "Apple container \(v) is installed; Tsugumi is validated with \(ContainerCLI.pinnedVersion)."
    case .serviceStopped:
      return "The container service is not running."
    case .imageMissing:
      return "The Tsugumi environment image is not available."
    case .startFailed(let reason, _):
      return "The environment failed to start: \(reason)"
    case .notReady(let s):
      return "The environment did not become ready: \(s)"
    case .locked:
      return "Another Tsugumi process is starting or stopping this environment."
    case .invalidProject(let s):
      return "Not a Tsugumi project: \(s)"
    }
  }

  /// What the user can do about it, shown with the error.
  public var recovery: String {
    switch self {
    case .containerMissing:
      return "Install Apple container \(ContainerCLI.pinnedVersion) from github.com/apple/container/releases, then retry."
    case .unsupportedVersion:
      return "Install container \(ContainerCLI.pinnedVersion), or continue at your own risk."
    case .serviceStopped:
      return "Click “Start container service” (runs `container system start`) and retry."
    case .imageMissing:
      return "Rebuild it with macOS/Container/build-image.sh, or reinstall the app."
    case .startFailed:
      return "Check the environment log below. Stop the environment and start it again; your documents and history are kept."
    case .notReady:
      return "Stop and start the environment. If it keeps failing, delete the environment data (documents are kept)."
    case .locked:
      return "Wait for the other operation to finish, then retry."
    case .invalidProject:
      return "Choose a folder that contains tsugumi.json."
    }
  }
}
