import { ModuleThumbnailQueue, ThumbnailRenderer, ThumbnailRequest } from '../model/moduleThumbnails'

export const VIEWER_LIBS = new Set(['plotly.min.js', 'vega.min.js', 'vega-lite.min.js', 'vega-embed.min.js'])
const libCode = new Map<string, Promise<string | undefined>>()
export function viewerLib(lib: string): Promise<string | undefined> {
  if (!libCode.has(lib)) libCode.set(lib, fetch(`/modules/${lib}`).then(r => r.ok ? r.text() : undefined, () => undefined))
  return libCode.get(lib)!
}

/** Only this hidden, sandboxed frame renders canvas thumbnails. Expanded views
 *  keep their own interactive frame. An idle or failed renderer is destroyed. */
class FrameRenderer implements ThumbnailRenderer {
  private frame?: HTMLIFrameElement
  private ready?: Promise<void>
  private readyResolve?: () => void
  private result?: { id: number; resolve(image: string): void; reject(error: Error): void }
  private sequence = 0
  private listener = (event: MessageEvent) => {
    if (event.source !== this.frame?.contentWindow) return
    const data = event.data
    if (data?.tsugumi === 'ready') this.readyResolve?.()
    else if (data?.tsugumi === 'need' && VIEWER_LIBS.has(data.lib)) {
      const frame = this.frame
      void viewerLib(data.lib).then(code => {
        if (frame === this.frame) frame?.contentWindow?.postMessage({ tsugumi: 'lib', lib: data.lib, code }, '*')
      })
    } else if (data?.tsugumi === 'state' && this.result && data.requestId === this.result.id) {
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
    frame.style.width = `${request.width}px`; frame.style.height = `${request.height}px`
    let timeout: ReturnType<typeof setTimeout> | undefined
    try {
      const image = await Promise.race([
        (async () => {
          await this.ready
          return await new Promise<string>((resolve, reject) => {
            const id = ++this.sequence
            this.result = { id, resolve, reject }
            frame.contentWindow?.postMessage({ name: request.name, json: request.json, interactive: false, snapshot: true, width: request.width, height: request.height, requestId: id }, '*')
          })
        })(),
        new Promise<never>((_, reject) => { timeout = setTimeout(() => reject(new Error('Chart preview timed out')), 30000) }),
      ])
      // Composite Vega figures may be larger than the requested frame. Count
      // their actual PNG dimensions, not the frame size, against the cache.
      const encoded = image.slice(image.indexOf(',') + 1, image.indexOf(',') + 45)
      const bytes = Uint8Array.from(atob(encoded), c => c.charCodeAt(0))
      const header = new DataView(bytes.buffer)
      if (header.getUint32(0) !== 0x89504e47 || header.getUint32(12) !== 0x49484452) throw new Error('Invalid chart PNG')
      return { image, pixelBytes: header.getUint32(16) * header.getUint32(20) * 4 }
    } finally {
      clearTimeout(timeout)
      this.result = undefined
    }
  }

  close() {
    window.removeEventListener('message', this.listener)
    this.frame?.remove(); this.frame = undefined
    this.ready = undefined; this.readyResolve = undefined
  }
}

export const moduleThumbnails = new ModuleThumbnailQueue(new FrameRenderer())
