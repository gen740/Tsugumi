import type { LiveViewRenderer, ViewMessage } from '../model/liveViews'
import { ViewAudio } from './browserAudio'
import { observeInput } from './browserInput'
import { viewFlags, type DawnInput, type TextArea, type ViewCommand, type ViewInput, type ViewNotice, type ViewSize } from './workerProtocol'

type DataCommand =
  | Omit<Extract<ViewCommand, { type: 'message' }>, 'id'>
  | Omit<Extract<ViewCommand, { type: 'input' }>, 'id'>
  | Omit<Extract<ViewCommand, { type: 'resize' }>, 'id'>
  | Omit<Extract<ViewCommand, { type: 'snapshot' }>, 'id'>

type ViewControls = {
  send(bytes: Uint8Array<ArrayBuffer>, route: Uint8Array<ArrayBuffer>): Promise<unknown>
  acceptsRoute(route: Uint8Array<ArrayBuffer>): boolean
  fatal(error: string): void
  gpuError(error: string): void
  sendError(error: string): void
  audioError(error: string): void
  waiting(on: boolean): void
  enableAudio(suspended: boolean): void
}

const errorText = (error: unknown) => error instanceof Error ? error.message : String(error)
const maxInputPending = 128

export class BrowserView implements LiveViewRenderer {
  private readonly worker = new Worker(new URL('./dawn-view-worker.js', import.meta.url), { type: 'module' })
  private readonly owner: Window & typeof globalThis
  private readonly observer: ResizeObserver
  private readonly events = new AbortController()
  private readonly queued: ViewMessage[] = []
  private readonly requests = new Map<number, (notice: Extract<ViewNotice, { type: 'complete' }>) => void>()
  private input?: ReturnType<typeof observeInput>
  private audio?: ViewAudio
  private textArea: TextArea | null = null
  private callTimer?: number
  private waiting = false
  private frame?: number
  private size: ViewSize
  private resizeAt = 0
  private resizeInFlight = false
  private refreshPeriod = 1000 / 60
  private readonly routes = new Set<string>()
  private nextId = 0
  private inputPending = 0
  private started = false
  private ready = false
  private disposed = false

  constructor(
    private readonly canvas: HTMLCanvasElement,
    private readonly textarea: HTMLTextAreaElement,
    private readonly flags: number,
    private readonly controls: ViewControls,
  ) {
    this.owner = canvas.ownerDocument.defaultView!
    this.size = this.measure()
    this.worker.onmessage = ({ data }: MessageEvent<ViewNotice>) => this.notice(data)
    this.worker.onerror = event => this.fail(event.message || 'Dawn view worker failed')
    this.worker.onmessageerror = () => this.fail('Dawn view worker message could not be read')
    this.observer = new this.owner.ResizeObserver(([entry]) => this.resize(entry?.devicePixelContentBoxSize?.[0]))
    try {
      this.observer.observe(canvas, { box: 'device-pixel-content-box' })
    } catch {
      this.observer.observe(canvas)
    }
    const { signal } = this.events
    this.owner.addEventListener('resize', () => this.resize(), { signal })
    canvas.addEventListener('pointerdown', () => { this.enableAudio() }, { signal })
    if (flags & viewFlags.hideCursor) canvas.style.cursor = 'none'
  }

  private now() {
    return this.owner.performance.timeOrigin + this.owner.performance.now()
  }

  private post(command: ViewCommand, transferable?: Transferable[]) {
    if (!this.disposed) this.worker.postMessage(command, transferable ?? [])
  }

  start(bytes: Uint8Array<ArrayBuffer>) {
    if (this.disposed) return
    const canvas = this.canvas.transferControlToOffscreen()
    this.post({ type: 'start', bytes, canvas, size: this.size, routes: [...this.routes], refreshPeriod: this.refreshPeriod },
      [bytes.buffer, canvas])
    this.started = true
    this.measureRefreshPeriod()
  }

  private measureRefreshPeriod() {
    const intervals: number[] = []
    let previous: number | undefined
    const measure = (timestamp: number) => {
      const delta = previous === undefined ? 0 : timestamp - previous
      previous = timestamp
      if (delta >= 3 && delta <= 40) intervals.push(delta)
      if (intervals.length < 12) {
        this.frame = this.owner.requestAnimationFrame(measure)
        return
      }
      this.frame = undefined
      intervals.sort((a, b) => a - b)
      this.refreshPeriod = intervals[intervals.length >> 1]
      this.post({ type: 'refresh', period: this.refreshPeriod })
    }
    this.frame = this.owner.requestAnimationFrame(measure)
  }

  private measure(box?: ResizeObserverSize) {
    const scale = this.owner.devicePixelRatio
    return {
      width: Math.max(1, box?.inlineSize ?? Math.round(this.canvas.clientWidth * scale)),
      height: Math.max(1, box?.blockSize ?? Math.round(this.canvas.clientHeight * scale)),
      scale,
    }
  }

  private resize(box?: ResizeObserverSize) {
    const size = this.measure(box)
    if (size.width === this.size.width && size.height === this.size.height && size.scale === this.size.scale) return
    this.size = size
    this.resizeAt = this.now()
    this.sendResize()
  }

  private sendResize() {
    if (!this.ready || this.resizeInFlight || this.disposed) return
    const size = this.size
    this.resizeInFlight = true
    const id = ++this.nextId
    this.requests.set(id, () => {
      this.resizeInFlight = false
      if (size !== this.size) this.sendResize()
    })
    this.post({ type: 'resize', id, size, at: this.resizeAt })
  }

  registerRoute(route: string) {
    this.routes.add(route)
    if (this.started) this.post({ type: 'route', route })
  }

  deliver(message: ViewMessage) {
    if (this.disposed) {
      message.complete?.()
      return
    }
    if (!this.ready) {
      this.queued.push(message)
      return
    }
    const id = ++this.nextId
    const bytes = message.bytes.slice()
    this.requests.set(id, () => message.complete?.())
    this.post({ type: 'message', id, bytes, at: message.at ?? this.now() }, [bytes.buffer])
  }

  private command(command: DataCommand): Promise<Uint8Array<ArrayBuffer> | undefined> {
    if (this.disposed || !this.ready) return Promise.reject(new Error('The Dawn view is unavailable'))
    return new Promise((resolve, reject) => {
      const id = ++this.nextId
      this.requests.set(id, notice => {
        if (notice.error) reject(new Error(notice.error))
        else resolve(notice.png)
      })
      this.post({ ...command, id })
    })
  }

  private async sendEvent(event: DawnInput, at: number) {
    if (this.inputPending >= maxInputPending) throw new Error('The Dawn view input queue is full')
    ++this.inputPending
    try {
      await this.command({ type: 'input', event, at })
    } finally {
      --this.inputPending
    }
  }

  sendInput(event: ViewInput) {
    return this.sendEvent(event, this.now())
  }

  async snapshot(): Promise<Uint8Array<ArrayBuffer>> {
    const png = await this.command({ type: 'snapshot' })
    if (!png) throw new Error('The Dawn view returned no snapshot')
    return png
  }

  enableAudio() {
    void this.audio?.enable().catch(error => this.controls.audioError(errorText(error)))
  }

  private createAudio() {
    try {
      this.audio = new ViewAudio(
        suspended => this.controls.enableAudio(suspended),
        (port, clock) => this.post({ type: 'audio', port, clock }, [port]),
        clock => this.post({ type: 'audioClock', clock }),
      )
      void this.audio.start().catch(error => {
        if (!this.disposed) this.controls.audioError(errorText(error))
      })
    } catch (error) {
      this.controls.audioError(errorText(error))
    }
  }

  private watchdog(started: boolean) {
    this.owner.clearTimeout(this.callTimer)
    if (started) {
      this.callTimer = this.owner.setTimeout(() => {
        this.waiting = true
        this.controls.waiting(true)
      }, 500)
    } else {
      this.callTimer = undefined
      if (this.waiting) {
        this.waiting = false
        this.controls.waiting(false)
      }
    }
  }

  private notice(notice: ViewNotice) {
    if (this.disposed) return
    switch (notice.type) {
      case 'ready':
        this.ready = true
        this.input = observeInput(this.canvas, this.textarea, this.flags, notice.features, (event, at) => {
          void this.sendEvent(event, at).catch(() => { /* renderer failures are reported separately */ })
        })
        if (this.textArea) this.input.textArea(this.textArea)
        if (notice.features.audio) this.createAudio()
        this.sendResize()
        for (const message of this.queued.splice(0)) this.deliver(message)
        break
      case 'call':
        this.watchdog(notice.started)
        break
      case 'complete': {
        const settle = this.requests.get(notice.id)
        this.requests.delete(notice.id)
        settle?.(notice)
        break
      }
      case 'send':
        void this.send(notice)
        break
      case 'textArea':
        this.textArea = notice.area
        this.input?.textArea(notice.area)
        break
      case 'gpuError':
        this.controls.gpuError(notice.message)
        break
      case 'sendError':
        this.controls.sendError(notice.message)
        break
      case 'fatal':
        this.fail(notice.message)
        break
    }
  }

  private async send({ bytes, route, id }: Extract<ViewNotice, { type: 'send' }>) {
    try {
      if (!this.controls.acceptsRoute(route)) throw new Error('This route does not belong to the view')
      await this.controls.send(bytes, route)
      this.post({ type: 'sendResult', id })
    } catch (error) {
      this.post({ type: 'sendResult', id, error: errorText(error) })
    }
  }

  private fail(error: string) {
    this.controls.fatal(error)
    this.dispose()
  }

  dispose() {
    if (this.disposed) return
    this.disposed = true
    this.worker.terminate()
    this.events.abort()
    this.observableTeardown()
    for (const [id, settle] of this.requests) settle({ type: 'complete', id, error: 'The Dawn view is closed' })
    this.requests.clear()
    for (const message of this.queued.splice(0)) message.complete?.()
  }

  private observableTeardown() {
    this.watchdog(false)
    this.controls.enableAudio(false)
    this.observer.disconnect()
    if (this.frame !== undefined) this.owner.cancelAnimationFrame(this.frame)
    this.audio?.dispose()
    this.input?.dispose()
    this.canvas.style.cursor = ''
  }
}
