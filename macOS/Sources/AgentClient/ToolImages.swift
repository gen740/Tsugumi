/// An image returned in a core-owned conversation snapshot.
public struct ToolImage: Codable, Equatable, Sendable {
  public var mediaType: String
  public var data: String  // base64

  enum CodingKeys: String, CodingKey {
    case mediaType = "media_type"
    case data
  }
}
