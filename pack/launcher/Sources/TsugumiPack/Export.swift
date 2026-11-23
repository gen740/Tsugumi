import Foundation

/// `tsugumi-pack PROJECT --output NAME.app`: builds every Card of the Project in
/// the pack guest and writes an app that runs it offline.
struct Export {
  var project: URL
  var output: URL
  var guest: URL
  var cache: URL
  var cpus: Int
  var memoryGiB: Int
  var storeGiB: Int
  var signIdentity: String
  var verbose: Bool

  static let usage = """
    usage: tsugumi-pack PROJECT --output NAME.app [--guest DIR] [--cache DIR]
                        [--cpus N] [--memory GiB] [--store-size GiB] [--sign IDENTITY] [--verbose]

    Builds every Card implementation of PROJECT for the pack guest and writes an
    app that runs the Workflow offline, with no Tsugumi or Nix installed.

      --guest DIR       the pack guest (vmlinux, export.erofs, run.erofs); default: next to
                        this command (../share/tsugumi-pack/guest) or $TSUGUMI_PACK_GUEST
      --cache DIR       the exporter's work folder and store disk, kept between
                        exports (default ~/Library/Caches/tsugumi-pack)
      --sign IDENTITY   code signing identity for the app (default:
                        $TSUGUMI_CODESIGN_IDENTITY, else ad hoc)
    """

  static func parse(_ arguments: [String]) throws -> Export {
    var project: String?
    var output: String?
    var guest = ProcessInfo.processInfo.environment["TSUGUMI_PACK_GUEST"]
    var cache: String?
    var cpus = GuestBoot.defaultCPUs()
    var memory = 8
    var store = 64
    var sign = ProcessInfo.processInfo.environment["TSUGUMI_CODESIGN_IDENTITY"] ?? "-"
    var verbose = false
    var i = 0
    func value() throws -> String {
      i += 1
      guard i < arguments.count else { throw PackError("\(arguments[i - 1]) needs a value") }
      return arguments[i]
    }
    while i < arguments.count {
      switch arguments[i] {
      case "--output", "-o": output = try value()
      case "--guest": guest = try value()
      case "--cache": cache = try value()
      case "--cpus": cpus = Int(try value()) ?? cpus
      case "--memory": memory = Int(try value()) ?? memory
      case "--store-size": store = Int(try value()) ?? store
      case "--sign": sign = try value()
      case "--verbose", "-v": verbose = true
      case let a where a.hasPrefix("-"): throw PackError("unknown option \(a)\n\n\(usage)")
      case let a:
        guard project == nil else { throw PackError("one Project at a time\n\n\(usage)") }
        project = a
      }
      i += 1
    }
    guard let project, let output else { throw PackError(usage) }
    guard output.hasSuffix(".app") else { throw PackError("--output must name an .app") }
    let exe = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath().deletingLastPathComponent()
    let guestURL = guest.map { URL(fileURLWithPath: $0) }
      ?? exe.appendingPathComponent("../share/tsugumi-pack/guest").standardized
    let cacheURL = cache.map { URL(fileURLWithPath: $0) }
      ?? FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("tsugumi-pack")
    return Export(project: URL(fileURLWithPath: project).standardized, output: URL(fileURLWithPath: output).standardized,
                  guest: guestURL, cache: cacheURL, cpus: cpus, memoryGiB: GuestBoot.defaultMemoryGiB(memory),
                  storeGiB: store, signIdentity: sign, verbose: verbose)
  }

  func run() throws {
    let fm = FileManager.default
    let started = Date()
    guard fm.fileExists(atPath: project.appendingPathComponent("tsugumi.json").path) else {
      throw PackError("\(project.path) is not a Tsugumi Project (no tsugumi.json)")
    }
    for name in ["vmlinux", "export.erofs", "run.erofs"] where !fm.fileExists(atPath: guest.appendingPathComponent(name).path) {
      throw PackError("the pack guest is missing \(name) in \(guest.path) (build it: pack/build.sh)")
    }
    try fm.createDirectory(at: cache, withIntermediateDirectories: true)
    // One export at a time uses the store disk.
    let lock = try FileLock(cache.appendingPathComponent("store.lock"))
    defer { lock.release() }
    let work = cache.appendingPathComponent("export")
    try? fm.removeItem(at: work)
    try fm.createDirectory(at: work.appendingPathComponent("out"), withIntermediateDirectories: true)
    try stageProject(into: work.appendingPathComponent("project"))
    let store = cache.appendingPathComponent("store.img")
    if !fm.fileExists(atPath: store.path) { try createSparseDisk(store, gib: storeGiB) }
    let console = work.appendingPathComponent("console.log")
    print("tsugumi-pack: building \(project.lastPathComponent) in the pack guest (\(cpus) CPUs, \(memoryGiB) GiB)")
    let boot = GuestBoot(mode: .export, kernel: guest.appendingPathComponent("vmlinux"),
                         rootfs: guest.appendingPathComponent("export.erofs"), disk: store, diskReadOnly: false,
                         share: work, network: true, cpus: cpus, memoryGiB: memoryGiB, consoleLog: console)
    try boot.run { line in
      if verbose || line.hasPrefix("PACK-") || line.hasPrefix("payload") || line.hasPrefix("built ")
        || line.hasPrefix("some Cards") || line.hasPrefix("  ") {
        print("  \(line)")
      }
    }
    let status = (try? String(contentsOf: work.appendingPathComponent("export.status"), encoding: .utf8))?
      .trimmingCharacters(in: .whitespacesAndNewlines)
    guard status == "0" else {
      throw PackError("the export failed (status \(status ?? "none")); console: \(console.path)")
    }
    let out = work.appendingPathComponent("out")
    let summary = try JSONSerialization.jsonObject(with: Data(contentsOf: out.appendingPathComponent("export.json"))) as? [String: Any] ?? [:]
    try writeApp(payload: out.appendingPathComponent("payload.erofs"), export: summary)
    try? fm.removeItem(at: work)
    let size = directorySize(output)
    print(String(format: "tsugumi-pack: wrote %@ (%.0f MiB) in %.0f s", output.path, Double(size) / 1048576, Date().timeIntervalSince(started)))
  }

  /// The Project's files without its state: run history and versions stay with the author.
  private func stageProject(into destination: URL) throws {
    let fm = FileManager.default
    try fm.createDirectory(at: destination, withIntermediateDirectories: true)
    for name in try fm.contentsOfDirectory(atPath: project.path) where ![".tsugumi", ".git", ".DS_Store"].contains(name) {
      try fm.copyItem(at: project.appendingPathComponent(name), to: destination.appendingPathComponent(name))
    }
  }

  private func writeApp(payload: URL, export: [String: Any]) throws {
    let fm = FileManager.default
    let name = output.deletingPathExtension().lastPathComponent
    let staging = output.deletingLastPathComponent().appendingPathComponent(".\(output.lastPathComponent).partial")
    try? fm.removeItem(at: staging)
    let contents = staging.appendingPathComponent("Contents")
    let resources = contents.appendingPathComponent("Resources")
    let macos = contents.appendingPathComponent("MacOS")
    try fm.createDirectory(at: resources, withIntermediateDirectories: true)
    try fm.createDirectory(at: macos, withIntermediateDirectories: true)
    let executable = URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
    try fm.copyItem(at: executable, to: macos.appendingPathComponent(name))
    try fm.copyItem(at: guest.appendingPathComponent("vmlinux"), to: resources.appendingPathComponent("vmlinux"))
    try fm.copyItem(at: guest.appendingPathComponent("run.erofs"), to: resources.appendingPathComponent("rootfs.erofs"))
    try fm.moveItem(at: payload, to: resources.appendingPathComponent("payload.erofs"))
    try writeManifest(to: resources.appendingPathComponent("pack.json"), name: name, export: export)
    let identifier = "org.tsugumi.pack." + name.lowercased().filter { $0.isLetter || $0.isNumber }
    let plist: [String: Any] = [
      "CFBundleName": name, "CFBundleDisplayName": name, "CFBundleExecutable": name,
      "CFBundleIdentifier": identifier, "CFBundlePackageType": "APPL",
      "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1",
      "LSMinimumSystemVersion": "14.0", "NSHighResolutionCapable": true,
    ]
    try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
      .write(to: contents.appendingPathComponent("Info.plist"))
    try sign(staging)
    try? fm.removeItem(at: output)
    try fm.moveItem(at: staging, to: output)
  }

  /// What the launcher shows and accepts: the trigger, and each Card's
  /// parameters. Prepared Code Cards retain the values used for their build.
  private func writeManifest(to url: URL, name: String, export: [String: Any]) throws {
    let document = try JSONSerialization.jsonObject(with: Data(contentsOf: project.appendingPathComponent("tsugumi.json"))) as? [String: Any] ?? [:]
    let canvas = (document["canvas"] as? [String: Any])?["cards"] as? [[String: Any]] ?? []
    var cards: [[String: Any]] = []
    var triggers: [String] = []
    let parameters = export["parameters"] as? [String: [[String: Any]]] ?? [:]
    for card in canvas {
      guard let id = card["id"] as? String else { continue }
      let file = project.appendingPathComponent("definitions/\(id)/_card.json")
      let definition = (try? JSONSerialization.jsonObject(with: Data(contentsOf: file))) as? [String: Any] ?? [:]
      let type = definition["type"] as? String ?? ""
      if type == "trigger" { triggers.append(id) }
      let params = (parameters[id] ?? []).map { p -> [String: Any] in
        var entry: [String: Any] = ["name": p["name"] ?? "", "type": p["type"] ?? ""]
        if let raw = p["effective_json"] as? String {
          entry["default"] = try? JSONSerialization.jsonObject(with: Data(raw.utf8), options: .fragmentsAllowed)
        }
        if type != "input" { entry["fixed"] = true }
        return entry
      }
      var entry: [String: Any] = ["id": id, "type": type, "params": params]
      // An input Card whose output is a File: the recipient may give another file.
      let outputs = (try? String(contentsOf: project.appendingPathComponent("definitions/\(id)/_outputs.fbs"), encoding: .utf8)) ?? ""
      if type == "input",
         outputs.range(of: #"tsugumi\s*:\s*"path""#, options: .regularExpression) != nil {
        entry["file"] = true
      }
      cards.append(entry)
    }
    let manifest: [String: Any] = [
      "name": name, "project": document["name"] ?? name, "triggers": triggers, "cards": cards,
      "exported_at": ISO8601DateFormatter().string(from: Date()),
      "guest": (try? String(contentsOf: guest.appendingPathComponent("identity"), encoding: .utf8))?
        .trimmingCharacters(in: .whitespacesAndNewlines) ?? "",
      "builds": export["cards"] ?? [], "payload_paths": export["payload_paths"] ?? 0,
    ]
    try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys]).write(to: url)
  }

  private func sign(_ app: URL) throws {
    let entitlements = FileManager.default.temporaryDirectory.appendingPathComponent("tsugumi-pack-\(UUID().uuidString).plist")
    try PropertyListSerialization.data(fromPropertyList: ["com.apple.security.virtualization": true], format: .xml, options: 0)
      .write(to: entitlements)
    defer { try? FileManager.default.removeItem(at: entitlements) }
    var arguments = ["--force", "--sign", signIdentity, "--entitlements", entitlements.path]
    if signIdentity != "-" { arguments += ["--options", "runtime", "--timestamp=none"] }
    try shell("/usr/bin/codesign", arguments + [app.path])
  }
}

func shell(_ tool: String, _ arguments: [String]) throws {
  let p = Process()
  p.executableURL = URL(fileURLWithPath: tool)
  p.arguments = arguments
  try p.run()
  p.waitUntilExit()
  guard p.terminationStatus == 0 else { throw PackError("\(tool) \(arguments.joined(separator: " ")) failed") }
}

func directorySize(_ url: URL) -> UInt64 {
  var total: UInt64 = 0
  let e = FileManager.default.enumerator(at: url, includingPropertiesForKeys: [.totalFileAllocatedSizeKey, .fileSizeKey])
  while let f = e?.nextObject() as? URL {
    total += UInt64((try? f.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0)
  }
  return total
}

/// An exclusive advisory lock held while the object lives.
final class FileLock {
  let fd: Int32
  init(_ url: URL) throws {
    fd = open(url.path, O_RDWR | O_CREAT | O_CLOEXEC, 0o600)
    guard fd >= 0 else { throw PackError("cannot open \(url.path)") }
    guard flock(fd, LOCK_EX | LOCK_NB) == 0 else {
      close(fd)
      throw PackError("another tsugumi-pack is using \(url.deletingLastPathComponent().path)")
    }
  }
  func release() { close(fd) }
}
