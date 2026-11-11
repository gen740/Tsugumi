import Foundation
import Testing
import TsugumiLauncher

private func cache(_ url: String = "https://cache.example.org/team") -> BinaryCache {
  var value = BinaryCache()
  value.name = "Research"
  value.url = url
  value.publicKey = "research:" + Data(repeating: 7, count: 32).base64EncodedString()
  return value
}

@Test func binaryCachesRejectInjectedSettingsAndCredentials() throws {
  for url in ["file:///tmp/cache", "https://user:secret@example.org", "https://example.org?token=x", "https://example.org\ntrusted-users = *", "https://example.org/#fragment", "https://example.org/\u{0}tail"] {
    #expect(throws: (any Error).self) { try BinaryCaches.validate([cache(url)]) }
  }
  var c = cache()
  c.publicKey += "\ntrusted-users = *"
  #expect(throws: (any Error).self) { try BinaryCaches.validate([c]) }
  c = cache("http://cache.example.org/team")
  c.username = "token"; c.password = "secret"
  #expect(throws: (any Error).self) { try BinaryCaches.validate([c]) }
  c.url = "https://cache.example.org/team"
  c.password = "secret\nmachine elsewhere password secret"
  #expect(throws: (any Error).self) { try BinaryCaches.validate([c]) }
  #expect(throws: (any Error).self) { try BinaryCaches.validate([cache(), cache()]) }
}

@Test func cacheCredentialsArePrivateAndFrozenUntilNextBoot() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let snapshot = root.appendingPathComponent("vm/binary-cache-config")
  var c = cache()
  c.username = "token"; c.password = "secret-for-test"
  try BinaryCaches.save([c], support: root)
  #expect(try BinaryCaches.load(support: root) == [c])
  try BinaryCaches.prepare(support: root, destination: snapshot)
  let conf = try String(contentsOf: snapshot.appendingPathComponent("nix.conf"), encoding: .utf8)
  let netrc = try String(contentsOf: snapshot.appendingPathComponent("netrc"), encoding: .utf8)
  #expect(conf.contains("extra-substituters = https://cache.example.org/team"))
  #expect(conf.contains("extra-trusted-public-keys = research:"))
  #expect(!conf.contains(c.password))
  #expect(netrc == "machine cache.example.org login token password secret-for-test\n")
  for file in [root.appendingPathComponent("binary-caches.json"), snapshot.appendingPathComponent("nix.conf"), snapshot.appendingPathComponent("netrc")] {
    #expect((try FileManager.default.attributesOfItem(atPath: file.path)[.posixPermissions] as? NSNumber)?.intValue == 0o600)
  }
  try BinaryCaches.save([], support: root)
  #expect(try String(contentsOf: snapshot.appendingPathComponent("netrc"), encoding: .utf8) == netrc)
  try BinaryCaches.prepare(support: root, destination: snapshot)
  #expect(try Data(contentsOf: snapshot.appendingPathComponent("netrc")).isEmpty)
  #expect(try Data(contentsOf: snapshot.appendingPathComponent("nix.conf")).isEmpty)
}

@Test func cachesOnOneHostHaveOneCredential() throws {
  var a = cache("https://attic.example.org/one")
  a.username = "token"; a.password = "first"
  var b = cache("https://attic.example.org/two")
  b.username = "token"; b.password = "second"
  #expect(throws: (any Error).self) { try BinaryCaches.validate([a, b]) }
  b.password = a.password
  try BinaryCaches.validate([a, b])
}
