// swift-tools-version:6.0
// Tsugumi macOS application (SwiftUI UI, RPC client, container launcher).
import Foundation
import PackageDescription

let rendererLib = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
  .appendingPathComponent("../build-renderer/ui/renderer").standardizedFileURL.path

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
    // Dawn renderer: ui/renderer/build.sh produces build-renderer/ui/renderer/libtsugumi_renderer.dylib.
    .systemLibrary(name: "TsugumiRendererC", path: "Sources/RendererC"),
    .target(
      name: "TsugumiRenderer",
      dependencies: ["TsugumiRendererC"],
      path: "Sources/Renderer",
      swiftSettings: [.swiftLanguageMode(.v5)],
      linkerSettings: [
        .unsafeFlags(["-L", rendererLib, "-Xlinker", "-rpath", "-Xlinker", rendererLib,
                      "-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks"]),
      ]),
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
      dependencies: ["TsugumiRPC", "TsugumiLauncher", "TsugumiApp", .product(name: "TsugumiAgent", package: "agent")],
      path: "Tests/TsugumiTests",
      swiftSettings: [.swiftLanguageMode(.v5)]),
  ]
)
