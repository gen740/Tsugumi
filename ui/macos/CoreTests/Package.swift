// swift-tools-version:6.2
// Platform adapters and launcher tests, built without the app's renderer.
// Core Agent behavior is tested through public RPC in tests/core.
//
//   cd ui/macos/CoreTests && swift test
import PackageDescription

let package = Package(
  name: "TsugumiCoreTests",
  platforms: [.macOS("26.0")],
  dependencies: [
    .package(url: "https://github.com/google/flatbuffers.git", exact: "25.12.19"),
    .package(name: "TsugumiUI", path: ".."),
    .package(path: "../../../client/swift"),
    .package(name: "TsugumiVM", path: "../../../vm/macos/runtime"),
  ],
  targets: [
    .testTarget(
      name: "CoreTests",
      dependencies: [
        .product(name: "FlatBuffers", package: "flatbuffers"),
        .product(name: "TsugumiTestSupport", package: "TsugumiUI"),
        .product(name: "TsugumiRPC", package: "swift"),
        .product(name: "TsugumiVMTypes", package: "TsugumiVM"),
        .product(name: "TsugumiLauncher", package: "TsugumiUI"),
        .product(name: "TsugumiAgentClient", package: "TsugumiUI"),
      ],
      path: "Tests/CoreTests",
      swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error)]),
  ]
)
