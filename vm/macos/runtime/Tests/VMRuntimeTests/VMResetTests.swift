import Foundation
import XCTest

@testable import TsugumiVMRuntime
import TsugumiVMTypes

final class VMResetTests: XCTestCase {
  func testAppliesTheHostsKeepListAndReportsActualEntries() throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    for name in ["scratch.img", "operator-chosen.img"] {
      try Data(name.utf8).write(to: directory.appendingPathComponent(name))
    }
    try FileManager.default.createDirectory(at: directory.appendingPathComponent("image"), withIntermediateDirectories: true)
    let report = try VMReset.apply(directory: directory, keepFiles: ["operator-chosen.img", "absent"], discardOtherFiles: true)
    XCTAssertEqual(report.kept, ["operator-chosen.img"])
    XCTAssertEqual(Set(report.discarded), ["image", "scratch.img"])
    XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: directory.path), ["operator-chosen.img"])
    XCTAssertEqual(try String(contentsOf: directory.appendingPathComponent("operator-chosen.img"), encoding: .utf8), "operator-chosen.img")
  }

  func testDiscardFlagAndEmptyKeepListAreAppliedLiterally() throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    _ = try VMReset.apply(directory: directory, keepFiles: [], discardOtherFiles: true)
    XCTAssertFalse(FileManager.default.fileExists(atPath: directory.path))
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try Data("scratch".utf8).write(to: directory.appendingPathComponent("scratch.img"))
    let scratch = directory.appendingPathComponent("scratch.img")
    _ = try VMReset.apply(directory: directory, keepFiles: [], discardOtherFiles: false)
    XCTAssertTrue(FileManager.default.fileExists(atPath: scratch.path))
    _ = try VMReset.apply(directory: directory, keepFiles: [], discardOtherFiles: true)
    XCTAssertFalse(FileManager.default.fileExists(atPath: scratch.path))
  }
}
