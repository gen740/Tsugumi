import Foundation

/// The export manifest names the built files. Only paths relative to the app's
/// Resources directory are written to the runtime manifest.
struct NativeExportPlan: Decodable {
  struct Card: Decodable {
    let card: String
    let plan: String
    let closure: [String]
    let system: String
    let reuseWorker: Bool
    let executionList: Bool
    let implementationLabels: [String]
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

    let plans = try cards.map { card in
      guard seenCards.insert(card.card).inserted, card.system == Self.target else {
        throw PackError("Invalid native build entry for Card \(card.card) (\(card.system))")
      }
      do {
        let plan = try JSONDecoder().decode(CardPlan.self, from: Data(contentsOf: URL(fileURLWithPath: card.plan)))
        guard plan.runs.count == card.implementationLabels.count else {
          throw PackError("The implementation labels do not match the build plan")
        }
        for package in plan.runs {
          let location = URL(fileURLWithPath: package, isDirectory: true).appendingPathComponent("lib")
          guard fm.fileExists(atPath: location.appendingPathComponent("main.so").path) else {
            throw PackError("Missing lib/main.so in \(package)")
          }
          try validatePackedCardEntry(location.appendingPathComponent("main.so"))
          if mode == .fast, fm.fileExists(atPath: location.appendingPathComponent("go-card.so").path) {
            if let prior = fastGoCard {
              throw PackError("Fast mode can load only one Go library (Cards \(prior) and \(card.card))")
            }
            fastGoCard = card.card
          }
        }
        return plan
      } catch {
        throw PackError("Card \(card.card): \(error)")
      }
    }

    try fm.createDirectory(at: base, withIntermediateDirectories: true)
    var builds: [PackedBuilds.Card] = []
    for (index, (card, plan)) in zip(cards, plans).enumerated() {
      do {
        let slot = String(index)
        let destination = base.appendingPathComponent(slot, isDirectory: true)
        let paths = try installPackedClosure(
          roots: card.closure.map { URL(fileURLWithPath: $0) }, in: destination,
          cardID: slot, identity: identity)
        func relocated(_ original: String) throws -> String {
          let components = original.split(separator: "/", omittingEmptySubsequences: false)
          guard components.count >= 4, components[0].isEmpty, components[1] == "nix",
                components[2] == "store",
                !components[3...].contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }),
                let replacement = paths["/nix/store/\(components[3])"] else {
            throw PackError("Build plan path lies outside the packaged closure: \(original)")
          }
          return ([relativeBase, slot, replacement.lastPathComponent] + components.dropFirst(4).map(String.init))
            .joined(separator: "/")
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
        throw PackError("Card \(card.card): \(error)")
      }
    }
    return PackedBuilds(version: version, system: system, mode: mode,
                        debug: debug, cards: builds, hostPaths: hostPaths)
  }
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
