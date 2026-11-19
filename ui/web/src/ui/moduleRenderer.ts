import { ModuleThumbnailQueue, ThumbnailRenderer, ThumbnailRequest } from '../model/moduleThumbnails'

import { moduleSource } from '../model/moduleSources'

/** Only this hidden, sandboxed frame renders canvas thumbnails. Expanded views
 *  keep their own interactive frame. An idle or failed renderer is destroyed. */
export class FrameRenderer implements ThumbnailRenderer {
  constructor(private source = moduleSource) {}
  private frame?: HTMLIFrameElement
  private ready?: Promise<void>
  private readyResolve?: () => void
  private cancelDraw?: () => void
  private result?: { id: number; resolve(image: string): void; reject(error: Error): void }
  private sequence = 0
  private sent?: ThumbnailRequest
  private listener = (event: MessageEvent) => {
    if (event.source !== this.frame?.contentWindow) return
    const data = event.data
    if (data?.tsugumi === 'ready') this.readyResolve?.()
    else if (data?.tsugumi === 'state' && this.result && data.requestId === this.result.id) {
      if (data.state === 'error') this.result.reject(new Error(String(data.message)))
      else if (typeof data.image === 'string' && data.image.startsWith('data:image/png;') && data.image.length <= 16 * 1024 * 1024) this.result.resolve(data.image)
      else this.result.reject(new Error('The chart did not return a PNG preview'))
    }
  }

  async draw(request: ThumbnailRequest): Promise<{ image: string; pixelBytes: number }> {
    if (!this.frame) {
      const frame = document.createElement('iframe')
      frame.title = 'Chart preview renderer'
      frame.setAttribute('aria-hidden', 'true')
      frame.setAttribute('sandbox', 'allow-scripts')
      frame.tabIndex = -1
      Object.assign(frame.style, { position: 'fixed', left: '-10000px', top: '0', border: '0', pointerEvents: 'none', width: `${request.width}px`, height: `${request.height}px` })
      this.frame = frame
      this.ready = new Promise(resolve => { this.readyResolve = resolve })
      window.addEventListener('message', this.listener)
      frame.src = '/modules/viewer.html'
      document.body.append(frame)
    }
    const frame = this.frame
    const ready = this.ready
    frame.style.width = `${request.width}px`; frame.style.height = `${request.height}px`

    try {
      const image = await Promise.race([
        (async () => {
          const code = await this.source(request.name)
          await ready
          if (this.frame !== frame) throw new Error('Chart preview closed')
          return await new Promise<string>((resolve, reject) => {
            const id = ++this.sequence
            this.result = { id, resolve, reject }
            frame.contentWindow?.postMessage({ name: request.name, ...(this.sent?.name !== request.name || this.sent?.json !== request.json || this.sent?.blobs !== request.blobs ? { json: request.json, blobs: request.blobs } : {}), ...(this.sent?.name !== request.name ? { code } : {}), colorScheme: request.colorScheme, interactive: false, snapshot: true, width: request.width, height: request.height, requestId: id }, '*')
            this.sent = request
          })
        })(),
        new Promise<never>((_, reject) => { this.cancelDraw = () => reject(new Error('Chart preview closed')) }),
      ])
      // Composite Vega figures may be larger than the requested frame. Count
      // their actual PNG dimensions, not the frame size, against the cache.
      const encoded = image.slice(image.indexOf(',') + 1, image.indexOf(',') + 45)
      const bytes = Uint8Array.from(atob(encoded), c => c.charCodeAt(0))
      const header = new DataView(bytes.buffer)
      if (header.getUint32(0) !== 0x89504e47 || header.getUint32(12) !== 0x49484452) throw new Error('Invalid chart PNG')
      return { image, pixelBytes: header.getUint32(16) * header.getUint32(20) * 4 }
    } finally {
      if (this.frame === frame) { this.cancelDraw = undefined; this.result = undefined }
    }
  }

  close() {
    this.cancelDraw?.()
    this.readyResolve?.()
    this.cancelDraw = undefined
    this.result = undefined
    window.removeEventListener('message', this.listener)
    this.frame?.remove(); this.frame = undefined
    this.sent = undefined
    this.ready = undefined; this.readyResolve = undefined
  }
}

export const moduleThumbnails = new ModuleThumbnailQueue(new FrameRenderer())
