import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiRPC

private let uuid = "0f8c2a4e-5b1d-4c3e-9a7f-2d6b8e1c4f90"

@Test func anArtifactIsNamedByItsUuidWithItsLabelNextToIt() {
  #expect(ArtifactText.name(id: uuid, label: "3.1") == uuid + " (3.1)")
  #expect(ArtifactText.name(id: uuid, label: nil) == uuid)
  #expect(ArtifactText.name(id: uuid, label: "") == uuid)
}

@Test func aValueShowsEachArtifactWithWhatItResolvesToNow() {
  func artifact(_ state: Tsugumi_Data_ArtifactState, label: String? = "2.1") -> Tsugumi_Data_ValueArtifactT {
    let a = Tsugumi_Data_ValueArtifactT()
    a.id = uuid
    a.label = label
    a.state = state
    a.sizeBytes = state == .available ? 12 : nil
    return a
  }
  let prefix = String(localized: "成果物 ")
  #expect(ArtifactText.line(artifact(.available)) == prefix + "\(uuid) (2.1) · " + String(localized: "利用できます") + " · 12 bytes")
  // Not kept, or an id from elsewhere: nothing is recorded under it, so no label either.
  #expect(ArtifactText.line(artifact(.unavailable, label: nil)) == prefix + uuid + " · " + String(localized: "利用できません（実行後に残していないか、不明な ID）"))

  let v = Tsugumi_Data_NamedValueT()
  v.name = "out"
  v.rootType = "Result"
  v.artifacts = [artifact(.available), artifact(.unavailable, label: nil)]
  let lines = describeValue(v).components(separatedBy: "\n")
  #expect(lines[1] == ArtifactText.line(artifact(.available)))
  #expect(lines[2] == ArtifactText.line(artifact(.unavailable, label: nil)))
}

@Test func aListedArtifactSaysWhetherItExpires() {
  let a = Tsugumi_RPC_ArtifactInfoT()
  a.artifactId = uuid
  a.label = "4.2"
  a.invocation = 4
  a.keep = true
  a.sizeBytes = 9
  let member = Tsugumi_RPC_ArtifactMemberT()
  member.path = "a.txt"
  a.members = [member]
  let files = String(localized: "\(1) 件")
  #expect(ArtifactText.name(a) == uuid + " (4.2)")
  #expect(ArtifactText.caption(a) == "#4 · 9 bytes · " + files)
  a.keep = false
  #expect(ArtifactText.caption(a) == "#4 · " + String(localized: "残さない（実行の終了時に削除）") + " · 9 bytes · " + files)
}

/// demo.Result { name, id: tsugumi.ArtifactId, ids: [tsugumi.ArtifactId], at: demo.Point { x:int; y:short } },
/// written by flatc 25.12.19 (schema and value as base64).
@Test func structsInARecordedValueReadInlineAndAnArtifactIdAsItsUuid() throws {
  let v = Tsugumi_Data_NamedValueT()
  v.name = "out"
  v.rootType = "demo.Result"
  v.schemaBfbs = [UInt8](try #require(Data(base64Encoded: "HAAAAEJGQlMAABIAJAAEAAgADAAQABQAGAAcABIAAAA4AAAAMAAAACQAAAAYAAAATAAAAAwAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMAAABcAQAAHAAAACwCAAAUABQABAAIAAAADAAAAAAAAAAQABQAAAAkAAAADAAAAAEAAAAgAgAABAAAACAAAACgAAAARAAAAPAAAAALAAAAZGVtby5SZXN1bHQAmP///wAAAAEDAAoAGAAAAAQAAAB8////AAAADwAAAAABAAAAAgAAAGF0AADE////AAAAAQIACAAoAAAAFAAAABAAEAAGAAcACAAAAAAADAAQAAAAAAAODwIAAAAQAAAAAwAAAGlkcwAcABQADAAQAAgACgAAAAAAAAAAAAAAAAAAAAcAHAAAAAAAAAEBAAYAKAAAABQAAAAQABAABwAAAAgAAAAAAAwAEAAAAAAAAA8CAAAAAQAAAAIAAABpZAAAHAAQAAgADAAAAAYAAAAAAAAAAAAAAAAAAAAFABwAAAAAAQQAFAAAAAQAAABI////AAAADQEAAAAEAAAAbmFtZQAAAAA8////AAAAASAAAAAQAAAABAAAAAgAAADcAAAAAgAAAHQAAAA0AAAACgAAAGRlbW8uUG9pbnQAAAAAHgAUAAwAEAAGAAgAAAAAAAAAAAAAAAAAAAAAAAoAHgAAAAAAAQAEAAIAKAAAABQAAAAQABAABwAAAAAAAAAIAAwAEAAAAAAAAAUCAAAAAQAAAAEAAAB5AAAAeP///yQAAAAUAAAAEAAMAAcAAAAAAAAAAAAIABAAAAAAAAAHAQAAAAEAAAB4AAAAFAAcAAgADAAHABAAFAAAAAAAGAAUAAAAAAAAASQAAAAYAAAAAQAAABAAAAAEAAAAAAAAAAAAAAABAAAAJAAAABIAAAB0c3VndW1pLkFydGlmYWN0SWQAAAgADAAEAAgACAAAACQAAAAUAAAAEAAMAAQABQAAAAYAAAAIABAAAAARBBAAAQAAAAUAAABieXRlcwAAAA==")))
  v.inlineBytes = [UInt8](try #require(Data(base64Encoded: "EAAAAAwAJAAEAAgAGAAcAAwAAAA0AAAAD4wqTlsdTD6afy1rjhxPkAwAAAD9////BwAAAAEAAAAAAQIDBAUGBwgJCgsMDQ4PAQAAAHIAAAA=")))
  let text = describeValue(v)
  let json = try #require(text.firstIndex(of: "{").map { String(text[$0...]) })
  let object = try #require(try JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any])
  #expect(object["id"] as? String == uuid)
  #expect(object["ids"] as? [String] == ["00010203-0405-0607-0809-0a0b0c0d0e0f"])
  let at = try #require(object["at"] as? [String: Any])
  #expect(at["x"] as? Int == -3 && at["y"] as? Int == 7)
  #expect(object["name"] as? String == "r")
}
