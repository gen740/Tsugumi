// The file the Agent is writing, as the Mac app's AgentPresence and WritingPanel:
// read out of a save's arguments while the model writes them, played slowly
// enough to see, and placed clear of the bird by its Card.

/** A file as it arrives in the arguments of a save (project_save,
 *  project_validate): `path` is undefined until its key has come, `text` is what
 *  has come of `bytes`, and `done` says the change is complete. */
export interface SourceWriting { path?: string; text: string; done: boolean }

type Frame = { object: false } | { object: true; key?: string; awaitingKey: boolean; isFile: boolean }
type Role = 'key' | 'path' | 'bytes' | 'other'

/** Reads the JSON of tool arguments as it streams in, one piece at a time and
 *  at any cut, for the object being written that has `path` and `bytes` (a
 *  PutSource). Nothing is parsed twice and nothing waits for the JSON to end. */
export class SourceWritingScanner {
  current?: SourceWriting
  private stack: Frame[] = []
  private role?: Role
  private token = ''
  private escaped = false
  private hex?: string

  feed(delta: string) {
    for (let i = 0; i < delta.length; i++) {
      if (this.role) this.string(delta[i])
      else this.structure(delta[i])
    }
  }

  private top() {
    const t = this.stack[this.stack.length - 1]
    return t?.object ? t : undefined
  }

  private structure(c: string) {
    if (c === '{') this.stack.push({ object: true, awaitingKey: true, isFile: false })
    else if (c === '[') this.stack.push({ object: false })
    else if (c === '}' || c === ']') {
      const t = this.stack.pop()
      if (t?.object && t.isFile && this.current) this.current.done = true
    } else if (c === ',') {
      const t = this.top()
      if (t) { t.awaitingKey = true; t.key = undefined }
    } else if (c === '"') {
      this.token = ''
      const t = this.top()
      if (!t) this.role = 'other'
      else if (t.awaitingKey) this.role = 'key'
      else if (t.key === 'path' || t.key === 'bytes') {
        this.role = t.key
        // The object at the top is the file being written from its first `path` or `bytes`.
        if (!t.isFile) {
          this.current = { text: '', done: false }
          t.isFile = true
        }
      } else this.role = 'other'
    }
  }

  private string(c: string) {
    if (this.hex !== undefined) {
      this.hex += c
      if (this.hex.length === 4) {
        const code = parseInt(this.hex, 16)
        this.hex = undefined
        this.append(Number.isNaN(code) ? '�' : String.fromCharCode(code))
      }
    } else if (this.escaped) {
      this.escaped = false
      if (c === 'u') this.hex = ''
      else this.append(({ n: '\n', t: '\t', r: '\r', b: '\b', f: '\f' } as Record<string, string>)[c] ?? c)
    } else if (c === '\\') this.escaped = true
    else if (c === '"') {
      if (this.role === 'key') {
        const t = this.top()
        if (t) { t.key = this.token; t.awaitingKey = false }
      } else if (this.role === 'path' && this.current) this.current.path = this.token
      this.role = undefined
    } else this.append(c)
  }

  private append(c: string) {
    if (this.role === 'bytes') { if (this.current) this.current.text += c }
    else if (this.role === 'key' || this.role === 'path') this.token += c
  }
}

/** The tools whose arguments carry whole files, shown as they are written. */
export const writesSources = (tool: string) => tool === 'project_save' || tool === 'project_validate'

/** `definitions/<id>/<file>` → its Card and file. */
export function fileOfCard(path: string): { card: string; file: string } | undefined {
  const parts = path.split('/').filter(Boolean)
  return parts.length >= 3 && parts[0] === 'definitions' ? { card: parts[1], file: parts.slice(2).join('/') } : undefined
}

export interface LiveCode { card: string; file: string; text: string; saved: boolean }

/** A file is played no faster than `rate` (characters a second), however fast
 *  it arrives, and each stays long enough to see; a long one speeds up so that
 *  the bird does not stay far behind (seconds). */
export const CODE_TIMING = { rate: 150, shortest: 2, longest: 5, hold: 0.8, backlog: 20, first: 1.6 }

/** The files a model writes, a scene each, one after another. Times in seconds. */
export class LiveCodePlayer {
  private scenes: { start: number; length: number; code: LiveCode }[] = []
  private scanner = new SourceWritingScanner()
  private index?: number
  private path?: string
  private live?: { start: number; length: number; code: LiveCode }

  /** A piece of a tool call's arguments as the model writes them. */
  input(index: number, delta: string, now: number) {
    if (this.index !== index) {
      this.arrived(now)
      this.path = undefined
      this.index = index
      this.scanner = new SourceWritingScanner()
    }
    this.scanner.feed(delta)
    const w = this.scanner.current
    const at = w?.path ? fileOfCard(w.path) : undefined
    if (!w?.path || !at) {
      if (w) this.arrived(now)
      return
    }
    if (this.path !== w.path) {
      this.arrived(now)
      this.path = w.path
      this.startScene({ ...at, text: w.text, saved: false }, now)
    } else if (this.live) {
      this.live.code.text = w.text
      // It goes on arriving: its scene goes on.
      if (this.live.start + this.live.length < now + 0.5) this.live.length = now - this.live.start + 1
    }
    if (w.done) this.arrived(now)
  }

  private startScene(code: LiveCode, now: number) {
    const last = this.scenes[this.scenes.length - 1]
    const start = Math.max(now, last ? last.start + last.length : now)
    if (start - now > CODE_TIMING.backlog) return // too far behind: not shown
    this.scenes = this.scenes.filter((s) => s.start + s.length > now)
    this.live = { start, length: CODE_TIMING.first, code }
    this.scenes.push(this.live)
  }

  /** The file has all arrived: its scene plays it out and holds its end. */
  private arrived(now: number) {
    if (!this.live) return
    const played = Math.min(CODE_TIMING.longest, Math.max(CODE_TIMING.shortest, this.live.code.text.length / CODE_TIMING.rate))
    this.live.length = Math.max(now - this.live.start, played) + CODE_TIMING.hold
    this.live = undefined
  }

  /** The call ended, was asked again or was given up. What a call that went
   *  through wrote plays on; `ok` false: it is not on the screen any more. */
  ended(ok: boolean, now: number) {
    this.arrived(now)
    this.path = undefined
    this.index = undefined
    if (!ok) this.scenes = []
  }

  /** The save has gone through: what still plays says so. */
  saved() {
    for (const s of this.scenes) s.code.saved = true
  }

  /** The file to show at `now`: as far as it has been played, never past what has arrived. */
  at(now: number): LiveCode | undefined {
    const s = this.scenes.find((x) => x.start <= now && now < x.start + x.length)
    if (!s) return undefined
    // A long file speeds up to end within `longest`.
    const rate = Math.max(CODE_TIMING.rate, s.code.text.length / CODE_TIMING.longest)
    let text = s.code.text.slice(0, Math.floor((now - s.start) * rate))
    // Not half of a character outside the BMP.
    if (/[\ud800-\udbff]$/.test(text)) text = text.slice(0, -1)
    return { ...s.code, text }
  }
}

export interface Box { x: number; y: number; width: number; height: number }

/** The panel at scale 1, its gap to what is around it and how many lines it shows. */
export const PANEL = { width: 300, height: 112, gap: 8, lines: 5, margins: { top: 52, left: 12, bottom: 48, right: 12 } }

/** Scaled with the canvas only as far as stays readable. */
export const panelScale = (zoom: number) => Math.min(1.3, Math.max(0.8, zoom))

/** The last lines of the text; the line being written is whole at its end.
 *  A tab takes the room of four spaces. */
export function panelTail(text: string, lines = PANEL.lines): string[] {
  return text.split(/\r\n|\r|\n/).slice(-lines).map((l) => l.replace(/\t/g, '    '))
}

/** Where the panel goes (points on the canvas), given the Card there: above the
 *  bird (`clear`: how far above the Card's top edge it reaches), or under the
 *  Card when there is no room above or it would cover something there (`others`)
 *  and would not under it. Wherever it is, inside the view. undefined: the Card
 *  is out of view. */
export function panelFrame(card: Box, clear: number, scale: number, view: Box, others: Box[] = []): Box | undefined {
  const inView = card.x < view.x + view.width && view.x < card.x + card.width && card.y < view.y + view.height && view.y < card.y + card.height
  if (!inView) return undefined
  const width = PANEL.width * scale, height = PANEL.height * scale
  const above = card.y - clear - PANEL.gap - height, below = card.y + card.height + PANEL.gap
  const maxY = view.y + view.height, maxX = view.x + view.width
  const x = Math.min(Math.max(card.x + card.width / 2 - width / 2, view.x), maxX - width)
  const fitsAbove = above >= view.y, fitsBelow = below + height <= maxY
  const free = (y: number) => !others.some((o) => o.x < x + width && x < o.x + o.width && o.y < y + height && y < o.y + o.height)
  const y = fitsAbove && free(above) ? above
    : fitsBelow && free(below) ? below
      : fitsAbove || (!fitsBelow && card.y - view.y > maxY - (card.y + card.height)) ? above : below
  return { x, y: Math.min(Math.max(y, view.y), maxY - height), width, height }
}
