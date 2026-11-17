import Foundation
import TsugumiVMTypes
import XCTest

final class VMImageTests: XCTestCase {
  func testArchiveAndManifestReplaceRuntimeSources() throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let payload = directory.appendingPathComponent("tsugumi-vm.tar.gz")
    try Data("archive".utf8).write(to: payload)
    let hash = String(repeating: "a", count: 64)
    let manifest: [String: Any] = ["identity": "guest-definition", "system": "aarch64-linux",
      "files": ["tsugumi-vm.tar.gz": hash],
      "members": ["vmlinux": hash, "rootfs.erofs": hash, "vm-version": hash]]
    let file = directory.appendingPathComponent("vm-manifest.json")
    try JSONSerialization.data(withJSONObject: manifest).write(to: file)
    let image = try XCTUnwrap(VMImage(directory: directory))
    XCTAssertEqual(image.version, "guest-definition")
    XCTAssertEqual(image.archive, payload)
    XCTAssertEqual(image.checksums["rootfs.erofs"], hash)
    try FileManager.default.removeItem(at: payload)
    XCTAssertNil(VMImage(directory: directory))
  }

  func testExpandedOldImageWithoutManifestIsNotAnImage() throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    for name in ["vmlinux", "rootfs.erofs", "vm-version"] {
      try Data("old".utf8).write(to: directory.appendingPathComponent(name))
    }
    XCTAssertNil(VMImage(directory: directory))
  }
}
