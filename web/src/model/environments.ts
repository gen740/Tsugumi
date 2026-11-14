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
  paths(run: string, environment: string, offset: bigint, query: string, excluding?: EnvironmentIdentity): Promise<{ paths: string[]; next: bigint | null; total: bigint }>
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
export async function environmentPaths(source: EnvironmentSource, run: string, id: string, offset: bigint, query: string, excluding?: EnvironmentIdentity) {
  const page = await source.paths(run, id, offset, query, excluding)
  if (page.next !== null && page.next <= offset) throw Error('Dependency pagination did not advance')
  return page
}

export interface EnvironmentIdentity { run: string; id: string }

/** A changed environment is not evidence that every runtime path changed. */
export function environmentChange(left: string, right: string): 'same' | 'changed' | 'unknown' {
  return !left || !right ? 'unknown' : left === right ? 'same' : 'changed'
}

export async function environmentPage(source: EnvironmentSource, run: string, offset: bigint) {
  const page = await source.environments(run, offset)
  if (page.next !== null && page.next <= offset) throw Error('Environment pagination did not advance')
  return page
}
