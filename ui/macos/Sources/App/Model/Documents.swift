import FlatBuffers
import Foundation
import TsugumiRPC

// The project documents as this UI edits them (the Web UI's documents.ts).
//
// - A project's layout.bin (protocol/layout_document.fbs) and its notes'
//   Markdown (notes/<id>.md) are one layout view, the JSON object hostd edits
//   under "layout.bin": {flow, cards: {key: place}, boundaries: {scope: bars},
//   boundary, notes: [{id, text, ...}], groups}.
// - A Card's README.md (front matter `description`, Markdown body) and its
//   _comments.json are one note object: {description, content, comments}.
enum Documents {
  // MARK: - Layout

  /// The layout view of a layout.bin and its notes' text ({} when there is none).
  static func layoutView(_ bytes: [UInt8], notes: [String: String]) -> JSONValue {
    guard !bytes.isEmpty else { return .object([]) }
    var buffer = ByteBuffer(bytes: bytes)
    guard let l: Tsugumi_Layout_Layout = try? getCheckedRoot(byteBuffer: &buffer) else { return .object([]) }
    var view: [(String, JSONValue)] = []
    func number(_ v: Double?) -> JSONValue? { v.map { .real($0) } }
    func strings(_ list: FlatbufferVector<String?>) -> JSONValue? {
      let values = list.compactMap { $0 }
      return values.isEmpty ? nil : .array(values.map { .string($0) })
    }
    func object(_ pairs: [(String, JSONValue?)]) -> JSONValue {
      .object(pairs.compactMap { k, v in v.map { (k, $0) } })
    }
    func bars(_ b: Tsugumi_Layout_BoundaryPlace) -> JSONValue {
      object([
        ("inputs", b.inputs.map { object([("x", number($0.x)), ("y", number($0.y))]) }),
        ("outputs", b.outputs.map { object([("x", number($0.x)), ("y", number($0.y))]) }),
      ])
    }
    if let flow = l.flow { view.append(("flow", .string(flow))) }
    let cards: [(String, JSONValue)] = l.cards.map { c in
      (c.key, object([("x", number(c.x)), ("y", number(c.y)), ("collapsed", c.collapsed.map { .bool($0) }),
                      ("inputOrder", strings(c.inputOrder)), ("outputOrder", strings(c.outputOrder))]))
    }
    if !cards.isEmpty { view.append(("cards", .object(cards))) }
    let boundaries: [(String, JSONValue)] = l.boundaries.compactMap { b in b.scope.map { ($0, bars(b)) } }
    if !boundaries.isEmpty { view.append(("boundaries", .object(boundaries))) }
    if let b = l.boundary { view.append(("boundary", bars(b))) }
    let placed: [JSONValue] = l.notes.map { n in
      object([("id", .string(n.id)), ("scope", n.scope.map { .string($0) }), ("text", notes[n.id].map { .string($0) }),
              ("color", n.color.map { .string($0) }), ("x", number(n.x)), ("y", number(n.y)),
              ("width", number(n.width)), ("height", number(n.height)), ("fontSize", number(n.fontSize))])
    }
    if !placed.isEmpty { view.append(("notes", .array(placed))) }
    let groups: [JSONValue] = l.groups.map { g in
      let outputs = g.outputs.map { object([("card", .string($0.card)), ("port", .string($0.port))]) }
      return object([("id", .string(g.id)), ("scope", g.scope.map { .string($0) }), ("name", g.name.map { .string($0) }),
                     ("color", g.color.map { .string($0) }), ("cards", strings(g.cards)),
                     ("outputs", outputs.isEmpty ? nil : .array(outputs))])
    }
    if !groups.isEmpty { view.append(("groups", .array(groups))) }
    return .object(view)
  }

  /// The note id of a project's notes/<id>.md, if the path is one.
  static func noteId(prefix: String, path: String) -> String? {
    guard path.hasPrefix(prefix + "notes/"), path.hasSuffix(".md") else { return nil }
    let id = String(path.dropFirst(prefix.count + 6).dropLast(3))
    guard !id.isEmpty, id.count <= 128, id != ".", id != "..",
          id.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || "_.-".contains($0)) }) else { return nil }
    return id
  }

  // MARK: - README.md

  /// A README.md: its front matter's description and its body.
  static func parseReadme(_ text: String) -> (description: String, body: String) {
    guard text.hasPrefix("---\n"), let end = text.range(of: "\n---\n", range: text.index(text.startIndex, offsetBy: 3)..<text.endIndex)
    else { return ("", text) }
    var description = ""
    let start = text.index(text.startIndex, offsetBy: 4)
    if end.lowerBound >= start {
      for line in text[start..<end.lowerBound].split(separator: "\n") {
        if line.hasPrefix("description: ") { description = String(line.dropFirst("description: ".count)) }
      }
    }
    var body = String(text[end.upperBound...])
    if body.hasPrefix("\n") { body.removeFirst() }
    return (description, body)
  }

  /// The text of a README.md; nil when it says nothing. The description is
  /// one line: line breaks become spaces.
  static func printReadme(description: String, body: String) -> String? {
    let line = description.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }
      .filter { !$0.isEmpty }.joined(separator: " ")
    if line.isEmpty && body.isEmpty { return nil }
    let front = line.isEmpty ? (body.hasPrefix("---\n") ? "---\n---\n" : "") : "---\ndescription: \(line)\n---\n"
    return front + (front.isEmpty || body.isEmpty ? "" : "\n") + body
  }

  // MARK: - A Card's note object

  /// {description, content, comments} from a Card's README.md and _comments.json.
  static func cardNotes(readme: String?, comments: String?) -> JSONValue? {
    var out: [(String, JSONValue)] = []
    if let readme {
      let r = parseReadme(readme)
      if !r.description.isEmpty { out.append(("description", .string(r.description))) }
      if !r.body.isEmpty { out.append(("content", .string(r.body))) }
    }
    if let comments, let list = (try? JSONValue.parse(comments))?["comments"]?.array, !list.isEmpty {
      out.append(("comments", .array(list)))
    }
    return out.isEmpty ? nil : .object(out)
  }

  /// A note object as the README.md text and the _comments.json document it is.
  static func splitCardNotes(_ notes: JSONValue?) -> (readme: String?, comments: JSONValue?) {
    guard let notes else { return (nil, nil) }
    let readme = printReadme(description: notes["description"]?.string ?? "", body: notes["content"]?.string ?? "")
    let list = notes["comments"]?.array ?? []
    return (readme, list.isEmpty ? nil : .object([("comments", .array(list))]))
  }

}
