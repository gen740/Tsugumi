/// A log as a terminal shows it. A carriage return goes back to the start of
/// the line and what follows writes over it, so a progress meter that redraws
/// one line (curl, a download's percentage) shows its latest state, not every
/// state on a line of its own. "\r\n" ends a line. Only what is shown changes:
/// copying and saving keep the bytes as they were written.
func terminalText(_ s: String) -> String {
  // (on scalars: "\r\n" is a single Character)
  guard s.unicodeScalars.contains("\r") else { return s }
  var out = String.UnicodeScalarView()
  var line: [Unicode.Scalar] = []
  var column = 0
  for u in s.unicodeScalars {
    switch u {
    case "\n":
      out.append(contentsOf: line)
      out.append(u)
      line.removeAll(keepingCapacity: true)
      column = 0
    case "\r":
      column = 0
    default:
      if column < line.count { line[column] = u } else { line.append(u) }
      column += 1
    }
  }
  out.append(contentsOf: line)
  return String(out)
}
