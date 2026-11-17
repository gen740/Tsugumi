// swift-tools-version:6.2
import Foundation
import PackageDescription
let clientLib = ProcessInfo.processInfo.environment["TSUGUMI_CLIENT_LIB_DIR"] ?? URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../build/client").standardizedFileURL.path
let transportLib = URL(fileURLWithPath: clientLib).deletingLastPathComponent().appendingPathComponent("rpc").path
let package = Package(
  name: "TsugumiWire", platforms: [.macOS("26.0")],
  products: [.library(name: "TsugumiRPC", targets: ["TsugumiRPC"])],
  dependencies: [.package(url: "https://github.com/google/flatbuffers.git", exact: "25.12.19")],
  targets: [
    .plugin(name: "FlatcPlugin", capability: .buildTool(), path: "Plugins/FlatcPlugin"),
    .systemLibrary(name: "TsugumiClientC", path: "Sources/ClientC"),
    .target(name: "TsugumiRPC", dependencies: ["TsugumiClientC", .product(name: "FlatBuffers", package: "flatbuffers")], path: "Sources/RPC",
      swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error)],
      linkerSettings: [.unsafeFlags(["-L", clientLib, "-L", transportLib]), .linkedLibrary("curl"), .linkedLibrary("c++")], plugins: ["FlatcPlugin"]),
  ])
