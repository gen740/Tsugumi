import { hostDocumentReply } from './hostDocument'
import { subgraphReply } from './subgraphHost'
// A deterministic RPC double for presentation tests. Identity behavior itself
// is checked against the core; unrelated fixtures keep their readable keys.
import { DisplayGeometriesT, DisplayGeometryT } from '../src/gen/tsugumi/host'
import type { ProjectSession } from '../src/model/session'
import { get, str } from '../src/shared/json'
export function cardIds<T extends ProjectSession>(s: T): T {
  // The same geometry returned by the host (covered by HostLayout's RPC test).
  s.displayGeometries = new DisplayGeometriesT(
    new DisplayGeometryT(1, 0, 40, 0, 320, 170), new DisplayGeometryT(0, 1, 0, 30, 320, 170))
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
  return s
}

export function hostEdits<T extends ProjectSession>(s: T): T {
  s.prepareTemplate = async (template, id) => hostDocumentReply({ template, id })
  s.prepareConnect = async (stateJson, from, to, forkId) => hostDocumentReply({ stateJson, fromCard: from[0], fromPort: from[1], toCard: to[0], toPort: to[1], forkId })
  s.prepareCopy = async (stateJson, paths, from, to, name) => hostDocumentReply({ stateJson, paths, from, to, name })
  s.prepareSubgraph = subgraphReply
  return s
}
