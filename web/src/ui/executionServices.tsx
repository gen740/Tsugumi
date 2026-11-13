import { useEffect, useState } from 'react'
import type { ExecutionServiceConnection, ExecutionServiceRegistration, ExecutionServicesState } from '../shared/executionServices'
import { L } from '../i18n'
import { api } from '../api'
import { errorText } from '../model/session'
import { Button, Form, Row, Section, Spinner, TextField, Value } from './controls'

const route = '/api/execution-services'

export function ExecutionServicesSettings({ changed }: { changed: () => void }) {
  const [hostPublicKey, setHostPublicKey] = useState('')
  function adopt(state: ExecutionServicesState) { setEntries(state.services); setHostPublicKey(state.hostPublicKey) }
  const [entries, setEntries] = useState<ExecutionServiceConnection[]>([])
  const [edit, setEdit] = useState<ExecutionServiceRegistration>()
  const [original, setOriginal] = useState<string>()
  const [busy, setBusy] = useState(false)
  const [problem, setProblem] = useState<string>()
  const [message, setMessage] = useState<string>()
  useEffect(() => { void api<ExecutionServicesState>(route).then(adopt, (e) => setProblem(errorText(e))) }, [])

  function choose(entry: ExecutionServiceRegistration, id?: string) {
    const { name, endpoint, workerPublicKey, id: serviceId } = entry
    setEdit({ id: serviceId, name, endpoint, workerPublicKey })
    setOriginal(id)
    setProblem(undefined)
    setMessage(undefined)
  }
  function update(field: keyof ExecutionServiceRegistration, value: string) {
    setEdit((previous) => previous && {
      ...previous,
      [field]: value,
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
  const caps = sameEndpoint && saved.capabilities.serverId ? saved.capabilities : undefined
  const valid = !!edit && [edit.id, edit.name, edit.endpoint, edit.workerPublicKey].every((value) => value.trim())

  return <Form>
    <Section title={L('このホストの公開鍵')}>
      <Row><code className="selectable">{hostPublicKey}</code>
        <Button disabled={!hostPublicKey} onClick={() => void perform(async () => { await navigator.clipboard.writeText(hostPublicKey) })}>{L('公開鍵をコピー')}</Button></Row>
      <Row><span className="caption">{L('ホストの公開鍵を実行デーモンの authorized_keys に追加してから、接続を確認してください。')}</span></Row>
    </Section>
    <Section title={L('実行サービス')}>
      {entries.map((entry) => <Row key={entry.id}>
        <Button kind="borderless" disabled={busy} onClick={() => choose(entry, entry.id)}>{entry.name} ({entry.id})</Button>
        <span className="secondary">{entry.capabilities.nativeSystem}</span>
      </Row>)}
      <Row><Button icon="plus" disabled={busy} onClick={() => choose({ id: '', name: '', endpoint: '', workerPublicKey: '' })}>{L('実行サービスを追加')}</Button></Row>
    </Section>
    {edit && <Section>
      <Row label={L('名前')}><TextField value={edit.name} disabled={busy} onChange={(value) => update('name', value)} /></Row>
      <Row label={L('サービス ID')}><TextField value={edit.id} mono disabled={busy || !!original} onChange={(value) => update('id', value)} /></Row>
      <Row label={L('実行デーモンの接続先')} help={L('TLS 接続先を入力してください（例: tls://worker.example:8765）。')}>
        <TextField value={edit.endpoint} placeholder="tls://worker.example:8765" mono disabled={busy} onChange={(value) => update('endpoint', value)} />
      </Row>
      <Row label={L('実行デーモンの公開鍵')} help={L('公開鍵は ed25519: で始まる SPKI 形式です。')}>
        <TextField value={edit.workerPublicKey} mono disabled={busy} onChange={(value) => update('workerPublicKey', value)} />
      </Row>
      {caps && <>
        <Value label={L('CPU アーキテクチャ')} value={caps.systems.join(', ')} />
        <Value label={L('隔離方式')} value={L('PID 名前空間')} />
        <Value label={L('CPU とメモリの上限')} value={caps.resourceLimits ? L('対応') : L('非対応')} />
        <Value label={L('最大同時ジョブ数')} value={String(caps.maxConcurrentJobs)} />
      </>}
      <Row>
        <Button kind="prominent" disabled={!valid || busy} onClick={() => void perform(async () => {
          const next = await api<ExecutionServicesState>(route + '/register', { ...edit, verify: !!original })
          const registered = next.services.find((entry) => entry.id === edit.id)
          if (!registered) throw Error(L('実行サービスを登録できませんでした。'))
          adopt(next)
          choose(registered, registered.id)
          setMessage(original ? L('接続を確認しました。') : L('公開鍵を登録しました。authorized_keys にホストの公開鍵を追加してから接続を確認してください。'))
          changed()
        })}>{original ? L('接続を確認して更新') : L('登録')}</Button>
        {original && <Button kind="destructive" disabled={busy} onClick={() => void perform(async () => {
          adopt(await api<ExecutionServicesState>(route + '/remove', { id: original }))
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
