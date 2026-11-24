import type { CardKey } from './session'

export const ViewMode = { panel: 0, window: 1, fullscreen: 2 } as const

export type ViewMessage = { bytes: Uint8Array<ArrayBuffer>; complete?: () => void }

export class LiveView {
  private receive?: (message: ViewMessage) => void
  private readonly pending: ViewMessage[] = []
  private readonly routes = new Set<string>()
  private closed = false

  constructor(
    readonly session: string, readonly card: CardKey, readonly name: string,
    readonly type: string, readonly mode: number, readonly width: number, readonly height: number,
  ) {}

  registerRoute(route: string) {
    this.routes.add(route.replaceAll('-', ''))
  }

  acceptsRoute(route: Uint8Array) {
    if (this.closed || route.length !== 16) return false
    if (route.every(byte => byte === 0)) return true
    const hex = Array.from(route, byte => byte.toString(16).padStart(2, '0')).join('')
    return this.routes.has(hex)
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
    if (this.closed) message.complete?.()
    else if (this.receive) this.receive(message)
    else this.pending.push(message)
  }

  close() {
    this.closed = true
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
