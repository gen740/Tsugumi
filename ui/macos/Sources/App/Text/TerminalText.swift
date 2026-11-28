import Foundation

/// Text for a person to read from what a program wrote for a terminal. The
/// core already removes terminal control from the failures it builds
/// (`text::strip_terminal_controls`); the app does the same where it shows a
/// failure, a status line or a log line, for records written before that.
///
/// Removes escape sequences (CSI such as colours and cursor moves, OSC such as
/// links and titles, DCS/SOS/PM/APC strings, two-byte escapes), their C1
/// forms, and every other control character except tab and line feed.
func strippingTerminalControls(_ text: String) -> String {
  func control(_ v: UInt32) -> Bool { (v < 0x20 && v != 0x09 && v != 0x0A) || (0x7F...0x9F).contains(v) }
  guard text.unicodeScalars.contains(where: { control($0.value) }) else { return text }
  let scalars = Array(text.unicodeScalars)
  func at(_ i: Int) -> UInt32 { i < scalars.count ? scalars[i].value : 0 }
  func csi(_ start: Int) -> Int {
    var i = start
    while (0x30...0x3F).contains(at(i)) { i += 1 }
    while (0x20...0x2F).contains(at(i)) { i += 1 }
    if (0x40...0x7E).contains(at(i)) { i += 1 }
    return i
  }
  // Ends at BEL or ST (ESC \ or U+009C); another escape also ends it.
  func stringControl(_ start: Int) -> Int {
    var i = start
    while i < scalars.count {
      let c = at(i)
      if c == 0x07 || c == 0x9C { return i + 1 }
      if c == 0x1B { return at(i + 1) == 0x5C ? i + 2 : i }
      i += 1
    }
    return i
  }
  var out = String.UnicodeScalarView()
  var i = 0
  while i < scalars.count {
    let c = at(i)
    if c == 0x1B {
      let next = at(i + 1)
      if next == 0x5B {  // [
        i = csi(i + 2)
      } else if [0x5D, 0x50, 0x58, 0x5E, 0x5F].contains(next) {  // ] P X ^ _
        i = stringControl(i + 2)
      } else if (0x20...0x2F).contains(next) {
        i += 2
        while (0x20...0x2F).contains(at(i)) { i += 1 }
        if (0x30...0x7E).contains(at(i)) { i += 1 }
      } else if (0x30...0x7E).contains(next) {
        i += 2
      } else {
        i += 1  // a lone ESC
      }
    } else if c == 0x9B {
      i = csi(i + 1)
    } else if [0x9D, 0x90, 0x98, 0x9E, 0x9F].contains(c) {
      i = stringControl(i + 1)
    } else if control(c) {
      i += 1
    } else {
      out.append(scalars[i])
      i += 1
    }
  }
  return String(out)
}
