import Testing
@testable import TsugumiApp

@Test func recordedStorePathsAreShownByPackageName() {
  let hash = String(repeating: "a", count: 32)
  #expect(recordedStoreName("/nix/store/\(hash)-python-3.13") == "python-3.13")
  // Anything that is not a store hash and a name is shown as it is.
  #expect(recordedStoreName("/nix/store/short-name") == "short-name")
  #expect(recordedStoreName("/nix/store/\(String(repeating: "A", count: 32))-upper") == "\(String(repeating: "A", count: 32))-upper")
  #expect(recordedStoreName("") == "")
}
