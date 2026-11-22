import Foundation
import Testing
@testable import TsugumiApp
import TsugumiRPC

@MainActor struct HostLayoutTests {
  @Test func camerasAreCheckedBeforeSavingAndAfterLoading() throws {
    let valid = ClientViewState.Camera(scope: [], x: 1.5, y: -2, zoom: 1.25)
    let bad = [0.0, .nan].map { ClientViewState.Camera(scope: [], x: 0, y: 0, zoom: $0) }
    let view = ClientViewState(scope: [], cameras: [valid] + bad, selectedCards: [["a"]])
    let decoded = try #require(ClientViewState(bytes: view.bytes()))
    let saved = try #require(decoded.cameras.first)
    #expect(decoded.cameras.count == 1 && saved.x == valid.x && saved.y == valid.y && saved.zoom == valid.zoom)
    #expect(!ClientViewState.Camera(scope: [], x: .infinity, y: 0, zoom: 1).valid)
  }

  @Test func savedPortOrderIgnoresStaleAndDuplicateNames() {
    let data = [PortData(name: "a"), PortData(name: "b"), PortData(name: "new")]
    #expect(orderedPorts(data, saved: ["gone", "b", "b", "a"]).map(\.name) == ["b", "a", "new"])
  }
}
