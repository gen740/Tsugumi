// A log as a terminal shows it. A carriage return goes back to the start of the
// line and what follows writes over it, so a progress meter that redraws one
// line (curl, a download's percentage) shows its latest state, not every state
// on a line of its own. "\r\n" ends a line. Only what is shown changes: copying
// and saving keep the bytes as they were written.
export function terminalText(s: string): string {
  if (!s.includes('\r')) return s
  return s.split('\n').map((text) => {
    const line: string[] = []
    let column = 0
    for (const c of text) {
      if (c === '\r') column = 0
      else line[column++] = c
    }
    return line.join('')
  }).join('\n')
}
