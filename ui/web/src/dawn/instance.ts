import { Handles, textureFormatValue, webgpuImports } from '../../shared/gen/webgpu_imports.gen.js'
import type { ViewMessage } from '../model/liveViews'
import { keyUsage, modifiers, viewKeysAttribute, wheelPixels } from './input'
import { checkViewAbi, viewExport, type ViewCallback } from './loadView'

const messageMax = 1 << 24

// TypeScript's DOM library does not include WebGPU. The generator owns the
// descriptor types; the host only needs the browser's device and canvas APIs.
interface DawnDevice {
  queue: object
  lost: Promise<{ message: string }>
  onuncapturederror: ((event: { error: { message: string } }) => void) | null
}
interface DawnGpu {
  requestAdapter(): Promise<{ requestDevice(): Promise<DawnDevice> } | null>
  getPreferredCanvasFormat(): string
}
interface DawnContext {
  configure(config: { device: DawnDevice; format: string; alphaMode: 'opaque' }): void
  unconfigure(): void
  getCurrentTexture(): { createView(): object }
}

export const browserGpu = (): DawnGpu | undefined => (navigator as Navigator & { gpu?: DawnGpu }).gpu

// Every canvas presented by this opener, including its child windows,
// uses one device. Each view still owns its WebGPU handles and canvas context.
interface SharedDawnDevice {
  device: DawnDevice
  format: string
  errors: Set<(message: string) => void>
}

let gpuDevice: Promise<SharedDawnDevice> | undefined

function sharedDevice() {
  if (!gpuDevice) {
    const pending = (async () => {
      const gpu = browserGpu()
      if (!gpu) throw new Error('WebGPU is unavailable')
      const adapter = await gpu.requestAdapter()
      if (!adapter) throw new Error('WebGPU did not provide an adapter')
      const device = await adapter.requestDevice()
      const errors = new Set<(message: string) => void>()
      const report = (message: string) => { for (const listener of errors) listener(message) }
      device.onuncapturederror = ({ error }) => report(error.message)
      void device.lost.then(({ message }) => {
        if (gpuDevice === pending) gpuDevice = undefined
        report(message || 'WebGPU device lost')
      })
      return { device, errors, format: gpu.getPreferredCanvasFormat() }
    })()
    gpuDevice = pending
    void pending.catch(() => { if (gpuDevice === pending) gpuDevice = undefined })
  }
  return gpuDevice
}

interface ViewHost {
  send(bytes: Uint8Array<ArrayBuffer>, route: Uint8Array<ArrayBuffer>): Promise<unknown>
  acceptsRoute?(route: Uint8Array<ArrayBuffer>): boolean
  onSendError(error?: unknown): void
  onGpuError(message: string): void
  onFatal(error: unknown): void
}

export class DawnViewInstance {
  private handles = new Handles()
  private readonly events = new AbortController()
  private exports?: WebAssembly.Exports
  private memory?: WebAssembly.Memory
  private observer?: ResizeObserver
  private frame?: number
  private wantsFrame = false
  private configured = false
  private scale = 1
  private stopped = false
  private disposed = false
  private currentMessage?: Uint8Array<ArrayBuffer>
  private readonly reverse: { bytes: Uint8Array<ArrayBuffer>; route: Uint8Array<ArrayBuffer> }[] = []
  private reverseBytes = 0
  private sending = false

  private constructor(
    private readonly canvas: HTMLCanvasElement,
    private readonly context: DawnContext,
    private readonly shared: SharedDawnDevice,
    private readonly owner: Window & typeof globalThis,
    private readonly options: ViewHost,
  ) {}

  static async mount(module: WebAssembly.Module, canvas: HTMLCanvasElement, options: ViewHost, signal: AbortSignal) {
    signal.throwIfAborted()
    const shared = await sharedDevice()
    signal.throwIfAborted()
    const owner = canvas.ownerDocument.defaultView
    if (!owner) throw new Error('The WebGPU canvas has no window')
    const { format } = shared
    const formatValue = textureFormatValue.get(format)
    if (formatValue === undefined) throw new Error(`Unsupported Dawn view texture format: ${format}`)
    const context = canvas.getContext('webgpu') as unknown as DawnContext | null
    if (!context) throw new Error('Could not create the WebGPU canvas context')
    const view = new DawnViewInstance(canvas, context, shared, owner, options)
    try {
      view.start(module, format, formatValue)
      return view
    } catch (error) {
      view.dispose()
      throw error
    }
  }

  private start(module: WebAssembly.Module, format: string, formatValue: number) {
    const webgpu = webgpuImports({
      memory: () => {
        if (!this.memory) throw new Error('Dawn view linear memory is unavailable')
        return this.memory.buffer
      },
      handles: this.handles,
    })
    const instance = new WebAssembly.Instance(module, {
      webgpu: { ...webgpu },
      tsugumi: {
        tsugumi_view_read: (ptr: number, size: number) => this.read(ptr, size),
        tsugumi_view_send: (route: number, ptr: number, size: number) => this.send(route, ptr, size),
      },
    })
    this.exports = instance.exports
    this.memory = instance.exports.memory as WebAssembly.Memory
    checkViewAbi(this.call('abi'))
    this.resizeCanvas()
    const { device, errors } = this.shared
    this.context.configure({ device, format, alphaMode: 'opaque' })
    this.configured = true
    errors.add(this.options.onGpuError)

    const deviceId = this.handles.add('Device', device)
    const queueId = this.handles.add('Queue', device.queue)
    this.call('start', deviceId, queueId, formatValue, this.canvas.width, this.canvas.height, this.scale)
    this.draw()
    if (!this.stopped) this.observe()
  }

  private callback(name: ViewCallback) {
    const callback = this.exports?.[viewExport(name)]
    return typeof callback === 'function' ? callback : undefined
  }

  private call(name: ViewCallback, ...args: number[]): number | undefined {
    const callback = this.callback(name)
    if (!callback) return
    const result = callback(...args)
    if (result < 0) throw new Error(`${viewExport(name)} returned ${result}`)
    return result
  }

  private event(name: 'resize' | 'pointer' | 'key', ...args: number[]) {
    if (this.stopped) return
    try {
      this.call(name, ...args)
      this.scheduleDraw()
    } catch (error) {
      this.fail(error)
    }
  }

  private scheduleDraw() {
    if (!this.stopped && this.frame === undefined)
      this.frame = this.owner.requestAnimationFrame(() => {
        this.frame = undefined
        this.draw()
      })
  }

  private draw() {
    if (this.stopped) return
    let target: object
    try {
      target = this.context.getCurrentTexture().createView()
    } catch (error) {
      this.options.onGpuError(String(error))
      if (this.wantsFrame) this.scheduleDraw()
      return
    }
    try {
      const id = this.handles.add('TextureView', target)
      try {
        this.wantsFrame = !!((this.call('draw', id) ?? 0) & 1)
      } finally {
        // The view may release this handle itself during draw.
        this.handles.takeIfPresent('TextureView', id)
      }
      if (this.wantsFrame) this.scheduleDraw()
    } catch (error) {
      this.fail(error)
    }
  }

  private resizeCanvas(box?: ResizeObserverSize): boolean {
    const scale = this.owner.devicePixelRatio
    const width = Math.max(1, box?.inlineSize ?? Math.round(this.canvas.clientWidth * scale))
    const height = Math.max(1, box?.blockSize ?? Math.round(this.canvas.clientHeight * scale))
    const changed = this.canvas.width !== width || this.canvas.height !== height || this.scale !== scale
    if (this.canvas.width !== width) this.canvas.width = width
    if (this.canvas.height !== height) this.canvas.height = height
    this.scale = scale
    return changed
  }

  private observe() {
    const { canvas } = this
    const signal = this.events.signal
    const resize = (box?: ResizeObserverSize) => {
      if (!this.stopped && this.resizeCanvas(box))
        this.event('resize', canvas.width, canvas.height, this.scale)
    }
    this.observer = new this.owner.ResizeObserver(([entry]) => resize(entry?.devicePixelContentBoxSize?.[0]))
    try {
      this.observer.observe(canvas, { box: 'device-pixel-content-box' })
    } catch {
      this.observer.observe(canvas)
    }
    this.owner.addEventListener('resize', () => resize(), { signal })

    if (this.callback('pointer')) {
      const pointer = (event: MouseEvent, kind: number, dx = 0, dy = 0) => {
        const rect = canvas.getBoundingClientRect()
        this.event('pointer', kind, event.clientX - rect.left, event.clientY - rect.top,
          event.buttons & 7, dx, dy, modifiers(event))
      }
      canvas.addEventListener('pointermove', event => pointer(event, 0), { signal })
      canvas.addEventListener('pointerdown', event => pointer(event, 1), { signal })
      canvas.addEventListener('pointerup', event => pointer(event, 2), { signal })
      canvas.addEventListener('wheel', event => {
        event.preventDefault()
        pointer(event, 3, ...wheelPixels(event, canvas))
      }, { signal, passive: false })
      canvas.addEventListener('pointerleave', event => pointer(event, 4), { signal })
    }
    if (this.callback('key')) {
      const key = (event: KeyboardEvent, kind: number) => {
        const usage = keyUsage(event.code)
        if (usage === undefined) return
        // Space and the arrows would scroll the page; Tab still moves the focus, and ⌘ keys stay the browser's.
        if (event.code !== 'Tab' && !event.metaKey) event.preventDefault()
        this.event('key', kind, usage, modifiers(event))
      }
      canvas.addEventListener('keydown', event => key(event, 0), { signal })
      canvas.addEventListener('keyup', event => key(event, 1), { signal })
      // While the canvas has the focus, the app's own shortcuts (Esc included) leave its keys alone.
      canvas.setAttribute(viewKeysAttribute, '')
    }
  }

  deliver(message: ViewMessage) {
    try {
      if (this.stopped || !this.callback('message')) return
      this.currentMessage = message.bytes
      this.call('message', message.bytes.length)
    } catch (error) {
      this.fail(error)
    } finally {
      this.currentMessage = undefined
      message.complete?.()
    }
    this.scheduleDraw()
  }

  private memoryBytes(ptr: number, size: number): Uint8Array<ArrayBuffer> | undefined {
    const buffer = this.memory?.buffer
    const offset = ptr >>> 0
    const length = size >>> 0
    if (!buffer || offset + length > buffer.byteLength) return
    return new Uint8Array(buffer, offset, length)
  }

  private read(ptr: number, size: number): number {
    if (!this.currentMessage || (size >>> 0) !== this.currentMessage.length) return -1
    const dst = this.memoryBytes(ptr, size)
    if (!dst) return -1
    dst.set(this.currentMessage)
    return 0
  }

  private send(routePtr: number, ptr: number, size: number): number {
    if (this.stopped) return -1
    // The relay accepts one full message while the pending bytes are at or below the limit.
    const src = (size >>> 0) <= messageMax && this.reverseBytes <= messageMax
      ? this.memoryBytes(ptr, size)
      : undefined
    const route = this.memoryBytes(routePtr, 16)
    if (!src || !route || this.options.acceptsRoute?.(route) === false) {
      this.options.onSendError()
      return -1
    }
    const bytes = src.slice()
    this.reverseBytes += Math.max(1, bytes.length)
    this.reverse.push({ bytes, route: route.slice() })
    if (!this.sending) {
      this.sending = true
      queueMicrotask(() => { void this.drain() })
    }
    return 0
  }

  private async drain() {
    while (this.reverse.length && !this.disposed) {
      const { bytes, route } = this.reverse.shift()!
      try {
        await this.options.send(bytes, route)
      } catch (error) {
        if (!this.disposed) this.options.onSendError(error)
      } finally {
        this.reverseBytes -= Math.max(1, bytes.length)
      }
    }
    this.sending = false
  }

  private fail(error: unknown) {
    this.stop()
    this.options.onFatal(error)
  }

  private stop() {
    if (this.stopped) return
    this.stopped = true
    this.events.abort()
    this.canvas.removeAttribute(viewKeysAttribute)
    this.observer?.disconnect()
    if (this.frame !== undefined) this.owner.cancelAnimationFrame(this.frame)
    this.exports = undefined
    this.memory = undefined
    this.currentMessage = undefined
    this.handles = new Handles()
    this.shared.errors.delete(this.options.onGpuError)
    if (this.configured) this.context.unconfigure()
  }

  dispose() {
    this.disposed = true
    this.reverse.length = 0
    this.stop()
  }
}
