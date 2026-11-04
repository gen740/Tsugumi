import Foundation
import Testing
@testable import TsugumiApp
@testable import TsugumiLauncher

@Test func versionsCompareByTheirNumbers() {
  #expect(UpdateCheck.components("v0.1.0") == [0, 1, 0] && UpdateCheck.components("1.2.3") == [1, 2, 3])
  #expect(UpdateCheck.components("1.2") == nil && UpdateCheck.components("1.2.3-rc1") == nil && UpdateCheck.components("v1..3") == nil)
  #expect(UpdateCheck.isNewer("v0.10.0", than: "0.9.9"))
  #expect(UpdateCheck.isNewer("v1.0.0", than: "0.99.99"))
  #expect(!UpdateCheck.isNewer("v0.1.0", than: "0.1.0"))
  #expect(!UpdateCheck.isNewer("v0.1.0", than: "0.2.0"))
  #expect(!UpdateCheck.isNewer("nightly", than: "0.1.0"))
}

private func release(tag: String, draft: Bool = false, prerelease: Bool = false, assets: [String]) -> Data {
  let list = assets.map { ["name": $0, "browser_download_url": "https://github.com/gen740/Tsugumi/releases/download/\(tag)/\($0)"] }
  return try! JSONSerialization.data(withJSONObject: ["tag_name": tag, "draft": draft, "prerelease": prerelease,
                                                      "html_url": "https://github.com/gen740/Tsugumi/releases/tag/\(tag)",
                                                      "assets": list])
}

@Test func aNewerReleaseWithTheAppAndItsChecksumIsOffered() throws {
  let both = ["Tsugumi.zip", "Tsugumi.zip.sha256"]
  let offer = try #require(try UpdateCheck.offer(release(tag: "v0.2.0", assets: both), current: "0.1.0"))
  #expect(offer.version == "0.2.0")
  #expect(offer.zip.lastPathComponent == "Tsugumi.zip" && offer.checksum.lastPathComponent == "Tsugumi.zip.sha256")
  // the same or an older version, drafts, pre-releases and releases lacking a file are not
  #expect(try UpdateCheck.offer(release(tag: "v0.1.0", assets: both), current: "0.1.0") == nil)
  #expect(try UpdateCheck.offer(release(tag: "v0.2.0", draft: true, assets: both), current: "0.1.0") == nil)
  #expect(try UpdateCheck.offer(release(tag: "v0.2.0", prerelease: true, assets: both), current: "0.1.0") == nil)
  #expect(try UpdateCheck.offer(release(tag: "v0.2.0", assets: ["Tsugumi.zip"]), current: "0.1.0") == nil)
}

@Test func checksumsAreReadFromShasumLines() throws {
  let h = String(repeating: "ab", count: 32)
  #expect(UpdateCheck.checksum(in: "\(h)  Tsugumi.zip\n", for: "Tsugumi.zip") == h)
  #expect(UpdateCheck.checksum(in: "\(h.uppercased()) *Tsugumi.zip", for: "Tsugumi.zip") == h)
  #expect(UpdateCheck.checksum(in: h, for: "Tsugumi.zip") == h)
  #expect(UpdateCheck.checksum(in: "\(h)  Other.zip", for: "Tsugumi.zip") == nil)
  #expect(UpdateCheck.checksum(in: "abc  Tsugumi.zip", for: "Tsugumi.zip") == nil)
  let file = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-sha-\(UUID().uuidString)")
  try Data("abc".utf8).write(to: file)
  defer { try? FileManager.default.removeItem(at: file) }
  #expect(try UpdateCheck.sha256(of: file) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
}

@Test func onlyTheReleaseOfTheSameAppAndVersionIsInstalled() throws {
  let app = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-update-\(UUID().uuidString)/Tsugumi.app")
  try FileManager.default.createDirectory(at: app.appendingPathComponent("Contents"), withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: app.deletingLastPathComponent()) }
  func write(_ info: [String: String]) throws {
    try (info as NSDictionary).write(to: app.appendingPathComponent("Contents/Info.plist"))
  }
  let good = ["CFBundleIdentifier": "app.tsugumi.Tsugumi", "TsugumiChannel": "release", "CFBundleShortVersionString": "0.2.0"]
  try write(good)
  try UpdateCheck.checkBundle(app, version: "0.2.0", bundleID: "app.tsugumi.Tsugumi")
  #expect(throws: UpdateCheck.Problem.self) { try UpdateCheck.checkBundle(app, version: "0.3.0", bundleID: "app.tsugumi.Tsugumi") }
  #expect(throws: UpdateCheck.Problem.self) { try UpdateCheck.checkBundle(app, version: "0.2.0", bundleID: "app.tsugumi.Tsugumi.dev") }
  var dev = good
  dev["TsugumiChannel"] = "dev"
  try write(dev)
  #expect(throws: UpdateCheck.Problem.self) { try UpdateCheck.checkBundle(app, version: "0.2.0", bundleID: "app.tsugumi.Tsugumi") }
}

@MainActor @Test func devAndReleaseBuildsKeepTheirVMsApart() throws {
  #expect(AppChannel.release.supportDirectory.lastPathComponent == "Tsugumi")
  #expect(AppChannel.dev.supportDirectory.lastPathComponent == "Tsugumi-dev")
  // a bundle without TsugumiChannel (swift run, tests) is dev
  #expect(AppChannel(bundle: Bundle(for: BundleToken.self)) == .dev)
  // a dev build never updates itself
  #expect(Updater(channel: .dev).unavailable != nil)
}

private final class BundleToken {}
