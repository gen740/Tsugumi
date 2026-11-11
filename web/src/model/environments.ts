/** Recorded history only: these readers must never evaluate the current project. */
export interface RecordedEnvironment {
  id: string
  derivation: string
  manifest: string
  system: string
  pathCount: bigint
}
export interface EnvironmentSource {
  environments(run: string, offset: bigint): Promise<{ environments: RecordedEnvironment[]; next: bigint | null }>
  paths(run: string, environment: string, offset: bigint, query: string): Promise<{ paths: string[]; next: bigint | null; total: bigint }>
}

export function storeName(path: string): string {
  const base = path.slice(path.lastIndexOf('/') + 1)
  return base.replace(/^[0-9a-z]{32}-/, '')
}

export async function findEnvironment(source: EnvironmentSource, run: string, id: string, signal?: AbortSignal) {
  if (!id) return undefined
  let offset: bigint | null = 0n
  while (offset !== null) {
    signal?.throwIfAborted()
    const page = await source.environments(run, offset)
    signal?.throwIfAborted()
    const found = page.environments.find(e => e.id === id)
    if (found) return found
    if (page.next !== null && page.next <= offset) throw Error('Environment pagination did not advance')
    offset = page.next
  }
  return undefined
}

/** Filter in the history database; each result page remains bounded. */
export async function environmentPaths(source: EnvironmentSource, run: string, id: string, offset: bigint, query: string) {
  const page = await source.paths(run, id, offset, query)
  if (page.next !== null && page.next <= offset) throw Error('Dependency pagination did not advance')
  return page
}

export interface CardEnvironment { card: string; environment?: string }
export function compareCardEnvironments(before: CardEnvironment[], after: CardEnvironment[]) {
  const old = new Map(before.map(r => [r.card, r.environment]))
  const current = new Map(after.map(r => [r.card, r.environment]))
  return [...new Set([...old.keys(), ...current.keys()])].sort().map(card => ({
    card, before: old.get(card), after: current.get(card),
    state: !old.has(card) ? 'added' : !current.has(card) ? 'removed'
      : !old.get(card) || !current.get(card) ? 'unrecorded'
      : old.get(card) === current.get(card) ? 'same' : 'changed',
  }))
}

export function groupCardEnvironments(rows: CardEnvironment[]) {
  const groups = new Map<string, string[]>()
  for (const row of rows) {
    const id = row.environment ?? ''
    groups.set(id, [...(groups.get(id) ?? []), row.card])
  }
  return [...groups].map(([id, cards]) => ({ id, cards: cards.sort() }))
}
