import FlatBuffers
import Foundation
import TsugumiRPC

// The project documents as this UI edits them (the Web UI's documents.ts).
//
// - A project's layout.bin (protocol/layout_document.fbs) and its notes'
//   Markdown (notes/<id>.md) are one layout view, the JSON object hostd edits
//   under "layout.bin": {cards: {key: place}, boundaries: {scope: bars},
//   boundary, notes: [{id, text, ...}], groups}.
// - A Card's README.md (front matter `title` and `description`, Markdown body) and its
//   _comments.json are one note object: {title, description, content, comments}.
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
    let cards: [(String, JSONValue)] = l.cards.map { c in
      let junctions = c.junctions.map { object([("port", .string($0.port)), ("x", number($0.x)), ("y", number($0.y))]) }
      return (c.key, object([("x", number(c.x)), ("y", number(c.y)), ("collapsed", c.collapsed.map { .bool($0) }),
                             ("inputOrder", strings(c.inputOrder)), ("outputOrder", strings(c.outputOrder)),
                             ("junctions", junctions.isEmpty ? nil : .array(junctions))]))
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

  /// A Card's required README front matter and its Markdown body.
  static func parseReadme(_ text: String) -> (title: String, description: String, body: String)? {
    guard text.hasPrefix("---\n"), let end = text.range(of: "\n---\n"),
          end.lowerBound >= text.index(text.startIndex, offsetBy: 4) else { return nil }
    let start = text.index(text.startIndex, offsetBy: 4)
    var fields: [String: String] = [:]
    for line in text[start..<end.lowerBound].split(separator: "\n", omittingEmptySubsequences: false) {
      let parts = line.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false)
      // Like the core: an empty value may omit the space after the colon.
      guard parts.count == 2, parts[1].isEmpty || parts[1].hasPrefix(" "), !parts[1].utf8.contains(13) else { return nil }
      let key = String(parts[0])
      guard ["title", "description"].contains(key), fields[key] == nil else { return nil }
      fields[key] = parts[1].isEmpty ? "" : String(parts[1].dropFirst())
    }
    guard let title = fields["title"], !title.trimmingCharacters(in: .whitespaces).isEmpty,
          let description = fields["description"] else { return nil }
    var body = String(text[end.upperBound...])
    if body.hasPrefix("\n") { body.removeFirst() }
    return (title, description, body)
  }

  /// The two front matter values occupy one line each.
  static func printReadme(title: String, description: String, body: String) -> String {
    func line(_ value: String) -> String {
      value.replacingOccurrences(of: #"\s*[\r\n]+\s*"#, with: " ", options: .regularExpression)
    }
    return "---\ntitle: \(line(title))\ndescription: \(line(description))\n---\n" + (body.isEmpty ? "" : "\n" + body)
  }

  // MARK: - A Card's note object

  /// {title, description, content, comments} from a Card's README.md and _comments.json.
  static func cardNotes(readme: String?, comments: String?) -> JSONValue? {
    var out: [(String, JSONValue)] = []
    if let readme { out.append(("readmeText", .string(readme))) }
    if let readme, let r = parseReadme(readme) {
      out.append(("title", .string(r.title)))
      out.append(("description", .string(r.description)))
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
    let original = notes["readmeText"]?.string
    let saved = original.flatMap(parseReadme)
    let description = notes["description"]?.string ?? "", body = notes["content"]?.string ?? ""
    // The write guard compares the exact original bytes; both front matter orders are valid.
    var readme = original
    if let title = notes["title"]?.string, saved?.title != title || saved?.description != description || saved?.body != body {
      readme = printReadme(title: title, description: description, body: body)
    }
    let list = notes["comments"]?.array ?? []
    return (readme, list.isEmpty ? nil : .object([("comments", .array(list))]))
  }

}
