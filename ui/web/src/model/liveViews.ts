import { routeKey, type ViewInput } from '../dawn/workerProtocol'
import type { CardKey } from './session'

export const ViewMode = { panel: 0, window: 1, fullscreen: 2 } as const

export type ViewMessage = { bytes: Uint8Array<ArrayBuffer>; complete?: () => void; at?: number }

export interface LiveViewRenderer {
  registerRoute(route: string): void
  sendInput(event: ViewInput): Promise<void>
  snapshot(): Promise<Uint8Array<ArrayBuffer>>
  dispose(): void
}

export class LiveView {
  private receive?: (message: ViewMessage) => void
  private readonly pending: ViewMessage[] = []
  private readonly routes = new Set<string>()
  private closed = false
  private renderer?: LiveViewRenderer

  constructor(
    readonly session: string, readonly card: CardKey, readonly name: string,
    readonly type: string, readonly mode: number, readonly width: number, readonly height: number,
    readonly flags = 0,
  ) {}

  registerRoute(route: string) {
    const key = route.replaceAll('-', '')
    this.routes.add(key)
    this.renderer?.registerRoute(key)
  }

  acceptsRoute(route: Uint8Array) {
    if (this.closed || route.length !== 16) return false
    if (route.every(byte => byte === 0)) return true
    return this.routes.has(routeKey(route))
  }

  attachRenderer(renderer: LiveViewRenderer): () => void {
    if (this.closed) {
      renderer.dispose()
      return () => {}
    }
    this.renderer?.dispose()
    this.renderer = renderer
    for (const route of this.routes) renderer.registerRoute(route)
    return () => {
      if (this.renderer === renderer) this.renderer = undefined
      renderer.dispose()
    }
  }

  async sendInput(event: ViewInput): Promise<void> {
    if (!this.renderer) throw new Error('This view has no active Dawn renderer')
    await this.renderer.sendInput(event)
  }

  async snapshot(): Promise<Uint8Array<ArrayBuffer>> {
    if (!this.renderer) throw new Error('This view has no active Dawn renderer')
    return await this.renderer.snapshot()
  }

  // The relay bounds the outstanding messages by credit. Keep the first
  // messages until the presentation mounts, including while a popup is blocked.
  listen(receive: (message: ViewMessage) => void): () => void {
    if (this.closed) return () => {}
    this.receive = receive
    for (const message of this.pending.splice(0)) receive(message)
    return () => {
      if (this.receive === receive) this.receive = message => message.complete?.()
    }
  }

  deliver(message: ViewMessage) {
    if (this.closed) {
      message.complete?.()
      return
    }
    const received = { ...message, at: performance.timeOrigin + performance.now() }
    if (this.receive) this.receive(received)
    else this.pending.push(received)
  }

  close() {
    this.closed = true
    this.renderer?.dispose()
    this.renderer = undefined
    this.routes.clear()
    this.receive = undefined
    for (const message of this.pending.splice(0)) message.complete?.()
  }
}

export class LiveViews extends Map<string, LiveView> {
  open(view: LiveView) {
    if (!this.has(view.session)) this.set(view.session, view)
  }

  close(session: string) {
    const view = this.get(session)
    if (!view) return false
    this.delete(session)
    view.close()
    return true
  }

  reset() {
    for (const session of this.keys()) this.close(session)
  }
}
