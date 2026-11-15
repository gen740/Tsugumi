import Foundation

/// Foundation's temporary-directory APIs ignore TMPDIR on macOS.
/// Use the isolated runner's folder, or Foundation's default outside the runner.
public var testTemporaryDirectory: URL {
  if let path = ProcessInfo.processInfo.environment["TMPDIR"], !path.isEmpty {
    return URL(fileURLWithPath: path, isDirectory: true)
  }
  return FileManager.default.temporaryDirectory
}
