// A deterministic RPC double for presentation tests. Identity behavior itself
// is checked against the core; unrelated fixtures keep their readable keys.
import type { ProjectSession } from '../src/model/session'
import { get, str } from '../src/shared/json'
export function cardIds<T extends ProjectSession>(s: T): T {
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
  return s
}
