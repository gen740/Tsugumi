import type { CardKey } from './session'

export interface ViewDeclaration {
  name: string
  type: string
  blob: string
}

export type ViewMessage = { type: 'data'; bytes: Uint8Array<ArrayBuffer>; complete?: () => void }

/** One entry of a Card's views list. Its index is how the core serves its file. */
export class LiveView {
  private readonly listeners = new Set<(message: ViewMessage) => void>()

  constructor(readonly declaration: ViewDeclaration, readonly index: number) {}

  listen(receive: (message: ViewMessage) => void): () => void {
    this.listeners.add(receive)
    return () => { this.listeners.delete(receive) }
  }

  deliver(message: ViewMessage) {
    const listeners = [...this.listeners]
    let remaining = listeners.length
    if (!remaining) message.complete?.()
    for (const receive of listeners)
      receive(message.complete ? {
        ...message,
        complete: () => { if (--remaining === 0) message.complete?.() },
      } : message)
  }
}

export class CardLiveViews {
  /** The declarations in the order the core sent them; names may repeat. */
  views: LiveView[] = []
  /** Names data arrived for that no declaration has. */
  readonly unknown = new Set<string>()
  /** Names more than one declaration has: data sent to them reaches no view. */
  ambiguous = new Set<string>()
  /** Why evaluating the declarations failed; unset when they were evaluated, even to an empty list. */
  failure?: string
  viewers = 0

  declare(declarations: ViewDeclaration[], failure?: string) {
    this.failure = failure
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

  data(name: string, bytes: Uint8Array<ArrayBuffer>, complete?: () => void) {
    const matching = this.views.filter(view => view.declaration.name === name)
    if (matching.length === 1) matching[0].deliver({ type: 'data', bytes, ...(complete ? { complete } : {}) })
    else {
      if (!matching.length) this.unknown.add(name)
      complete?.()
    }
  }

  clear() {
    this.views = []
    this.unknown.clear()
    this.ambiguous = new Set()
    this.failure = undefined
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
