import { memo, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { L } from '../i18n'
import { LogForm, LogHeights, LogReadFrames, LogWindow, ReadKind } from '../model/logWindow'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { errorText, key, ProjectSession } from '../model/session'
import { D, RPCFailure, s, segs } from '../wire/rpc'
import { Button } from './controls'

export interface LogSource { runId: string; card: string[] | null; invocation: bigint | null; stream: D.LogStream }

const EMPTY = new LogWindow('terminal')
type View = Pick<LogWindow, 'blocks' | 'loaded' | 'size' | 'start' | 'end' | 'waiting' | 'following' | 'truncatedAt'> & {
  error?: string
  follow: (value: boolean) => void
  page: (kind: 'earlier' | 'later') => void
  jump: () => void
  id: string
}

export function useLogStream(session: ProjectSession, source: LogSource | undefined, form: LogForm): View {
  const id = source ? JSON.stringify([source.runId, source.card, String(source.invocation), source.stream, form]) : ''
  const empty: View = {
    id, blocks: EMPTY.blocks, loaded: false, size: 0, start: 0, end: 0, waiting: false, following: true,
    follow: () => {}, page: () => {}, jump: () => {},
  }
  const [snapshot, setSnapshot] = useState<View>(empty)
  const rpc = session.rpc
  useEffect(() => {
    if (!source || !rpc) { setSnapshot(empty); return }
    const window = new LogWindow(form)
    let demand: ReadKind | undefined = 'tail'
    let changed = false
    let sizeDirty = false
    let error: string | undefined
    const publish = () => setSnapshot({
      id, blocks: window.blocks, loaded: window.loaded, start: window.start, end: window.end,
      size: window.size, waiting: window.waiting, following: window.following, truncatedAt: window.truncatedAt,
      error, follow, page, jump,
    })
    const follow = (value: boolean) => {
      if (window.following === value) return
      window.follow(value)
      if (value && window.end < window.size) wake()
      publish()
    }
    const page = (kind: 'earlier' | 'later') => {
      if (demand === 'tail') return
      demand = kind
      frames.wake()
    }
    const jump = () => {
      window.follow(true)
      demand = 'tail'
      frames.wake()
    }
    const wake = () => {
      changed = true
      frames.wake()
    }
    const frames = new LogReadFrames(requestAnimationFrame, cancelAnimationFrame, async () => {
      if (sizeDirty) {
        sizeDirty = false
        publish()
      }
      const kind = demand ?? (changed ? 'status' : undefined)
      demand = undefined
      changed = false
      if (kind === undefined) return
      const request = window.request(kind)
      if (!request) return
      const before = request.kind === 'earlier' ? BigInt(request.offset) : null
      try {
        const result = await rpc.logRange(source.runId, source.card, source.invocation, source.stream,
          before === null ? BigInt(request.offset) : 0n, request.kind === 'tail', request.limit, before)
        return () => {
          const slice = {
            text: s(result.text), offset: Number(result.offset), end: Number(result.nextOffset),
            size: Number(result.sizeBytes), line: Number(result.lineNumber), startsLine: result.startsLine,
            truncatedAt: result.logTruncatedAtBytes ?? undefined,
          }
          const pausedDuringRead = !window.following && (request.kind === 'append' || request.kind === 'tail')
          const previous = window.blocks
          window.apply(pausedDuringRead ? { ...request, kind: 'status' } : request, slice)
          error = undefined
          if (window.following && window.end < window.size) wake()
          if ((kind === 'earlier' || kind === 'later') && !demand
              && previous.length === window.blocks.length && previous.every((b, i) => b === window.blocks[i])
              && window.request(kind)) page(kind)
          publish()
        }
      } catch (failure) {
        return () => {
          window.loaded = true
          error = failure instanceof RPCFailure && failure.kind === D.FailureKind.not_found ? undefined : errorText(failure)
          publish()
        }
      }
    })
    const off = session.subscribeLogs((notice) => {
      if (notice) {
        if (s(notice.runId) !== source.runId || (key(segs(notice.card)) !== key(source.card ?? []))
            || (source.invocation !== null && notice.invocation !== source.invocation)
            || (notice.endOffset !== null && notice.stream !== source.stream)) return
        if (notice.endOffset !== null && source.invocation === notice.invocation) {
          const size = Number(notice.endOffset)
          if (size <= window.size) return
          window.size = size
          if (!window.following) {
            sizeDirty = true
            frames.wake()
            return
          }
        }
      }
      wake()
    })
    publish()
    frames.wake()
    return () => { off(); frames.close() }
    // Source identity is represented by id.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [session, rpc, id])
  return snapshot.id === id ? snapshot : empty
}

const OVERDRAW = 160
const EDGE = 12

const Rows = memo(function Rows({ block }: { block: View['blocks'][number] }) {
  return <>{block.lines.map(({ number, text }) => (
    <div key={number} className="log-row" data-line={number}>
      <span className="log-number" aria-hidden="true">{number}</span><span className="log-line">{text}</span>
    </div>
  ))}</>
})

/** Keep one native scrollbar for the resident window. Only nearby blocks
 *  mount, so the browser wraps and measures those rows as they are needed. */
export function LogText({ log, placeholder }: { log: View; placeholder: string }) {
  useObserve(prefs)
  const viewport = useRef<HTMLDivElement>(null)
  const [area, setArea] = useState({ width: 0, height: 0 })
  const [top, setTop] = useState(0)
  const [measurement, remeasure] = useState(0)
  const fontSize = prefs.editorFontSize - 1
  const lineHeight = Math.ceil(fontSize * 1.45)
  const heights = useMemo(() => new WeakMap<View['blocks'][number], number>(), [area.width, lineHeight])
  const layout = useMemo(() => new LogHeights(log.blocks, heights, lineHeight), [log.blocks, heights, lineHeight, measurement])
  const current = useRef<{ layout: LogHeights; id: string }>()
  const lastScroll = useRef(0)
  const anchor = current.current?.id === log.id ? current.current.layout.anchor(top) : undefined
  const maxTop = Math.max(0, layout.total - area.height)
  const target = log.following ? maxTop : Math.max(0, Math.min(maxTop, anchor ? layout.position(anchor) : top))
  const from = layout.index(Math.max(0, target - OVERDRAW))
  const to = Math.min(layout.blocks.length, layout.index(target + area.height + OVERDRAW) + 1)

  useLayoutEffect(() => {
    const element = viewport.current!
    current.current = { layout, id: log.id }
    element.scrollTop = log.following ? element.scrollHeight : target
    lastScroll.current = element.scrollTop
    if (top !== element.scrollTop) setTop(element.scrollTop)
  }, [layout, target, log.id, log.following, top])

  useLayoutEffect(() => {
    const element = viewport.current!
    let frame: number | undefined
    const observer = new ResizeObserver(() => {
      if (frame !== undefined) return
      frame = requestAnimationFrame(() => {
        frame = undefined
        const width = element.clientWidth, height = element.clientHeight
        setArea((old) => old.width === width && old.height === height ? old : { width, height })
        let modified = false
        for (const item of element.querySelectorAll<HTMLElement>('[data-log-block]')) {
          const block = layout.blocks[Number(item.dataset.logBlock)]
          const measured = item.offsetHeight
          if ((heights.get(block) ?? block.lines.length * lineHeight) !== measured) {
            heights.set(block, measured)
            modified = true
          }
        }
        if (modified) remeasure((n) => n + 1)
      })
    })
    observer.observe(element)
    for (const item of element.querySelectorAll('[data-log-block]')) observer.observe(item)
    return () => {
      observer.disconnect()
      if (frame !== undefined) cancelAnimationFrame(frame)
    }
  }, [layout.blocks, from, to, heights, lineHeight])

  useEffect(() => () => { current.current = undefined }, [])

  const atEdge = (element: HTMLElement, direction: number) => {
    if (direction < 0 && element.scrollTop <= EDGE && log.start > 0) {
      log.follow(false)
      log.page('earlier')
    } else if (direction > 0 && element.scrollHeight - element.clientHeight - element.scrollTop <= EDGE) {
      if (log.end < log.size) log.page('later')
      else log.follow(true)
    }
  }
  return <div className="log-pane">
    <div className="log-text" ref={viewport} tabIndex={0} style={{ fontSize, lineHeight: `${lineHeight}px` }}
      onScroll={(event) => {
        const el = event.currentTarget
        const next = el.scrollTop
        const movement = next - lastScroll.current
        if (movement === 0) return
        lastScroll.current = next
        setTop(next)
        if (movement < 0) log.follow(false)
        atEdge(el, movement)
      }}
      onWheel={(event) => atEdge(event.currentTarget, event.deltaY)}
      onKeyDown={(event) => {
        if (event.key === 'Home' || event.key === 'PageUp' || event.key === 'ArrowUp') atEdge(event.currentTarget, -1)
        if (event.key === 'End' || event.key === 'PageDown' || event.key === 'ArrowDown') atEdge(event.currentTarget, 1)
      }}>
      {layout.blocks.length === 0 ? <div className="secondary log-placeholder">{placeholder}</div> : <>
        <div style={{ height: layout.top[from] }} />
        {layout.blocks.slice(from, to).map((block, i) => (
          <div key={block.key} data-log-block={from + i}><Rows block={block} /></div>
        ))}
        <div style={{ height: layout.total - layout.top[to] }} />
      </>}
    </div>
    {log.waiting && <div className="log-waiting"><Button small onClick={log.jump}>{L('新しい出力 · 末尾へ')}</Button></div>}
  </div>
}
