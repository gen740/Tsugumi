import Darwin
import Foundation
import XCTest
@testable import TsugumiVMRuntime
import TsugumiVMTypes

/// The helper's socket works however deep its folder is.
final class VMHostIPCTests: XCTestCase {
  private var root: URL!
  private var previous: String?

  override func setUpWithError() throws {
    // A short private root keeps the links this test makes inside its folder.
    previous = ProcessInfo.processInfo.environment["TSUGUMI_SOCKET_DIR"]
    root = URL(fileURLWithPath: previous ?? "/tmp").appendingPathComponent("vi-\(UUID().uuidString.prefix(8))")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
    setenv("TSUGUMI_SOCKET_DIR", root.path, 1)
  }

  override func tearDownWithError() throws {
    if let previous { setenv("TSUGUMI_SOCKET_DIR", previous, 1) } else { unsetenv("TSUGUMI_SOCKET_DIR") }
    try FileManager.default.removeItem(at: root)
  }

  func testListensAndConnectsInAFolderDeeperThanSunPath() throws {
    let folder = (0..<8).reduce(root) { url, _ in url.appendingPathComponent(String(repeating: "d", count: 20)) }
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let path = folder.appendingPathComponent("vmhost.sock").path
    XCTAssertGreaterThan(path.utf8.count, 104)
    let listener = try VMHostIPC.listen(path: path)
    defer { Darwin.close(listener) }
    // The socket file is in its own folder, under that folder's permissions.
    var status = stat()
    XCTAssertEqual(lstat(path, &status), 0)
    XCTAssertEqual(status.st_mode & S_IFMT, S_IFSOCK)
    let peer = try VMHostIPC.connect(path: path)
    Darwin.close(peer)
  }

  func testNamesThePathAndTheLimitWhenANameCannotBeShortened() throws {
    let path = root.appendingPathComponent(String(repeating: "n", count: 120)).path
    XCTAssertThrowsError(try VMHostIPC.address(path)) { error in
      guard case LauncherError.notReady(let message) = error else { return XCTFail("\(error)") }
      XCTAssertTrue(message.contains(path), message)
      XCTAssertTrue(message.contains("this system allows 103"), message)
    }
  }
}
