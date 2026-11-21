import Foundation
import Testing
@testable import TsugumiApp
import TsugumiRPC

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
    let module = Tsugumi_Data_DisplayModuleT()
    module.module = "/nix/store/00000000000000000000000000000000-view"
    module.configJson = "{}"
    let a = Tsugumi_Data_DisplayBlobT(); a.name = "same"
    let b = Tsugumi_Data_DisplayBlobT(); b.name = "same"
    module.blobs = [a,b]
    let wire = Tsugumi_Data_DisplayT()
    wire.value = Tsugumi_Data_DisplayValueUnion(module, type: .displaymodule)
    let display = try #require(recordedDisplay(wire))
    if case .unavailable = display.value {} else { Issue.record("Duplicate blob names were accepted") }
  }

}
