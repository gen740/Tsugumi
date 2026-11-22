import Foundation
import Testing
@testable import TsugumiApp

struct InvalidDisplayTests {
  @Test func malformedPresentationHasAnExplicitState() throws {
    let values: [DisplayValue] = [
      .scalar(.nan, "value"),
      .table(columns: [], rows: []), .table(columns: ["one"], rows: [["one", "two"]]),
      .image(mediaType: "image/png", data: Data([1,2,3]), alt: ""),
      .points(x: [0], y: [], z: [0], colors: [])
    ]
    for value in values {
      let display = DisplaySnapshot(runId: "run", invocation: 1, revision: 0, value: value)
      if case .unavailable = display.value {} else { Issue.record("Malformed presentation was accepted") }
    }
  }

}
