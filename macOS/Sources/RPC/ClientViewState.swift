import Foundation

public struct ClientViewState: Codable {
  public struct Camera: Codable {
    public var scope: [String]
    public var x: Double
    public var y: Double
    public var zoom: Double
    public init(scope: [String], x: Double, y: Double, zoom: Double) {
      self.scope = scope; self.x = x; self.y = y; self.zoom = zoom
    }
    public var valid: Bool { x.isFinite && y.isFinite && zoom.isFinite && zoom > 0 }
  }
  public var scope: [String]
  public var cameras: [Camera]
  public var selected_cards: [[String]]
  public init(scope: [String], cameras: [Camera], selectedCards: [[String]]) {
    self.scope = scope; self.cameras = cameras.filter(\.valid); self.selected_cards = selectedCards
  }
  public func json() throws -> String {
    String(decoding: try JSONEncoder().encode(self), as: UTF8.self)
  }
}
