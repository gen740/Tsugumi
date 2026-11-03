// swift-tools-version:6.0
// Tests of the model, RPC, Agent and launcher: built without the app (its
// views, editors and renderer), so they run in a fraction of its build.
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
