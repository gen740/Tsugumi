import { useEffect, useMemo, useState } from 'react'
import { L } from '../i18n'
import { DisplaySnapshot, errorText, recordedDisplay } from '../model/session'
import { R, RPCClient, s } from '../wire/rpc'
import { Button, Select } from './controls'

interface DisplayIndexEntry { invocation: bigint; revision: bigint }
interface DisplayIndex { entries: DisplayIndexEntry[]; latest?: DisplaySnapshot }

/** ABI 8 stores at most one display per invocation. Its revision gives arrival order. */
export async function loadDisplayIndex(rpc: Pick<RPCClient, 'invocations' | 'record'>, run: string, card: string[], known: R.RecordResultT | undefined, signal: AbortSignal): Promise<DisplayIndex> {
  const entries: DisplayIndexEntry[] = []
  let latest: DisplaySnapshot | undefined
  let cursor: string | null = null
  do {
    const page: R.InvocationPageT = await rpc.invocations(run, card, cursor)
    for (const row of page.records) {
      signal.throwIfAborted()
      if (row.invocation === null || row.invocation === 0n) continue
      const record = row.invocation === known?.record?.summary?.invocation ? known : await rpc.record(run, card, row.invocation)
      const d = record?.record?.display
      if (!d) continue
      const entry = { invocation: row.invocation, revision: d.revision ?? 0n }
      entries.push(entry)
      if (!latest || latest.revision <= entry.revision) latest = recordedDisplay(d)
    }
    cursor = s(page.nextCursor) || null
  } while (cursor && !signal.aborted)
  entries.sort((a, b) => a.revision < b.revision ? -1 : a.revision > b.revision ? 1 : 0)
  return { entries, latest }
}

export function useRecordedDisplays(rpc: RPCClient | undefined, run: string | undefined, card: string, known?: R.RecordResultT, exact = false) {
  const raw = known?.record?.display
  const [index, setIndex] = useState<DisplayIndex>()
  const [selection, setSelection] = useState<bigint>()
  const [selected, setSelected] = useState<DisplaySnapshot>()
  const [error, setError] = useState<string>()
  const [attempt, setAttempt] = useState(0)
  const many = (known?.aggregate?.calls ?? known?.record?.summary?.invocation ?? 0n) > 1n
  useEffect(() => {
    setIndex(undefined); setSelection(undefined); setSelected(undefined); setError(undefined)
    if (!rpc || !run || exact || !many) return
    const controller = new AbortController()
    void loadDisplayIndex(rpc, run, card.split('/'), known, controller.signal)
      .then(v => { if (!controller.signal.aborted) setIndex(v) })
      .catch(e => { if (!controller.signal.aborted) setError(errorText(e)) })
    return () => controller.abort()
  }, [rpc, run, card, known, exact, many, attempt])
  const fallback = useMemo(() => raw ? recordedDisplay(raw) : undefined, [raw])
  const latest = index?.latest ?? fallback
  useEffect(() => {
    setSelected(undefined)
    if (!rpc || !run || selection === undefined || selection === index?.entries.at(-1)?.invocation) return
    let alive = true
    void rpc.record(run, card.split('/'), selection).then(r => {
      if (alive && r.record?.display) setSelected(recordedDisplay(r.record.display))
    }).catch(e => { if (alive) setError(errorText(e)) })
    return () => { alive = false }
  }, [rpc, run, card, selection, index])
  const last = index?.entries.at(-1)?.invocation
  return {
    display: selection === undefined || selection === last ? latest : selected,
    control: <>
      {index && index.entries.length > 1 && <Select label={L('表示の記録')} value={String(selection ?? last)}
        onChange={value => { setSelected(undefined); setSelection(BigInt(value)) }}
        options={index.entries.map((entry, i) => ({ value: String(entry.invocation), label: `${i + 1} / ${index.entries.length} · #${entry.invocation}` }))} />}
      {error && <p role="alert">{error} <Button onClick={() => setAttempt(n => n + 1)}>{L('再試行')}</Button></p>}
    </>,
  }
}
