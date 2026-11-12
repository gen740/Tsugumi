import Foundation
import Darwin

/// Trusted binary caches belong to the human's VM settings, never a project.
public struct BinaryCache: Codable, Identifiable, Equatable, Sendable {
  public var id = UUID()
  public var name = ""
  public var url = ""
  public var publicKey = ""
  public var username = ""
  public var password = ""
  public init() {}
}

public enum BinaryCaches {
  public struct Invalid: LocalizedError {
    let message: String
    public var errorDescription: String? { message }
  }

  public static func validate(_ caches: [BinaryCache]) throws {
    guard caches.count <= 32 else { throw Invalid(message: "At most 32 binary caches may be configured.") }
    guard Set(caches.map(\.id)).count == caches.count else { throw Invalid(message: "Each cache must have a distinct identity.") }
    var urls = Set<String>()
    var credentials: [String: String] = [:]
    for cache in caches {
      guard !cache.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, cache.name.count <= 80 else {
        throw Invalid(message: "Each cache needs a name of at most 80 characters.")
      }
      guard let u = URLComponents(string: cache.url), let host = u.host, !host.isEmpty,
            ["https", "http"].contains(u.scheme), u.user == nil, u.password == nil,
            u.query == nil, u.fragment == nil,
            !cache.url.contains(where: { $0.isWhitespace || $0.isNewline || $0 == "#" }),
            cache.url.unicodeScalars.allSatisfy({ $0.value >= 32 && $0.value != 127 }),
            urls.insert(cache.url).inserted else {
        throw Invalid(message: "Use distinct HTTP(S) cache endpoints without credentials, query strings or whitespace.")
      }
      let key = cache.publicKey.split(separator: ":", omittingEmptySubsequences: false)
      guard key.count == 2, !key[0].isEmpty,
            key[0].allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || "._+-".contains($0)) }),
            Data(base64Encoded: String(key[1]))?.count == 32 else {
        throw Invalid(message: "Each cache needs its complete Nix signing public key (name:base64).")
      }
      guard cache.username.isEmpty == cache.password.isEmpty else {
        throw Invalid(message: "Provide both an authentication username and password, or leave both empty.")
      }
      if !cache.password.isEmpty {
        guard u.scheme == "https", [cache.username, cache.password, host].allSatisfy({ text in
          text.utf8.allSatisfy { $0 > 32 && $0 < 127 && $0 != 34 && $0 != 92 && $0 != 35 }
        }) else { throw Invalid(message: "Cache credentials require HTTPS and printable values without whitespace, quotes or backslashes.") }
        let credential = "\(cache.username)\n\(cache.password)"
        if let previous = credentials[host], previous != credential {
          throw Invalid(message: "Caches on the same host must use the same authentication credentials.")
        }
        credentials[host] = credential
      }
    }
  }

  public static func load(support: URL) throws -> [BinaryCache] {
    let file = support.appendingPathComponent("binary-caches.json")
    guard FileManager.default.fileExists(atPath: file.path) else { return [] }
    let caches = try JSONDecoder().decode([BinaryCache].self, from: Data(contentsOf: file))
    try validate(caches)
    return caches
  }

  public static func save(_ caches: [BinaryCache], support: URL) throws {
    try validate(caches)
    try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    let file = support.appendingPathComponent("binary-caches.json")
    try writePrivate(JSONEncoder().encode(caches), to: file)
  }

  /// Freeze the next boot's settings separately from the editable configuration.
  /// The guest mounts this read-only under /run/tsugumi, hidden from project views.
  ///
  /// Settings that cannot be read or do not validate never stop the VM: the
  /// snapshot is then written empty (none of the connections, not the valid
  /// part of them) and the reason is returned for the person to read. That
  /// leaves the pinned default caches, so it only loses downloads; no key is
  /// trusted and no credential is sent that the settings did not validate. A
  /// snapshot that cannot be written still throws: an older one, possibly with
  /// credentials since removed, must not be shared in its place.
  @discardableResult
  public static func prepare(support: URL, destination: URL) throws -> String? {
    var caches: [BinaryCache] = []
    var problem: String?
    do { caches = try load(support: support) }
    catch {
      let reason = (error as? Invalid)?.message ?? "binary-caches.json cannot be read (\(error.localizedDescription))"
      problem = "The binary cache settings were not applied: \(reason) Only the default caches are used until they are saved again in Settings."
    }
    try FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
    var conf = ""
    if !caches.isEmpty {
      conf += "extra-substituters = \(caches.map(\.url).joined(separator: " "))\n"
      conf += "extra-trusted-public-keys = \(caches.map(\.publicKey).joined(separator: " "))\n"
      conf += "netrc-file = /run/tsugumi/binary-caches/netrc\n"
    }
    var hosts = Set<String>()
    let netrc = caches.compactMap { cache -> String? in
      guard !cache.password.isEmpty, let host = URLComponents(string: cache.url)?.host, hosts.insert(host).inserted else { return nil }
      return "machine \(host) login \(cache.username) password \(cache.password)\n"
    }.joined()
    for (name, text) in [("nix.conf", conf), ("netrc", netrc)] {
      let file = destination.appendingPathComponent(name)
      try writePrivate(Data(text.utf8), to: file)
    }
    return problem
  }

  /// Restrict access before writing any secret, including the atomic temporary file.
  private static func writePrivate(_ data: Data, to file: URL) throws {
    let temporary = file.deletingLastPathComponent().appendingPathComponent(".cache-\(UUID().uuidString)")
    let fd = open(temporary.path, O_WRONLY | O_CREAT | O_EXCL, 0o600)
    guard fd >= 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
    let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
    defer { try? handle.close(); try? FileManager.default.removeItem(at: temporary) }
    try handle.write(contentsOf: data)
    try handle.close()
    guard rename(temporary.path, file.path) == 0 else {
      throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }
  }
}
