// swift-tools-version:6.2
import PackageDescription
let package = Package(
  name: "TsugumiVM", platforms: [.macOS("26.0")],
  products: [.library(name: "TsugumiVMTypes", targets: ["TsugumiVMTypes"]), .library(name: "TsugumiVMRuntime", targets: ["TsugumiVMRuntime"]), .executable(name: "tsugumi-vmhost", targets: ["TsugumiVMHost"])],
  dependencies: [.package(path: "../../../client/swift"), .package(url: "https://github.com/google/flatbuffers.git", exact: "25.12.19")],
  targets: [
    .target(name: "TsugumiVMTypes", path: "Sources/VMTypes", swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error, .when(configuration: .debug))]),
    .target(name: "TsugumiVMRuntime", dependencies: ["TsugumiVMTypes", .product(name: "TsugumiRPC", package: "swift"), .product(name: "FlatBuffers", package: "flatbuffers")], path: "Sources/VMRuntime", swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error, .when(configuration: .debug))]),
    .executableTarget(name: "TsugumiVMHost", dependencies: ["TsugumiVMTypes", "TsugumiVMRuntime", .product(name: "TsugumiRPC", package: "swift"), .product(name: "FlatBuffers", package: "flatbuffers")], path: "Sources/VMHost", swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error, .when(configuration: .debug))]),
    .testTarget(name: "VMRuntimeTests", dependencies: ["TsugumiVMTypes", "TsugumiVMRuntime", .product(name: "TsugumiRPC", package: "swift"), .product(name: "FlatBuffers", package: "flatbuffers")], path: "Tests/VMRuntimeTests", swiftSettings: [.swiftLanguageMode(.v5), .treatAllWarnings(as: .error, .when(configuration: .debug))]),
  ])
