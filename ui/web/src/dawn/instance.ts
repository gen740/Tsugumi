import { Handles, textureFormatValue, webgpuImports } from '../../shared/gen/webgpu_imports.gen.js'
import { checkViewAbi, viewExport, type ViewCallback } from './loadView'
import { sampleRate, type DawnInput, type TextArea, type ViewSize } from './workerProtocol'

const messageMax = 1 << 24
const mapRead = 1
const copyDst = 8
const copySrc = 1
const renderAttachment = 16

interface GpuTexture {
  createView(): object
  destroy(): void
}
interface GpuReadBuffer {
  mapAsync(mode: number): Promise<void>
  getMappedRange(): ArrayBuffer
  destroy(): void
}
interface DawnDevice {
  queue: { submit(commands: object[]): void }
  lost: Promise<{ message: string }>
  onuncapturederror: ((event: { error: { message: string } }) => void) | null
  createTexture(descriptor: { size: number[]; format: string; usage: number }): GpuTexture
  createBuffer(descriptor: { size: number; usage: number }): GpuReadBuffer
  createCommandEncoder(): {
    copyTextureToBuffer(source: { texture: GpuTexture }, destination: { buffer: GpuReadBuffer; bytesPerRow: number }, size: number[]): void
    finish(): object
  }
  destroy(): void
}
interface DawnGpu {
  requestAdapter(): Promise<{ requestDevice(): Promise<DawnDevice> } | null>
  getPreferredCanvasFormat(): string
}
interface DawnContext {
  configure(config: { device: DawnDevice; format: string; alphaMode: 'opaque' }): void
  unconfigure(): void
  getCurrentTexture(): GpuTexture
}

interface ViewHost {
  send(bytes: Uint8Array<ArrayBuffer>, route: Uint8Array<ArrayBuffer>): Promise<unknown>
  acceptsRoute(route: Uint8Array<ArrayBuffer>): boolean
  onSendError(error?: unknown): void
  onGpuError(message: string): void
  onFatal(error: unknown): void
  onCall(started: boolean): void
  onTextArea(area: TextArea | null): void
}

export const absoluteNow = () => performance.timeOrigin + performance.now()
const encoder = new TextEncoder()

export class DawnViewInstance {
  private handles = new Handles()
  private exports?: WebAssembly.Exports
  private memory?: WebAssembly.Memory
  private frame?: number
  private wantsFrame = false
  private scale = 1
  private stopped = false
  private currentBytes?: Uint8Array<ArrayBuffer>
  private readonly reverse: { bytes: Uint8Array<ArrayBuffer>; route: Uint8Array<ArrayBuffer> }[] = []
  private reverseBytes = 0
  private sending = false
  private origin = 0
  private lastPresent = 0n
  private textArea: TextArea | null = null

  private constructor(
    private readonly canvas: OffscreenCanvas,
    private readonly context: DawnContext,
    private readonly device: DawnDevice,
    private readonly format: string,
    private readonly options: ViewHost,
    public refreshPeriod: number,
  ) {}

  static async mount(module: WebAssembly.Module, canvas: OffscreenCanvas, size: ViewSize, options: ViewHost, refreshPeriod: number) {
    const gpu = (navigator as Navigator & { gpu?: DawnGpu }).gpu
    if (!gpu) throw new Error('WebGPU is unavailable in this worker')
    const adapter = await gpu.requestAdapter()
    if (!adapter) throw new Error('WebGPU did not provide an adapter')
    const format = gpu.getPreferredCanvasFormat()
    const formatValue = textureFormatValue.get(format)
    if (formatValue === undefined) throw new Error(`Unsupported Dawn view texture format: ${format}`)
    const context = canvas.getContext('webgpu') as unknown as DawnContext | null
    if (!context) throw new Error('Could not create the WebGPU canvas context')
    const device = await adapter.requestDevice()
    const view = new DawnViewInstance(canvas, context, device, format, options, refreshPeriod)
    try {
      view.start(module, size, formatValue)
      return view
    } catch (error) {
      view.dispose()
      throw error
    }
  }

  private start(module: WebAssembly.Module, size: ViewSize, formatValue: number) {
    const webgpu = webgpuImports({
      memory: () => {
        if (!this.memory) throw new Error('Dawn view linear memory is unavailable')
        return this.memory.buffer
      },
      handles: this.handles,
    })
    // Instantiation can run a Wasm start function too.
    this.options.onCall(true)
    try {
      const instance = new WebAssembly.Instance(module, {
        webgpu: { ...webgpu },
        tsugumi: {
          tsugumi_view_read: (ptr: number, length: number) => this.read(ptr, length),
          tsugumi_view_send: (route: number, ptr: number, length: number) => this.send(route, ptr, length),
        },
      })
      this.exports = instance.exports
      this.memory = instance.exports.memory as WebAssembly.Memory
    } finally {
      this.options.onCall(false)
    }
    checkViewAbi(this.call('abi'))
    this.resizeCanvas(size)
    this.context.configure({ device: this.device, format: this.format, alphaMode: 'opaque' })
    this.device.onuncapturederror = ({ error }) => this.options.onGpuError(error.message)
    void this.device.lost.then(({ message }) => {
      if (!this.stopped) this.options.onGpuError(message || 'WebGPU device lost')
    })
    const deviceId = this.handles.add('Device', this.device)
    const queueId = this.handles.add('Queue', this.device.queue)
    this.origin = absoluteNow()
    this.call('start', deviceId, queueId, formatValue, size.width, size.height, size.scale)
    this.draw(absoluteNow() + this.refreshPeriod)
  }

  get features() {
    return {
      pointer: !!this.callback('pointer'), key: !!this.callback('key'), gamepad: !!this.callback('gamepad'),
      composition: !!this.callback('composition'), audio: !!this.callback('audio'),
    }
  }

  private callback(name: ViewCallback) {
    const callback = this.exports?.[viewExport(name)]
    return typeof callback === 'function' ? callback : undefined
  }

  private invoke(name: ViewCallback, args: (number | bigint)[]): number | undefined {
    const callback = this.callback(name)
    if (!callback) return
    this.options.onCall(true)
    try {
      return callback(...args)
    } finally {
      this.options.onCall(false)
    }
  }

  private call(name: ViewCallback, ...args: (number | bigint)[]): number | undefined {
    const result = this.invoke(name, args)
    if (result !== undefined && result < 0) throw new Error(`${viewExport(name)} returned ${result}`)
    return result
  }

  private time(absolute: number): bigint {
    return BigInt(Math.max(0, Math.floor((absolute - this.origin) * sampleRate / 1000)))
  }

  private event(action: () => void) {
    if (this.stopped) throw new Error('The Dawn view is closed')
    try {
      action()
      this.scheduleDraw()
    } catch (error) {
      this.fail(error)
      throw error
    }
  }

  private scheduleDraw() {
    if (!this.stopped && this.frame === undefined)
      this.frame = requestAnimationFrame(timestamp => {
        this.frame = undefined
        this.draw(performance.timeOrigin + timestamp + this.refreshPeriod)
      })
  }

  private drawTarget(target: object, present: bigint) {
    const id = this.handles.add('TextureView', target)
    try {
      if (present > this.lastPresent) this.lastPresent = present
      return !!((this.call('draw', id, this.lastPresent) ?? 0) & 1)
    } finally {
      // The view may release this handle itself during draw.
      this.handles.takeIfPresent('TextureView', id)
    }
  }

  private updateTextArea() {
    const ptr = this.invoke('text_input', [])
    if (ptr === undefined) return
    if (!ptr) {
      if (this.textArea) this.options.onTextArea(null)
      this.textArea = null
      return
    }
    const bytes = this.memoryBytes(ptr, 16)
    if (!bytes) throw new Error('Dawn view returned an invalid text area')
    const data = new DataView(bytes.buffer, bytes.byteOffset, bytes.length)
    const x = data.getFloat32(0, true), y = data.getFloat32(4, true)
    const width = data.getFloat32(8, true), height = data.getFloat32(12, true)
    if (![x, y, width, height].every(Number.isFinite) || width < 0 || height < 0)
      throw new Error('Dawn view returned an invalid text area')
    const previous = this.textArea
    if (previous?.x === x && previous.y === y && previous.width === width && previous.height === height) return
    this.textArea = { x, y, width, height }
    this.options.onTextArea(this.textArea)
  }

  private draw(present: number) {
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
      this.wantsFrame = this.drawTarget(target, this.time(present))
      this.updateTextArea()
      if (this.wantsFrame) this.scheduleDraw()
    } catch (error) {
      this.fail(error)
    }
  }

  private resizeCanvas({ width, height, scale }: ViewSize) {
    if (this.canvas.width !== width) this.canvas.width = width
    if (this.canvas.height !== height) this.canvas.height = height
    this.scale = scale
  }

  resize(size: ViewSize, at: number) {
    if (size.width === this.canvas.width && size.height === this.canvas.height && size.scale === this.scale) return
    this.event(() => {
      this.resizeCanvas(size)
      this.call('resize', size.width, size.height, size.scale, this.time(at))
    })
  }

  deliver(bytes: Uint8Array<ArrayBuffer>, at: number) {
    this.event(() => this.withBytes('message', bytes, this.time(at)))
  }

  input(event: DawnInput, absolute: number) {
    const at = this.time(absolute)
    this.event(() => {
      switch (event.type) {
        case 'pointer':
          this.call('pointer', event.kind, event.x, event.y, event.buttons, event.wheelX, event.wheelY, event.modifiers, at)
          break
        case 'key':
          this.call('key', event.kind, event.code, event.modifiers, at)
          break
        case 'text':
          this.withBytes('text', encoder.encode(event.text), at)
          break
        case 'composition':
          this.withBytes('composition', encoder.encode(event.text), event.cursor, at)
          break
        case 'gamepad':
          this.call('gamepad', event.pad, event.kind, event.buttons, event.lx, event.ly, event.rx, event.ry, event.lt, event.rt, at)
          break
      }
    })
  }

  private withBytes(name: 'message' | 'text' | 'composition', bytes: Uint8Array<ArrayBuffer>, ...args: (number | bigint)[]) {
    this.currentBytes = bytes
    try {
      this.call(name, bytes.length, ...args)
    } finally {
      this.currentBytes = undefined
    }
  }

  audio(frames: number, heard: number): Float32Array<ArrayBuffer> | undefined {
    if (this.stopped) return
    try {
      const ptr = this.invoke('audio', [frames, this.time(heard)])
      if (!ptr) return
      const bytes = this.memoryBytes(ptr, frames * 8)
      if (!bytes) throw new Error('Dawn view returned an invalid audio buffer')
      return new Float32Array(bytes.slice().buffer)
    } catch (error) {
      this.fail(error)
    }
  }

  async snapshot(): Promise<Uint8Array<ArrayBuffer>> {
    if (this.stopped) throw new Error('The Dawn view is closed')
    const { width, height } = this.canvas
    const size = [width, height, 1]
    const bytesPerRow = Math.ceil(width * 4 / 256) * 256
    const texture = this.device.createTexture({ size, format: this.format, usage: renderAttachment | copySrc })
    let buffer: GpuReadBuffer | undefined
    try {
      try {
        this.wantsFrame = this.drawTarget(texture.createView(), this.time(absoluteNow() + this.refreshPeriod))
        this.updateTextArea()
        if (this.wantsFrame) this.scheduleDraw()
      } catch (error) {
        this.fail(error)
        throw error
      }
      buffer = this.device.createBuffer({ size: bytesPerRow * height, usage: mapRead | copyDst })
      const commands = this.device.createCommandEncoder()
      commands.copyTextureToBuffer({ texture }, { buffer, bytesPerRow }, size)
      this.device.queue.submit([commands.finish()])
      await buffer.mapAsync(mapRead)
      const source = new Uint8Array(buffer.getMappedRange())
      const pixels = new Uint8ClampedArray(width * height * 4)
      for (let y = 0; y < height; ++y)
        pixels.set(source.subarray(y * bytesPerRow, y * bytesPerRow + width * 4), y * width * 4)
      for (let i = 0; i < pixels.length; i += 4) {
        if (this.format === 'bgra8unorm') [pixels[i], pixels[i + 2]] = [pixels[i + 2], pixels[i]]
        pixels[i + 3] = 255
      }
      const image = new OffscreenCanvas(width, height)
      const context = image.getContext('2d')
      if (!context) throw new Error('Could not create the PNG encoder canvas')
      context.putImageData(new ImageData(pixels, width, height), 0, 0)
      return new Uint8Array(await (await image.convertToBlob({ type: 'image/png' })).arrayBuffer())
    } finally {
      buffer?.destroy()
      texture.destroy()
    }
  }

  private memoryBytes(ptr: number, size: number): Uint8Array<ArrayBuffer> | undefined {
    const buffer = this.memory?.buffer
    const offset = ptr >>> 0
    const length = size >>> 0
    if (!buffer || offset + length > buffer.byteLength) return
    return new Uint8Array(buffer, offset, length)
  }

  private read(ptr: number, size: number): number {
    if (!this.currentBytes || (size >>> 0) !== this.currentBytes.length) return -1
    const dst = this.memoryBytes(ptr, size)
    if (!dst) return -1
    dst.set(this.currentBytes)
    return 0
  }

  private send(routePtr: number, ptr: number, size: number): number {
    if (this.stopped) return -1
    const src = (size >>> 0) <= messageMax && this.reverseBytes <= messageMax
      ? this.memoryBytes(ptr, size)
      : undefined
    const route = this.memoryBytes(routePtr, 16)
    if (!src || !route || !this.options.acceptsRoute(route)) {
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
    while (this.reverse.length && !this.stopped) {
      const { bytes, route } = this.reverse.shift()!
      const size = Math.max(1, bytes.length)
      try {
        await this.options.send(bytes, route)
      } catch (error) {
        if (!this.stopped) this.options.onSendError(error)
      } finally {
        this.reverseBytes -= size
      }
    }
    this.sending = false
  }

  private fail(error: unknown) {
    this.dispose()
    this.options.onFatal(error)
  }

  dispose() {
    if (this.stopped) return
    this.stopped = true
    this.reverse.length = 0
    if (this.frame !== undefined) cancelAnimationFrame(this.frame)
    this.exports = undefined
    this.memory = undefined
    this.currentBytes = undefined
    this.handles = new Handles()
    this.device.onuncapturederror = null
    this.context.unconfigure()
    this.device.destroy()
  }
}
