// Text for a person to read from what a program wrote for a terminal. The
// core already removes terminal control from the failures it builds
// (text::strip_terminal_controls); the UI does the same where it shows a
// failure, a status line or a log line, for records written before that.

const inRange = (c: number, low: number, high: number) => c >= low && c <= high

/** Removes escape sequences (CSI such as colours and cursor moves, OSC such as
 *  links and titles, DCS/SOS/PM/APC strings, two-byte escapes), their C1
 *  forms, and every other control character except tab and line feed. */
export function plainText(text: string): string {
  // Fast path: most text has nothing to remove.
  if (!/[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/.test(text)) return text
  const at = (i: number) => (i < text.length ? text.charCodeAt(i) : 0)
  const csi = (i: number) => {
    while (inRange(at(i), 0x30, 0x3f)) ++i
    while (inRange(at(i), 0x20, 0x2f)) ++i
    if (inRange(at(i), 0x40, 0x7e)) ++i
    return i
  }
  // Ends at BEL or ST (ESC \ or U+009C); another escape also ends it.
  const stringControl = (i: number) => {
    while (i < text.length) {
      const c = at(i)
      if (c === 0x07 || c === 0x9c) return i + 1
      if (c === 0x1b) return at(i + 1) === 0x5c ? i + 2 : i
      ++i
    }
    return i
  }
  let out = ''
  let i = 0
  while (i < text.length) {
    const c = at(i)
    if (c === 0x1b) {
      const next = at(i + 1)
      if (next === 0x5b) i = csi(i + 2) // [
      else if (next === 0x5d || next === 0x50 || next === 0x58 || next === 0x5e || next === 0x5f) i = stringControl(i + 2) // ] P X ^ _
      else if (inRange(next, 0x20, 0x2f)) {
        i += 2
        while (inRange(at(i), 0x20, 0x2f)) ++i
        if (inRange(at(i), 0x30, 0x7e)) ++i
      } else if (inRange(next, 0x30, 0x7e)) i += 2
      else ++i // a lone ESC
    } else if (c === 0x9b) {
      i = csi(i + 1)
    } else if (c === 0x9d || c === 0x90 || c === 0x98 || c === 0x9e || c === 0x9f) {
      i = stringControl(i + 1)
    } else if ((c < 0x20 && c !== 0x09 && c !== 0x0a) || inRange(c, 0x7f, 0x9f)) {
      ++i
    } else {
      const start = i
      while (i < text.length) {
        const d = at(i)
        if (d === 0x1b || (d < 0x20 && d !== 0x09 && d !== 0x0a) || inRange(d, 0x7f, 0x9f)) break
        ++i
      }
      out += text.slice(start, i)
    }
  }
  return out
}
