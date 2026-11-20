// swift-tools-version:6.2
import PackageDescription

let package = Package(
  name: "InputTemplates",
  products: [.library(name: "InputTemplates", targets: ["InputTemplates"])],
  targets: [.target(name: "InputTemplates", resources: [.copy("inputs.json")])]
)
