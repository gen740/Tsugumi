import Foundation
import Testing
import TsugumiTestSupport
@testable import TsugumiApp
@testable import TsugumiLauncher
@testable import TsugumiRPC

/// The native UI applies the host's proposals as one undoable layout edit.
@MainActor @Suite(.hostService) struct HostLayoutTests {
  @Test func camerasAreCheckedBeforeSavingAndAfterLoading() throws {
    let valid = ClientViewState.Camera(scope: [], x: 1.5, y: -2, zoom: 1.25)
    let bad = [0.0, -1, .infinity, .nan].map { ClientViewState.Camera(scope: [], x: 0, y: 0, zoom: $0) }
    let view = ClientViewState(scope: [], cameras: [valid] + bad, selectedCards: [["a"]])
    let decoded = try JSONDecoder().decode(ClientViewState.self, from: Data(try view.json().utf8))
    #expect(decoded.cameras.count == 1)
    #expect(decoded.cameras[0].zoom == 1.25)
    #expect(!ClientViewState.Camera(scope: [], x: .infinity, y: 0, zoom: 1).valid)
  }

  @Test func savedPortOrderIgnoresStaleAndDuplicateNames() {
    let data = [PortData(name: "a"), PortData(name: "b"), PortData(name: "new")]
    #expect(orderedPorts(data, saved: ["gone", "b", "b", "a"]).map(\.name) == ["b", "a", "new"])
  }


}
