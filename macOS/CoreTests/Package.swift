// swift-tools-version:6.0
// Platform adapters and launcher tests, built without the app's renderer.
// Core Agent behavior is tested through public RPC in tests/core.
//
//   cd macOS/CoreTests && swift test
import PackageDescription

let package = Package(
  name: "TsugumiCoreTests",
  platforms: [.macOS("26.0")],
  dependencies: [
    .package(url: "https://github.com/google/flatbuffers.git", exact: "25.12.19"),
    .package(path: ".."),
    .package(path: "../../agent"),
  ],
  targets: [
    .testTarget(
      name: "CoreTests",
      dependencies: [
        .product(name: "FlatBuffers", package: "flatbuffers"),
        .product(name: "TsugumiRPC", package: "macOS"),
        .product(name: "TsugumiLauncher", package: "macOS"),
        .product(name: "TsugumiAgent", package: "agent"),
      ],
      path: "Tests/CoreTests",
      swiftSettings: [.swiftLanguageMode(.v5)]),
  ]
)
