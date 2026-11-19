import type { LogSliceT } from '../../shared/gen/tsugumi/rpc/log-slice'

export const LOG_READ_BYTES = 1 << 20
const HISTORY_BYTES = LOG_READ_BYTES >> 2
const WINDOW_BYTES = 2 * LOG_READ_BYTES
const BLOCK_LINES = 128
// Keep one wrapped row to at most one section of a long terminal line.
const FRAGMENT_CELLS = 16 * 1024

// LogRange returns the original UTF-8 bytes. A BOM inside the stream is
// content even if the first byte of a page happens to be its start.
const decoder = new TextDecoder('utf-8', { ignoreBOM: true })
export const decodeLog = (text: LogSliceT['text']) =>
  typeof text === 'string' ? text : decoder.decode(text ?? undefined)

export async function readLogFile(size: number, read: (offset: bigint) => Promise<LogSliceT>): Promise<Blob> {
  const chunks: Uint8Array<ArrayBuffer>[] = []
  let offset = 0n
  while (offset < size) {
    const slice = await read(offset)
    const next = slice.nextOffset ?? offset
    if (slice.offset !== offset || !(slice.text instanceof Uint8Array) || next - offset !== BigInt(slice.text.length) || next <= offset)
      throw new Error('Invalid log range while exporting')
    chunks.push(slice.text.slice())
    offset = next
  }
  return new Blob(chunks).slice(0, size, 'text/plain')
}

export type LogForm = 'terminal' | 'journal'
export interface LogSlice {
  text: string
  offset: number
  end: number
  size: number
  line: number
  startsLine: boolean
  truncatedAt?: bigint
  rawSize?: number
  reset?: boolean
}

type Block = { key: string; first: number; lines: string[]; numbers?: number[] }
type Fragment = { number: number; offset: number; text: string; starts: boolean; ends: boolean }
type Page = { slice: LogSlice; head?: Fragment; body: Block[]; tail?: Fragment }

export function logLineNumber(block: Block, index: number): number {
  return block.numbers?.[index] ?? block.first + index
}

function cellCount(text: string): number {
  return text.length - (text.match(/[\uDC00-\uDFFF]/g)?.length ?? 0)
}

function terminalFragments(text: string, offset: number, startsLine: boolean) {
  const cells = cellCount(text) === text.length ? text : Array.from(text)
  const column = (offset / 4) % FRAGMENT_CELLS
  const fragments: { offset: number; text: string }[] = []
  // The first grid section needs its full prefix to wrap consistently.
  let start = startsLine || column === 0 ? 0 : FRAGMENT_CELLS - column
  while (start < cells.length) {
    const length = FRAGMENT_CELLS - ((column + start) % FRAGMENT_CELLS)
    const fragment = cells.slice(start, start + length)
    fragments.push({ offset: offset + start * 4, text: typeof fragment === 'string' ? fragment : fragment.join('') })
    start += length
  }
  return fragments
}

function journalLine(text: string): string | undefined {
  try {
    const v = JSON.parse(text)
    if (v?.phase === 'validate' || v?.phase === 'save') return `${v.phase} · ${v.event ?? ''}: ${v.message ?? ''}`
  } catch (error) {
    if (!(error instanceof SyntaxError)) throw error
  }
}

function page(slice: LogSlice, form: LogForm): Page {
  const body: Block[] = []
  let number = slice.line
  let head: Fragment | undefined
  let start = 0
  let projectedOffset = slice.offset
  if (!slice.startsLine) {
    const lf = slice.text.indexOf('\n')
    head = { number: number++, offset: slice.offset, text: lf < 0 ? slice.text : slice.text.slice(0, lf),
      starts: false, ends: lf >= 0 }
    if (lf < 0) return { slice, head, body }
    start = lf + 1
    if (form === 'terminal') projectedOffset += 4 * (cellCount(head.text) + 1)
  }
  let block: Block | undefined
  let lf = slice.text.indexOf('\n', start)
  while (lf >= 0) {
    const raw = slice.text.slice(start, lf)
    const line = form === 'terminal' ? raw : journalLine(raw)
    const cells = form === 'terminal' ? cellCount(raw) : 0
    if (cells >= FRAGMENT_CELLS) {
      for (const part of terminalFragments(raw, projectedOffset, true)) {
        body.push({ first: number, key: `fragment:${part.offset}`, lines: [part.text] })
      }
      block = undefined
    } else if (line !== undefined) {
      if (!block || block.lines.length === BLOCK_LINES || (line.length > 4096 && block.lines.length > 0)) {
        block = { first: number, key: `lines:${number}`, lines: [], numbers: form === 'journal' ? [] : undefined }
        body.push(block)
      }
      block.lines.push(line)
      block.numbers?.push(number)
      if (line.length > 4096) block = undefined
    }
    if (form === 'terminal') projectedOffset += 4 * (cells + 1)
    ++number
    start = lf + 1
    lf = slice.text.indexOf('\n', start)
  }
  let tail: Fragment | undefined
  if (start < slice.text.length) {
    const text = slice.text.slice(start)
    const length = form === 'terminal'
      ? 4 * cellCount(text)
      : new TextEncoder().encode(text).length
    tail = { number, offset: slice.end - length, text, starts: true, ends: false }
  }
  return { slice, head, body, tail }
}

export type ReadKind = 'tail' | 'earlier' | 'later' | 'append' | 'status'
export type LogRead = { kind: ReadKind; offset: number; limit: number; since?: number }
export type VisibleLines = { first: number; last: number; fragments: ReadonlySet<string> }

/** The same ordered byte window is viewed from either edge. The RPC supplies
 *  the starting line of each page, so discarded bytes need not be counted. */
export class LogWindow {
  private pages: Page[] = []
  private boundaryBlocks = new Map<string, { raw: string; block: Block }>()
  blocks: Block[] = []
  loaded = false
  following = true
  size = 0
  truncatedAt?: bigint
  rawSize = 0
  private displayedRawSize = 0
  private seenEnd = 0
  visible?: VisibleLines

  constructor(readonly form: LogForm) {}

  get start() { return this.pages[0]?.slice.offset ?? 0 }
  get end() { return this.pages.at(-1)?.slice.end ?? 0 }
  get hasUnread() { return this.rawSize > this.displayedRawSize }
  get waiting() { return !this.following && this.rawSize > this.seenEnd }

  follow(value: boolean) {
    if (this.following && !value) this.seenEnd = this.displayedRawSize
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
        return { kind, offset: this.end, limit: LOG_READ_BYTES, since: this.form === 'terminal' ? this.displayedRawSize : undefined }
    }
  }

  apply(read: LogRead, slice: LogSlice) {
    this.loaded = true
    this.size = slice.size
    const rawSize = slice.rawSize ?? slice.size
    this.rawSize = Math.max(this.rawSize, rawSize)
    this.truncatedAt = slice.truncatedAt ?? this.truncatedAt
    if (read.kind === 'status') return
    if (read.kind === 'tail' || read.kind === 'append') this.displayedRawSize = rawSize
    const visible = read.kind === 'tail' || slice.reset ? undefined : this.visible
    if (read.kind === 'tail' || slice.reset) this.pages = []
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
    this.assemble(visible)
  }

  private assemble(visible?: VisibleLines) {
    const blocks: Block[] = []
    const boundary = new Map<string, { raw: string; block: Block }>()
    let pieces: Fragment[] = []
    const add = (key: string, number: number, raw: string) => {
      const previous = this.boundaryBlocks.get(key)
      const text = previous?.raw === raw ? previous.block.lines[0]
        : this.form === 'terminal' ? raw : journalLine(raw)
      if (text === undefined) return
      const block = previous?.raw === raw ? previous.block : { key, first: number, lines: [text] }
      blocks.push(block)
      boundary.set(key, { raw, block })
    }
    const finish = () => {
      const line = pieces
      pieces = []
      const first = line[0], last = line.at(-1)
      if (!first || (this.form === 'journal' && (!first.starts || !last?.ends))) return
      const text = line.map((piece) => piece.text).join('')
      const long = this.form === 'terminal' && cellCount(text) >= FRAGMENT_CELLS
      if (long) {
        for (const part of terminalFragments(text, first.offset, first.starts)) {
          add(`fragment:${part.offset}`, first.number, part.text)
        }
      } else if (first.starts) {
        add(`edge:${first.number}`, first.number, text)
      }
    }
    for (const { head, body, tail } of this.pages) {
      if (head) {
        pieces.push(head)
        if (!head.ends) continue
      }
      finish()
      blocks.push(...body)
      if (tail) pieces.push(tail)
    }
    finish()
    this.boundaryBlocks = boundary
    if (visible) {
      const rowKey = (block: Block, number: number) =>
        block.key.startsWith('fragment:') ? block.key : number
      const inView = (block: Block, number: number) =>
        block.key.startsWith('fragment:') ? visible.fragments.has(block.key)
          : number >= visible.first && number <= visible.last
      const intersect = (block: Block) =>
        block.first <= visible.last && logLineNumber(block, block.lines.length - 1) >= visible.first
      const rendered = new Set<number | string>()
      for (const block of blocks.filter(intersect)) {
        for (let i = 0; i < block.lines.length; i++) {
          const number = logLineNumber(block, i)
          if (inView(block, number)) rendered.add(rowKey(block, number))
        }
      }
      for (const block of this.blocks.filter(intersect)) {
        for (let i = 0; i < block.lines.length; i++) {
          const number = logLineNumber(block, i)
          if (inView(block, number) && !rendered.has(rowKey(block, number))) {
            const key = rowKey(block, number)
            blocks.push({ key: typeof key === 'string' ? key : `visible:${number}`, first: number, lines: [block.lines[i]] })
          }
        }
      }
      // At most one end was trimmed. The retained rows belong next to it.
      blocks.sort((a, b) => a.first - b.first || Number(a.key.split(':')[1]) - Number(b.key.split(':')[1]))
    }
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
      const result = this.result
      this.result = undefined
      result?.()
      this.frame = undefined
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
    return block && { key: block.key, line: block.first, inset: y - this.top[index] }
  }

  position(anchor: LogAnchor): number {
    const index = this.blocks.findIndex((block) => block.key === anchor.key)
    if (index >= 0) return this.top[index] + anchor.inset
    const fallback = this.blocks.findIndex((block) => logLineNumber(block, block.lines.length - 1) >= anchor.line)
    return fallback >= 0 ? this.top[fallback] : this.total
  }
}
