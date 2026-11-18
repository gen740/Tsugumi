import Foundation
import XCTest
@testable import TsugumiAgentClient
@testable import TsugumiLauncher

final class EnvironmentSetupTests: XCTestCase {
  func testFailingRuntimeCallIsAFailureRatherThanNegativeChecks() async {
    struct Unavailable: Error, CustomStringConvertible {
      var description: String { "runtime request failed" }
    }
    let status = await EnvironmentManager.SetupStatus.query { throw Unavailable() }
    XCTAssertEqual(status, .failed("runtime request failed"))
    XCTAssertFalse(status.ready)
  }

  func testConfirmedNegativeChecksRemainDistinctFromFailure() async {
    let status = await EnvironmentManager.SetupStatus.query {
      ["virtualization_supported": false, "image_available": false]
    }
    XCTAssertEqual(status, .checked(virtualizationSupported: false, imageAvailable: false, imageVersion: nil))
    XCTAssertFalse(status.ready)
  }

  func testSuccessfulRuntimeReplyKeepsBothChecksAndVersion() async {
    let status = await EnvironmentManager.SetupStatus.query {
      ["virtualization_supported": true, "image_available": true, "image_version": "bundled-image"]
    }
    XCTAssertEqual(status, .checked(virtualizationSupported: true, imageAvailable: true, imageVersion: "bundled-image"))
    XCTAssertTrue(status.ready)
  }

  func testMalformedReplyIsAFailureRatherThanMissingImage() async {
    let status = await EnvironmentManager.SetupStatus.query { ["virtualization_supported": true] }
    guard case .failed(let message) = status else { return XCTFail("Missing fields are not negative answers") }
    XCTAssertFalse(message.isEmpty)
  }

}
