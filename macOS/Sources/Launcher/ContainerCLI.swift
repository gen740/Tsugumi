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
  case deleteFailed(what: String, log: String)

  /// Shown in the app: Japanese source strings, English from the app's
  /// string table (macOS/Resources/tools/en_strings.py).
  public var description: String {
    switch self {
    case .containerMissing:
      return String(localized: "Apple container がインストールされていません（/usr/local/bin/container）。", bundle: .main)
    case .unsupportedVersion(let v):
      return String(localized: "Apple container \(v) が入っています。Tsugumi は \(ContainerCLI.pinnedVersion) で確かめています。", bundle: .main)
    case .serviceStopped:
      return String(localized: "container のサービスが動いていません。", bundle: .main)
    case .imageMissing:
      return String(localized: "Tsugumi の実行環境のイメージが見つかりません。", bundle: .main)
    case .startFailed(let reason, _):
      return String(localized: "実行環境が起動の途中で止まりました: \(Self.reasonText(reason))", bundle: .main)
    case .notReady(let s):
      return String(localized: "実行環境の準備ができませんでした: \(s)", bundle: .main)
    case .locked:
      return String(localized: "別の Tsugumi の処理がこの実行環境を起動または停止しています。", bundle: .main)
    case .invalidProject(let s):
      return String(localized: "Tsugumi のプロジェクトではありません: \(s)", bundle: .main)
    case .deleteFailed(let what, let log):
      return String(localized: "\(what) を削除できませんでした: \(log.trimmingCharacters(in: .whitespacesAndNewlines))", bundle: .main)
    }
  }

  private static func reasonText(_ r: String) -> String {
    switch r {
    case "tsugumid exited during startup": return String(localized: "このプロジェクトの tsugumid が起動中に終了しました", bundle: .main)
    case "no readiness signal within 90 s": return String(localized: "90 秒たっても準備ができませんでした", bundle: .main)
    default: return r
    }
  }

  /// What the user can do about it, shown with the error.
  public var recovery: String {
    switch self {
    case .containerMissing:
      return String(localized: "github.com/apple/container/releases から Apple container \(ContainerCLI.pinnedVersion) を入れてから、再試行してください。", bundle: .main)
    case .unsupportedVersion:
      return String(localized: "container \(ContainerCLI.pinnedVersion) を入れてください（このまま使うこともできますが、動作は確かめていません）。", bundle: .main)
    case .serviceStopped:
      return String(localized: "「container のサービスを開始」（container system start）を押してから、再試行してください。", bundle: .main)
    case .imageMissing:
      return String(localized: "macOS/Container/build-image.sh で作り直すか、アプリを入れ直してください。", bundle: .main)
    case .startFailed:
      return String(localized: "下の原因とログを確かめてください。直したら再試行します。文書と実行履歴は消えません。", bundle: .main)
    case .notReady:
      return String(localized: "実行環境を停止してから再試行してください。続くときは実行環境のデータを削除します（文書は消えません）。", bundle: .main)
    case .locked:
      return String(localized: "もう一方の処理が終わるのを待ってから、再試行してください。", bundle: .main)
    case .invalidProject:
      return String(localized: "tsugumi.json のあるフォルダを選んでください。", bundle: .main)
    case .deleteFailed:
      return String(localized: "これより後の削除はしていません。実行環境のファイルは残っています。もう一度削除してください。", bundle: .main)
    }
  }
}
