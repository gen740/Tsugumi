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
  private linger = 0
  constructor(private renderer: ThumbnailRenderer, private budget = 32 * 1024 * 1024, private idleMs = 1000) {}

  /** `lingerMs`: keep the renderer loaded at least this long after the work is
   *  done (a figure followed live draws again soon; loading the libraries
   *  again for every revision would cost more than keeping them). */
  request(request: ThumbnailRequest, signal: AbortSignal, lingerMs = 0): Promise<string | undefined> {
    if (signal.aborted) return Promise.resolve(undefined)
    this.linger = Math.max(this.linger, lingerMs)
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
    clearTimeout(this.release)
    this.release = setTimeout(() => { this.linger = 0; this.renderer.close() }, Math.max(this.idleMs, this.linger))
  }
}

/** How a Card's figure follows its results (the Card setting `displayMode`):
 *  `update` shows the last figure, dimmed while the Card or one upstream of
 *  it runs; `live` replaces each revision of a running Card in place. */
export type FigureMode = 'update' | 'live'
export const figureMode = (setting: unknown): FigureMode => (setting === 'live' ? 'live' : 'update')

/** The result a frame is to show: its content, the size it is drawn at, and
 *  the run and invocation it is of (`lineage`). */
export interface FigureWanted { name: string; json: string; width: number; height: number; lineage: string }
/** A picture that was made, and what it is a picture of. */
export interface FigurePicture extends FigureWanted { image?: string; error?: string }
export interface FigureState { mode: FigureMode; running: boolean; stale: boolean }
export const holdsDisplay = (state: FigureState) => state.mode === 'update' && (state.running || state.stale)
export interface FigureLook { image?: string; dimmed: boolean; waiting: boolean; error?: string }

/** What the frame shows. A picture is at full strength only as what it is:
 *  the wanted content, or (live) the previous revision of the invocation that
 *  is still running. Every other picture kept on screen is dimmed. */
export function figureLook(wanted: FigureWanted | undefined, picture: FigurePicture | undefined, state: FigureState): FigureLook {
  if (!wanted) return { dimmed: false, waiting: false }
  const same = picture !== undefined && picture.name === wanted.name && picture.json === wanted.json
  if (!picture?.image) return same && picture?.error ? { dimmed: false, waiting: false, error: picture.error } : { dimmed: false, waiting: true }
  // The same content at another size (a resize, a zoom) is still this result's picture.
  if (same) return { image: picture.image, dimmed: holdsDisplay(state), waiting: false }
  const following = state.mode === 'live' && state.running && picture.lineage === wanted.lineage
  return { image: picture.image, dimmed: !following, waiting: !following }
}

const sameWanted = (a?: FigureWanted, b?: FigureWanted) =>
  a === b || (!!a && !!b && a.name === b.name && a.json === b.json && a.width === b.width && a.height === b.height && a.lineage === b.lineage)

/** The picture one frame holds, and the work under way for the next one.
 *  The last picture stays until another replaces it (`figureLook` says how it
 *  may be shown); out of view it is dropped, and made again from the cache. */
export class FigurePreview {
  picture?: FigurePicture
  private wanted?: FigureWanted
  private following = false
  private drawing?: { wanted: FigureWanted; abort: AbortController }
  private next?: FigureWanted
  constructor(
    private queue: { request(request: ThumbnailRequest, signal: AbortSignal, lingerMs?: number): Promise<string | undefined> },
    private changed: () => void,
    private keyOf: (name: string, json: string, width: number, height: number) => Promise<string> = moduleThumbnailKey,
    private lingerMs = 10000,
  ) {}

  /** `wanted` undefined: the frame is out of view. `follow`: the Card runs and
   *  is followed live. A revision that arrives while the one before it (same
   *  run and invocation) is being drawn waits for it, and only the newest
   *  waits: the picture keeps advancing however fast revisions arrive, where
   *  restarting for each would never finish one. */
  want(wanted: FigureWanted | undefined, follow = false) {
    if (sameWanted(wanted, this.wanted) && follow === this.following) return
    this.wanted = wanted
    this.following = follow
    if (!wanted) {
      this.stop()
      if (this.picture) { this.picture = undefined; this.changed() }
      return
    }
    if (!wanted.width || !wanted.height || (this.picture?.image && sameWanted(wanted, this.picture))) {
      this.stop()
      return
    }
    if (follow && this.drawing && this.drawing.wanted.lineage === wanted.lineage) { this.next = wanted; return }
    this.stop()
    this.draw(wanted, follow)
  }

  close() { this.stop(); this.wanted = undefined }

  private stop() {
    this.drawing?.abort.abort(); this.drawing = undefined; this.next = undefined
  }

  private draw(wanted: FigureWanted, follow: boolean) {
    const abort = new AbortController()
    const drawing = this.drawing = { wanted, abort }
    const done = (picture?: FigurePicture) => {
      if (this.drawing !== drawing) return
      this.drawing = undefined
      const next = this.next
      // A revision that failed while a newer one waits is passed over: the last picture stays.
      if (picture && !(picture.error && next)) { this.picture = picture; this.changed() }
      this.next = undefined
      if (next) this.draw(next, true)
    }
    void (async () => {
      const key = await this.keyOf(wanted.name, wanted.json, wanted.width, wanted.height)
      if (abort.signal.aborted) return
      const { name, json, width, height } = wanted
      const image = await this.queue.request({ key, name, json, width, height }, abort.signal, follow ? this.lingerMs : 0)
      done(image ? { ...wanted, image } : undefined)
    })().catch(error => done({ ...wanted, error: String((error as Error)?.message ?? error) }))
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
