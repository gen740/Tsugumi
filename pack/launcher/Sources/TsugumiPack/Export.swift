import CryptoKit
import Darwin
import Foundation

/// Builds the Project's Cards for Apple Silicon and exports a standalone app.
struct Export {
  let project: URL
  let output: URL
  let mode: PackedExecutionMode
  let debug: Bool
  let core: URL?
  let signIdentity: String

  // TODO: Add an explicit VM export option.
  static let usage = """
    usage: tsugumi-pack PROJECT --output NAME.app [--fast] [--debug]
                        [--tsugumid PATH] [--sign IDENTITY]

    Builds the Project's native Cards for a standalone macOS 26 app.

      --fast            run Cards in a single process without isolation
      --debug           include the debug endpoint
      --tsugumid PATH   use this export-time core (default: the distributed core)
      --sign IDENTITY   code signing identity (default: $TSUGUMI_CODESIGN_IDENTITY, else ad hoc)
    """

  static func parse(_ arguments: [String]) throws -> Export {
    var project: String?
    var output: String?
    var core: String?
    var mode = PackedExecutionMode.resident
    var debug = false
    var sign = ProcessInfo.processInfo.environment["TSUGUMI_CODESIGN_IDENTITY"] ?? "-"
    var i = 0
    func value() throws -> String {
      i += 1
      guard i < arguments.count else { throw PackError("\(arguments[i - 1]) needs a value") }
      return arguments[i]
    }
    while i < arguments.count {
      switch arguments[i] {
      case "--output", "-o": output = try value()
      case "--fast": mode = .fast
      case "--debug": debug = true
      case "--tsugumid": core = try value()
      case "--sign": sign = try value()
      case let a where a.hasPrefix("-"): throw PackError("unknown option \(a)\n\n\(usage)")
      case let a:
        guard project == nil else { throw PackError("one Project at a time\n\n\(usage)") }
        project = a
      }
      i += 1
    }
    guard let project, let output else { throw PackError(usage) }
    let requestedProject = URL(fileURLWithPath: project).standardizedFileURL
    let projectURL = canonicalURL(requestedProject) ?? requestedProject
    let outputURL = URL(fileURLWithPath: output).standardizedFileURL
    guard outputURL.pathExtension == "app" else { throw PackError("--output must name an .app") }
    return Export(project: projectURL, output: outputURL, mode: mode, debug: debug,
                  core: core.map { URL(fileURLWithPath: $0).standardizedFileURL }, signIdentity: sign)
  }

  @MainActor func run() throws {
    let fm = FileManager.default
    guard fm.fileExists(atPath: project.appendingPathComponent("tsugumi.json").path) else {
      throw PackError("\(project.path) is not a Tsugumi Project (no tsugumi.json)")
    }
    let executable = Bundle.main.executableURL?.resolvingSymlinksInPath()
      ?? URL(fileURLWithPath: CommandLine.arguments[0]).resolvingSymlinksInPath()
    let distribution = executable.deletingLastPathComponent().deletingLastPathComponent()
    let daemon = core ?? distribution.appendingPathComponent("core/bin/tsugumid")
    guard fm.isExecutableFile(atPath: daemon.path) else {
      throw PackError("missing export-time core at \(daemon.path) (run pack/build.sh or pass --tsugumid)")
    }
    let requestedParent = output.deletingLastPathComponent()
    let outputParent = canonicalURL(requestedParent) ?? requestedParent

    // Work on a snapshot so the exporter never opens or changes the author's state.
    // The core checks each ancestor of its private scratch directories.
    // Use the macOS canonical path, including the /private component.
    let work = URL(fileURLWithPath: "/private/tmp/tsugumi-pack-export-\(UUID().uuidString)", isDirectory: true)
    try fm.createDirectory(at: work, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
    defer { try? fm.removeItem(at: work) }
    let snapshot = work.appendingPathComponent("project")
    try stageProject(from: project, into: snapshot, root: project, omitting: outputParent.appendingPathComponent(output.lastPathComponent))
    let manifest = work.appendingPathComponent("builds.json")
    print("tsugumi-pack: building \(project.lastPathComponent) for \(NativeExportPlan.target)")
    try shell(daemon.path, ["--project", snapshot.path, "--prepare-native-cards", manifest.path,
                            "--scratch-dir", work.appendingPathComponent("scratch").path,
                            "--run-scratch-dir", work.appendingPathComponent("runs").path])
    let plan = try NativeExportPlan.read(manifest)
    let staging = outputParent.appendingPathComponent(".tsugumi-pack-\(UUID().uuidString).app")
    defer { try? fm.removeItem(at: staging) }
    try writeApp(staging, plan: plan, snapshot: snapshot, executable: executable, distribution: distribution)
    // Publish with a same-volume rename. A failed export leaves an existing bundle intact.
    if (try? fm.attributesOfItem(atPath: output.path)) != nil {
      let result = renameatx_np(AT_FDCWD, staging.path, AT_FDCWD, output.path, UInt32(RENAME_SWAP))
      guard result == 0 else { throw PackError("cannot replace \(output.path): \(String(cString: strerror(errno)))") }
    } else {
      try fm.moveItem(at: staging, to: output)
    }
    print("tsugumi-pack: wrote \(output.path)")
  }

  private func stageProject(from source: URL, into destination: URL, root: URL, omitting: URL? = nil) throws {
    let fm = FileManager.default
    try fm.createDirectory(at: destination, withIntermediateDirectories: false)
    for name in try fm.contentsOfDirectory(atPath: source.path).sorted()
      where ![".tsugumi", ".git", ".DS_Store"].contains(name) {
      let child = source.appendingPathComponent(name)
      if child.path == omitting?.path { continue }
      let target = destination.appendingPathComponent(name)
      switch (try fm.attributesOfItem(atPath: child.path))[.type] as? FileAttributeType {
      case .typeDirectory:
        try stageProject(from: child, into: target, root: root, omitting: omitting)
      case .typeRegular:
        try fm.copyItem(at: child, to: target)
      case .typeSymbolicLink:
        let link = try fm.destinationOfSymbolicLink(atPath: child.path)
        guard !link.hasPrefix("/"), let resolved = canonicalURL(child)?.path,
              resolved.hasPrefix(root.path + "/"),
              !resolved.dropFirst(root.path.count + 1).split(separator: "/").contains(where: {
                [".tsugumi", ".git", ".DS_Store"].contains(String($0))
              }) else {
          throw PackError("Project symlink cannot be packed: \(child.path)")
        }
        try fm.copyItem(at: child, to: target)
      default:
        throw PackError("unsupported Project file: \(child.path)")
      }
    }
  }

  private func writeApp(_ app: URL, plan: NativeExportPlan, snapshot: URL,
                        executable: URL, distribution: URL) throws {
    let fm = FileManager.default
    let name = output.deletingPathExtension().lastPathComponent
    let contents = app.appendingPathComponent("Contents")
    let resources = contents.appendingPathComponent("Resources")
    let macos = contents.appendingPathComponent("MacOS")
    let frameworks = contents.appendingPathComponent("Frameworks")
    let helpers = contents.appendingPathComponent("Helpers")
    for directory in [resources, macos, frameworks, helpers] {
      try fm.createDirectory(at: directory, withIntermediateDirectories: true)
    }
    try fm.copyItem(at: executable, to: macos.appendingPathComponent(name))
    let wasmtime = frameworks.appendingPathComponent("libwasmtime.dylib")
    try fm.copyItem(at: distribution.appendingPathComponent("Frameworks/libwasmtime.dylib"), to: wasmtime)
    let nativeCore = distribution.appendingPathComponent("core")
    try fm.copyItem(at: nativeCore.appendingPathComponent("share/tsugumi/protocol/tsugumi.bfbs"),
                    to: resources.appendingPathComponent("tsugumi.bfbs"))
    try fm.copyItem(at: distribution.appendingPathComponent("share/tsugumi-pack/view-licenses"),
                    to: resources.appendingPathComponent("licenses"))
    let stagedProject = resources.appendingPathComponent("project")
    try stageProject(from: snapshot, into: stagedProject, root: snapshot)
    let builds = try plan.install(in: resources, mode: mode, debug: debug, identity: signIdentity)
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
    try encoder.encode(builds).write(to: resources.appendingPathComponent("native-builds.json"))
    try writeManifest(project: stagedProject, name: name, into: resources)

    let slug = name.lowercased().filter { $0.isASCII && ($0.isLetter || $0.isNumber) }
    let digest = SHA256.hash(data: Data(name.utf8)).prefix(6).map { String(format: "%02x", $0) }.joined()
    let identifier = "org.tsugumi.pack." + (slug.isEmpty ? digest : "\(slug)-\(digest)")
    let plist: [String: Any] = [
      "CFBundleName": name, "CFBundleDisplayName": name, "CFBundleExecutable": name,
      "CFBundleIdentifier": identifier, "CFBundlePackageType": "APPL",
      "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1",
      "LSMinimumSystemVersion": "26.0", "NSHighResolutionCapable": true,
    ]
    try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
      .write(to: contents.appendingPathComponent("Info.plist"))
    try relocatePackedTree(stagedProject, identity: signIdentity)
    try shell("/usr/bin/codesign", ["--force", "--sign", signIdentity, "--timestamp=none", wasmtime.path])
    let entitlements = app.deletingLastPathComponent().appendingPathComponent("tsugumi-pack-\(UUID().uuidString).plist")
    defer { try? fm.removeItem(at: entitlements) }
    func writeEntitlements(_ values: [String: Bool]) throws {
      try PropertyListSerialization.data(fromPropertyList: values, format: .xml, options: 0)
        .write(to: entitlements)
    }

    let jit = ["com.apple.security.cs.allow-jit": true]
    try writeEntitlements(jit)
    try installPackedExecutables(nativeCore.appendingPathComponent("bin"), helpers: helpers,
                                 frameworks: frameworks, identity: signIdentity, entitlements: entitlements)
    // The bundled Wasmtime maps executable code without MAP_JIT. Only the
    // Swift app that hosts Dawn views needs this hardened runtime entitlement.
    var presentationEntitlements = jit
    presentationEntitlements["com.apple.security.cs.allow-unsigned-executable-memory"] = true
    try writeEntitlements(presentationEntitlements)
    try sign(app, entitlements: entitlements)
    try shell("/usr/bin/codesign", ["--verify", "--deep", "--strict", app.path])
  }

  private func writeManifest(project: URL, name: String, into resources: URL) throws {
    let document = try JSONSerialization.jsonObject(with: Data(contentsOf: project.appendingPathComponent("tsugumi.json"))) as? [String: Any] ?? [:]
    let canvas = document["canvas"] as? [String: Any] ?? [:]
    let cards = canvas["cards"] as? [[String: Any]] ?? []
    let incoming = Set((canvas["connections"] as? [[String: Any]] ?? []).compactMap {
      ($0["to"] as? [String: Any])?["card"] as? String
    })
    var triggers: [String] = []
    var entryCards: [String] = []
    for card in cards {
      guard let id = card["id"] as? String else { continue }
      let file = project.appendingPathComponent("definitions/\(id)/_card.json")
      let definition = try JSONSerialization.jsonObject(with: Data(contentsOf: file)) as? [String: Any]
      if definition?["type"] as? String == "trigger" {
        triggers.append(id)
      } else if definition?["type"] as? String == "code", !incoming.contains(id) {
        entryCards.append(id)
      }
    }
    if triggers.isEmpty { triggers = entryCards }
    let manifest: [String: Any] = [
      "name": name, "project": document["name"] ?? name, "triggers": triggers,
      "mode": mode.rawValue, "debug": debug,
    ]
    try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys])
      .write(to: resources.appendingPathComponent("pack.json"))
  }

  private func sign(_ app: URL, entitlements: URL) throws {
    var arguments = ["--force", "--sign", signIdentity, "--timestamp=none", "--entitlements", entitlements.path]
    if signIdentity != "-" { arguments += ["--options", "runtime"] }
    try shell("/usr/bin/codesign", arguments + [app.path])
  }
}

private func canonicalURL(_ url: URL) -> URL? {
  guard let path = realpath(url.path, nil) else { return nil }
  defer { free(path) }
  return URL(fileURLWithPath: String(cString: path))
}

func shell(_ tool: String, _ arguments: [String]) throws {
  let process = Process()
  process.executableURL = URL(fileURLWithPath: tool)
  process.arguments = arguments
  try process.run()
  process.waitUntilExit()
  guard process.terminationReason == .exit, process.terminationStatus == 0 else {
    throw PackError("\(tool) failed (status \(process.terminationStatus))")
  }
}
