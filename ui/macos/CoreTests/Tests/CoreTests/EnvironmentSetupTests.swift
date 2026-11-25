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

  func testHostStateFromAnotherVersionLeavesRecoveryToTheProjectChooser() async {
    let issue = HostStateFormatMismatch(
      path: "/state/host.sqlite", found: 0,
      recoveryURL: URL(string: "http://127.0.0.1:1/?key=secret-key"), recoveryToken: "secret-token")
    let status = await EnvironmentManager.SetupStatus.query { throw issue }
    XCTAssertEqual(status, .hostStateFormatMismatch)
    XCTAssertTrue(status.ready)
    let shown = "\(issue)"
    XCTAssertTrue(shown.contains("/state/host.sqlite"))
    XCTAssertFalse(shown.contains("secret-key"))
    XCTAssertFalse(shown.contains("secret-token"))
  }

  func testHostLockStartupFailureShowsTheHolderAndHowToRetry() async {
    let status = await EnvironmentManager.SetupStatus.query {
      try CoreHostFailure.checkStartup([
        "code": "host_state_locked", "message": "Internal diagnostic",
        "lock_holder": ["pid": 2345, "application": "Tsugumi Dev"],
      ])
      return Tsugumi_Host_HostRuntimeT()
    }
    let shown = String(format: String(localized: "%@（PID %@）がホストの状態フォルダを使用しています。そのプロセスを終了してから再試行してください。"), "Tsugumi Dev", "2345")
    XCTAssertEqual(status, .failed(shown))
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
