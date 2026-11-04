import Foundation

/// Which build of the app this is. A release (built by the release workflow
/// and installed from GitHub Releases) keeps its VM, store and chats in
/// `~/Library/Application Support/Tsugumi` and updates itself. A build from
/// source (`macOS/build-app.sh` without `--release`, `swift run`, tests) is
/// dev: it keeps everything in `Tsugumi-dev` instead and has its own bundle
/// id, so a dev build and the checks run with it never start, stop or reset
/// the VM of the release in daily use, and both can run at the same time
/// (each support folder has its own VM and MAC address).
public enum AppChannel: String, Sendable {
  case release, dev

  /// From the bundle's `TsugumiChannel` (build-app.sh); dev without one.
  public static let current = AppChannel(bundle: .main)

  public init(bundle: Bundle) {
    self = (bundle.object(forInfoDictionaryKey: "TsugumiChannel") as? String).flatMap(AppChannel.init(rawValue:)) ?? .dev
  }

  public var supportFolderName: String { self == .release ? "Tsugumi" : "Tsugumi-dev" }

  /// `~/Library/Application Support/<supportFolderName>`.
  public var supportDirectory: URL {
    FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent(supportFolderName, isDirectory: true)
  }
}
