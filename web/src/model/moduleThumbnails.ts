/** A bounded, cancellable queue. One renderer owns one chart runtime at a time. */
export interface ThumbnailRequest {
  key: string
  name: string
  json: string
  width: number
  height: number
}
export interface ThumbnailRenderer {
  draw(request: ThumbnailRequest): Promise<{ image: string; pixelBytes: number }>
  close(): void
}
type Waiter = { resolve(image: string | undefined): void; reject(error: unknown): void }
type Job = { request: ThumbnailRequest; waiting: Set<Waiter> }

export class ModuleThumbnailQueue {
  private cache = new Map<string, { image: string; cost: number }>()
  private cost = 0
  private jobs = new Map<string, Job>()
  private active?: Job
  private release?: ReturnType<typeof setTimeout>
  constructor(private renderer: ThumbnailRenderer, private budget = 32 * 1024 * 1024, private idleMs = 1000) {}

  request(request: ThumbnailRequest, signal: AbortSignal): Promise<string | undefined> {
    if (signal.aborted) return Promise.resolve(undefined)
    const cached = this.cache.get(request.key)
    if (cached) {
      this.cache.delete(request.key); this.cache.set(request.key, cached)
      return Promise.resolve(cached.image)
    }
    clearTimeout(this.release)
    let job = this.jobs.get(request.key)
    if (!job) { job = { request, waiting: new Set() }; this.jobs.set(request.key, job) }
    const target = job
    return new Promise((resolve, reject) => {
      const waiter: Waiter = {
        resolve: image => { signal.removeEventListener('abort', abort); resolve(image) },
        reject: error => { signal.removeEventListener('abort', abort); reject(error) },
      }
      const abort = () => {
        target.waiting.delete(waiter); waiter.resolve(undefined)
        if (!target.waiting.size && target !== this.active) this.jobs.delete(request.key)
      }
      target.waiting.add(waiter)
      signal.addEventListener('abort', abort, { once: true })
      void this.work()
    })
  }

  private async work() {
    if (this.active) return
    for (const job of this.jobs.values()) {
      if (!job.waiting.size) { this.jobs.delete(job.request.key); continue }
      this.active = job
      try {
        // Yield between charts, allowing input and paint before the next library call.
        await new Promise(resolve => setTimeout(resolve, 0))
        if (!job.waiting.size) continue
        const { image, pixelBytes } = await this.renderer.draw(job.request)
        // Keep both encoded bytes and decoded pixels within the cache budget.
        const cost = image.length * 2 + pixelBytes
        if (job.waiting.size && cost <= this.budget) {
          while (this.cost + cost > this.budget && this.cache.size) {
            const first = this.cache.keys().next().value!
            this.cost -= this.cache.get(first)!.cost; this.cache.delete(first)
          }
          this.cache.set(job.request.key, { image, cost }); this.cost += cost
        }
        for (const waiter of job.waiting) waiter.resolve(image)
      } catch (error) {
        for (const waiter of job.waiting) waiter.reject(error)
        this.renderer.close()
      } finally {
        this.jobs.delete(job.request.key)
        this.active = undefined
      }
    }
    this.release = setTimeout(() => this.renderer.close(), this.idleMs)
  }
}

/** Content identity includes every byte, not point counts or a run's revision number. */
export async function moduleThumbnailKey(name: string, json: string, width: number, height: number): Promise<string> {
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(json))
  return `${name}:${width}:${height}:` + [...new Uint8Array(digest)].map(n => n.toString(16).padStart(2, '0')).join('')
}

/** Preserve a preview through layout's sub-pixel rounding at any canvas zoom. */
export function settledThumbnailSize(width: number, height: number, zoom: number, previous: { width: number; height: number }) {
  const settle = (pixels: number, before: number, max: number) => {
    const raw = Math.max(16, Math.min(max, pixels / Math.max(zoom, 0.01)))
    return before && Math.abs(before - raw) <= 12 ? before : Math.round(raw / 16) * 16
  }
  const w = settle(width, previous.width, 1600), h = settle(height, previous.height, 1200)
  return w === previous.width && h === previous.height ? previous : { width: w, height: h }
}
