import { useEffect, useState } from 'react'
import type { ExecutionServiceConnection, ExecutionServiceRegistration } from '../../server/executionServices'
import { L } from '../i18n'
import { api } from '../api'
import { errorText } from '../model/session'
import { Button, Form, Row, Section, Spinner, TextField, Value } from './controls'

const route = '/api/execution-services'

export function ExecutionServicesSettings({ changed }: { changed: () => void }) {
  const [entries, setEntries] = useState<ExecutionServiceConnection[]>([])
  const [edit, setEdit] = useState<ExecutionServiceRegistration>()
  const [original, setOriginal] = useState<string>()
  const [busy, setBusy] = useState(false)
  const [problem, setProblem] = useState<string>()
  const [message, setMessage] = useState<string>()
  useEffect(() => { void api<ExecutionServiceConnection[]>(route).then(setEntries, (e) => setProblem(errorText(e))) }, [])

  function choose(entry: ExecutionServiceRegistration, id?: string) {
    const { name, endpoint, id: serviceId } = entry
    setEdit({ id: serviceId, name, endpoint })
    setOriginal(id)
    setProblem(undefined)
    setMessage(undefined)
  }
  function update(field: keyof ExecutionServiceRegistration, value: string) {
    setEdit((previous) => previous && {
      ...previous,
      [field]: value,
      ...(field === 'endpoint' ? { token: undefined } : {}),
    })
    setMessage(undefined)
  }
  async function perform(action: () => Promise<void>) {
    setBusy(true)
    setProblem(undefined)
    setMessage(undefined)
    try { await action() }
    catch (e) { setProblem(errorText(e)) }
    finally { setBusy(false) }
  }
  const saved = entries.find((entry) => entry.id === original)
  const sameEndpoint = !!saved && saved.endpoint === edit?.endpoint.trim()
  const caps = sameEndpoint ? saved.capabilities : undefined
  const valid = !!edit && [edit.id, edit.name, edit.endpoint].every((value) => value.trim()) &&
    (sameEndpoint && !edit.token || /^[a-f0-9]{64}$/.test(edit.token ?? ''))

  return <Form>
    <Section title={L('実行サービス')}>
      {entries.map((entry) => <Row key={entry.id}>
        <Button kind="borderless" disabled={busy} onClick={() => choose(entry, entry.id)}>{entry.name} ({entry.id})</Button>
        <span className="secondary">{entry.capabilities.nativeSystem}</span>
      </Row>)}
      <Row><Button icon="plus" disabled={busy} onClick={() => choose({ id: '', name: '', endpoint: '' })}>{L('実行サービスを追加')}</Button></Row>
    </Section>
    {edit && <Section>
      <Row label={L('名前')}><TextField value={edit.name} disabled={busy} onChange={(value) => update('name', value)} /></Row>
      <Row label={L('サービス ID')}><TextField value={edit.id} mono disabled={busy || !!original} onChange={(value) => update('id', value)} /></Row>
      <Row label={L('実行デーモンの接続先')} help={L('HTTPS の接続先、またはループバックアドレスの HTTP 接続先を入力してください。')}>
        <TextField value={edit.endpoint} placeholder="https://compute.example.org:8781" mono disabled={busy} onChange={(value) => update('endpoint', value)} />
      </Row>
      <Row label={L('認証トークン')} help={L('認証トークンは小文字の16進数64文字です。')}>
        <input type="password" className="field mono" autoComplete="off" spellCheck={false} aria-label={L('認証トークン')}
          value={edit.token ?? ''} placeholder={sameEndpoint ? L('保存済みトークンを使用') : undefined} disabled={busy} onChange={(event) => update('token', event.target.value)} />
      </Row>
      {caps && <>
        <Value label={L('CPU アーキテクチャ')} value={caps.systems.join(', ')} />
        <Value label={L('隔離方式')} value={L('PID 名前空間')} />
        <Value label={L('CPU とメモリの上限')} value={caps.resourceLimits ? L('対応') : L('非対応')} />
        <Value label={L('最大同時ジョブ数')} value={String(caps.maxConcurrentJobs)} />
      </>}
      <Row>
        <Button kind="prominent" disabled={!valid || busy} onClick={() => void perform(async () => {
          const next = await api<ExecutionServiceConnection[]>(route + '/register', { ...edit, token: edit.token || undefined })
          const registered = next.find((entry) => entry.id === edit.id)
          if (!registered) throw Error(L('実行サービスを登録できませんでした。'))
          setEntries(next)
          choose(registered, registered.id)
          setMessage(L('接続を確認しました。'))
          changed()
        })}>{original ? L('接続を確認して更新') : L('接続を確認して登録')}</Button>
        {original && <Button kind="destructive" disabled={busy} onClick={() => void perform(async () => {
          setEntries(await api<ExecutionServiceConnection[]>(route + '/remove', { id: original }))
          setEdit(undefined)
          setOriginal(undefined)
          changed()
        })}>{L('削除')}</Button>}
        {busy && <Spinner small />}
      </Row>
    </Section>}
    {(problem || message) && <Row><span className="caption selectable" style={{ color: problem ? 'var(--red)' : undefined }}>{problem ?? message}</span></Row>}
  </Form>
}
