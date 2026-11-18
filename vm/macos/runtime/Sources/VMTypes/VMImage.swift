import Foundation

/// One release-built guest archive and its manifest.
public struct VMImage: Sendable, Equatable {
  public let directory: URL
  public var archive: URL { directory.appendingPathComponent("tsugumi-vm.tar.gz") }
  public let version: String
  public let sha256: String
  public let checksums: [String: String]

  private struct Manifest: Decodable {
    let identity: String
    let system: String
    let files: [String: String]
    let members: [String: String]
  }

  public init?(directory: URL) {
    guard let data = try? Data(contentsOf: directory.appendingPathComponent("vm-manifest.json")),
          let manifest = try? JSONDecoder().decode(Manifest.self, from: data),
          manifest.system == "aarch64-linux", !manifest.identity.isEmpty,
          let hash = manifest.files["tsugumi-vm.tar.gz"], hash.count == 64,
          Set(manifest.members.keys) == Set(["vmlinux", "rootfs.erofs", "vm-version"]),
          manifest.members.values.allSatisfy({ $0.count == 64 }),
          FileManager.default.fileExists(atPath: directory.appendingPathComponent("tsugumi-vm.tar.gz").path) else { return nil }
    self.directory = directory
    version = manifest.identity
    sha256 = hash
    checksums = manifest.members
  }

  public static func find(bundle: Bundle = .main) -> VMImage? {
    if let dir = bundle.url(forResource: "tsugumi-vm", withExtension: nil),
       let image = VMImage(directory: dir) { return image }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<6 { repo.deleteLastPathComponent() }
    return VMImage(directory: repo.appendingPathComponent("dist/tsugumi-vm"))
    #else
    return nil
    #endif
  }
}

/// Size of the shared VM (all projects share it).
public struct VMResources: Sendable, Equatable {
  public var cpus: Int
  public var memoryGiB: Int
  public var storeGiB: Int

  /// Half the Mac's memory (4–16 GiB), all but two cores (2–8).
  public static var `default`: VMResources {
    let mem = Int(ProcessInfo.processInfo.physicalMemory >> 30)
    let cores = ProcessInfo.processInfo.activeProcessorCount
    return VMResources(cpus: min(8, max(2, cores - 2)), memoryGiB: min(16, max(4, mem / 2)), storeGiB: 64)
  }
}


