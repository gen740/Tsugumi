import Foundation

public struct VMImage: Sendable, Equatable {
  public static let files = ["vmlinux", "rootfs.erofs"]
  public let kernel: URL
  public let rootfs: URL
  /// The source folder; `sourceEntries` lists what of it the VM gets (all of
  /// it when nil, as in the bundle, which holds only those).
  public let source: URL
  public let sourceEntries: [String]?
  public let version: String

  public init?(directory: URL, source: URL, sourceEntries: [String]? = nil) {
    let urls = Self.files.map { directory.appendingPathComponent($0) }
    guard urls.allSatisfy({ FileManager.default.fileExists(atPath: $0.path) }),
          FileManager.default.fileExists(atPath: source.appendingPathComponent("flake.nix").path) else { return nil }
    kernel = urls[0]
    rootfs = urls[1]
    self.source = source
    self.sourceEntries = sourceEntries
    version = (try? String(contentsOf: directory.appendingPathComponent("vm-version"), encoding: .utf8))?
      .trimmingCharacters(in: .whitespacesAndNewlines) ?? "dev"
  }

  public static func find(bundle: Bundle = .main) -> VMImage? {
    if let dir = bundle.url(forResource: "tsugumi-vm", withExtension: nil),
       let source = bundle.url(forResource: "tsugumi-source", withExtension: nil),
       let image = VMImage(directory: dir, source: source) { return image }
    #if DEBUG
    // A development build (tsugumi-vmctl, tests) uses the repository.
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<4 { repo.deleteLastPathComponent() }
    return VMImage(directory: repo.appendingPathComponent("dist/tsugumi-vm"), source: repo,
                   sourceEntries: sourceList(repo.appendingPathComponent("macOS/Container/vm-source.txt")))
    #else
    return nil
    #endif
  }

  /// macOS/Container/vm-source.txt: one path per line, `#` comments.
  static func sourceList(_ url: URL) -> [String]? {
    guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
    return text.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty && !$0.hasPrefix("#") }
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


/// Installation details passed to agentd; this never launches a helper or VM.
public enum HostRuntimeConfiguration {
  public static func arguments(support: URL, image: VMImage? = VMImage.find(), resources: VMResources = .default) -> [String] {
    let environment = ProcessInfo.processInfo.environment
    let sibling = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent().appendingPathComponent("tsugumi-vmhost")
    let helper = environment["TSUGUMI_VMHOST"].map { URL(fileURLWithPath: $0) }
      ?? Bundle.main.url(forAuxiliaryExecutable: "tsugumi-vmhost")
      ?? (FileManager.default.isExecutableFile(atPath: sibling.path) ? sibling : nil)
    guard let helper else { return [] }
    #if DEBUG
    var repo = URL(fileURLWithPath: #filePath)
    for _ in 0..<4 { repo.deleteLastPathComponent() }
    #else
    let repo = Bundle.main.bundleURL
    #endif
    let directory = image?.kernel.deletingLastPathComponent()
      ?? environment["TSUGUMI_VM_IMAGE"].map { URL(fileURLWithPath: $0) }
      ?? Bundle.main.url(forResource: "tsugumi-vm", withExtension: nil) ?? repo.appendingPathComponent("dist/tsugumi-vm")
    let source = image?.source ?? environment["TSUGUMI_VM_SOURCE"].map { URL(fileURLWithPath: $0) }
      ?? Bundle.main.url(forResource: "tsugumi-source", withExtension: nil) ?? repo
    var arguments = ["--vmhost", helper.path, "--vm-support", support.path, "--vm-image", directory.path, "--vm-source", source.path,
      "--vm-cpus", "\(resources.cpus)", "--vm-memory", "\(resources.memoryGiB)", "--vm-store", "\(resources.storeGiB)"]
    for entry in image?.sourceEntries ?? [] { arguments += ["--vm-source-entry", entry] }
    return arguments
  }
}
