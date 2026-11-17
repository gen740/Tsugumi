// Presentation of a landed Agent save. Every changed Card and new connection
// shares one short clock. The next event completes it; there is no queue.
import { arr, get, J, str } from '../../shared/json'

export const SAVE_CUE_SECONDS = 0.24
export const connectionKey = (c: J): string => JSON.stringify([
  str(get(get(c, 'from'), 'card')), str(get(get(c, 'from'), 'port')),
  str(get(get(c, 'to'), 'card')), str(get(get(c, 'to'), 'port')),
])
export const connectionsOf = (project: J) => arr(get(get(project, 'canvas'), 'connections')) ?? []

export class SavePresence {
  step?: { at: number; cards: Set<string>; wires: Set<string> }

  saved(cards: string[], before: J[], after: J[], now: number) {
    const old = new Set(before.map(connectionKey))
    const wires = new Set(after.map(connectionKey).filter(w => !old.has(w)))
    this.step = cards.length || wires.size ? { at: now, cards: new Set(cards), wires } : undefined
  }

  finish() { this.step = undefined }

  progress(now: number): number | undefined {
    const s = this.step
    if (!s || now < s.at || now >= s.at + SAVE_CUE_SECONDS) return undefined
    return (now - s.at) / SAVE_CUE_SECONDS
  }
}
