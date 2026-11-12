import { useEffect, useState } from 'react'
import type { ExecutionServiceConnection } from '../../server/executionServices'
import { L } from '../i18n'
import { api } from '../api'
import { errorText } from '../model/session'
import { Button, Form, Row, Section, Spinner, TextField, Value } from './controls'

const endpoint = '/api/execution-services'
const blank = (): ExecutionServiceConnection => ({ id: '', name: '', sshHost: '' })

export function ExecutionServicesSettings({ changed }: { changed: () => void }) {
  const [entries, setEntries] = useState<ExecutionServiceConnection[]>([])
  const [edit, setEdit] = useState<ExecutionServiceConnection>()
  const [original, setOriginal] = useState<string>()
  const [busy, setBusy] = useState(false)
  const [problem, setProblem] = useState<string>()
  const [message, setMessage] = useState<string>()
  useEffect(() => { void api<ExecutionServiceConnection[]>(endpoint).then(setEntries, (e) => setProblem(errorText(e))) }, [])

  function choose(entry: ExecutionServiceConnection, id?: string) {
    setEdit(entry)
    setOriginal(id)
    setProblem(undefined)
    setMessage(undefined)
  }
  function update(field: 'id' | 'name' | 'sshHost' | 'gateway', value: string) {
    setEdit((previous) => {
      if (!previous) return previous
      const verified = field === 'name' || field === 'id'
      return { ...previous, [field]: field === 'gateway' && !value ? undefined : value, nativeSystem: verified ? previous.nativeSystem : undefined, systems: verified ? previous.systems : undefined }
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
  const valid = !!edit && [edit.id, edit.name, edit.sshHost].every((value) => value.trim())

  return <Form>
    <Section title={L('実行サービス')}>
      {entries.map((entry) => <Row key={entry.id}>
        <Button kind="borderless" disabled={busy} onClick={() => choose(entry, entry.id)}>{entry.name} ({entry.id})</Button>
        <span className="secondary">{entry.nativeSystem ?? L('接続未確認')}</span>
      </Row>)}
      <Row><Button icon="plus" disabled={busy} onClick={() => choose(blank())}>{L('実行サービスを追加')}</Button></Row>
    </Section>
    {edit && <Section>
      <Row label={L('名前')}><TextField value={edit.name} disabled={busy} onChange={(value) => update('name', value)} /></Row>
      <Row label={L('サービス ID')}><TextField value={edit.id} mono disabled={busy || !!original} onChange={(value) => update('id', value)} /></Row>
      <Value label={L('接続方法')} value="SSH" />
      <Value label={L('実行方式')} value={L('SSH ホストで直接実行')} />
      <Row label={L('SSH ホスト')} help={L('SSH 設定ファイルにあるホストの別名を使います。')}>
        <TextField value={edit.sshHost} mono disabled={busy} onChange={(value) => update('sshHost', value)} />
      </Row>
      <Row label={L('ゲートウェイ実行ファイル')} help={L('既定名で起動できない場合は、SSH 先の絶対パスを指定します。')}>
        <TextField value={edit.gateway ?? ''} placeholder="tsugumi-remote-gateway" mono disabled={busy} onChange={(value) => update('gateway', value)} />
      </Row>
      {edit.systems?.length ? <Value label={L('CPU アーキテクチャ')} value={edit.systems.join(', ')} /> : null}
      <Row>
        <Button disabled={!valid || busy} onClick={() => void perform(async () => {
          setEdit(await api<ExecutionServiceConnection>(endpoint + '/probe', edit))
          setMessage(L('接続を確認しました。'))
        })}>{L('接続をテスト')}</Button>
        <Button kind="prominent" disabled={!valid || busy} onClick={() => void perform(async () => {
          setEntries(await api<ExecutionServiceConnection[]>(endpoint, [...entries.filter((entry) => entry.id !== original), edit]))
          setOriginal(edit.id)
          setMessage(L('保存しました。'))
          changed()
        })}>{L('保存')}</Button>
        {original && <Button kind="destructive" disabled={busy} onClick={() => void perform(async () => {
          setEntries(await api<ExecutionServiceConnection[]>(endpoint, entries.filter((entry) => entry.id !== original)))
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
