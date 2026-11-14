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

  func testLockLoserWaitsForTheWinningDaemonToPublishReadiness() async throws {
    var reads = 0
    var pauses = 0
    let ready = try await AgentReadiness.wait(read: {
      reads += 1
      return reads == 1 ? nil : ["host_socket": "winner.host"]
    }, exitStatus: { 4 }, log: URL(fileURLWithPath: "/unused/agent.log"), pause: { pauses += 1 })
    XCTAssertEqual(ready["host_socket"] as? String, "winner.host")
    XCTAssertEqual(pauses, 1)
  }

  func testOtherStartupFailuresAreStillReported() async {
    do {
      _ = try await AgentReadiness.wait(read: { nil }, exitStatus: { 1 }, log: URL(fileURLWithPath: "/unused/agent.log"),
                                       pause: { XCTFail("An actual startup failure must not wait") })
      XCTFail("The failed launch must be reported")
    } catch let error as CoreAgentFailure {
      XCTAssertEqual(error.code, "unavailable")
      XCTAssertTrue(error.message.contains("/unused/agent.log"))
    } catch { XCTFail("Unexpected error: \(error)") }
  }
}
