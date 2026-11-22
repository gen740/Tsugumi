import type { CardKey } from './session'

export interface ViewDeclaration {
  name: string
  type: string
  blob: string
}

export type ViewRunState = 'started' | 'ended'
export type ViewMessage = { type: 'data'; bytes: Uint8Array<ArrayBuffer> } | { type: 'run'; state: ViewRunState }

/** One entry of a Card's views list. Its index is how the core serves its file. */
export class LiveView {
  latest?: Uint8Array<ArrayBuffer>
  private readonly listeners = new Set<(message: ViewMessage) => void>()

  constructor(readonly declaration: ViewDeclaration, readonly index: number) {}

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
  /** The declarations in the order the core sent them; names may repeat. */
  views: LiveView[] = []
  /** Names data arrived for that no declaration has. */
  readonly unknown = new Set<string>()
  /** Names more than one declaration has: data sent to them reaches no view. */
  ambiguous = new Set<string>()
  activeRun?: string
  viewers = 0

  declare(declarations: ViewDeclaration[]) {
    const prior = this.views
    this.views = declarations.map((declaration, index) => {
      const { name, type, blob } = declaration
      const same = prior[index]?.declaration
      return same?.name === name && same.type === type && same.blob === blob ? prior[index] : new LiveView(declaration, index)
    })
    const seen = new Set<string>()
    this.ambiguous = new Set()
    for (const { name } of declarations) {
      if (seen.has(name)) this.ambiguous.add(name)
      seen.add(name)
      this.unknown.delete(name)
    }
  }

  data(name: string, bytes: Uint8Array<ArrayBuffer>) {
    const matching = this.views.filter(view => view.declaration.name === name)
    if (matching.length === 1) matching[0].deliver({ type: 'data', bytes })
    else if (!matching.length) this.unknown.add(name)
  }

  run(id: string, state: ViewRunState) {
    if (state === 'started') this.activeRun = id
    else if (this.activeRun === id) this.activeRun = undefined
    for (const view of this.views) view.deliver({ type: 'run', state })
  }

  clear() {
    this.views = []
    this.unknown.clear()
    this.ambiguous = new Set()
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
