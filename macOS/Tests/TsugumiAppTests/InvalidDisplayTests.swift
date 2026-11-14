import Foundation
import FlatBuffers
import Testing
@testable import TsugumiApp
import TsugumiRPC

struct InvalidDisplayTests {
  @Test func malformedPresentationHasAnExplicitState() throws {
    let values: [DisplayValue] = [
      .scalar(.nan, "value"), .scalar(.infinity, "value"),
      .table(columns: [], rows: []), .table(columns: ["one"], rows: [["one", "two"]]),
      .image(mediaType: "image/png", data: Data([1,2,3]), alt: ""),
      .points(x: [0], y: [], z: [0], colors: []),
      .module(name: "/nix/store/00000000000000000000000000000000-view", json: "{", blobs: [:])
    ]
    for value in values {
      let display = DisplaySnapshot(runId: "run", invocation: 1, revision: 0, value: value)
      if case .unavailable = display.value {} else { Issue.record("Malformed presentation was accepted") }
      #expect(!display.summary.isEmpty)
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

  @Test func invalidUTF8DoesNotReachTheGeneratedStringInitializer() throws {
    var builder = FlatBufferBuilder(initialSize: 128)
    let text = builder.create(string: "x")
    let content = Tsugumi_Data_DisplayText.createDisplayText(&builder, textOffset: text)
    let segments = builder.createVector(ofStrings: ["card"])
    let card = Tsugumi_Data_CardPath.createCardPath(&builder, segmentsVectorOffset: segments)
    let run = builder.create(string: "run")
    let display = Tsugumi_Data_Display.createDisplay(&builder, cardOffset: card, runIdOffset: run,
      invocation: 1, revision: 0, valueType: .displaytext, valueOffset: content)
    builder.finish(offset: display)
    var bytes = builder.data
    // The sole 'x' is the display string; its length and terminator stay valid.
    let index = try #require(bytes.firstIndex(of: UInt8(ascii: "x")))
    bytes[index] = 255
    var buffer = ByteBuffer(data: bytes)
    let wire: Tsugumi_Data_Display = try getCheckedRoot(byteBuffer: &buffer)
    let decoded = unpackDisplay(wire)
    if case .unavailable? = recordedDisplay(decoded)?.value {} else { Issue.record("Invalid UTF-8 was displayed") }
  }
}
