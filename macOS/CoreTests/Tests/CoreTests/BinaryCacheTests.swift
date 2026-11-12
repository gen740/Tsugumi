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

/// Settings the launcher cannot use never stop the VM and never apply in part:
/// the snapshot is empty, earlier credentials are gone from it, and the reason
/// is readable.
@Test func unusableSettingsLeaveAnEmptySnapshotAndAReadableReason() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  defer { try? FileManager.default.removeItem(at: root) }
  let snapshot = root.appendingPathComponent("vm/binary-cache-config")
  let settings = root.appendingPathComponent("binary-caches.json")
  var good = cache()
  good.username = "token"; good.password = "secret-for-test"
  try BinaryCaches.save([good], support: root)
  #expect(try BinaryCaches.prepare(support: root, destination: snapshot) == nil)
  #expect(try !Data(contentsOf: snapshot.appendingPathComponent("netrc")).isEmpty)

  // One valid connection beside one whose key is not a key: neither applies.
  var bad = cache("https://other.example.org/team")
  bad.publicKey = "research:not-a-key"
  let mixed = try JSONEncoder().encode([good, bad])
  let truncated = mixed.prefix(mixed.count / 2)
  for (contents, expected) in [(mixed, "signing public key"), (Data(truncated), "cannot be read"), (Data("{}".utf8), "cannot be read")] {
    try contents.write(to: settings)
    #expect(throws: (any Error).self) { try BinaryCaches.load(support: root) }
    let problem = try #require(try BinaryCaches.prepare(support: root, destination: snapshot))
    #expect(problem.contains(expected), "\(problem)")
    #expect(problem.contains("Only the default caches are used"))
    #expect(!problem.contains(good.password))
    for name in ["nix.conf", "netrc"] {
      let file = snapshot.appendingPathComponent(name)
      #expect(try Data(contentsOf: file).isEmpty)
      #expect((try FileManager.default.attributesOfItem(atPath: file.path)[.posixPermissions] as? NSNumber)?.intValue == 0o600)
    }
    // The next valid settings apply again.
    try BinaryCaches.save([good], support: root)
    #expect(try BinaryCaches.prepare(support: root, destination: snapshot) == nil)
    #expect(try String(contentsOf: snapshot.appendingPathComponent("nix.conf"), encoding: .utf8).contains("https://cache.example.org/team"))
  }
}

/// Nix itself reads the snapshot this launcher writes: macOS/tools/test-binary-cache.py
/// serves a signed path from a private HTTPS cache in temporary stores, and gets it
/// only with the generated nix.conf and netrc. Needs nix, python3 and openssl on PATH.
@Test func nixDownloadsFromAPrivateCacheWithTheGeneratedSnapshot() throws {
  let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
  defer { try? FileManager.default.removeItem(at: root) }
  let script = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("tools/test-binary-cache.py")
  let errors = root.appendingPathComponent("stderr")
  FileManager.default.createFile(atPath: errors.path, contents: nil)
  let probe = Process()
  probe.executableURL = URL(fileURLWithPath: "/usr/bin/env")
  probe.arguments = ["python3", script.path, "--interactive"]
  let input = Pipe(), output = Pipe()
  probe.standardInput = input
  probe.standardOutput = output
  probe.standardError = try FileHandle(forWritingTo: errors)
  try probe.run()
  defer { if probe.isRunning { probe.terminate() } }
  func said() -> String { (try? String(contentsOf: errors, encoding: .utf8)) ?? "" }

  // The private cache the probe started, as a person would enter it in Settings.
  var line = Data()
  while !line.contains(UInt8(ascii: "\n")) {
    let more = output.fileHandleForReading.availableData
    if more.isEmpty { break }
    line.append(more)
  }
  let offered = try #require(try? JSONDecoder().decode([String: String].self, from: line), "no cache from the probe: \(said())")
  var c = BinaryCache()
  c.name = "Probe"
  c.url = try #require(offered["url"])
  c.publicKey = try #require(offered["publicKey"])
  c.username = try #require(offered["username"])
  c.password = try #require(offered["password"])
  let snapshot = root.appendingPathComponent("vm/binary-cache-config")
  try BinaryCaches.save([c], support: root)
  #expect(try BinaryCaches.prepare(support: root, destination: snapshot) == nil)
  try input.fileHandleForWriting.write(contentsOf: Data((snapshot.path + "\n").utf8))
  try input.fileHandleForWriting.close()
  let rest = output.fileHandleForReading.readDataToEndOfFile()
  probe.waitUntilExit()
  #expect(probe.terminationStatus == 0, "\(said())")
  #expect(String(decoding: rest, as: UTF8.self).contains("private cache used only with the snapshot's credentials"))
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
