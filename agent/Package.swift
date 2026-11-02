// swift-tools-version:6.0
// Tsugumi Agent: model connection, conversation and RPC tool adapter (UI side).
import PackageDescription

let package = Package(
  name: "agent",
  platforms: [.macOS("26.0")],
  products: [.library(name: "TsugumiAgent", targets: ["TsugumiAgent"])],
  dependencies: [.package(url: "https://github.com/google/flatbuffers.git", exact: "25.12.19")],
  targets: [
    .target(
      name: "TsugumiAgent",
      dependencies: [.product(name: "FlatBuffers", package: "flatbuffers")],
      path: "Sources/TsugumiAgent",
      swiftSettings: [.swiftLanguageMode(.v5)]),
  ]
)
