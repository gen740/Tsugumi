import { operationFixture } from './operationFixture'
// A deterministic RPC double for presentation tests. Identity behavior itself
// is checked against the core; unrelated fixtures keep their readable keys.
import type { ProjectSession } from '../src/model/session'
import { get, str } from '../shared/json'
export function cardIds<T extends ProjectSession>(s: T): T {
  hostEdits(s)
  s.connected = true // This boundary stands in for the connected core.
  const issued = new Set<string>()
  s.cardIdProvider = async (name, prefix) => {
    const stem = name.toLowerCase().replace(/[^a-z0-9_-]/g, '-') || 'card'
    const scope = [...s.effectiveExternals].find(([p]) => p === prefix)?.[1]
    const used = new Set(prefix ? [...(scope?.definitions.keys() ?? [])] : [
      ...s.definitionDrafts.keys(), ...s.cards([]).map((c) => str(get(c, 'id')) ?? ''),
    ])
    let id = stem
    for (let n = 2; used.has(id) || issued.has(prefix + id); n++) id = `${stem}-${n}`
    issued.add(prefix + id)
    return id
  }
  const references = new Map<string, Set<string>>()
  s.referenceIdProvider = async (kind, scope, occupied) => {
    const key = kind + ':' + scope
    const used = new Set([...(references.get(key) ?? []), ...occupied])
    const prefix = kind === 'comment' ? '' : kind + '-'
    let number = used.size + 1
    while (used.has(prefix + number)) number++
    const id = prefix + number
    used.add(id)
    references.set(key, used)
    return id
  }
  return s
}

export const hostEdits = operationFixture
