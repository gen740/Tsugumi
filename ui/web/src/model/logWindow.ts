import { terminalText } from './terminalText'

export const LOG_READ_BYTES = 1 << 20
const HISTORY_BYTES = LOG_READ_BYTES >> 2
const WINDOW_BYTES = 2 * LOG_READ_BYTES
const BLOCK_LINES = 128

export type LogForm = 'terminal' | 'journal'
export interface LogSlice {
  text: string
  offset: number
  end: number
  size: number
  line: number
  startsLine: boolean
  truncatedAt?: bigint
}

type Line = { number: number; text: string }
type Block = { key: string; lines: Line[] }
type Fragment = Line & { starts: boolean; ends: boolean }
type Page = { slice: LogSlice; head?: Fragment; body: Block[]; tail?: Fragment }

function journalLine(text: string): string | undefined {
  try {
    const v = JSON.parse(text)
    if (v?.phase === 'validate' || v?.phase === 'save') return `${v.phase} · ${v.event ?? ''}: ${v.message ?? ''}`
  } catch (error) {
    if (!(error instanceof SyntaxError)) throw error
  }
}

function showLine(raw: Line, form: LogForm): Line | undefined {
  const text = form === 'terminal' ? terminalText(raw.text) : journalLine(raw.text)
  if (text !== undefined) return { number: raw.number, text }
}

function page(slice: LogSlice, form: LogForm): Page {
  const body: Block[] = []
  let number = slice.line
  let head: Fragment | undefined
  let start = 0
  if (!slice.startsLine) {
    const lf = slice.text.indexOf('\n')
    head = { number: number++, text: lf < 0 ? slice.text : slice.text.slice(0, lf), starts: false, ends: lf >= 0 }
    if (lf < 0) return { slice, head, body }
    start = lf + 1
  }
  let lines: Line[] = []
  let lf = slice.text.indexOf('\n', start)
  while (lf >= 0) {
    const line = showLine({ number: number++, text: slice.text.slice(start, lf) }, form)
    if (line) {
      lines.push(line)
      if (lines.length === BLOCK_LINES) {
        body.push({ key: `lines:${lines[0].number}`, lines })
        lines = []
      }
    }
    start = lf + 1
    lf = slice.text.indexOf('\n', start)
  }
  if (lines.length) body.push({ key: `lines:${lines[0].number}`, lines })
  const tail = start < slice.text.length ? { number, text: slice.text.slice(start), starts: true, ends: false } : undefined
  return { slice, head, body, tail }
}

export type ReadKind = 'tail' | 'earlier' | 'later' | 'append' | 'status'
export type LogRead = { kind: ReadKind; offset: number; limit: number }

/** The same ordered byte window is viewed from either edge. The RPC supplies
 *  the starting line of each page, so discarded bytes need not be counted. */
export class LogWindow {
  private pages: Page[] = []
  private boundaryBlocks = new Map<number, { raw: Fragment; block: Block }>()
  blocks: Block[] = []
  loaded = false
  following = true
  size = 0
  truncatedAt?: bigint
  private seenEnd = 0

  constructor(readonly form: LogForm) {}

  get start() { return this.pages[0]?.slice.offset ?? 0 }
  get end() { return this.pages.at(-1)?.slice.end ?? 0 }
  get waiting() { return !this.following && this.size > this.seenEnd }

  follow(value: boolean) {
    if (this.following && !value) this.seenEnd = this.end
    this.following = value
  }

  request(kind: ReadKind): LogRead | undefined {
    switch (kind) {
      case 'tail': return { kind, offset: 0, limit: LOG_READ_BYTES }
      case 'earlier': return this.start > 0 ? { kind, offset: this.start, limit: HISTORY_BYTES } : undefined
      case 'later': return this.end < this.size ? { kind, offset: this.end, limit: HISTORY_BYTES } : undefined
      case 'status':
        if (!this.following && this.loaded) return { kind, offset: this.end, limit: 4 }
        return this.request(this.loaded ? 'append' : 'tail')
      case 'append':
        if (this.size - this.end > LOG_READ_BYTES) return this.request('tail')
        return { kind, offset: this.end, limit: LOG_READ_BYTES }
    }
  }

  apply(read: LogRead, slice: LogSlice) {
    this.loaded = true
    this.size = Math.max(this.size, slice.size)
    this.truncatedAt = slice.truncatedAt ?? this.truncatedAt
    if (read.kind === 'status') return
    if (read.kind === 'tail') this.pages = []
    if (slice.end > slice.offset) {
      if (read.kind === 'earlier') this.pages.unshift(page(slice, this.form))
      else {
        const last = this.pages.at(-1)?.slice
        // Keep frequent small appends in one page. Rebuilding at most 16 KiB
        // keeps both the parse cost and the number of resident pages bounded.
        if (last && slice.end - last.offset <= 16 * 1024) {
          this.pages.pop()
          slice = { ...last, text: last.text + slice.text, end: slice.end, size: slice.size }
        }
        this.pages.push(page(slice, this.form))
      }
      while (this.end - this.start > WINDOW_BYTES) {
        if (read.kind === 'earlier') this.pages.pop()
        else this.pages.shift()
      }
    }
    this.assemble()
  }

  private assemble() {
    const blocks: Block[] = []
    const boundary = new Map<number, { raw: Fragment; block: Block }>()
    let pending: Fragment | undefined
    const finish = () => {
      if (!pending || (this.form === 'journal' && (!pending.starts || !pending.ends))) return
      const { number, text } = pending
      const previous = this.boundaryBlocks.get(number)
      const line = previous?.raw.text === text ? previous.block.lines[0] : showLine(pending, this.form)
      if (line) {
        const block = previous?.block.lines[0] === line ? previous.block : { key: `edge:${number}`, lines: [line] }
        blocks.push(block)
        boundary.set(number, { raw: pending, block })
      }
    }
    for (const { head, body, tail } of this.pages) {
      if (head) {
        pending = pending
          ? { ...pending, text: pending.text + head.text, ends: head.ends }
          : head
        if (!head.ends) continue
      }
      finish()
      blocks.push(...body)
      pending = tail
    }
    finish()
    this.boundaryBlocks = boundary
    this.blocks = blocks
  }
}

/** Coalesce demand and publish completed reads on a frame. A request received
 *  during I/O stays pending, and the next read cannot overtake the response. */
export class LogReadFrames {
  private frame?: number
  private busy = false
  private closed = false
  private pending = false
  private result?: () => void

  constructor(
    private readonly schedule: (fn: () => void) => number,
    private readonly cancel: (id: number) => void,
    private readonly read: () => Promise<(() => void) | undefined>,
  ) {}

  wake() {
    this.pending = true
    this.scheduleFrame()
  }

  close() {
    this.closed = true
    if (this.frame !== undefined) this.cancel(this.frame)
  }

  private scheduleFrame() {
    if (this.closed || this.frame !== undefined || this.busy) return
    this.frame = this.schedule(() => {
      this.frame = undefined
      const result = this.result
      this.result = undefined
      result?.()
      if (!this.pending) return
      this.pending = false
      this.busy = true
      void this.read().then((apply) => {
        this.result = apply
        this.busy = false
        if (this.result || this.pending) this.scheduleFrame()
      })
    })
  }
}

export type LogAnchor = { key: string; line: number; inset: number }

/** Keep a height per measured block. Unmounted blocks use one line height
 *  per row until they first enter the viewport. */
export class LogHeights {
  readonly top: number[] = [0]

  constructor(readonly blocks: readonly Block[], heights: Pick<ReadonlyMap<Block, number>, 'get'>, estimate: number) {
    for (const block of blocks) this.top.push(this.total + (heights.get(block) ?? block.lines.length * estimate))
  }

  get total() { return this.top[this.top.length - 1] }

  index(y: number): number {
    let lo = 0, hi = this.blocks.length
    while (lo < hi) {
      const middle = (lo + hi) >>> 1
      if (this.top[middle + 1] <= y) lo = middle + 1
      else hi = middle
    }
    return lo
  }

  anchor(y: number): LogAnchor | undefined {
    const index = this.index(y)
    const block = this.blocks[index]
    return block && { key: block.key, line: block.lines[0].number, inset: y - this.top[index] }
  }

  position(anchor: LogAnchor): number {
    const index = this.blocks.findIndex((block) => block.key === anchor.key)
    if (index >= 0) return this.top[index] + anchor.inset
    const fallback = this.blocks.findIndex((block) => block.lines.at(-1)!.number >= anchor.line)
    return fallback >= 0 ? this.top[fallback] : this.total
  }
}
