import Foundation
import CryptoKit

func validatePackedCardEntry(_ library: URL) throws {
  guard try isPackedMachO(library),
        try !PackedMachO(source: library, output: library).isExecutable else {
    throw PackedRelocationError("lib/main.so is not a loadable Mach-O library")
  }
  _ = try relocationTool("/usr/bin/lipo", ["-verify_arch", "arm64", library.path])
  let symbols = try relocationTool("/usr/bin/nm", ["-arch", "arm64", "-gjU", library.path])
  guard symbols.split(whereSeparator: \.isNewline).contains("_tsugumi_main") else {
    throw PackedRelocationError("lib/main.so does not export tsugumi_main")
  }
}
import MachO

/// Copies a Darwin closure into the caller's private app staging directory.
/// Keep each logical Card in its own destination so dyld sees distinct images.
func installPackedClosure(roots: [URL], in destination: URL, cardID: String, identity: String) throws -> [String: URL] {
  do {
    let installed = try PackedClosure(roots: roots, destination: destination, cardID: cardID).install(identity: identity)
    return roots.reduce(into: [:]) { paths, root in
      paths[root.path] = installed[root.standardizedFileURL.path]
    }
  } catch {
    throw PackedRelocationError("Card \(cardID) closure: \(error.localizedDescription)")
  }
}

private struct PackedRelocationError: LocalizedError {
  let errorDescription: String?
  init(_ message: String) { errorDescription = message }
}

private struct PackedClosure {
  let destinations: [String: URL]
  let cardID: String
  let destination: URL
  let fm = FileManager.default

  init(roots: [URL], destination: URL, cardID: String) throws {
    var destinations: [String: URL] = [:]
    var names: Set<String> = []
    for root in roots.map(\.standardizedFileURL) where destinations[root.path] == nil {
      let name = root.lastPathComponent
      guard !name.isEmpty, names.insert(name.lowercased()).inserted else {
        throw PackedRelocationError("conflicting closure root \(root.path)")
      }
      guard destinations.keys.allSatisfy({
        !$0.hasPrefix(root.path + "/") && !root.path.hasPrefix($0 + "/")
      }) else {
        throw PackedRelocationError("overlapping closure root \(root.path)")
      }
      destinations[root.path] = destination.appendingPathComponent(name)
    }
    self.destinations = destinations
    self.destination = destination
    self.cardID = cardID
  }

  func install(identity: String) throws -> [String: URL] {
    try fm.createDirectory(at: destination, withIntermediateDirectories: true)
    var images: [String: PackedMachO] = [:]
    var bundles: Set<URL> = []
    for root in destinations.keys.sorted() {
      let source = URL(fileURLWithPath: root)
      try copy(source, images: &images, bundles: &bundles)
    }
    let dependencies = try resolveDependencies(in: images)

    let namespace = SHA256.hash(data: Data(cardID.utf8)).prefix(16).map { String(format: "%02x", $0) }.joined()
    for (index, image) in images.values.sorted(by: { $0.source.path < $1.source.path }).enumerated() {
      try rewrite(image, installName: "@rpath/\(namespace)/\(index)",
                  dependencies: dependencies[image.source.path, default: [:]])
    }

    // Sign leaf images before the containing code bundles seal their signatures.
    let signingArguments = ["--force", "--sign", identity, "--timestamp=none"]
    let imagePaths = images.values.map { $0.output.path }.sorted()
    for start in stride(from: 0, to: imagePaths.count, by: 24) {
      let batch = imagePaths[start..<min(start + 24, imagePaths.count)]
      _ = try relocationTool("/usr/bin/codesign", signingArguments + batch)
    }
    for bundle in bundles.sorted(by: { $0.pathComponents.count > $1.pathComponents.count }) {
      _ = try relocationTool("/usr/bin/codesign", signingArguments + [bundle.path])
    }
    return destinations
  }

  private func relocated(_ path: String) -> URL? {
    var url = URL(fileURLWithPath: path)
    var components: [String] = []
    while true {
      if let root = destinations[url.path] {
        return components.reversed().reduce(root) { $0.appendingPathComponent($1) }
      }
      if url.path == "/" { return nil }
      components.append(url.lastPathComponent)
      url.deleteLastPathComponent()
    }
  }

  private func copy(_ source: URL, images: inout [String: PackedMachO], bundles: inout Set<URL>) throws {
    guard let output = relocated(source.path) else {
      throw PackedRelocationError("path is outside the closure: \(source.path)")
    }
    let attributes = try fm.attributesOfItem(atPath: source.path)
    let type = attributes[.type] as? FileAttributeType
    switch type {
    case .typeSymbolicLink:
      let target = try fm.destinationOfSymbolicLink(atPath: source.path)
      let absolute = target.hasPrefix("/") ? target : source.deletingLastPathComponent().path + "/" + target
      let normalized = (absolute as NSString).standardizingPath
      if let newTarget = relocated(normalized) {
        let actual = URL(fileURLWithPath: absolute).resolvingSymlinksInPath().path
        guard relocated(actual) != nil || isPackedSystemPath(actual) else {
          throw PackedRelocationError("closure symlink \(source.path) resolves outside the bundle: \(actual)")
        }
        let relative = relativePackedPath(to: newTarget, from: output.deletingLastPathComponent())
        try fm.createSymbolicLink(atPath: output.path, withDestinationPath: relative)
      } else if isPackedSystemPath(normalized) {
        try fm.createSymbolicLink(atPath: output.path, withDestinationPath: normalized)
      } else {
        throw PackedRelocationError("closure symlink \(source.path) points outside the bundle: \(target)")
      }
    case .typeDirectory:
      try fm.createDirectory(at: output, withIntermediateDirectories: false)
      for child in try fm.contentsOfDirectory(atPath: source.path).sorted() {
        try copy(source.appendingPathComponent(child), images: &images, bundles: &bundles)
      }
      if ["app", "appex", "bundle", "framework", "plugin", "xpc"].contains(output.pathExtension),
         let executable = Bundle(url: output)?.executableURL,
         images.values.contains(where: {
           $0.output.standardizedFileURL.path == executable.resolvingSymlinksInPath().path
         }) {
        bundles.insert(output)
      }
    case .typeRegular:
      try fm.copyItem(at: source, to: output)
      if try isPackedMachO(source) {
        let mode = (attributes[.posixPermissions] as? Int ?? 0o555) | 0o200
        try fm.setAttributes([.posixPermissions: mode], ofItemAtPath: output.path)
        images[source.path] = try PackedMachO(source: source, output: output)
      }
    default:
      throw PackedRelocationError("unsupported closure file type at \(source.path)")
    }
  }

  private func resolveDependencies(in images: [String: PackedMachO]) throws -> [String: [String: String]] {
    // The dyld run-path stack contains paths from the image's loading ancestors.
    // Propagate known edges until all inherited run paths have been discovered.
    var parents: [String: Set<String>] = [:]
    struct Context: Hashable {
      let paths: [String]
      let executable: String?
    }

    func contexts(for image: PackedMachO, visited: Set<String> = []) -> [Context] {
      guard !visited.contains(image.source.path) else { return [] }
      let ownExecutable = image.isExecutable ? image.source.path : nil
      let ancestors = parents[image.source.path, default: []].sorted()
      let inherited = ancestors.flatMap { parent in
        contexts(for: images[parent]!, visited: visited.union([image.source.path]))
      }
      let bases = inherited.isEmpty || image.isExecutable
        ? [Context(paths: [], executable: ownExecutable)] : inherited
      return Array(Set(bases.map { base in
        let ownPaths = image.rpaths.compactMap { image.expanded($0, executable: base.executable) }
        return Context(paths: ownPaths + base.paths, executable: base.executable)
      }))
    }

    func resolve(_ dependency: String, for image: PackedMachO, using context: Context) -> String? {
      let candidates: [String]
      if dependency.hasPrefix("@rpath/") {
        let suffix = String(dependency.dropFirst("@rpath/".count))
        candidates = context.paths.map { $0 + "/" + suffix }
      } else {
        candidates = image.expanded(dependency, executable: context.executable).map { [$0] } ?? []
      }
      for candidate in candidates {
        let url = URL(fileURLWithPath: candidate)
        let normalized = url.standardizedFileURL.path
        if isPackedSystemPath(normalized), _dyld_shared_cache_contains_path(normalized) {
          return normalized
        }
        if fm.fileExists(atPath: candidate) { return url.resolvingSymlinksInPath().path }
      }
      return nil
    }

    var dependencies: [String: [String: String]] = [:]
    let sortedImages = images.values.sorted { $0.source.path < $1.source.path }
    var changed = true
    while changed {
      changed = false
      for image in sortedImages {
        let executable = image.isExecutable ? image.source.path : nil
        let ownContext = Context(
          paths: image.rpaths.compactMap { image.expanded($0, executable: executable) },
          executable: executable)
        var inheritedContexts: [Context]?
        for dependency in image.dependencies {
          let ownMatch = resolve(dependency, for: image, using: ownContext)
          let usesInheritedRPaths = dependency.hasPrefix("@rpath/") &&
            (ownMatch == nil || image.rpaths.contains(where: { $0.hasPrefix("@executable_path") }))
          let usesExecutable = dependency.hasPrefix("@executable_path/")
          var matches = Set(ownMatch.map { [$0] } ?? [])
          if !image.isExecutable && (usesInheritedRPaths || usesExecutable) {
            if inheritedContexts == nil { inheritedContexts = contexts(for: image) }
            matches = Set((inheritedContexts ?? []).compactMap { resolve(dependency, for: image, using: $0) })
          }
          guard matches.count <= 1 else {
            throw PackedRelocationError("ambiguous dependency \(dependency) of \(image.source.path): \(matches.sorted().joined(separator: ", "))")
          }
          guard let target = matches.first else {
            dependencies[image.source.path]?.removeValue(forKey: dependency)
            continue
          }
          guard images[target] != nil || isPackedSystemPath(target) else {
            throw PackedRelocationError("dependency \(dependency) of \(image.source.path) resolves outside the supplied Mach-O closure: \(target)")
          }
          if let previous = dependencies[image.source.path]?[dependency], previous != target {
            throw PackedRelocationError("dependency \(dependency) of \(image.source.path) resolved to both \(previous) and \(target)")
          }
          dependencies[image.source.path, default: [:]][dependency] = target
          if images[target] != nil && target != image.source.path,
             parents[target, default: []].insert(image.source.path).inserted {
            changed = true
          }
        }
      }
    }
    for image in sortedImages {
      for dependency in image.dependencies where dependencies[image.source.path]?[dependency] == nil {
        throw PackedRelocationError("unresolved dependency \(dependency) of \(image.source.path)")
      }
    }
    return dependencies
  }

  private func rewrite(_ image: PackedMachO, installName: String, dependencies: [String: String]) throws {
    let outputParent = image.output.deletingLastPathComponent()
    var arguments: [String] = []
    if image.hasInstallName {
      arguments += ["-id", installName]
    }
    for (old, path) in dependencies.sorted(by: { $0.key < $1.key }) {
      let new: String
      if isPackedSystemPath(path) {
        new = path
      } else if let target = relocated(path) {
        new = "@loader_path/" + relativePackedPath(to: target, from: outputParent)
      } else {
        throw PackedRelocationError("dependency \(path) of \(image.source.path) escapes the closure")
      }
      if old != new { arguments += ["-change", old, new] }
    }
    // Preserve run paths for explicit runtime loads, after rebasing bundle paths.
    var newRpaths = Set(image.rpaths)
    for old in image.rpaths {
      let new: String?
      if let path = image.expanded(old, executable: image.isExecutable ? image.source.path : nil) {
        let normalized = URL(fileURLWithPath: path).standardizedFileURL.path
        if isPackedSystemPath(normalized) {
          new = normalized
        } else if let target = relocated(normalized) {
          new = "@loader_path/" + relativePackedPath(to: target, from: outputParent)
        } else {
          new = nil
        }
      } else {
        new = nil
      }
      if new == old { continue }
      if let new, newRpaths.insert(new).inserted {
        arguments += ["-rpath", old, new]
      } else {
        arguments += ["-delete_rpath", old]
      }
    }
    if !arguments.isEmpty {
      _ = try relocationTool("/usr/bin/install_name_tool", arguments + [image.output.path])
    }
  }
}

private struct PackedMachO {
  let source: URL
  let output: URL
  let dependencies: [String]
  let rpaths: [String]
  let hasInstallName: Bool
  let isExecutable: Bool

  init(source: URL, output: URL) throws {
    self.source = source
    self.output = output
    let listing = try relocationTool("/usr/bin/otool", ["-arch", "all", "-l", source.path])
    var command = ""
    var dependencies: [String] = []
    var rpaths: [String] = []
    var hasInstallName = false
    var isExecutable = false
    for raw in listing.split(separator: "\n") {
      let line = raw.trimmingCharacters(in: .whitespaces)
      if line.hasPrefix("cmd ") {
        command = String(line.dropFirst(4))
        if command == "LC_ID_DYLIB" { hasInstallName = true }
        if command == "LC_MAIN" || command == "LC_LOAD_DYLINKER" { isExecutable = true }
        continue
      }
      guard let end = line.range(of: " (offset ", options: .backwards) else { continue }
      if command == "LC_RPATH", line.hasPrefix("path ") {
        let value = String(line[line.index(line.startIndex, offsetBy: 5)..<end.lowerBound])
        if !rpaths.contains(value) { rpaths.append(value) }
      } else if ["LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LAZY_LOAD_DYLIB",
                 "LC_LOAD_UPWARD_DYLIB"].contains(command), line.hasPrefix("name ") {
        let value = String(line[line.index(line.startIndex, offsetBy: 5)..<end.lowerBound])
        if !dependencies.contains(value) { dependencies.append(value) }
      }
    }
    self.dependencies = dependencies
    self.rpaths = rpaths
    self.hasInstallName = hasInstallName
    self.isExecutable = isExecutable
  }

  func expanded(_ path: String, executable: String?) -> String? {
    if path.hasPrefix("/") { return path }
    let anchors: [(String, String?)] = [("@loader_path", source.path), ("@executable_path", executable)]
    for (token, image) in anchors {
      if let image, path == token || path.hasPrefix(token + "/") {
        return (image as NSString).deletingLastPathComponent + String(path.dropFirst(token.count))
      }
    }
    return nil
  }
}

private func isPackedSystemPath(_ path: String) -> Bool {
  ["/usr/lib", "/System/Library"].contains { path == $0 || path.hasPrefix($0 + "/") }
}

private func relativePackedPath(to target: URL, from base: URL) -> String {
  let targetParts = target.pathComponents
  let baseParts = base.pathComponents
  let common = zip(targetParts, baseParts).prefix { $0 == $1 }.count
  let parts = Array(repeating: "..", count: baseParts.count - common) + targetParts.dropFirst(common)
  return parts.isEmpty ? "." : parts.joined(separator: "/")
}

private func isPackedMachO(_ source: URL) throws -> Bool {
  let file = try FileHandle(forReadingFrom: source)
  defer { try? file.close() }
  guard let bytes = try file.read(upToCount: 32), bytes.count >= 16 else { return false }
  let thin = [Data([0xcf, 0xfa, 0xed, 0xfe]), Data([0xce, 0xfa, 0xed, 0xfe]),
              Data([0xfe, 0xed, 0xfa, 0xcf]), Data([0xfe, 0xed, 0xfa, 0xce])]
  func loadable(_ header: Data) -> Bool {
    guard header.count >= 16, thin.contains(header.prefix(4)) else { return false }
    let bigEndian = header.first == 0xfe
    let type = bigEndian ? header[15] : header[12]
    // MH_EXECUTE, MH_DYLIB, MH_BUNDLE. Ignore objects and debug symbols.
    return [2, 6, 8].contains(type)
  }
  if thin.contains(bytes.prefix(4)) { return loadable(bytes) }
  let magic = Array(bytes.prefix(4))
  let fatMagic: [[UInt8]] = [[0xca, 0xfe, 0xba, 0xbe], [0xca, 0xfe, 0xba, 0xbf],
                            [0xbe, 0xba, 0xfe, 0xca], [0xbf, 0xba, 0xfe, 0xca]]
  guard let format = fatMagic.firstIndex(of: magic), bytes.count >= 24 else { return false }
  let offsetBytes = bytes[16..<(format.isMultiple(of: 2) ? 20 : 24)]
  let offset = (format < 2 ? Array(offsetBytes) : Array(offsetBytes.reversed()))
    .reduce(UInt64(0)) { ($0 << 8) | UInt64($1) }
  // Java class files share the 32-bit fat Mach-O magic.
  guard offset >= 28, offset <= (try file.seekToEnd()) - 16 else { return false }
  try file.seek(toOffset: offset)
  return try loadable(file.read(upToCount: 16) ?? Data())
}

private func relocationTool(_ tool: String, _ arguments: [String]) throws -> String {
  let process = Process()
  process.executableURL = URL(fileURLWithPath: tool)
  process.arguments = arguments
  let output = Pipe()
  process.standardOutput = output
  process.standardError = output
  try process.run()
  let data = output.fileHandleForReading.readDataToEndOfFile()
  process.waitUntilExit()
  let message = String(decoding: data, as: UTF8.self)
  guard process.terminationStatus == 0 else {
    throw PackedRelocationError("\(tool) failed for \(arguments.last ?? ""): \(message.trimmingCharacters(in: .whitespacesAndNewlines))")
  }
  return message
}
