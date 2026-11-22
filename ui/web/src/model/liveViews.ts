import type { CardKey } from './session'

export interface ViewDeclaration {
  name: string
  type: string
  blob: string
}

export type ViewRunState = 'started' | 'ended'
export type ViewMessage = { type: 'data'; bytes: Uint8Array<ArrayBuffer> } | { type: 'run'; state: ViewRunState }

export class LiveView {
  latest?: Uint8Array<ArrayBuffer>
  private readonly listeners = new Set<(message: ViewMessage) => void>()

  constructor(readonly declaration: ViewDeclaration) {}

  listen(activeRun: string | undefined, receive: (message: ViewMessage) => void): () => void {
    this.listeners.add(receive)
    if (activeRun !== undefined) receive({ type: 'run', state: 'started' })
    if (this.latest) receive({ type: 'data', bytes: this.latest })
    return () => { this.listeners.delete(receive) }
  }

  deliver(message: ViewMessage) {
    if (message.type === 'data') this.latest = message.bytes
    for (const receive of this.listeners) receive(message)
  }
}

export class CardLiveViews {
  views = new Map<string, LiveView>()
  readonly unknown = new Set<string>()
  activeRun?: string
  viewers = 0

  declare(declarations: ViewDeclaration[]) {
    const views = new Map<string, LiveView>()
    for (const declaration of declarations) {
      const { name, type, blob } = declaration
      const prior = this.views.get(name)
      views.set(name, prior?.declaration.type === type && prior.declaration.blob === blob ? prior : new LiveView(declaration))
      this.unknown.delete(name)
    }
    this.views = views
  }

  data(name: string, bytes: Uint8Array<ArrayBuffer>) {
    const view = this.views.get(name)
    if (view) view.deliver({ type: 'data', bytes })
    else this.unknown.add(name)
  }

  run(id: string, state: ViewRunState) {
    if (state === 'started') this.activeRun = id
    else if (this.activeRun === id) this.activeRun = undefined
    for (const view of this.views.values()) view.deliver({ type: 'run', state })
  }

  clear() {
    this.views.clear()
    this.unknown.clear()
    this.activeRun = undefined
  }
}

/** Only Cards with an open viewer have live state. */
export class LiveViews extends Map<CardKey, CardLiveViews> {
  observe(card: CardKey, visibilityChanged: () => void): () => void {
    let entry = this.get(card)
    if (!entry) {
      entry = new CardLiveViews()
      this.set(card, entry)
    }
    const observed = entry
    if (++observed.viewers === 1) visibilityChanged()
    return () => {
      if (--observed.viewers > 0 || this.get(card) !== observed) return
      observed.clear()
      this.delete(card)
      visibilityChanged()
    }
  }

  reset() {
    for (const entry of this.values()) entry.clear()
  }
}
