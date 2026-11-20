import { Builder, ByteBuffer } from 'flatbuffers'
import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { LogSource, LogText, useLogStream } from '../src/ui/logWindowView'
import { D, R, RPCClient } from '../src/wire/rpc'
import '../src/styles.css'

function check(value: unknown, message: string): asserts value {
  if (!value) throw new Error(message)
}

async function test() {
  Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true })
  const frames = new Map<number, FrameRequestCallback>()
  const requesters = new Map<number, string>()
  let frameId = 0
  window.requestAnimationFrame = (fn) => {
    frames.set(++frameId, fn)
    requesters.set(frameId, new Error().stack?.split('\n').slice(2, 6).join('\n') ?? '')
    return frameId
  }
  window.cancelAnimationFrame = (id) => { frames.delete(id) }
  // Deliver resize notifications with the test's frames. Measurements come
  // from the styled element in Chromium.
  window.ResizeObserver = class {
    private active = true
    constructor(private readonly notify: () => void) {}
    observe() { queueMicrotask(() => { if (this.active) this.notify() }) }
    unobserve() {}
    disconnect() { this.active = false }
  } as unknown as typeof ResizeObserver

  let text = Array.from({ length: 7000 }, (_, i) => `${i}:${'w'.repeat(160)}\n`).join('')
  const requests: R.LogRangeRequestT[] = []
  const rpc = new RPCClient({
    close() {},
    async call(frame) {
      const message = R.Message.getRootAsMessage(new ByteBuffer(frame)).unpack()
      const read = (message.payload as R.RequestT).value as R.LogRangeRequestT
      requests.push(read)
      const end = text.length * 4
      const limit = read.limitBytes!
      const high = read.tail ? end : read.before !== null ? Number(read.before) : Math.min(end, Number(read.offset) + limit)
      const low = read.tail || read.before !== null ? Math.max(0, high - limit) : Number(read.offset)
      const prefix = text.slice(0, low / 4)
      const value = new R.LogSliceT(
        text.slice(low / 4, high / 4), BigInt(low), BigInt(high), BigInt(end), true, false, null,
        BigInt(prefix.split('\n').length), !low || prefix.endsWith('\n'), BigInt(text.length),
      )
      const builder = new Builder()
      builder.finish(new R.MessageT(message.requestId, R.Payload.Reply,
        new R.ReplyT(R.ReplyPayload.LogSlice, value)).pack(builder), 'TSG1')
      return builder.asUint8Array()
    },
  })
  let notify: (note: R.LogAvailableT) => void = () => {}
  const session = {
    rpc,
    subscribeLogs(callback: typeof notify) {
      notify = callback
      return () => {}
    },
  }
  const source: LogSource = { runId: 'fixture', card: ['work'], invocation: 1n, stream: D.LogStream.build }
  function TestView() {
    const log = useLogStream(session, source, 'terminal')
    return <LogText log={log} placeholder="empty" />
  }
  const root = createRoot(document.getElementById('root')!)
  async function tick() {
    await act(async () => {
      const current = [...frames.values()]
      frames.clear()
      current.forEach((fn) => fn(0))
    })
  }
  // The viewport after each frame, for a report when frames do not settle.
  const snapshot = () => {
    const element = document.querySelector<HTMLElement>('.log-text')
    if (!element) return 'no viewport'
    const blocks = Array.from(element.querySelectorAll<HTMLElement>('[data-log-block]'))
      .map((block) => `${block.dataset.logBlock}:${block.offsetHeight}`)
    return `width ${element.clientWidth} height ${element.clientHeight} scroll ${element.scrollTop}/${element.scrollHeight} blocks ${blocks.join(' ')}`
  }
  async function settle() {
    const states: string[] = []
    for (let i = 0; frames.size > 0; ++i) {
      check(i < 20, `frames did not settle; requested by:\n${[...frames.keys()].map((id) => requesters.get(id)).join('\n--\n')}\nlast frames:\n${states.slice(-6).join('\n')}`)
      await tick()
      states.push(snapshot())
    }
  }

  await act(async () => root.render(<TestView />))
  await settle()
  const view = document.querySelector<HTMLElement>('.log-text')!
  const rows = () => Array.from(view.querySelectorAll<HTMLElement>('.log-row'))
  const visible = () => {
    const row = rows().find((row) => row.offsetTop + row.offsetHeight > view.scrollTop)!
    return [row.dataset.line, view.scrollTop - row.offsetTop].join('/')
  }
  const scroll = async (top: number) => {
    await act(async () => {
      view.scrollTop = top
      view.dispatchEvent(new Event('scroll'))
    })
  }
  check(rows().some((row) => row.offsetHeight > Number.parseFloat(getComputedStyle(view).lineHeight)),
    'the long line did not wrap')
  check(view.scrollWidth === view.clientWidth, 'the wrapped log can scroll horizontally')

  await scroll(0)
  const oldTop = visible()
  await settle()
  check(requests.some((r) => r.before !== null), 'scrolling up did not read the preceding page')
  check(visible() === oldTop, 'prepending moved the visible wrapped row')

  for (let i = 0; i < 8; ++i) {
    await scroll(0)
    await settle()
  }
  const beforeDown = requests.length
  await scroll(view.scrollHeight)
  await settle()
  check(requests.slice(beforeDown).some((r) => !r.tail && r.before === null), 'scrolling down did not reload discarded output')

  const announce = () => notify(Object.assign(new R.LogAvailableT(), {
    runId: source.runId, invocation: 1n, stream: source.stream,
    card: new D.CardPathT(['work']), endOffset: BigInt(text.length),
  }))
  const paused = visible()
  const oldReads = requests.length
  text += 'new while paused\n'
  await act(async () => { for (let i = 0; i < 100; ++i) announce() })
  await settle()
  check(visible() === paused && requests.length === oldReads, 'new output moved the paused viewport or read the stream')
  const jump = document.querySelector<HTMLButtonElement>('.log-waiting button')
  check(jump, 'the new output button was absent while paused')
  await act(async () => jump.click())
  await settle()
  check(rows().some((r) => r.textContent?.includes('new while paused')), 'jump did not display the end')

  const appendedReads = requests.length
  const oldContent = view.textContent
  await act(async () => {
    for (let i = 0; i < 100; ++i) {
      text += `frame ${i}\n`
      announce()
    }
  })
  await tick()
  check(requests.length === appendedReads + 1 && view.textContent === oldContent, 'one frame should request one read')
  await tick()
  check(requests.length === appendedReads + 1 && rows().some((r) => r.textContent?.includes('frame 99')),
    'the next frame did not publish the coalesced read')
  check(!document.querySelector('.log-waiting button'), 'following showed the pause indicator')
  await act(async () => root.unmount())
}

Object.assign(globalThis, { logBrowserResult: test().then(() => null, (error: Error) => error.stack) })
