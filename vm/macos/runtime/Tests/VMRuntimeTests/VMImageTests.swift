import Foundation
import TsugumiVMTypes
import XCTest

final class VMImageTests: XCTestCase {
  func testManifestIdentifiesTheImage() throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: directory) }
    let payload = directory.appendingPathComponent("tsugumi-vm.tar.gz")
    try Data("archive".utf8).write(to: payload)
    let manifest: [String: Any] = ["identity": "guest-definition"]
    let file = directory.appendingPathComponent("vm-manifest.json")
    try JSONSerialization.data(withJSONObject: manifest).write(to: file)
    let image = try XCTUnwrap(VMImage(directory: directory))
    XCTAssertEqual(image.version, "guest-definition")
    XCTAssertEqual(image.archive, payload)
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
