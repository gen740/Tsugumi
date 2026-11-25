import Foundation

/// The export manifest names the built files. Only paths relative to the app's
/// Resources directory are written to the runtime manifest.
struct NativeExportPlan: Decodable {
  struct Card: Decodable {
    let card: String
    let plan: String
    let closure: [String]
    let system: String
    let language: String?
    let reuseWorker: Bool
    let executionList: Bool
    let implementationLabels: [String]
  }

  private struct RuntimeMetadata: Decodable {
    struct Runtime: Decodable {
      let name: String
      let roots: Set<String>
    }
    let version: Int
    let runtimes: [Runtime]

    static func read(_ package: URL) throws -> [Runtime] {
      let file = package.appendingPathComponent("share/tsugumi/runtime.json")
      guard FileManager.default.fileExists(atPath: file.path) else { return [] }
      let descriptor = try JSONDecoder().decode(Self.self, from: Data(contentsOf: file))
      guard descriptor.version == 1 else { throw PackError("Unsupported runtime metadata in \(file.path)") }
      return descriptor.runtimes
    }
  }

  static let target = "aarch64-darwin"

  let version: Int
  let system: String
  let cards: [Card]
  let hostPaths: [String]

  static func read(_ url: URL) throws -> NativeExportPlan {
    let decoder = JSONDecoder()
    decoder.keyDecodingStrategy = .convertFromSnakeCase
    let manifest = try decoder.decode(Self.self, from: Data(contentsOf: url))
    guard manifest.version == 1, manifest.system == target else {
      throw PackError("Unsupported native export manifest for \(manifest.system)")
    }
    return manifest
  }

  func install(in resources: URL, mode: PackedExecutionMode, debug: Bool,
               identity: String) throws -> PackedBuilds {
    let fm = FileManager.default
    let relativeBase = "cards"
    let base = resources.appendingPathComponent(relativeBase, isDirectory: true)
    var seenCards = Set<String>()
    var fastGoCard: String?

    var runtimes: [String: (roots: Set<String>, card: String)] = [:]
    let plans = try cards.map { card in
      guard seenCards.insert(card.card).inserted, card.system == Self.target else {
        throw PackError("Invalid native build entry for Card \(card.card) (\(card.system))")
      }
      do {
        let roots = Set(card.closure)
        guard roots.allSatisfy({ storeRoot($0) == $0 }), roots.contains(card.plan) else {
          throw PackError("The build manifest contains an invalid Nix closure")
        }
        let plan = try JSONDecoder().decode(CardPlan.self, from: Data(contentsOf: URL(fileURLWithPath: card.plan)))
        guard !plan.runs.isEmpty, plan.runs.count == card.implementationLabels.count else {
          throw PackError("The implementation labels do not match the build plan")
        }
        var declared = Set<String>()
        func register(_ runtime: RuntimeMetadata.Runtime) throws {
          declared.insert(runtime.name)
          guard mode == .fast else { return }
          if let prior = runtimes[runtime.name], prior.roots != runtime.roots {
            throw PackError("Fast mode cannot share conflicting \(runtime.name) runtimes (Cards \(prior.card) and \(card.card))")
          }
          runtimes[runtime.name] = (runtime.roots, card.card)
        }
        var goImages = Set<String>()
        for package in Set(plan.runs).sorted() {
          guard let root = storeRoot(package), roots.contains(root) else {
            throw PackError("Implementation is outside the packaged closure: \(package)")
          }
          let location = URL(fileURLWithPath: package, isDirectory: true).appendingPathComponent("lib")
          let entry = location.appendingPathComponent("main.so")
          guard fm.fileExists(atPath: entry.path) else {
            throw PackError("Missing lib/main.so in \(package)")
          }
          try validatePackedCardEntry(entry)
          let goLibrary = location.appendingPathComponent("go-card.so")
          if fm.fileExists(atPath: goLibrary.path) {
            goImages.insert(goLibrary.resolvingSymlinksInPath().path)
          } else if card.language == "go" {
            goImages.insert(entry.resolvingSymlinksInPath().path)
          }
          for runtime in try RuntimeMetadata.read(URL(fileURLWithPath: package)) {
            guard !runtime.name.isEmpty, !runtime.roots.isEmpty, runtime.roots.isSubset(of: roots) else {
              throw PackError("Invalid runtime metadata for \(runtime.name) in \(package)")
            }
            try register(runtime)
          }
        }
        if mode == .fast {
          for runtime in sdkRuntimes(in: roots) where !declared.contains(runtime.name) {
            try register(runtime)
          }
        }
        if mode == .fast, !goImages.isEmpty {
          guard goImages.count == 1, fastGoCard == nil else {
            let owners = fastGoCard.map { "Cards \($0) and \(card.card)" } ?? "Card \(card.card)"
            throw PackError("Fast mode can load only one Go library (\(owners))")
          }
          fastGoCard = card.card
        }
        return plan
      } catch {
        throw PackError("Card \(card.card): \(error.localizedDescription)")
      }
    }

    var shared: [String: URL] = [:]
    let runtimeRoots = Set(runtimes.values.flatMap(\.roots))
    if !runtimeRoots.isEmpty {
      let closure = try packedRuntimeClosure(runtimeRoots)
      let knownRoots = Set(cards.flatMap(\.closure))
      guard closure.isSubset(of: knownRoots) else {
        throw PackError("A shared runtime references files outside the export build closures")
      }
      let implementationRoots = Set(plans.flatMap(\.runs).compactMap(storeRoot))
      guard closure.isDisjoint(with: implementationRoots) else {
        throw PackError("A shared runtime cannot contain a Card implementation")
      }
      shared = try installPackedClosure(
        roots: closure.sorted().map { URL(fileURLWithPath: $0) },
        in: resources.appendingPathComponent("runtimes"), cardID: "shared runtimes", identity: identity)
    }

    try fm.createDirectory(at: base, withIntermediateDirectories: true)
    var builds: [PackedBuilds.Card] = []
    for (index, (card, plan)) in zip(cards, plans).enumerated() {
      do {
        let slot = String(index)
        let destination = base.appendingPathComponent(slot, isDirectory: true)
        let paths = try installPackedClosure(
          roots: card.closure.map { URL(fileURLWithPath: $0) }, in: destination,
          cardID: "card " + slot, identity: identity, shared: shared)
        func relocated(_ original: String) throws -> String {
          guard let root = storeRoot(original), let replacement = paths[root] else {
            throw PackError("Build plan path lies outside the packaged closure: \(original)")
          }
          let bundled = replacement.path + original.dropFirst(root.count)
          guard bundled.hasPrefix(resources.path + "/"), fm.fileExists(atPath: bundled) else {
            throw PackError("Missing installed build plan path: \(original)")
          }
          return String(bundled.dropFirst(resources.path.count + 1))
        }
        let runs = try plan.runs.map(relocated)
        let views = try plan.views.map { view in
          CardPlan.View(blob: try relocated(view.blob), name: view.name, type: view.type)
        }
        builds.append(PackedBuilds.Card(
          card: card.card, plan: CardPlan(runs: runs, views: views),
          reuseWorker: card.reuseWorker, executionList: card.executionList,
          implementationLabels: card.implementationLabels))
      } catch {
        throw PackError("Card \(card.card): \(error.localizedDescription)")
      }
    }
    return PackedBuilds(version: version, system: system, mode: mode,
                        debug: debug, cards: builds, hostPaths: hostPaths)
  }
  // The Kit's native shims own process-wide runtime state. Keep the shims and
  // their dependencies together even when the package emits no descriptor.
  private func sdkRuntimes(in roots: Set<String>) -> [RuntimeMetadata.Runtime] {
    let shims = [
      "tsugumi-java-vm": "java", "tsugumi-julia-process": "julia",
      "tsugumi-node-process": "node", "tsugumi-r-process": "r",
      "tsugumi-ruby-process": "ruby",
    ]
    return roots.sorted().compactMap { root in
      let outputName = URL(fileURLWithPath: root).lastPathComponent.split(separator: "-", maxSplits: 1).last
      if let outputName, let name = shims[String(outputName)] {
        return RuntimeMetadata.Runtime(name: name, roots: [root])
      }
      if FileManager.default.fileExists(atPath: root + "/share/dotnet/host/fxr") {
        return RuntimeMetadata.Runtime(name: "dotnet", roots: [root])
      }
      return nil
    }
  }
}

private func storeRoot(_ path: String) -> String? {
  let prefix = "/nix/store/"
  guard path.hasPrefix(prefix) else { return nil }
  let parts = path.dropFirst(prefix.count).split(separator: "/", omittingEmptySubsequences: false)
  guard let name = parts.first, !parts.contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }) else {
    return nil
  }
  return prefix + name
}

enum PackedExecutionMode: String, Codable {
  case resident = "default"
  case fast
}

struct CardPlan: Codable {
  struct View: Codable {
    let blob: String
    let name: String
    let type: String
  }
  let runs: [String]
  let views: [View]
}

struct PackedBuilds: Codable {
  struct Card: Codable {
    let card: String
    let plan: CardPlan
    let reuseWorker: Bool
    let executionList: Bool
    let implementationLabels: [String]
  }
  let version: Int
  let system: String
  let mode: PackedExecutionMode
  let debug: Bool
  let cards: [Card]
  let hostPaths: [String]
}
