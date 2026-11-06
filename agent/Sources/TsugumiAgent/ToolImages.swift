import Foundation

/// An image in a tool's result (a Card's display in run_record, a PNG read from
/// the project or an artifact), which the model sees as an image, never as text.
public struct ToolImage: Codable, Equatable, Sendable {
  public var mediaType: String
  public var data: String  // base64

  enum CodingKeys: String, CodingKey {
    case mediaType = "media_type"
    case data
  }

  /// What one image may be, in base64 characters (the Claude API's limit).
  static let sizeLimit = 10_000_000

  /// Told in the text of a result whose images the provider did not accept.
  static let refusedNote = "\n[the model service did not accept the image of this result, so it is not shown]"

  /// A tool's result without the data of its images, and those images. Byte
  /// fields come as {"base64": ...}: PNG and JPEG data among them leaves the
  /// text, whole or not (a cut image is no use to read).
  static func split(_ result: String) -> (text: String, images: [ToolImage]) {
    guard result.contains(#""base64":"#), let parsed = try? JSONSerialization.jsonObject(with: Data(result.utf8)) else { return (result, []) }
    var images: [ToolImage] = []
    var found = false
    let out = replacing(parsed, &images, &found)
    guard found, let data = try? JSONSerialization.data(withJSONObject: out, options: [.sortedKeys]) else { return (result, []) }
    return (String(decoding: data, as: UTF8.self), images)
  }

  private static func replacing(_ value: Any, _ images: inout [ToolImage], _ found: inout Bool) -> Any {
    if let list = value as? [Any] { return list.map { replacing($0, &images, &found) } }
    guard let object = value as? [String: Any] else { return value }
    if object.count == 1, let base64 = object["base64"] as? String, let (type, name) = kind(base64), let bytes = Data(base64Encoded: base64) {
      found = true
      let about = "\(name) image, \(bytes.count) bytes"
      if !isWhole(bytes, type) { return "\(about): only part of it, not shown" }
      if base64.count > sizeLimit { return "\(about): too large to show" }
      images.append(ToolImage(mediaType: type, data: base64))
      return "\(about): shown with this result"
    }
    return object.mapValues { replacing($0, &images, &found) }
  }

  private static func kind(_ base64: String) -> (type: String, name: String)? {
    if base64.hasPrefix("iVBORw0KGgo") { return ("image/png", "PNG") }
    if base64.hasPrefix("/9j/") { return ("image/jpeg", "JPEG") }
    return nil
  }

  /// Whether the data runs to its end marker (a read can stop in the middle).
  private static func isWhole(_ bytes: Data, _ type: String) -> Bool {
    if type == "image/png" { return bytes.count >= 12 && bytes.suffix(8).prefix(4).elementsEqual("IEND".utf8) }
    return bytes.suffix(2).elementsEqual([0xFF, 0xD9])
  }
}

extension Conversation {
  var hasToolImages: Bool {
    messages.contains { $0.content.contains { if case .toolResult(_, _, _, let images) = $0 { return !images.isEmpty }; return false } }
  }

  /// A request with an image was refused: without them the conversation goes
  /// on, and the model is told which results had one.
  mutating func dropToolImages() {
    for m in messages.indices {
      messages[m].content = messages[m].content.map { block in
        guard case .toolResult(let id, let content, let isError, let images) = block, !images.isEmpty else { return block }
        return .toolResult(toolUseId: id, content: content + ToolImage.refusedNote, isError: isError)
      }
    }
  }
}
