import { useEffect, useReducer } from 'react'

/** A mutable model object that tells its views when it changed (like
 *  @Observable). Notifications are coalesced into one per microtask. */
export class Observable {
  private listeners = new Set<() => void>()
  private scheduled = false
  version = 0

  subscribe(fn: () => void): () => void {
    this.listeners.add(fn)
    return () => this.listeners.delete(fn)
  }

  changed(): void {
    this.version++
    if (this.scheduled) return
    this.scheduled = true
    queueMicrotask(() => {
      this.scheduled = false
      for (const l of [...this.listeners]) l()
    })
  }
}

/** Re-renders the component whenever one of the objects changes. */
export function useObserve(...objs: (Observable | null | undefined)[]): void {
  const [, force] = useReducer((x: number) => x + 1, 0)
  useEffect(() => {
    const offs = objs.filter((o): o is Observable => !!o).map((o) => o.subscribe(force))
    return () => offs.forEach((f) => f())
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, objs)
}
