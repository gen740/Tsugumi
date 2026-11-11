/** Project-local evidence addresses. Never accept hosts, project paths or arbitrary query fields. */
export type Evidence = { kind: 'card' | 'run' | 'log' | 'artifact'; card?: string[]; run?: string; invocation?: bigint; stream?: 'stdout' | 'stderr' | 'build' | 'journal'; offset?: bigint; artifact?: string; member?: string }
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i
const uint = (v: string | null): bigint | undefined => v !== null && /^(0|[1-9][0-9]*)$/.test(v) && BigInt(v) <= 18446744073709551615n ? BigInt(v) : undefined
export const isEvidence = (href: string) => /^(card|run|log|artifact):/i.test(href)
export function parseEvidence(href: string): Evidence | undefined {
  const match = /^(card|run|log|artifact):([^?#]*)(?:\?([^#]*))?(?:#([^#]*))?$/.exec(href)
  if (!match) return
  const [, kind, address, query, fragment] = match
  const parts = address.split('/')
  const card = (values: string[]) => values.length > 0 && values.every(v => /^[A-Za-z0-9_.-]+$/.test(v) && v !== '.' && v !== '..' && v.length <= 128)
  try { if (query !== undefined) decodeURIComponent(query.replace(/\+/g, ' ')) } catch { return }
  const q = new URLSearchParams(query)
  if ([...q.keys()].some((k, i, all) => all.indexOf(k) !== i)) return
  if (kind === 'card') return query === undefined && fragment === undefined && card(parts) ? { kind, card: parts } : undefined
  const run = parts.shift()!
  if (!uuid.test(run)) return
  if (parts.length && !card(parts)) return
  if (kind === 'run') {
    if (query !== undefined || fragment !== undefined && (!parts.length || !uint(fragment))) return
    return { kind, run, card: parts.length ? parts : undefined, invocation: uint(fragment ?? null) }
  }
  if (fragment !== undefined) return
  if (kind === 'artifact') {
    if (parts.length || [...q.keys()].some(k => !['id', 'path'].includes(k)) || !q.get('id')) return
    const member = q.get('path') ?? undefined
    if (member !== undefined && (!member || member.startsWith('/') || member.split('/').some(p => !p || p === '.' || p === '..'))) return
    return { kind, run, artifact: q.get('id')!, member }
  }
  if ([...q.keys()].some(k => !['invocation', 'stream', 'offset'].includes(k))) return
  const invocation = uint(q.get('invocation')), offset = uint(q.get('offset')) ?? 0n
  if (q.has('invocation') && (!parts.length || !invocation) || q.has('offset') && uint(q.get('offset')) === undefined) return
  const stream = q.get('stream') ?? 'stdout'
  if (!['stdout', 'stderr', 'build', 'journal'].includes(stream)) return
  return { kind: 'log', run, card: parts.length ? parts : undefined, invocation, offset, stream: stream as Evidence['stream'] }
}

/** Resolve through the current project's public contract; no path can select another project. */
export interface EvidenceSource {
  cardExists(card: string[]): boolean
  runExists(run: string): Promise<boolean>
  recordExists(run: string, card: string[], invocation?: bigint): Promise<boolean>
  logExists(target: Evidence): Promise<boolean>
  artifactExists(target: Evidence): Promise<boolean>
}
export async function evidenceExists(target: Evidence, source: EvidenceSource): Promise<boolean> {
  try {
    if (target.kind === 'card') return source.cardExists(target.card!)
    if (!await source.runExists(target.run!)) return false
    if (target.card && !await source.recordExists(target.run!, target.card, target.invocation)) return false
    if (target.kind === 'log') return await source.logExists(target)
    if (target.kind === 'artifact') return await source.artifactExists(target)
    return true
  } catch { return false }
}
