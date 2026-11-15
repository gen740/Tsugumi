import Foundation
import XCTest
import TsugumiTestSupport
@testable import TsugumiLauncher

final class VMResetTests: XCTestCase {
  func testAppliesTheHostsKeepListAndReportsActualEntries() throws {
    let directory = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    for name in ["history.img", "store.img", ".lock", "operator-chosen.img"] {
      try Data(name.utf8).write(to: directory.appendingPathComponent(name))
    }
    try FileManager.default.createDirectory(at: directory.appendingPathComponent("image"), withIntermediateDirectories: true)
    let report = try VMReset.apply(directory: directory, keepFiles: ["operator-chosen.img", "absent"], discardOtherFiles: true)
    XCTAssertEqual(report.kept, ["operator-chosen.img"])
    XCTAssertEqual(report.discarded, [".lock", "history.img", "image", "store.img"])
    XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: directory.path), ["operator-chosen.img"])
    XCTAssertEqual(try String(contentsOf: directory.appendingPathComponent("operator-chosen.img"), encoding: .utf8), "operator-chosen.img")
  }

  func testDiscardFlagAndEmptyKeepListAreAppliedLiterally() throws {
    let directory = testTemporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: directory) }
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    try Data("history".utf8).write(to: directory.appendingPathComponent("history.img"))
    let retained = try VMReset.apply(directory: directory, keepFiles: [], discardOtherFiles: false)
    XCTAssertEqual(retained.kept, ["history.img"])
    XCTAssertTrue(retained.discarded.isEmpty)
    let removed = try VMReset.apply(directory: directory, keepFiles: [], discardOtherFiles: true)
    XCTAssertTrue(removed.kept.isEmpty)
    XCTAssertEqual(removed.discarded, ["history.img"])
  }

  func testMissingDirectoryHasNothingToReport() throws {
    let report = try VMReset.apply(directory: testTemporaryDirectory.appendingPathComponent(UUID().uuidString), keepFiles: [], discardOtherFiles: true)
    XCTAssertTrue(report.kept.isEmpty)
    XCTAssertTrue(report.discarded.isEmpty)
  }
}
