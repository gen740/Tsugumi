// swift-tools-version:6.0
// Tsugumi macOS application (SwiftUI UI, RPC client, container launcher).
import PackageDescription

let package = Package(
  name: "Tsugumi",
  platforms: [.macOS("26.0")],
  products: [
    .executable(name: "Tsugumi", targets: ["TsugumiApp"]),
    .library(name: "TsugumiRPC", targets: ["TsugumiRPC"]),
  ],
  dependencies: [
    .package(url: "https://github.com/google/flatbuffers.git", exact: "25.12.19"),
    .package(path: "../agent"),
  ],
  targets: [
    // Runs the standard flatc on protocol/*.fbs; output stays in .build.
    .plugin(name: "FlatcPlugin", capability: .buildTool(), path: "Plugins/FlatcPlugin"),
    .target(
      name: "TsugumiRPC",
      dependencies: [.product(name: "FlatBuffers", package: "flatbuffers")],
      path: "Sources/RPC",
      swiftSettings: [.swiftLanguageMode(.v5)],
      plugins: ["FlatcPlugin"]),
    .target(
      name: "TsugumiLauncher",
      dependencies: ["TsugumiRPC"],
      path: "Sources/Launcher",
      swiftSettings: [.swiftLanguageMode(.v5)]),
    .target(
      name: "TsugumiRenderer",
      path: "Sources/Renderer",
      swiftSettings: [.swiftLanguageMode(.v5)]),
    .executableTarget(
      name: "TsugumiApp",
      dependencies: [
        "TsugumiRPC", "TsugumiLauncher", "TsugumiRenderer",
        .product(name: "TsugumiAgent", package: "agent"),
      ],
      path: "Sources/App",
      swiftSettings: [.swiftLanguageMode(.v5)]),
    .testTarget(
      name: "TsugumiTests",
      dependencies: ["TsugumiRPC", "TsugumiLauncher", .product(name: "TsugumiAgent", package: "agent")],
      path: "Tests/TsugumiTests",
      swiftSettings: [.swiftLanguageMode(.v5)]),
  ]
)
