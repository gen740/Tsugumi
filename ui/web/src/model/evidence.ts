import { validName } from './names'
/** Project-local evidence addresses. Never accept hosts, project paths or arbitrary query fields. */
export type Evidence = { kind: 'card' | 'run' | 'log' | 'artifact'; card?: string[]; run?: string; invocation?: bigint; stream?: 'output' | 'build' | 'journal'; offset?: bigint; artifact?: string; member?: string; file?: string; lineStart?: number; lineEnd?: number }
const runNumber = /^[1-9][0-9]{0,18}$/
const uint = (v: string | null): bigint | undefined => v !== null && /^(0|[1-9][0-9]*)$/.test(v) && BigInt(v) <= 18446744073709551615n ? BigInt(v) : undefined
export const isEvidence = (href: string) => /^(card|run|log|artifact):/i.test(href)
export function parseEvidence(href: string): Evidence | undefined {
  const match = /^(card|run|log|artifact):([^?#]*)(?:\?([^#]*))?(?:#([^#]*))?$/.exec(href)
  if (!match) return
  const [, kind, address, query, fragment] = match
  const parts = address.split('/')
  const card = (values: string[]) => values.length > 0 && values.every(validName)
  try { if (query !== undefined) decodeURIComponent(query.replace(/\+/g, ' ')) } catch { return }
  const q = new URLSearchParams(query)
  if ([...q.keys()].some((k, i, all) => all.indexOf(k) !== i)) return
  if (kind === 'card') {
    if (!card(parts) || fragment !== undefined) return
    if (query === undefined) return { kind, card: parts }
    if ([...q.keys()].some(k => !['file', 'lines'].includes(k))) return
    const file = q.get('file'), lines = /^([1-9][0-9]*)-([1-9][0-9]*)$/.exec(q.get('lines') ?? '')
    if (!file || /[\\\x00-\x1f]/.test(file) || file.split('/').some(p => !p || p === '.' || p === '..') || !lines) return
    const lineStart = Number(lines[1]), lineEnd = Number(lines[2])
    if (!Number.isSafeInteger(lineEnd) || lineStart < 1 || lineEnd < lineStart) return
    return { kind, card: parts, file, lineStart, lineEnd }
  }
  if (kind === 'artifact') {
    if (fragment !== undefined || !/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(address) || [...q.keys()].some(k => k !== 'path')) return
    const member = q.get('path') ?? undefined
    if (member !== undefined && (/[\\\x00-\x1f]/.test(member) || member.split('/').some(p => !p || p === '.' || p === '..'))) return
    return { kind, artifact: address.toLowerCase(), member }
  }
  const run = parts.shift()!
  if (!runNumber.test(run) || BigInt(run) > 9223372036854775807n) return
  if (parts.length && !card(parts)) return
  if (kind === 'run') {
    if (query !== undefined || fragment !== undefined && (!parts.length || !uint(fragment))) return
    return { kind, run, card: parts.length ? parts : undefined, invocation: uint(fragment ?? null) }
  }
  if (fragment !== undefined) return
  if ([...q.keys()].some(k => !['invocation', 'stream', 'offset'].includes(k))) return
  const invocation = uint(q.get('invocation')), offset = uint(q.get('offset')) ?? 0n
  if (q.has('invocation') && (!parts.length || !invocation) || q.has('offset') && uint(q.get('offset')) === undefined) return
  const stream = q.get('stream') ?? 'output'
  if (!['output', 'build', 'journal'].includes(stream)) return
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
    if (target.kind === 'artifact') return await source.artifactExists(target)
    if (!await source.runExists(target.run!)) return false
    if (target.card && !await source.recordExists(target.run!, target.card, target.invocation)) return false
    if (target.kind === 'log') return await source.logExists(target)
    return true
  } catch { return false }
}

/** Each source-citing paragraph is an independently selectable finding. */
export function findings(text: string): { text: string; target: Evidence }[] {
  return text.split(/\n\s*\n/).flatMap(text => {
    const links = [...text.matchAll(/\[[^\]]*\]\((card:[^\s)]+)\)/g)]
    const target = links.map(m => parseEvidence(m[1])).find(t => t?.file)
    return target ? [{ text, target }] : []
  })
}
