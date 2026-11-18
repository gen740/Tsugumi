import Foundation
import XCTest
import TsugumiRPC
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
      Tsugumi_Host_HostRuntimeT()
    }
    XCTAssertEqual(status, .checked(virtualizationSupported: false, imageAvailable: false, imageVersion: nil))
    XCTAssertFalse(status.ready)
  }

  func testSuccessfulRuntimeReplyKeepsBothChecksAndVersion() async {
    let status = await EnvironmentManager.SetupStatus.query {
      let reply = Tsugumi_Host_HostRuntimeT()
      reply.virtualizationSupported = true
      reply.imageAvailable = true
      reply.imageVersion = "bundled-image"
      return reply
    }
    XCTAssertEqual(status, .checked(virtualizationSupported: true, imageAvailable: true, imageVersion: "bundled-image"))
    XCTAssertTrue(status.ready)
  }
}
