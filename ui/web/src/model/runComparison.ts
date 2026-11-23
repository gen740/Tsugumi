// A comparison reads retained records only; it never loads them onto the canvas.
import { D, R, RPCClient, s, segs } from '../wire/rpc'
import { describeValue } from '../wire/flatjson'
import { L } from '../i18n'
import { errorText } from './session'

export interface ComparedCard { summary: R.InvocationSummaryT; record?: R.RecordResultT; error?: string }
export interface ComparedRow { card: string; left?: ComparedCard; right?: ComparedCard }
/** `unknown`: the records do not hold enough to tell, so it is never shown as unchanged. */
export type Difference = 'same' | 'changed' | 'unknown'
export interface ComparedField { name: string; left?: string; right?: string; difference: Difference }
export type ComparisonRPC = Pick<RPCClient, 'latestInvocations' | 'record'>

export async function loadRunComparison(rpc: ComparisonRPC, left: string, right: string, signal?: AbortSignal): Promise<ComparedRow[]> {
  async function load(run: string) {
    const page = await rpc.latestInvocations(run)
    const entries: ComparedCard[] = page.records.map(summary => ({ summary }))
    // Bound retained image/value downloads even for a large Workflow.
    let next = 0
    await Promise.all(Array.from({ length: Math.min(4, entries.length) }, async () => {
      while (next < entries.length && !signal?.aborted) {
        const item = entries[next++]
        try { item.record = await rpc.record(run, segs(item.summary.card), item.summary.invocation, false) }
        catch (e) { item.error = errorText(e) }
      }
    }))
    return new Map(entries.map(e => [segs(e.summary.card).join('/'), e]))
  }
  const [a, b] = await Promise.all([load(left), load(right)])
  return [...new Set([...a.keys(), ...b.keys()])].sort().map(card => ({ card, left: a.get(card), right: b.get(card) }))
}

const names = (a: Iterable<string>, b: Iterable<string>) => [...new Set([...a, ...b])].sort()

/** Parameters pair by name. A side whose record holds no parameters cannot be compared. */
export function comparedParams(left?: R.RecordResultT, right?: R.RecordResultT): ComparedField[] {
  const values = (r?: R.RecordResultT) =>
    new Map((r?.record?.params?.entries ?? []).map(p => [s(p.name), s(p.value)]))
  const unrecorded = (r?: R.RecordResultT) => !!r?.record && r.record.params === null
  const a = values(left), b = values(right), unknown = unrecorded(left) || unrecorded(right)
  return names(a.keys(), b.keys()).map(name => ({
    name, left: a.get(name), right: b.get(name), difference: unknown ? 'unknown' : a.get(name) === b.get(name) ? 'same' : 'changed',
  }))
}

const sameBytes = (a: number[], b: number[]) => a.length === b.length && a.every((v, i) => v === b[i])

/** The kind and schema come first, then the digest or the returned payload.
 *  The response may omit a large payload. Its size alone cannot show equality. */
export function valueDifference(a?: D.NamedValueT, b?: D.NamedValueT): Difference {
  if (!a || !b) return a || b ? 'unknown' : 'same'
  if (s(a.rootType) !== s(b.rootType) || !sameBytes(a.schemaBfbs, b.schemaBfbs)) return 'changed'
  const ids = (value: D.NamedValueT) => [...new Set(value.artifacts.map(artifact => s(artifact.id)))].sort()
  const leftIds = ids(a), rightIds = ids(b)
  if (leftIds.length !== rightIds.length || leftIds.some((id, i) => id !== rightIds[i])) return 'changed'
  const leftHash = s(a.contentHash), rightHash = s(b.contentHash)
  if (leftHash && rightHash) return leftHash === rightHash ? 'same' : 'changed'
  if ((!a.inlineBytes.length && a.sizeBytes !== 0n) || (!b.inlineBytes.length && b.sizeBytes !== 0n)) return 'unknown'
  return sameBytes(a.inlineBytes, b.inlineBytes) ? 'same' : 'changed'
}

export function comparedValues(side: 'inputs' | 'outputs', left?: R.RecordResultT, right?: R.RecordResultT): ComparedField[] {
  const values = (r?: R.RecordResultT) => new Map((r?.record?.[side] ?? []).map(v => [s(v.name), v]))
  const text = (v?: D.NamedValueT) => v && describeValue({
    name: s(v.name), rootType: s(v.rootType), sizeBytes: v.sizeBytes, artifacts: v.artifacts.map(a => ({ id: s(a.id), sizeBytes: a.sizeBytes })), inlineBytes: v.inlineBytes, schemaBfbs: v.schemaBfbs,
  })
  const a = values(left), b = values(right)
  return names(a.keys(), b.keys()).map(name => ({ name, left: text(a.get(name)), right: text(b.get(name)), difference: valueDifference(a.get(name), b.get(name)) }))
}

/** Where a Card's inputs came from: a Card of the same run, or a value saved by an earlier run. */
function inputSources(card: ComparedCard): Map<string, string> {
  const record = card.record?.record
  const out = new Map((record?.inputs ?? []).map(v => [s(v.name), L('保存した入力')]))
  for (const x of record?.inputSources ?? []) out.set(s(x.port), `${segs(x.card).join('/')} #${x.invocation ?? 0n}`)
  return out
}

/** What a Card ran under besides its parameters and code: the recorded Nix
 *  environment, the system it ran as, and where each input came from. */
export function comparedConditions(left?: ComparedCard, right?: ComparedCard, displayPath = (segments: string[]) => segments.join('/')): ComparedField[] {
  const summary = (c?: ComparedCard) => c && (c.record?.record?.summary ?? c.summary)
  const a = summary(left), b = summary(right)
  const field = (name: string, x: string | undefined, y: string | undefined, known: boolean): ComparedField =>
    ({ name, left: x, right: y, difference: !a || !b ? 'changed' : !known ? 'unknown' : x === y ? 'same' : 'changed' })
  const environment = (v?: R.InvocationSummaryT) => v && (s(v.environmentId) || L('記録なし'))
  const service = (v?: R.InvocationSummaryT) => s(v?.executionService) === 'local' ? L('プロジェクトの実行環境') : s(v?.executionService) || undefined
  const method = (v?: R.InvocationSummaryT) => {
    if (s(v?.emulator)) return L('エミュレーション: {0}', s(v?.emulator))
    const id = s(v?.executionService)
    return id === 'local' ? L('直接実行') : id ? L('外部の実行デーモン') : undefined
  }
  const fields = [
    // Recorded on one side only (a Card that stopped before its build, say): not comparable.
    field(L('環境'), environment(a), environment(b), !!s(a?.environmentId) && !!s(b?.environmentId)),
    field(L('実行方式'), method(a), method(b), !!method(a) && !!method(b)),
    field(L('実行サービス'), service(a), service(b), !!service(a) && !!service(b)),
  ]
  const aSystem = s(a?.executionSystem) || undefined, bSystem = s(b?.executionSystem) || undefined
  if (aSystem || bSystem) fields.push(field(L('CPU アーキテクチャ'), aSystem, bSystem, !!aSystem && !!bSystem))
  const x = left ? inputSources(left) : new Map<string, string>(), y = right ? inputSources(right) : new Map<string, string>()
  const available = (c: ComparedCard | undefined, source: string | undefined) => !c || !!source || !!c.record?.record && c.record.record.recordIo !== false
  const other = (c?: ComparedCard) => c?.record?.record ? L('この実行の Card からは届いていません') : undefined
  for (const port of names(x.keys(), y.keys())) {
    const source = x.get(port), target = y.get(port)
    const value = field(L('入力元 {0}', port), source ?? other(left), target ?? other(right), available(left, source) && available(right, target))
    const label = (v?: string) => v?.replace(/^([A-Za-z0-9_./-]+) (#\d+)$/, (_, id: string, invocation: string) => `${displayPath(id.split('/'))} ${invocation}`)
    fields.push({ ...value, left: label(value.left), right: label(value.right) })
  }
  return fields
}

/** A run starts at one Card and either goes on downstream or runs that Card alone. */
export function runScope(run?: R.RunSummaryT, displayPath = (segments: string[]) => segments.join('/')): string | undefined {
  if (!run) return
  const target = displayPath(segs(run.target))
  return run.propagate === false ? L('{0} だけ（保存した入力）', target) : L('{0} から下流', target)
}

/** What names a member of an execution list: its implementation label (a run
 *  list entry's `label`, an Input sweep's `name=value`), else its number. */
export function memberName(i: R.InvocationSummaryT): string {
  const label = s(i.implementationLabel)
  return label || (i.implementationIndex != null ? L('実装 {0}', i.implementationIndex) : L('{0} 回目', String(i.invocation ?? 0n)))
}

/** Whether an invocation belongs to an execution list whose members can be chosen. */
export const inBatch = (i?: R.InvocationSummaryT) => !!i && i.batchActivation != null && (!!s(i.implementationLabel) || (i.batchSize ?? 0) > 1)

export type MemberRPC = Pick<RPCClient, 'invocations'>

/** Every member of the batch an invocation belongs to, in batch order. */
export async function batchMembers(rpc: MemberRPC, run: string, summary: R.InvocationSummaryT, signal?: AbortSignal): Promise<R.InvocationSummaryT[]> {
  const out: R.InvocationSummaryT[] = []
  let cursor: string | null = null
  // A batch has at most 256 members; a loop's earlier batches come first.
  for (let pages = 0; pages < 64 && !signal?.aborted; pages++) {
    const page: R.InvocationPageT = await rpc.invocations(run, segs(summary.card), cursor)
    out.push(...page.records.filter(r => r.batchActivation === summary.batchActivation))
    cursor = s(page.nextCursor) || null
    if (!cursor) break
  }
  return out.sort((x, y) => (x.batchIndex ?? 0) - (y.batchIndex ?? 0))
}

export interface ComparedMember { key: string; name: string; left?: R.InvocationSummaryT; right?: R.InvocationSummaryT }

/** Pairs two batches' members by name, so a moved or added implementation
 *  does not pair different conditions. A name that occurs several times (once
 *  per arriving input) pairs by its occurrence. */
export function pairMembers(left: R.InvocationSummaryT[], right: R.InvocationSummaryT[]): ComparedMember[] {
  const keyed = (members: R.InvocationSummaryT[]) => {
    const seen = new Map<string, number>(), total = new Map<string, number>()
    for (const m of members) total.set(memberName(m), (total.get(memberName(m)) ?? 0) + 1)
    return members.map(m => {
      const name = memberName(m), n = (seen.get(name) ?? 0) + 1
      seen.set(name, n)
      return { key: `${name}\u0000${n}`, name: (total.get(name) ?? 1) > 1 ? `${name} (${n})` : name, member: m }
    })
  }
  const out = new Map<string, ComparedMember>()
  for (const { key, name, member } of keyed(left)) out.set(key, { key, name, left: member })
  for (const { key, name, member } of keyed(right)) {
    const pair = out.get(key)
    if (pair) pair.right = member
    else out.set(key, { key, name, right: member })
  }
  return [...out.values()]
}
