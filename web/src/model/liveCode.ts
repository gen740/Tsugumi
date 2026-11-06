// The file the Agent is writing, as the Mac app's AgentPresence and WritingPanel:
// read out of a save's arguments while the model writes them, shown as far as
// they have arrived, and placed clear of the bird by its Card.

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

/** The API sends a file in lumps of about 200 characters, about every 0.43 s
 *  (measured: docs/ui-view.md), which at once would replace the panel's lines each
 *  time: what arrives is written out over `ease`, up to the next lump. A short file
 *  is seen `shortest` at the least; its last lines stay `hold` when no other file
 *  comes after it; a pause in what arrives may last `stalled` before its scene ends
 *  by itself (seconds). */
export const CODE_TIMING = { ease: 0.45, shortest: 1.5, hold: 0.8, stalled: 5 }

interface Scene { start: number; length: number; code: LiveCode; shownFrom: number; grewAt: number }

/** How many characters of the scene's file are shown at `now`. */
function shownOf(s: Scene, now: number): number {
  const all = s.code.text.length
  const u = (now - s.grewAt) / CODE_TIMING.ease
  return u >= 1 ? all : Math.min(all, s.shownFrom + Math.floor((all - s.shownFrom) * Math.max(0, u)))
}

/** The files a model writes, a scene each, one after another. Times in seconds. */
export class LiveCodePlayer {
  private scenes: Scene[] = []
  private scanner = new SourceWritingScanner()
  private index?: number
  private path?: string
  private live?: Scene

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
      this.live.shownFrom = shownOf(this.live, now)
      this.live.grewAt = now
      this.live.code.text = w.text
      // It goes on arriving: its scene goes on.
      if (this.live.start + this.live.length < now + CODE_TIMING.stalled / 2) this.live.length = now - this.live.start + CODE_TIMING.stalled
    }
    if (w.done) this.arrived(now)
  }

  private startScene(code: LiveCode, now: number) {
    // The file being written goes first: one that has all arrived and still
    // waits is not shown, and the one on the screen gives up its hold. So the
    // window is never further behind than `shortest`.
    this.scenes = this.scenes.filter((s) => s.start <= now && s.start + s.length > now)
    const last = this.scenes[this.scenes.length - 1]
    if (last) last.length = Math.max(now - last.start, CODE_TIMING.shortest)
    this.live = { start: last ? last.start + last.length : now, length: CODE_TIMING.stalled, code, shownFrom: 0, grewAt: now }
    this.scenes.push(this.live)
  }

  /** The file has all arrived: its scene holds its end. */
  private arrived(now: number) {
    if (!this.live) return
    this.live.length = Math.max(now - this.live.start, CODE_TIMING.shortest) + CODE_TIMING.hold
    this.live = undefined
  }

  /** The call ended, was asked again or was given up. What a call that went
   *  through wrote holds its last lines; `ok` false: it is not on the screen any more. */
  ended(ok: boolean, now: number) {
    this.arrived(now)
    this.path = undefined
    this.index = undefined
    if (!ok) this.stop()
  }

  /** No file is on the screen from now on: a call failed, or the workflow runs. */
  stop() {
    this.scenes = []
  }

  /** The save has gone through: a file that still waits is not shown, and the
   *  one on the screen says so for what is left of its hold. */
  saved(now: number) {
    this.scenes = this.scenes.filter((s) => s.start <= now)
    for (const s of this.scenes) s.code.saved = true
  }

  /** The file to show at `now`, as far as it has been written out, never past what has arrived. */
  at(now: number): LiveCode | undefined {
    const s = this.scenes.find((x) => x.start <= now && now < x.start + x.length)
    if (!s) return undefined
    let text = s.code.text.slice(0, shownOf(s, now))
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

/** Where the panel of a Card that is not on the canvas goes (a new one has no
 *  place until it is saved): the first place that covers nothing, from the top
 *  middle of the view on (`slot`: a later panel starts further on). */
export function panelFreeFrame(scale: number, view: Box, others: Box[] = [], slot = 0): Box {
  const width = PANEL.width * scale, height = PANEL.height * scale
  const xs = [view.x + view.width / 2 - width / 2, view.x, view.x + view.width - width]
  const ys = [view.y, view.y + view.height - height]
  const places = [[0, 0], [0, 1], [1, 0], [2, 0], [1, 1], [2, 1]].map(([i, j]) => ({ x: xs[i], y: ys[j], width, height }))
  const turn = slot % places.length
  const turned = [...places.slice(turn), ...places.slice(0, turn)]
  const free = (p: Box) => !others.some((o) => o.x < p.x + p.width && p.x < o.x + o.width && o.y < p.y + p.height && p.y < o.y + o.height)
  return turned.find(free) ?? turned[0]
}
